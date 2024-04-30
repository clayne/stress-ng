/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2024 Colin Ian King.
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
#include "core-sort.h"

#if defined(HAVE_SEARCH_H)
#include <search.h>
#endif

#define TSEARCH_SIZE_SHIFT	(22)
#define MIN_TSEARCH_SIZE	(1 * KB)
#define MAX_TSEARCH_SIZE	(1U << TSEARCH_SIZE_SHIFT)	/* 4 MB */
#define DEFAULT_TSEARCH_SIZE	(64 * KB)

static const stress_help_t help[] = {
	{ NULL,	"tsearch N",		"start N workers that exercise a tree search" },
	{ NULL,	"tsearch-ops N",	"stop after N tree search bogo operations" },
	{ NULL,	"tsearch-size N",	"number of 32 bit integers to tsearch" },
	{ NULL,	NULL,			NULL }
};

/*
 *  stress_set_tsearch_size()
 *      set tsearch size from given option string
 */
static int stress_set_tsearch_size(const char *opt)
{
	uint64_t tsearch_size;

	tsearch_size = stress_get_uint64(opt);
	stress_check_range("tsearch-size", tsearch_size,
		MIN_TSEARCH_SIZE, MAX_TSEARCH_SIZE);
	return stress_set_setting("tsearch-size", TYPE_ID_UINT64, &tsearch_size);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_tsearch_size,	stress_set_tsearch_size },
	{ 0,			NULL }
};

#if defined(HAVE_TSEARCH)

/*
 *  stress_tsearch()
 *	stress tsearch
 */
static int stress_tsearch(stress_args_t *args)
{
	uint64_t tsearch_size = DEFAULT_TSEARCH_SIZE;
	int32_t *data;
	size_t i, n;
	double rate, duration = 0.0, count = 0.0, sorted = 0.0;
	int rc = EXIT_SUCCESS;

	if (!stress_get_setting("tsearch-size", &tsearch_size)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			tsearch_size = MAX_TSEARCH_SIZE;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			tsearch_size = MIN_TSEARCH_SIZE;
	}
	n = (size_t)tsearch_size;

	if ((data = calloc(n, sizeof(*data))) == NULL) {
		pr_fail("%s: calloc failed allocating %zd integers, skipping stressor\n",
			args->name, n);
		return EXIT_NO_RESOURCE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	stress_sort_data_int32_init(data, n);

	do {
		double t;
		void *root = NULL;

		stress_sort_data_int32_shuffle(data, n);

		/* Step #1, populate tree */
		for (i = 0; i < n; i++) {
			if (tsearch(&data[i], &root, stress_sort_cmp_fwd_int32) == NULL) {
				size_t j;

				pr_err("%s: cannot allocate new "
					"tree node\n", args->name);
				for (j = 0; j < i; j++)
					tdelete(&data[j], &root, stress_sort_cmp_fwd_int32);
				goto abort;
			}
		}
		/* Step #2, find */
		stress_sort_compare_reset();
		t = stress_time_now();
		for (i = 0; stress_continue_flag() && (i < n); i++) {
			const void **result = tfind(&data[i], &root, stress_sort_cmp_fwd_int32);

			if (g_opt_flags & OPT_FLAGS_VERIFY) {
				if (!result) {
					pr_fail("%s: element %zu could not be found\n",
						args->name, i);
					rc = EXIT_FAILURE;
					break;
				} else {
					const int32_t *val = *result;

					if (*val != data[i]) {
						pr_fail("%s: element "
							"%zu found %" PRIu32
							", expecting %" PRIu32 "\n",
							args->name, i, *val, data[i]);
						rc = EXIT_FAILURE;
						break;
					}
				}
			}
		}
		duration += stress_time_now() - t;
		count += (double)stress_sort_compare_get();
		sorted += (double)i;

		/* Step #3, delete */
		for (i = 0; i < n; i++) {
			const void **result = tdelete(&data[i], &root, stress_sort_cmp_fwd_int32);

			if ((g_opt_flags & OPT_FLAGS_VERIFY) && (result == NULL)) {
				pr_fail("%s: element %zu could not be found\n",
					args->name, i);
				rc = EXIT_FAILURE;
				break;
			}
		}
		stress_bogo_inc(args);
	} while ((rc == EXIT_SUCCESS) && stress_continue(args));
abort:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	rate = (duration > 0.0) ? count / duration : 0.0;
	stress_metrics_set(args, 0, "tsearch comparisons per sec",
		rate, STRESS_HARMONIC_MEAN);
	stress_metrics_set(args, 1, "tsearch comparisons per item",
		count / sorted, STRESS_HARMONIC_MEAN);

	free(data);
	return rc;
}

stressor_info_t stress_tsearch_info = {
	.stressor = stress_tsearch,
	.class = CLASS_CPU_CACHE | CLASS_CPU | CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};

#else

stressor_info_t stress_tsearch_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_CPU_CACHE | CLASS_CPU | CLASS_MEMORY | CLASS_SEARCH,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help,
	.unimplemented_reason = "built without libc tsearch() support"
};

#endif
