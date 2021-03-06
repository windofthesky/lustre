/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2015, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/user_namespace.h>
#ifdef HAVE_UIDGID_HEADER
# include <linux/uidgid.h>
#endif
#include <linux/atomic.h>
#include <linux/list.h>

#include <obd_support.h>
#include <obd_class.h>
#include <lnet/lnetctl.h>
#include <lustre_debug.h>
#include <lustre_kernelcomm.h>
#include <lprocfs_status.h>
#include <lustre_ver.h>
#include <cl_object.h>
#ifdef HAVE_SERVER_SUPPORT
# include <dt_object.h>
# include <md_object.h>
#endif /* HAVE_SERVER_SUPPORT */
#include <uapi/linux/lustre_ioctl.h>
#include "llog_internal.h"

#ifdef CONFIG_PROC_FS
static __u64 obd_max_alloc;
#else
__u64 obd_max_alloc;
#endif

static DEFINE_SPINLOCK(obd_updatemax_lock);

/* The following are visible and mutable through /proc/sys/lustre/. */
unsigned int obd_debug_peer_on_timeout;
EXPORT_SYMBOL(obd_debug_peer_on_timeout);
unsigned int obd_dump_on_timeout;
EXPORT_SYMBOL(obd_dump_on_timeout);
unsigned int obd_dump_on_eviction;
EXPORT_SYMBOL(obd_dump_on_eviction);
unsigned long obd_max_dirty_pages;
EXPORT_SYMBOL(obd_max_dirty_pages);
atomic_long_t obd_dirty_pages;
EXPORT_SYMBOL(obd_dirty_pages);
unsigned int obd_timeout = OBD_TIMEOUT_DEFAULT;   /* seconds */
EXPORT_SYMBOL(obd_timeout);
unsigned int ldlm_timeout = LDLM_TIMEOUT_DEFAULT; /* seconds */
EXPORT_SYMBOL(ldlm_timeout);
unsigned int obd_timeout_set;
EXPORT_SYMBOL(obd_timeout_set);
unsigned int ldlm_timeout_set;
EXPORT_SYMBOL(ldlm_timeout_set);
/* bulk transfer timeout, give up after 100s by default */
unsigned int bulk_timeout = 100; /* seconds */
EXPORT_SYMBOL(bulk_timeout);
/* Adaptive timeout defs here instead of ptlrpc module for /proc/sys/ access */
unsigned int at_min = 0;
EXPORT_SYMBOL(at_min);
unsigned int at_max = 600;
EXPORT_SYMBOL(at_max);
unsigned int at_history = 600;
EXPORT_SYMBOL(at_history);
int at_early_margin = 5;
EXPORT_SYMBOL(at_early_margin);
int at_extra = 30;
EXPORT_SYMBOL(at_extra);

atomic_long_t obd_dirty_transit_pages;
EXPORT_SYMBOL(obd_dirty_transit_pages);

char obd_jobid_var[JOBSTATS_JOBID_VAR_MAX_LEN + 1] = JOBSTATS_DISABLE;

#ifdef CONFIG_PROC_FS
struct lprocfs_stats *obd_memory = NULL;
EXPORT_SYMBOL(obd_memory);
#endif

char obd_jobid_node[LUSTRE_JOBID_SIZE + 1];

/* Get jobid of current process by reading the environment variable
 * stored in between the "env_start" & "env_end" of task struct.
 *
 * TODO:
 * It's better to cache the jobid for later use if there is any
 * efficient way, the cl_env code probably could be reused for this
 * purpose.
 *
 * If some job scheduler doesn't store jobid in the "env_start/end",
 * then an upcall could be issued here to get the jobid by utilizing
 * the userspace tools/api. Then, the jobid must be cached.
 */
int lustre_get_jobid(char *jobid)
{
	int jobid_len = LUSTRE_JOBID_SIZE;
	char tmp_jobid[LUSTRE_JOBID_SIZE] = { 0 };
	int rc = 0;
	ENTRY;

	/* Jobstats isn't enabled */
	if (strcmp(obd_jobid_var, JOBSTATS_DISABLE) == 0)
		GOTO(out, rc = 0);

	/* Whole node dedicated to single job */
	if (strcmp(obd_jobid_var, JOBSTATS_NODELOCAL) == 0) {
		memcpy(tmp_jobid, obd_jobid_node, LUSTRE_JOBID_SIZE);
		GOTO(out, rc = 0);
	}

	/* Use process name + fsuid as jobid */
	if (strcmp(obd_jobid_var, JOBSTATS_PROCNAME_UID) == 0) {
		snprintf(tmp_jobid, LUSTRE_JOBID_SIZE, "%s.%u",
			 current_comm(),
			 from_kuid(&init_user_ns, current_fsuid()));
		GOTO(out, rc = 0);
	}

	rc = cfs_get_environ(obd_jobid_var, tmp_jobid, &jobid_len);
	if (rc) {
		if (rc == -EOVERFLOW) {
			/* For the PBS_JOBID and LOADL_STEP_ID keys (which are
			 * variable length strings instead of just numbers), it
			 * might make sense to keep the unique parts for JobID,
			 * instead of just returning an error.  That means a
			 * larger temp buffer for cfs_get_environ(), then
			 * truncating the string at some separator to fit into
			 * the specified jobid_len.  Fix later if needed. */
			static bool printed;
			if (unlikely(!printed)) {
				LCONSOLE_ERROR_MSG(0x16b, "%s value too large "
						   "for JobID buffer (%d)\n",
						   obd_jobid_var, jobid_len);
				printed = true;
			}
		} else {
			CDEBUG((rc == -ENOENT || rc == -EINVAL ||
				rc == -EDEADLK) ? D_INFO : D_ERROR,
			       "Get jobid for (%s) failed: rc = %d\n",
			       obd_jobid_var, rc);
		}
	}

out:
	if (rc != 0)
		RETURN(rc);

	/* Only replace the job ID if it changed. */
	if (strcmp(jobid, tmp_jobid) != 0)
		memcpy(jobid, tmp_jobid, jobid_len);

	RETURN(0);
}
EXPORT_SYMBOL(lustre_get_jobid);

static int class_resolve_dev_name(__u32 len, const char *name)
{
        int rc;
        int dev;

        ENTRY;
        if (!len || !name) {
                CERROR("No name passed,!\n");
                GOTO(out, rc = -EINVAL);
        }
        if (name[len - 1] != 0) {
                CERROR("Name not nul terminated!\n");
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s\n", name);
        dev = class_name2dev(name);
        if (dev == -1) {
                CDEBUG(D_IOCTL, "No device for name %s!\n", name);
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s, dev %d\n", name, dev);
        rc = dev;

out:
        RETURN(rc);
}

int class_handle_ioctl(unsigned int cmd, unsigned long arg)
{
        char *buf = NULL;
        struct obd_ioctl_data *data;
        struct libcfs_debug_ioctl_data *debug_data;
        struct obd_device *obd = NULL;
        int err = 0, len = 0;
        ENTRY;

        /* only for debugging */
        if (cmd == LIBCFS_IOC_DEBUG_MASK) {
                debug_data = (struct libcfs_debug_ioctl_data*)arg;
                libcfs_subsystem_debug = debug_data->subs;
                libcfs_debug = debug_data->debug;
                return 0;
        }

        CDEBUG(D_IOCTL, "cmd = %x\n", cmd);
	if (obd_ioctl_getdata(&buf, &len, (void __user *)arg)) {
                CERROR("OBD ioctl: data error\n");
                RETURN(-EINVAL);
        }
        data = (struct obd_ioctl_data *)buf;

        switch (cmd) {
        case OBD_IOC_PROCESS_CFG: {
                struct lustre_cfg *lcfg;

                if (!data->ioc_plen1 || !data->ioc_pbuf1) {
                        CERROR("No config buffer passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                OBD_ALLOC(lcfg, data->ioc_plen1);
                if (lcfg == NULL)
                        GOTO(out, err = -ENOMEM);
		err = copy_from_user(lcfg, data->ioc_pbuf1,
                                         data->ioc_plen1);
                if (!err)
                        err = lustre_cfg_sanity_check(lcfg, data->ioc_plen1);
                if (!err)
                        err = class_process_config(lcfg);

                OBD_FREE(lcfg, data->ioc_plen1);
                GOTO(out, err);
        }

	case OBD_GET_VERSION:
		if (!data->ioc_inlbuf1) {
			CERROR("No buffer passed in ioctl\n");
			GOTO(out, err = -EINVAL);
		}

		if (strlen(LUSTRE_VERSION_STRING) + 1 > data->ioc_inllen1) {
			CERROR("ioctl buffer too small to hold version\n");
			GOTO(out, err = -EINVAL);
		}

		memcpy(data->ioc_bulk, LUSTRE_VERSION_STRING,
		       strlen(LUSTRE_VERSION_STRING) + 1);

		if (copy_to_user((void __user *)arg, data, len))
			err = -EFAULT;
		GOTO(out, err);

        case OBD_IOC_NAME2DEV: {
                /* Resolve a device name.  This does not change the
                 * currently selected device.
                 */
                int dev;

                dev = class_resolve_dev_name(data->ioc_inllen1,
                                             data->ioc_inlbuf1);
                data->ioc_dev = dev;
                if (dev < 0)
                        GOTO(out, err = -EINVAL);

		if (copy_to_user((void __user *)arg, data, sizeof(*data)))
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_UUID2DEV: {
                /* Resolve a device uuid.  This does not change the
                 * currently selected device.
                 */
                int dev;
                struct obd_uuid uuid;

                if (!data->ioc_inllen1 || !data->ioc_inlbuf1) {
                        CERROR("No UUID passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inlbuf1[data->ioc_inllen1 - 1] != 0) {
                        CERROR("UUID not NUL terminated!\n");
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s\n", data->ioc_inlbuf1);
                obd_str2uuid(&uuid, data->ioc_inlbuf1);
                dev = class_uuid2dev(&uuid);
                data->ioc_dev = dev;
                if (dev == -1) {
                        CDEBUG(D_IOCTL, "No device for UUID %s!\n",
                               data->ioc_inlbuf1);
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s, dev %d\n", data->ioc_inlbuf1,
                       dev);
		if (copy_to_user((void __user *)arg, data, sizeof(*data)))
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_GETDEVICE: {
                int     index = data->ioc_count;
                char    *status, *str;

                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inllen1 < 128) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }

                obd = class_num2obd(index);
                if (!obd)
                        GOTO(out, err = -ENOENT);

                if (obd->obd_stopping)
                        status = "ST";
                else if (obd->obd_set_up)
                        status = "UP";
                else if (obd->obd_attached)
                        status = "AT";
                else
                        status = "--";
                str = (char *)data->ioc_bulk;
                snprintf(str, len - sizeof(*data), "%3d %s %s %s %s %d",
                         (int)index, status, obd->obd_type->typ_name,
                         obd->obd_name, obd->obd_uuid.uuid,
			 atomic_read(&obd->obd_refcount));

		if (copy_to_user((void __user *)arg, data, len))
			err = -EFAULT;

		GOTO(out, err);
        }

        }

        if (data->ioc_dev == OBD_DEV_BY_DEVNAME) {
                if (data->ioc_inllen4 <= 0 || data->ioc_inlbuf4 == NULL)
                        GOTO(out, err = -EINVAL);
                if (strnlen(data->ioc_inlbuf4, MAX_OBD_NAME) >= MAX_OBD_NAME)
                        GOTO(out, err = -EINVAL);
                obd = class_name2obd(data->ioc_inlbuf4);
        } else if (data->ioc_dev < class_devno_max()) {
                obd = class_num2obd(data->ioc_dev);
        } else {
                CERROR("OBD ioctl: No device\n");
                GOTO(out, err = -EINVAL);
        }

        if (obd == NULL) {
                CERROR("OBD ioctl : No Device %d\n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);

        if (!obd->obd_set_up || obd->obd_stopping) {
                CERROR("OBD ioctl: device not setup %d \n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }

        switch(cmd) {
        case OBD_IOC_NO_TRANSNO: {
                if (!obd->obd_attached) {
                        CERROR("Device %d not attached\n", obd->obd_minor);
                        GOTO(out, err = -ENODEV);
                }
                CDEBUG(D_HA, "%s: disabling committed-transno notification\n",
                       obd->obd_name);
                obd->obd_no_transno = 1;
                GOTO(out, err = 0);
        }

        default: {
                err = obd_iocontrol(cmd, obd->obd_self_export, len, data, NULL);
                if (err)
                        GOTO(out, err);

		if (copy_to_user((void __user *)arg, data, len))
                        err = -EFAULT;
                GOTO(out, err);
        }
        }

out:
	OBD_FREE_LARGE(buf, len);
	RETURN(err);
} /* class_handle_ioctl */

#define OBD_INIT_CHECK
#ifdef OBD_INIT_CHECK
static int obd_init_checks(void)
{
        __u64 u64val, div64val;
        char buf[64];
        int len, ret = 0;

	CDEBUG(D_INFO, "OBD_OBJECT_EOF = %#llx\n", (__u64)OBD_OBJECT_EOF);

        u64val = OBD_OBJECT_EOF;
	CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = %#llx\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EINVAL;
        }
	len = snprintf(buf, sizeof(buf), "%#llx", u64val);
        if (len != 18) {
		CWARN("u64 hex wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }

        div64val = OBD_OBJECT_EOF;
	CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = %#llx\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EOVERFLOW;
        }
        if (u64val >> 8 != OBD_OBJECT_EOF >> 8) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                return -EOVERFLOW;
        }
        if (do_div(div64val, 256) != (u64val & 255)) {
		CERROR("do_div(%#llx,256) != %llu\n", u64val, u64val & 255);
                return -EOVERFLOW;
        }
        if (u64val >> 8 != div64val) {
		CERROR("do_div(%#llx,256) %llu != %llu\n",
                       u64val, div64val, u64val >> 8);
                return -EOVERFLOW;
        }
	len = snprintf(buf, sizeof(buf), "%#llx", u64val);
        if (len != 18) {
		CWARN("u64 hex wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }
	len = snprintf(buf, sizeof(buf), "%llu", u64val);
        if (len != 20) {
		CWARN("u64 wrong length! strlen(%s)=%d != 20\n", buf, len);
                ret = -EINVAL;
        }
	len = snprintf(buf, sizeof(buf), "%lld", u64val);
        if (len != 2) {
		CWARN("s64 wrong length! strlen(%s)=%d != 2\n", buf, len);
                ret = -EINVAL;
        }
	if ((u64val & ~PAGE_MASK) >= PAGE_SIZE) {
		CWARN("mask failed: u64val %llu >= %llu\n", u64val,
		      (__u64)PAGE_SIZE);
                ret = -EINVAL;
        }

        return ret;
}
#else
#define obd_init_checks() do {} while(0)
#endif

static int __init obdclass_init(void)
{
	int err;

	LCONSOLE_INFO("Lustre: Build Version: "LUSTRE_VERSION_STRING"\n");

	libcfs_kkuc_init();

	err = obd_init_checks();
	if (err == -EOVERFLOW)
		return err;

#ifdef CONFIG_PROC_FS
	obd_memory = lprocfs_alloc_stats(OBD_STATS_NUM,
					 LPROCFS_STATS_FLAG_NONE |
					 LPROCFS_STATS_FLAG_IRQ_SAFE);
	if (obd_memory == NULL) {
		CERROR("kmalloc of 'obd_memory' failed\n");
		return -ENOMEM;
	}

	lprocfs_counter_init(obd_memory, OBD_MEMORY_STAT,
			     LPROCFS_CNTR_AVGMINMAX,
			     "memused", "bytes");
#endif
	err = obd_zombie_impexp_init();
	if (err)
		goto cleanup_obd_memory;

	err = class_handle_init();
	if (err)
		goto cleanup_zombie_impexp;

	err = misc_register(&obd_psdev);
	if (err) {
		CERROR("cannot register %d err %d\n", OBD_DEV_MINOR, err);
		goto cleanup_class_handle;
	}

	/* Default the dirty page cache cap to 1/2 of system memory.
	 * For clients with less memory, a larger fraction is needed
	 * for other purposes (mostly for BGL). */
	if (totalram_pages <= 512 << (20 - PAGE_SHIFT))
		obd_max_dirty_pages = totalram_pages / 4;
	else
		obd_max_dirty_pages = totalram_pages / 2;

	err = obd_init_caches();
	if (err)
		goto cleanup_deregister;

	err = class_procfs_init();
	if (err)
		goto cleanup_caches;

	err = lu_global_init();
	if (err)
		goto cleanup_class_procfs;

	err = cl_global_init();
	if (err != 0)
		goto cleanup_lu_global;

#ifdef HAVE_SERVER_SUPPORT
	err = dt_global_init();
	if (err != 0)
		goto cleanup_cl_global;

	err = lu_ucred_global_init();
	if (err != 0)
		goto cleanup_dt_global;
#endif /* HAVE_SERVER_SUPPORT */

	err = llog_info_init();
	if (err)
#ifdef HAVE_SERVER_SUPPORT
		goto cleanup_lu_ucred_global;
#else /* !HAVE_SERVER_SUPPORT */
		goto cleanup_cl_global;
#endif /* HAVE_SERVER_SUPPORT */

	err = lustre_register_fs();

	/* simulate a late OOM situation now to require all
	 * alloc'ed/initialized resources to be freed */
	if (OBD_FAIL_CHECK(OBD_FAIL_OBDCLASS_MODULE_LOAD)) {
		/* fake error but filesystem has been registered */
		lustre_unregister_fs();
		/* force error to ensure module will be unloaded/cleaned */
		err = -ENOMEM;
	}

	if (err)
		goto cleanup_llog_info;

	return 0;

cleanup_llog_info:
	llog_info_fini();

#ifdef HAVE_SERVER_SUPPORT
cleanup_lu_ucred_global:
	lu_ucred_global_fini();

cleanup_dt_global:
	dt_global_fini();
#endif /* HAVE_SERVER_SUPPORT */

cleanup_cl_global:
	cl_global_fini();

cleanup_lu_global:
	lu_global_fini();

cleanup_class_procfs:
	obd_sysctl_clean();
	class_procfs_clean();

cleanup_caches:
	obd_cleanup_caches();

cleanup_deregister:
	misc_deregister(&obd_psdev);

cleanup_class_handle:
	class_handle_cleanup();

cleanup_zombie_impexp:
	obd_zombie_impexp_stop();

cleanup_obd_memory:
#ifdef CONFIG_PROC_FS
	lprocfs_free_stats(&obd_memory);
#endif

	return err;
}

void obd_update_maxusage(void)
{
	__u64 max;

	max = obd_memory_sum();

	spin_lock(&obd_updatemax_lock);
	if (max > obd_max_alloc)
		obd_max_alloc = max;
	spin_unlock(&obd_updatemax_lock);
}
EXPORT_SYMBOL(obd_update_maxusage);

#ifdef CONFIG_PROC_FS
__u64 obd_memory_max(void)
{
	__u64 ret;

	obd_update_maxusage();
	spin_lock(&obd_updatemax_lock);
	ret = obd_max_alloc;
	spin_unlock(&obd_updatemax_lock);

	return ret;
}
#endif /* CONFIG_PROC_FS */

static void __exit obdclass_exit(void)
{
#ifdef CONFIG_PROC_FS
	__u64 memory_leaked;
	__u64 memory_max;
#endif /* CONFIG_PROC_FS */
	ENTRY;

	lustre_unregister_fs();

	misc_deregister(&obd_psdev);
	llog_info_fini();
#ifdef HAVE_SERVER_SUPPORT
	lu_ucred_global_fini();
	dt_global_fini();
#endif /* HAVE_SERVER_SUPPORT */
	cl_global_fini();
	lu_global_fini();

        obd_cleanup_caches();
        obd_sysctl_clean();

        class_procfs_clean();

        class_handle_cleanup();
	class_del_uuid(NULL); /* Delete all UUIDs. */
        obd_zombie_impexp_stop();

#ifdef CONFIG_PROC_FS
	memory_leaked = obd_memory_sum();
	memory_max = obd_memory_max();

	lprocfs_free_stats(&obd_memory);
	CDEBUG((memory_leaked) ? D_ERROR : D_INFO,
	       "obd_memory max: %llu, leaked: %llu\n",
	       memory_max, memory_leaked);
#endif /* CONFIG_PROC_FS */

	EXIT;
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Class Driver");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

module_init(obdclass_init);
module_exit(obdclass_exit);
