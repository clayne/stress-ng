/*
 * Copyright (C) 2025      Colin Ian King.
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
#include "core-builtin.h"
#include "core-cpu-cache.h"
#include "core-killpid.h"
#include "core-numa.h"
#include "core-out-of-memory.h"

static const stress_help_t help[] = {
	{ NULL,	"mmaptorture N",	"start N workers torturing page mappings" },
	{ NULL, "mmaptorture-bytes N",	"size of file backed region to be memory mapped" },
	{ NULL, "mmaptorture-msync N",	"percentage of pages to be msync'd (default 10%)" },
	{ NULL,	"mmaptorture-ops N",	"stop after N mmaptorture bogo operations" },
	{ NULL,	NULL,			NULL }
};

typedef struct {
	uint8_t *addr;
	size_t	size;
	off_t	offset;
} mmap_info_t;

#define MMAP_MAPPINGS_MAX	(128)
#define MMAP_SIZE_MAP		(512)	/* in pages */

#define PAGE_WR_FLAG		(0x01)
#define PAGE_RD_FLAG		(0x02)

#define MIN_MMAPTORTURE_BYTES		(16 * MB)
#define MAX_MMAPTORTURE_BYTES   	(MAX_MEM_LIMIT)
#define DEFAULT_MMAPTORTURE_BYTES	(256 * MB)

typedef struct {
	uint64_t	mmap_pages;
	uint64_t	sync_pages;
	uint64_t	lock_pages;
	uint64_t	mprotect_pages;
	uint64_t	madvise_pages;
	uint64_t	remapped_pages;
	uint64_t	sigbus_traps;
	uint64_t	sigsegv_traps;
	uint64_t	mmap_retries;
} mmap_stats_t;

static sigjmp_buf jmp_env;
static int mmap_fd;
static const char *name = "mmaptorture";
static uint8_t *mmap_data;
static size_t mmap_bytes = DEFAULT_MMAPTORTURE_BYTES;
static bool mmap_bytes_adjusted = false;
static mmap_stats_t *mmap_stats;

#if defined(HAVE_MADVISE)
static const int madvise_options[] = {
#if defined(MADV_NORMAL)
	MADV_NORMAL,
#endif
#if defined(MADV_RANDOM)
	MADV_RANDOM,
#endif
#if defined(MADV_SEQUENTIAL)
	MADV_SEQUENTIAL,
#endif
#if defined(MADV_WILLNEED)
	MADV_WILLNEED,
#endif
#if defined(MADV_DONTNEED)
	MADV_DONTNEED,
#endif
#if defined(MADV_REMOVE)
	MADV_REMOVE,
#endif
#if defined(MADV_DONTFORK)
	MADV_DONTFORK,
#endif
#if defined(MADV_DOFORK)
	MADV_DOFORK,
#endif
#if defined(MADV_MERGEABLE)
	MADV_MERGEABLE,
#endif
#if defined(MADV_UNMERGEABLE)
	MADV_UNMERGEABLE,
#endif
#if defined(MADV_SOFT_OFFLINE)
	MADV_SOFT_OFFLINE,
#endif
#if defined(MADV_HUGEPAGE)
	MADV_HUGEPAGE,
#endif
#if defined(MADV_NOHUGEPAGE)
	MADV_NOHUGEPAGE,
#endif
#if defined(MADV_DONTDUMP)
	MADV_DONTDUMP,
#endif
#if defined(MADV_DODUMP)
	MADV_DODUMP,
#endif
#if defined(MADV_FREE)
	MADV_FREE,
#endif
#if defined(MADV_WIPEONFORK)
	MADV_WIPEONFORK,
#endif
#if defined(MADV_KEEPONFORK)
	MADV_KEEPONFORK,
#endif
#if defined(MADV_INHERIT_ZERO)
	MADV_INHERIT_ZERO,
#endif
#if defined(MADV_COLD)
	MADV_COLD,
#endif
#if defined(MADV_PAGEOUT)
	MADV_PAGEOUT,
#endif
#if defined(MADV_POPULATE_READ)
	MADV_POPULATE_READ,
#endif
#if defined(MADV_POPULATE_WRITE)
	MADV_POPULATE_WRITE,
#endif
#if defined(MADV_DONTNEED_LOCKED)
	MADV_DONTNEED_LOCKED,
#endif
/* Linux 6.0 */
#if defined(MADV_COLLAPSE)
	MADV_COLLAPSE,
#endif
/* FreeBSD */
#if defined(MADV_AUTOSYNC)
	MADV_AUTOSYNC,
#endif
/* FreeBSD and DragonFlyBSD */
#if defined(MADV_CORE)
	MADV_CORE,
#endif
/* FreeBSD */
#if defined(MADV_PROTECT)
	MADV_PROTECT,
#endif
/* Linux 5.14 */
#if defined(MADV_POPULATE_READ)
	MADV_POPULATE_READ,
#endif
/* Linux 5.14 */
#if defined(MADV_POPULATE_WRITE)
	MADV_POPULATE_WRITE,
#endif
/* OpenBSD */
#if defined(MADV_SPACEAVAIL)
	MADV_SPACEAVAIL,
#endif
/* OS X */
#if defined(MADV_ZERO_WIRED_PAGES)
	MADV_ZERO_WIRED_PAGES,
#endif
/* Solaris */
#if defined(MADV_ACCESS_DEFAULT)
	MADV_ACCESS_DEFAULT,
#endif
/* Solaris */
#if defined(MADV_ACCESS_LWP)
	MADV_ACCESS_LWP,
#endif
/* Solaris */
#if defined(MADV_ACCESS_MANY)
	MADV_ACCESS_MANY,
#endif
};
#endif

#if defined(HAVE_MPROTECT)
static const int mprotect_flags[] = {
#if defined(PROT_READ)
	PROT_READ,
#endif
#if defined(PROT_WRITE)
	PROT_WRITE,
#endif
#if defined(PROT_READ) &&	\
    defined(PROT_WRITE)
	PROT_READ | PROT_WRITE,
#endif
#if defined(PROT_NONE)
	PROT_NONE,
#endif
};
#endif

static const int mmap_flags[] = {
#if defined(MAP_32BIT)
	MAP_32BIT,
#endif
#if defined(MAP_LOCKED)
	MAP_LOCKED,
#endif
#if defined(MAP_STACK)
	MAP_STACK,
#endif
#if defined(MAP_SHARED_VALIDATE)
	MAP_SHARED_VALIDATE,
#endif
#if defined(MAP_POPULATE)
	MAP_POPULATE,
#endif
#if defined(MAP_NORESERVE)
	MAP_NORESERVE,
#endif
#if defined(MAP_NONBLOCK) &&	\
    defined(MAP_POPULATE)
	MAP_NONBLOCK | MAP_POPULATE,
#endif
#if defined(MAP_SYNC)
	MAP_SYNC,
#endif
#if defined(MAP_UNINITIALIZED)
	MAP_UNINITIALIZED,
#endif
#if defined(MAP_HUGETLB)
	/* MAP_HUGETLB | (21 << MAP_HUGE_SHIFT), */
#endif
	0,
};

#if defined(HAVE_MLOCKALL) &&	\
    defined(MCL_CURRENT) && 	\
    defined(MCL_FUTURE)
static const int mlockall_flags[] = {
#if defined(MCL_CURRENT)
	MCL_CURRENT,
#endif
#if defined(MCL_FUTURE)
	MCL_FUTURE,
#endif
#if defined(MCL_CURRENT) && 	\
    defined(MCL_FUTURE)
	MCL_CURRENT | MCL_FUTURE,
#endif
};
#endif

static void stress_mmaptorture_init(const uint32_t num_instances)
{
	char path[PATH_MAX];
	const pid_t pid = getpid();
	const size_t page_size = stress_get_page_size();

	(void)num_instances;

	mmap_bytes = DEFAULT_MMAPTORTURE_BYTES;
	stress_get_setting("mmaptorture-bytes", &mmap_bytes);

	mmap_bytes = (num_instances < 1) ? mmap_bytes : mmap_bytes / num_instances;
	mmap_bytes &= ~(page_size - 1);
	if (mmap_bytes < page_size * MMAP_SIZE_MAP * 2) {
		mmap_bytes = (page_size * MMAP_SIZE_MAP * 2);
		mmap_bytes_adjusted = true;
	}

	if (stress_temp_dir_mk(name, pid, 0) < 0) {
		mmap_fd = -1;
		return;
	}
	stress_temp_filename(path, sizeof(path), name, pid, 0, stress_mwc32());
	mmap_fd = open(path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IXUSR);
	if (mmap_fd < 0) {
		mmap_fd = -1;
		(void)stress_temp_dir_rm(path, pid, 0);
		return;
	}
	(void)unlink(path);

	VOID_RET(int, ftruncate(mmap_fd, (off_t)mmap_bytes));
	mmap_data = mmap(NULL, mmap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mmap_fd, 0);
}

static void stress_mmaptorture_deinit(void)
{
	if (mmap_fd == -1)
		return;
	if (mmap_data != MAP_FAILED)
		(void)munmap((void *)mmap_data, mmap_bytes);
	(void)stress_temp_dir_rm(name, getpid(), 0);
}

static void NORETURN MLOCKED_TEXT stress_mmaptorture_sighandler(int signum)
{
	(void)signum;

	switch (signum) {
	case SIGBUS:
		if (mmap_stats)
			mmap_stats->sigbus_traps++;
		break;
	case SIGSEGV:
		if (mmap_stats)
			mmap_stats->sigbus_traps++;
		break;
	default:
		break;
	}
	siglongjmp(jmp_env, 1);	/* Ugly, bounce back */
}

static void stress_mmaptorture_msync(
	uint8_t *addr,
	const size_t length,
	const size_t page_size,
	const uint32_t mmaptorture_msync)
{
#if defined(HAVE_MSYNC)
	size_t i;
	uint32_t percent;

	if (mmaptorture_msync > 100)
		percent = 10000000 * 100;
	else
		percent = 10000000 * mmaptorture_msync;


	for (i = 0; i < length; i += page_size) {
		if (stress_mwc32modn(1000000000) < percent) {
			const int flag = (stress_mwc1() ? MS_SYNC : MS_ASYNC) |
					 (stress_mwc1() ? 0 : MS_INVALIDATE);

			(void)msync((void *)(addr + i), page_size, flag);
			mmap_stats->sync_pages++;
		}
	}
#else
	(void)addr;
	(void)length;
	(void)page_size;
#endif
}

static void stress_mmaptorture_vm_name(
	uint8_t *ptr,
	const size_t size,
	const size_t page_size)
{
	char name[32];
	size_t i, j;
	char hex[] = "0123456789ABCDEF";

	for (i = 0, j = 0; i < size; i += page_size, j++) {
		(void)stress_rndstr(name, sizeof(name));
		name[0] = hex[(j >> 4) & 0xf];
		name[1] = hex[j & 0xf];

		stress_set_vma_anon_name(ptr + i, page_size, name);
	}
}

static int stress_mmaptorture_child(stress_args_t *args, void *context)
{
	const size_t page_size = args->page_size;
	const size_t page_mask = ~(page_size - 1);
	const pid_t mypid = getpid();
	NOCLOBBER uint32_t mmaptorture_msync = 10;
	NOCLOBBER mmap_info_t *mappings;
#if defined(HAVE_LINUX_MEMPOLICY_H)
	NOCLOBBER stress_numa_mask_t *numa_mask = NULL;
#endif
	size_t i;
	(void)context;

	stress_get_setting("mmaptorture-msync", &mmaptorture_msync);

	if (sigsetjmp(jmp_env, 1)) {
		pr_inf_skip("%s: premature SIGSEGV caught, skipping stressor\n",
			args->name);
		return EXIT_NO_RESOURCE;
        }
	if (stress_sighandler(args->name, SIGBUS, stress_mmaptorture_sighandler, NULL) < 0)
		return EXIT_NO_RESOURCE;
	if (stress_sighandler(args->name, SIGSEGV, stress_mmaptorture_sighandler, NULL) < 0)
		return EXIT_NO_RESOURCE;

	mappings = (mmap_info_t *)calloc((size_t)MMAP_MAPPINGS_MAX, sizeof(*mappings));
	if (UNLIKELY(!mappings)) {
		pr_fail("%s: calloc failed, out of memory\n", args->name);
		return EXIT_NO_RESOURCE;
	}

#if defined(HAVE_LINUX_MEMPOLICY_H)
	if (stress_numa_nodes() > 0)
		numa_mask = stress_numa_mask_alloc();
#endif
	for (i = 0; i < MMAP_MAPPINGS_MAX; i++) {
		mappings[i].addr = MAP_FAILED;
		mappings[i].size = 0;
		mappings[i].offset = 0;
	}

	do {
		unsigned char vec[MMAP_SIZE_MAP];
		NOCLOBBER uint8_t *ptr;
		NOCLOBBER size_t n, mmap_size;
		NOCLOBBER pid_t pid = -1;
		NOCLOBBER uint64_t total_bytes = 0;
		off_t offset;

		if (sigsetjmp(jmp_env, 1))
			goto mappings_unmap;

		VOID_RET(int, ftruncate(mmap_fd, (off_t)stress_mwc64modn((uint64_t)mmap_bytes)));
		VOID_RET(int, ftruncate(mmap_fd, (off_t)mmap_bytes));

		offset = stress_mwc64modn((uint64_t)mmap_bytes) & page_mask;
		if (lseek(mmap_fd, offset, SEEK_SET) == offset) {
			char data[page_size];

			shim_memset(data, stress_mwc8(), sizeof(data));

			if (write(mmap_fd, data, sizeof(data)) == (ssize_t)sizeof(data)) {
				volatile uint8_t *vptr = (volatile uint8_t *)(mmap_data + offset);

				(*vptr)++;
				stress_mmaptorture_msync(mmap_data, mmap_bytes, page_size, mmaptorture_msync);
			}
		}
#if defined(HAVE_REMAP_FILE_PAGES) &&   \
    defined(MAP_NONBLOCK) &&		\
    !defined(STRESS_ARCH_SPARC)
		if (remap_file_pages(mmap_data, mmap_bytes, PROT_NONE, 0, MAP_SHARED | MAP_NONBLOCK) == 0)
			mmap_stats->remapped_pages += mmap_bytes / page_size;
		if (mprotect(mmap_data, mmap_bytes, PROT_READ | PROT_WRITE) == 0)
			mmap_stats->mprotect_pages += mmap_bytes / page_size;
#endif
		for (n = 0; n < MMAP_MAPPINGS_MAX; n++) {
			mappings[n].addr = MAP_FAILED;
			mappings[n].size = 0;
			mappings[n].offset = 0;
		}

		for (n = 0; n < MMAP_MAPPINGS_MAX; n++) {
			int flag = 0, mmap_flag;

#if defined(HAVE_MADVISE)
			const int madvise_option = madvise_options[stress_mwc8modn(SIZEOF_ARRAY(madvise_options))];
#endif
#if defined(HAVE_MPROTECT)
			const int mprotect_flag = mprotect_flags[stress_mwc8modn(SIZEOF_ARRAY(mprotect_flags))];
#else
			const int mprotect_flag = ~0;
#endif

retry:
			if (UNLIKELY(!stress_continue(args)))
				break;

			/* Don't exceed mmap limit */
			if (total_bytes >= mmap_bytes)
				break;

			mmap_flag = mmap_flags[stress_mwc8modn(SIZEOF_ARRAY(mmap_flags))] |
				    mmap_flags[stress_mwc8modn(SIZEOF_ARRAY(mmap_flags))];

			mmap_size = page_size * (1 + stress_mwc16modn(MMAP_SIZE_MAP));
			offset = stress_mwc64modn((uint64_t)mmap_bytes) & page_mask;
#if defined(HAVE_FALLOCATE)
#if defined(FALLOC_FL_PUNCH_HOLE) &&	\
    defined(FALLOC_FL_KEEP_SIZE)
			if (stress_mwc1()) {
				(void)shim_fallocate(mmap_fd, 0, offset, mmap_size);
				flag = PAGE_WR_FLAG | PAGE_RD_FLAG;
			} else {
				(void)shim_fallocate(mmap_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, mmap_size);
			}
#else
			(void)shim_fallocate(mmap_fd, 0, offset, mmap_size);
			flag = PAGE_WR_FLAG | PAGE_RD_FLAG;
#endif
#endif
			if (stress_mwc1()) {
				/* file based mmap */
				ptr = (uint8_t *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
							MAP_SHARED | mmap_flag, mmap_fd, offset);
				if (ptr != MAP_FAILED)
					goto mapped_ok;
				ptr = (uint8_t *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
							MAP_SHARED, mmap_fd, offset);
				if (ptr != MAP_FAILED)
					goto mapped_ok;
				mmap_stats->mmap_retries++;
				goto retry;
			}
#if defined(HAVE_LIB_RT) &&	\
    defined(HAVE_SHM_OPEN) &&	\
    defined(HAVE_SHM_UNLINK)
			if (stress_mwc1()) {
				/* anonymous shm mapping */
				int shm_fd;
				char name[128];

				(void)snprintf(name, sizeof(name), "%s-%" PRIdMAX "-%zd",
						args->name, (intmax_t)mypid, n);
				shm_fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
				if (shm_fd < 0)
					goto retry;
				ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
						MAP_SHARED | mmap_flag, shm_fd, offset);
				if (ptr != MAP_FAILED) {
					(void)shm_unlink(name);
					(void)close(shm_fd);
					goto mapped_ok;
				}
				ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
						MAP_SHARED, shm_fd, offset);
				if (ptr != MAP_FAILED) {
					(void)shm_unlink(name);
					(void)close(shm_fd);
					goto mapped_ok;
				}
				mmap_stats->mmap_retries++;
				goto retry;
			}
#endif
			/* anonymous mmap mapping */
			ptr = (uint8_t *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
						MAP_SHARED | MAP_ANONYMOUS | mmap_flag, -1, 0);
			if (LIKELY(ptr != MAP_FAILED))
				goto mapped_ok;
			ptr = (uint8_t *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
						MAP_SHARED | MAP_ANONYMOUS, -1, 0);
			if (LIKELY(ptr != MAP_FAILED))
				goto mapped_ok;
			mmap_stats->mmap_retries++;
			goto retry;

mapped_ok:
			total_bytes += mmap_size;
			mmap_stats->mmap_pages += mmap_size / page_size;
			mappings[n].addr = ptr;
			mappings[n].size = mmap_size;
			mappings[n].offset = offset;
			stress_mmaptorture_vm_name((void *)ptr, mmap_size, page_size);

			if (stress_mwc1()) {
				for (i = 0; i < mmap_size; i += 64)
					shim_builtin_prefetch((void *)(ptr + i));
			}
#if defined(HAVE_LINUX_MEMPOLICY_H)
			if (numa_mask && stress_mwc1())
				stress_numa_randomize_pages(numa_mask, (void *)ptr, page_size, mmap_size);

#if defined(HAVE_MSYNC) &&	\
    defined(MS_SYNC) &&		\
    defined(MS_ASYNC)
			stress_mmaptorture_msync(ptr, mmap_size, page_size, mmaptorture_msync);
#endif
#endif
#if defined(HAVE_MADVISE)
			if (madvise((void *)ptr, mmap_size, madvise_option) == 0)
				mmap_stats->madvise_pages += mmap_size / page_size;
#endif
			(void)shim_mincore((void *)ptr, mmap_size, vec);
			for (i = 0; i < mmap_size; i += page_size) {
				if (stress_mwc1()) {
					if (shim_mlock((void *)(ptr + i), page_size) == 0)
						mmap_stats->lock_pages++;
				}
				if ((flag & PAGE_WR_FLAG) && (mprotect_flag & PROT_WRITE))
					*(volatile uint8_t *)(ptr + i) = stress_mwc64();
				if ((flag & PAGE_RD_FLAG) && (mprotect_flag & PROT_READ))
					*(volatile uint8_t *)(ptr + i);
			}
#if defined(MAP_FIXED_NOREPLACE)
			{
				void *tmp;

				/* mmap onto an existing virt addr, should fail */
				tmp = mmap((void *)ptr, mmap_size, PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_FIXED_NOREPLACE, mmap_fd, offset);
				if (tmp != MAP_FAILED) {
					mmap_stats->mmap_pages += mmap_size / page_size;
					(void)munmap(tmp, mmap_size);
				}
			}
#endif

#if defined(HAVE_MPROTECT)
			if (stress_mwc1())
				if (mprotect((void *)ptr, mmap_size, mprotect_flag) == 0)
					mmap_stats->mprotect_pages += mmap_size / page_size;
#endif
#if defined(HAVE_LINUX_MEMPOLICY_H)
			if (stress_mwc1() && (numa_mask))
				stress_numa_randomize_pages(numa_mask, (void *)ptr, page_size, mmap_size);
#endif
			for (i = 0; i < mmap_size; i += page_size) {
				if (stress_mwc1())
					(void)shim_munlock((void *)(ptr + i), page_size);
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_PAGEOUT)
				if (stress_mwc1()) {
					if (madvise((void *)(ptr + i), page_size, MADV_PAGEOUT) == 0)
						mmap_stats->madvise_pages++;
				}
#endif
				stress_mmaptorture_msync(ptr + i, page_size, page_size, mmaptorture_msync);
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_FREE)
				if (stress_mwc1()) {
					if (madvise((void *)(ptr + i), page_size, MADV_FREE) == 0)
						mmap_stats->madvise_pages++;
				}
#endif
			}

			if (stress_mwc1())
				(void)shim_mincore((void *)ptr, mmap_size, vec);

			if (stress_mwc1()) {
				int ret;

				ret = stress_munmap_retry_enomem((void *)ptr, mmap_size);
				if (ret == 0) {
#if defined(MAP_FIXED)
					if (stress_mwc1()) {
						mappings[n].addr = mmap((void *)mappings[n].addr, page_size,
								PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED,
								mmap_fd, mappings[n].offset);
						if (UNLIKELY(mappings[n].addr == MAP_FAILED)) {
							mappings[n].size = 0;
						} else {
							stress_mmaptorture_vm_name((void *)mappings[n].addr, page_size, page_size);
							mmap_stats->mmap_pages++;
						}
					} else {
						mappings[n].addr = MAP_FAILED;
						mappings[n].size = 0;
					}
#endif
				} else {
					mappings[n].addr = MAP_FAILED;
					mappings[n].size = 0;
				}
			}
			stress_bogo_inc(args);
		}

		if (stress_mwc1()) {
			pid = fork();
			if (pid == 0) {
#if defined(HAVE_MLOCKALL) &&	\
    defined(MCL_CURRENT) && 	\
    defined(MCL_FUTURE)
				{
					const size_t idx = stress_mwc8modn(SIZEOF_ARRAY(mlockall_flags));

					(void)shim_mlockall(mlockall_flags[idx]);
				}
#endif
				/* Pass 1, free random pages */
				for (i = 0; i < n; i++) {
					ptr = mappings[i].addr;
					mmap_size = mappings[i].size;

					if (madvise((void *)ptr, mmap_size, MADV_DONTNEED) == 0)
						mmap_stats->madvise_pages += mmap_size / page_size;

					if (stress_mwc1()) {
						if ((ptr != MAP_FAILED) && (mmap_size > 0)) {
							(void)stress_munmap_retry_enomem((void *)(ptr + i), page_size);
							mappings[i].addr = MAP_FAILED;
							mappings[i].size = 0;
						}
					}
				}

				for (i = 0; i < n; i++) {
					ptr = mappings[i].addr;
					mmap_size = mappings[i].size;

					if ((ptr != MAP_FAILED) && (mmap_size > 0)) {
						(void)shim_mseal(ptr, mmap_size, 0);
						break;
					}
				}

				/* Pass 2, free unfreed pages */
				for (i = 0; i < n; i++) {
					ptr = mappings[i].addr;
					mmap_size = mappings[i].size;

					if ((ptr != MAP_FAILED) && (mmap_size > 0)) {
						(void)stress_munmap_retry_enomem((void *)ptr, mmap_size);
						mappings[i].addr = MAP_FAILED;
						mappings[i].size = 0;
					}
				}
#if defined(HAVE_MUNLOCKALL)
				shim_munlockall();
#endif
				_exit(0);
			}
		}

mappings_unmap:
		for (i = 0; i < n; i++) {
			ptr = mappings[i].addr;
			mmap_size = mappings[i].size;

			if ((ptr != MAP_FAILED) && (mmap_size > 0)) {
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_REMOVE)
				size_t j;
#endif

#if defined(HAVE_MREMAP)
				if (mmap_size > page_size) {
					uint8_t *newptr;
					size_t new_size = mmap_size - page_size;

					newptr = (uint8_t *)mremap(ptr, mmap_size, new_size, MREMAP_MAYMOVE);
					if (newptr != MAP_FAILED) {
						ptr = newptr;
						mmap_size -= page_size;
						mmap_stats->remapped_pages += mmap_size / new_size;
					}
				}
#endif

#if defined(HAVE_MADVISE) &&	\
    defined(MADV_NORMAL)
				if (madvise((void *)ptr, mmap_size, MADV_NORMAL) == 0)
					mmap_stats->madvise_pages += mmap_size / page_size;
#endif
#if defined(HAVE_MPROTECT)
				if (mprotect((void *)ptr, mmap_size, PROT_READ | PROT_WRITE) == 0)
					mmap_stats->mprotect_pages += mmap_size / page_size;
#endif
				(void)shim_munlock((void *)ptr, mmap_size);
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_DONTNEED)
				if (stress_mwc1()) {
					if (madvise((void *)ptr, mmap_size, MADV_DONTNEED) == 0)
						mmap_stats->madvise_pages += mmap_size / page_size;
				}
#endif
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_REMOVE)
				for (j = 0; j < mmap_size; j += page_size) {
					if (stress_mwc1()) {
						if (madvise((void *)(ptr + j), page_size, MADV_REMOVE) == 0)
							mmap_stats->madvise_pages += 1;
					}
					(void)stress_munmap_retry_enomem((void *)(ptr + j), page_size);
				}
#endif
				(void)shim_mincore((void *)ptr, mmap_size, vec);
			}
			mappings[i].addr = MAP_FAILED;
			mappings[i].size = 0;
		}
		if (pid > 0)
			(void)stress_kill_and_wait(args, pid, SIGKILL, false);
	} while (stress_continue(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

#if defined(HAVE_LINUX_MEMPOLICY_H)
	if (numa_mask)
		stress_numa_mask_free(numa_mask);
#endif
	free(mappings);
	return EXIT_SUCCESS;
}

/*
 *  stress_mmaptorture()
 *	stress mmap with many pages being mapped
 */
static int stress_mmaptorture(stress_args_t *args)
{
	int ret;
	double t_start, duration, rate;

	mmap_stats = (mmap_stats_t *)stress_mmap_populate(NULL, sizeof(*mmap_stats),
					PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mmap_stats == MAP_FAILED) {
		pr_inf_skip("%s: cannot mmap stats shared page, skipping stressor\n", args->name);
		return EXIT_NO_RESOURCE;
	}

	if (args->instance == 0) {
		char str1[64], str2[64];

		stress_uint64_to_str(str1, sizeof(str1), (uint64_t)mmap_bytes);
		stress_uint64_to_str(str2, sizeof(str2), (uint64_t)mmap_bytes * args->num_instances);

		pr_inf("%s: using %smmap'd size %s per stressor (total %s)\n", args->name,
			mmap_bytes_adjusted ? "adjusted " : "", str1, str2);
	}
	stress_set_proc_state(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	t_start = stress_time_now();
	ret = stress_oomable_child(args, NULL, stress_mmaptorture_child, STRESS_OOMABLE_NORMAL);
	duration = stress_time_now() - t_start;

	rate = (duration > 0.0) ? (double)mmap_stats->mmap_pages / duration : 0.0;
	stress_metrics_set(args, 0, "pages mapped pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->sync_pages / duration : 0.0;
	stress_metrics_set(args, 1, "pages synced pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->lock_pages / duration : 0.0;
	stress_metrics_set(args, 2, "pages locked pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->mprotect_pages / duration : 0.0;
	stress_metrics_set(args, 3, "pages mprotected pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->madvise_pages / duration : 0.0;
	stress_metrics_set(args, 4, "pages madvised pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->remapped_pages / duration : 0.0;
	stress_metrics_set(args, 5, "pages remapped pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->mmap_retries / duration : 0.0;
	stress_metrics_set(args, 6, "mmap retries pec sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->sigbus_traps / duration : 0.0;
	stress_metrics_set(args, 7, "intentional SIGBUS signals sec", rate, STRESS_METRIC_HARMONIC_MEAN);
	rate = (duration > 0.0) ? (double)mmap_stats->sigsegv_traps / duration : 0.0;
	stress_metrics_set(args, 8, "intentional SIGSEGV signals sec", rate, STRESS_METRIC_HARMONIC_MEAN);

	(void)munmap((void *)mmap_stats, sizeof(*mmap_stats));

	return ret;
}

static const stress_opt_t opts[] = {
        { OPT_mmaptorture_bytes, "mmaptorture-bytes", TYPE_ID_SIZE_T_BYTES_VM, MIN_MMAPTORTURE_BYTES, MAX_MMAPTORTURE_BYTES, NULL },
	{ OPT_mmaptorture_msync, "mmaptorture-msync", TYPE_ID_UINT32, 0, 100, NULL },
};

const stressor_info_t stress_mmaptorture_info = {
	.stressor = stress_mmaptorture,
	.class = CLASS_VM | CLASS_OS,
	.verify = VERIFY_NONE,
	.init = stress_mmaptorture_init,
	.deinit = stress_mmaptorture_deinit,
	.opts = opts,
	.help = help
};
