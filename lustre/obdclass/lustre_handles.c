/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_support.h>
#include <lustre_handles.h>
#include <lustre_lib.h>

#if !defined(HAVE_RCU) || !defined(__KERNEL__)
# define list_add_rcu            list_add
# define list_del_rcu            list_del
# define list_for_each_rcu       list_for_each
# define list_for_each_safe_rcu  list_for_each_safe
# define rcu_read_lock()         spin_lock(&bucket->lock)
# define rcu_read_unlock()       spin_unlock(&bucket->lock)
#endif /* ifndef HAVE_RCU */

static __u64 handle_base;
#define HANDLE_INCR 7
static spinlock_t handle_base_lock;

static struct handle_bucket {
        spinlock_t lock;
        struct list_head head;
} *handle_hash;

static atomic_t handle_count = ATOMIC_INIT(0);

#define HANDLE_HASH_SIZE (1 << 14)
#define HANDLE_HASH_MASK (HANDLE_HASH_SIZE - 1)

/*
 * Generate a unique 64bit cookie (hash) for a handle and insert it into
 * global (per-node) hash-table.
 */
void class_handle_hash(struct portals_handle *h, portals_handle_addref_cb cb)
{
        struct handle_bucket *bucket;
        ENTRY;

        LASSERT(h != NULL);
        LASSERT(list_empty(&h->h_link));

        /*
         * This is fast, but simplistic cookie generation algorithm, it will
         * need a re-do at some point in the future for security.
         */
        spin_lock(&handle_base_lock);
        handle_base += HANDLE_INCR;

        h->h_cookie = handle_base;
        if (unlikely(handle_base == 0)) {
                /*
                 * Cookie of zero is "dangerous", because in many places it's
                 * assumed that 0 means "unassigned" handle, not bound to any
                 * object.
                 */
                CWARN("The universe has been exhausted: cookie wrap-around.\n");
                handle_base += HANDLE_INCR;
        }
        spin_unlock(&handle_base_lock);

        atomic_inc(&handle_count);
        h->h_addref = cb;
        spin_lock_init(&h->h_lock);

        bucket = &handle_hash[h->h_cookie & HANDLE_HASH_MASK];
        spin_lock(&bucket->lock);
        list_add_rcu(&h->h_link, &bucket->head);
        spin_unlock(&bucket->lock);

        CDEBUG(D_INFO, "added object %p with handle "LPX64" to hash\n",
               h, h->h_cookie);
        EXIT;
}

static void class_handle_unhash_nolock(struct portals_handle *h)
{
        if (list_empty(&h->h_link)) {
                CERROR("removing an already-removed handle ("LPX64")\n",
                       h->h_cookie);
                return;
        }

        CDEBUG(D_INFO, "removing object %p with handle "LPX64" from hash\n",
               h, h->h_cookie);

        spin_lock(&h->h_lock);
        if (h->h_cookie == 0) {
                spin_unlock(&h->h_lock);
                return;
        }
        h->h_cookie = 0;
        spin_unlock(&h->h_lock);
        list_del_rcu(&h->h_link);
}

void class_handle_unhash(struct portals_handle *h)
{
        struct handle_bucket *bucket;
        bucket = handle_hash + (h->h_cookie & HANDLE_HASH_MASK);

        spin_lock(&bucket->lock);
        class_handle_unhash_nolock(h);
        spin_unlock(&bucket->lock);

        atomic_dec(&handle_count);
}

void *class_handle2object(__u64 cookie)
{
        struct handle_bucket *bucket;
        struct list_head *tmp;
        void *retval = NULL;
        ENTRY;

        LASSERT(handle_hash != NULL);

        /* Be careful when you want to change this code. See the 
         * rcu_read_lock() definition on top this file. - jxiong */
        bucket = handle_hash + (cookie & HANDLE_HASH_MASK);

        rcu_read_lock();
        list_for_each_rcu(tmp, &bucket->head) {
                struct portals_handle *h;
                h = list_entry(tmp, struct portals_handle, h_link);
                if (h->h_cookie != cookie)
                        continue;

                spin_lock(&h->h_lock);
                if (likely(h->h_cookie != 0)) {
                        h->h_addref(h);
                        retval = h;
                }
                spin_unlock(&h->h_lock);
                break;
        }
        rcu_read_unlock();

        RETURN(retval);
}

void class_handle_free_cb(struct rcu_head *rcu)
{
        struct portals_handle *h = RCU2HANDLE(rcu);
        if (h->h_free_cb) {
                h->h_free_cb(h->h_ptr, h->h_size);
        } else {
                void *ptr = h->h_ptr;
                unsigned int size = h->h_size;
                OBD_FREE(ptr, size);
        }
}


int class_handle_init(void)
{
        struct handle_bucket *bucket;

        LASSERT(handle_hash == NULL);

        OBD_VMALLOC(handle_hash, sizeof(*bucket) * HANDLE_HASH_SIZE);
        if (handle_hash == NULL)
                return -ENOMEM;

        spin_lock_init(&handle_base_lock);
        for (bucket = handle_hash + HANDLE_HASH_SIZE - 1; bucket >= handle_hash;
             bucket--) {
                CFS_INIT_LIST_HEAD(&bucket->head);
                spin_lock_init(&bucket->lock);
        }

        ll_get_random_bytes(&handle_base, sizeof(handle_base));
        LASSERT(handle_base != 0ULL);

        return 0;
}

static void cleanup_all_handles(void)
{
        int i;

        for (i = 0; i < HANDLE_HASH_SIZE; i++) {
                struct list_head *tmp, *pos;
                spin_lock(&handle_hash[i].lock);
                list_for_each_safe_rcu(tmp, pos, &(handle_hash[i].head)) {
                        struct portals_handle *h;
                        h = list_entry(tmp, struct portals_handle, h_link);

                        CERROR("force clean handle "LPX64" addr %p addref %p\n",
                               h->h_cookie, h, h->h_addref);

                        class_handle_unhash_nolock(h);
                }
                spin_unlock(&handle_hash[i].lock);
        }
}

void class_handle_cleanup(void)
{
        int count;
        LASSERT(handle_hash != NULL);

        count = atomic_read(&handle_count);
        if (count != 0) {
                CERROR("handle_count at cleanup: %d\n", count);
                cleanup_all_handles();
        }

        OBD_VFREE(handle_hash, sizeof(*handle_hash) * HANDLE_HASH_SIZE);
        handle_hash = NULL;

        if (atomic_read(&handle_count))
                CERROR("leaked %d handles\n", atomic_read(&handle_count));
}
