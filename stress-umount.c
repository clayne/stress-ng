/*
 * Copyright (C) 2023      Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-capabilities.h"

#if defined(HAVE_SYS_MOUNT_H)
#include <sys/mount.h>
#endif

static const stress_help_t help[] = {
	{ NULL,	"umount N",	 "start N workers exercising umount races" },
	{ NULL,	"umount-ops N",	 "stop after N bogo umount operations" },
	{ NULL,	NULL,		 NULL }
};

/*
 *  stress_umount_supported()
 *      check if we can run this with SHIM_CAP_SYS_ADMIN capability
 */
static int stress_umount_supported(const char *name)
{
	if (!stress_check_capability(SHIM_CAP_SYS_ADMIN)) {
		pr_inf_skip("%s stressor will be skipped, "
			"need to be running with CAP_SYS_ADMIN "
			"rights for this stressor\n", name);
		return -1;
	}
	return 0;
}

#if defined(__linux__)
/*
 *  stress_umount_umount()
 *	umount a path with retries.
 */
static void stress_umount_umount(const stress_args_t *args, const char *path, const uint64_t ns_delay)
{
	int i;
	int ret;

	/*
	 *  umount is attempted at least twice, the first successful mount
	 *  and then a retry. In theory the EINVAL should be returned
	 *  on a umount of a path that has already been umounted, so we
	 *  know that umount been successful and can then return.
	 */
	for (i = 0; i < 100; i++) {
#if defined(HAVE_UMOUNT2) &&	\
    defined(MNT_FORCE)
		if (stress_mwc1()) {
			ret = umount2(path, MNT_FORCE);
		} else {
			ret = umount(path);
		}
#else
		ret = umount(path);
#endif
		if (ret == 0) {
			if (i > 1) {
				shim_nanosleep_uint64(ns_delay);
			}
			continue;
		}
		switch (errno) {
		case EAGAIN:
		case EBUSY:
		case ENOMEM:
			/* Wait and then re-try */
			shim_nanosleep_uint64(ns_delay);
			break;
		case EINVAL:
		case ENOENT:
			/*
			 *  EINVAL if it's either invalid path or
			 *  it can't be umounted.  We now assume it
			 *  has been successfully umounted
			 */
			return;
		default:
			/* Unexpected, so report it */
			pr_inf("%s: umount failed %s: %d %s\n", args->name,
				path, errno, strerror(errno));
			return;
		}
	}
}

/*
 *  stress_umount_read_proc_mounts()
 *	exercise reading of proc mounts
 */
static void stress_umount_read_proc_mounts(const stress_args_t *args, const pid_t parent, const char *path)
{
	(void)path;

	do {
		int fd;
		char buffer[4096];
		ssize_t ret;

		fd = open("/proc/mounts", O_RDONLY);
		if (fd < 0)
			break;
		do {
			ret = read(fd, buffer, sizeof(buffer));
		} while (ret > 0);
		(void)close(fd);

		shim_nanosleep_uint64(stress_mwc64modn(1000000));
	} while (keep_stressing(args));

	(void)kill(parent, SIGALRM);
	_exit(0);
}

/*
 *  stress_umount_umounter()
 *	racy unmount, hammer time!
 */
static void stress_umount_umounter(const stress_args_t *args, const pid_t parent, const char *path)
{
	stress_parent_died_alarm();
	(void)sched_settings_apply(true);

	do {
		stress_umount_umount(args, path, 10000);
		shim_nanosleep_uint64(stress_mwc64modn(10000));
	} while (keep_stressing(args));

	(void)kill(parent, SIGALRM);
	_exit(0);
}


/*
 *  stress_umount_mounter()
 *	aggressively perform ramfs mounts, this can force out of memory
 *	conditions
 */
static void stress_umount_mounter(const stress_args_t *args, const pid_t parent, const char *path)
{
	const uint64_t ramfs_size = 64 * KB;
	int i = 0;

	stress_parent_died_alarm();
	(void)sched_settings_apply(true);

	do {
		int ret;
		char opt[32];
		const char *fs = (i++ & 1) ? "ramfs" : "tmpfs";

		(void)snprintf(opt, sizeof(opt), "size=%" PRIu64, ramfs_size);
		ret = mount("", path, fs, 0, opt);
		if (ret < 0) {
			if ((errno != ENOSPC) &&
			    (errno != ENOMEM) &&
			    (errno != ENODEV))
				pr_fail("%s: mount failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
			/* Just in case, force umount */
			goto cleanup;
		} else {
			inc_counter(args);
		}
		stress_umount_umount(args, path, 1000000);
	} while (keep_stressing(args));

	(void)kill(parent, SIGALRM);
cleanup:
	stress_umount_umount(args, path, 100000000);
	_exit(0);
}

/*
 *  stress_umount_spawn()
 *	spawn off child processes
 */
static pid_t stress_umount_spawn(
	const stress_args_t *args,
	const pid_t parent,
	const char *path,
	void (*func)(const stress_args_t *args, const pid_t parent, const char *path))
{
	pid_t pid;

again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(errno))
			goto again;
		if (!keep_stressing(args))
			return 0;
		pr_inf("%s: fork failed: %d (%s), skipping stressor\n",
			args->name, errno, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		stress_parent_died_alarm();
		(void)sched_settings_apply(true);

		stress_set_proc_state(args->name, STRESS_STATE_RUN);
		func(args, parent, path);
		stress_set_proc_state(args->name, STRESS_STATE_WAIT);

		_exit(EXIT_SUCCESS);
	}
	return pid;
}

/*
 *  stress_umount()
 *      stress unmounting
 */
static int stress_umount(const stress_args_t *args)
{
	pid_t pids[3] = { -1, -1, -1 };
	const pid_t mypid = getpid();
	size_t i;
	int ret = EXIT_NO_RESOURCE;
	char pathname[PATH_MAX], realpathname[PATH_MAX];

	stress_temp_dir(pathname, sizeof(pathname), args->name, args->pid, args->instance);
	if (mkdir(pathname, S_IRGRP | S_IWGRP) < 0) {
		pr_fail("%s: cannot mkdir %s, errno=%d (%s)\n",
			args->name, pathname, errno, strerror(errno));
		return EXIT_FAILURE;
	}
	if (!realpath(pathname, realpathname)) {
		pr_fail("%s: cannot realpath %s, errno=%d (%s)\n",
			args->name, pathname, errno, strerror(errno));
		(void)stress_temp_dir_rm_args(args);
		return EXIT_FAILURE;
	}

	pids[0] = stress_umount_spawn(args, mypid, realpathname, stress_umount_mounter);
	if (pids[0] < 1)
		goto reap;
	pids[1] = stress_umount_spawn(args, mypid, realpathname, stress_umount_umounter);
	if (pids[1] < 1)
		goto reap;
	pids[2] = stress_umount_spawn(args, mypid, realpathname, stress_umount_read_proc_mounts);
	if (pids[2] < 1)
		goto reap;

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	/* Wait for SIGALARMs */
	do {
		pause();
	} while (keep_stressing(args));

	ret = EXIT_SUCCESS;
reap:
	for (i = 0; i < SIZEOF_ARRAY(pids); i++) {
		if (pids[i] > 1) {
			int status;

			(void)kill(pids[i], SIGALRM);
			(void)waitpid(pids[i], &status, 0);
		}
	}

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	(void)stress_temp_dir_rm_args(args);

	return ret;
}

stressor_info_t stress_umount_info = {
	.stressor = stress_umount,
	.class = CLASS_OS,
	.supported = stress_umount_supported,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_umount_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_OS,
	.supported = stress_umount_supported,
	.help = help,
	.unimplemented_reason = "only supported on Linux"
};
#endif