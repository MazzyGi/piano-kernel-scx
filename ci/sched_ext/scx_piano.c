/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_piano.bpf.skel.h"

#define DEFAULT_MODE_PATH "/run/piano-sched.mode"
#define CFG_MODE 0
#define CFG_BOOST 1
#define BOOST_NS (5ULL * 1000 * 1000)

const char help_fmt[] =
"Piano power-aware sched_ext scheduler.\n"
"\n"
"Usage: %s [-m MODE] [-p PATH] [-q] [-v]\n"
"\n"
"  -m MODE   0=power 1=balanced 2=performance (default: file or 1)\n"
"  -p PATH   mode file (default " DEFAULT_MODE_PATH ")\n"
"  -q        quiet\n"
"  -v        libbpf debug\n";

static bool verbose, quiet;
static volatile int exit_req;
static const char *mode_path = DEFAULT_MODE_PATH;

static int libbpf_print_fn(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, fmt, args);
}

static void sig_handler(int sig)
{
	exit_req = 1;
}

static struct bpf_map *cfg_map(struct scx_piano *skel)
{
	return bpf_object__find_map_by_name(skel->obj, "cfg");
}

static int set_cfg(struct scx_piano *skel, __u32 idx, __u64 val)
{
	struct bpf_map *m = cfg_map(skel);
	int ret;

	if (!m) {
		errno = ENOENT;
		perror("cfg map");
		return -1;
	}
	ret = bpf_map__update_elem(m, &idx, sizeof(idx), &val, sizeof(val), 0);
	if (ret)
		perror("cfg update");
	return ret;
}

static int read_mode_file(void)
{
	FILE *f = fopen(mode_path, "r");
	int mode = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &mode) != 1)
		mode = -1;
	fclose(f);
	if (mode < 0 || mode > 2)
		return -1;
	return mode;
}

static void read_stats(struct scx_piano *skel, __u64 *out)
{
	int nr = libbpf_num_possible_cpus(), cpu, idx;
	__u64 cnts[3][nr];

	if (nr <= 0)
		return;
	memset(out, 0, sizeof(__u64) * 3);
	for (idx = 0; idx < 3; idx++) {
		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &idx,
					cnts[idx]) < 0)
			continue;
		for (cpu = 0; cpu < nr; cpu++)
			out[idx] += cnts[idx][cpu];
	}
}

int main(int argc, char **argv)
{
	struct scx_piano *skel;
	struct bpf_link *link;
	__u64 ecode, stats[3];
	int opt, mode = -1, cur = -1, nr = 0;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	while ((opt = getopt(argc, argv, "m:p:qvh")) != -1) {
		switch (opt) {
		case 'm':
			mode = atoi(optarg);
			break;
		case 'p':
			mode_path = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	if (mode < 0)
		mode = read_mode_file();
	if (mode < 0)
		mode = 1;
	if (mode > 2)
		mode = 1;

restart:
	skel = SCX_OPS_OPEN(piano_ops, scx_piano);
	if (!skel)
		return 1;

	if (set_cfg(skel, CFG_MODE, mode) || set_cfg(skel, CFG_BOOST, BOOST_NS)) {
		fprintf(stderr, "failed to initialize cfg map\n");
		return 1;
	}
	SCX_OPS_LOAD(skel, piano_ops, scx_piano, uei);
	link = SCX_OPS_ATTACH(skel, piano_ops, scx_piano);
	cur = mode;

	if (!quiet)
		fprintf(stderr, "piano: mode=%d\n", mode);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		int m = read_mode_file();

		if (m >= 0 && m != cur) {
			__u64 v = m;

			__u32 key = CFG_MODE;
			struct bpf_map *m = cfg_map(skel);

			if (m && bpf_map__update_elem(m, &key, sizeof(key),
						      &v, sizeof(v), 0) == 0) {
				cur = m;
				if (!quiet)
					fprintf(stderr, "piano: mode=%d\n", m);
			}
		}
		if (!quiet && ++nr % 5 == 0) {
			read_stats(skel, stats);
			fprintf(stderr, "piano: local=%llu global=%llu boost=%llu\n",
				(unsigned long long)stats[0],
				(unsigned long long)stats[1],
				(unsigned long long)stats[2]);
		}
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_piano__destroy(skel);
	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
