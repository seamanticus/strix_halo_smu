// SPDX-License-Identifier: GPL-2.0
/* shsctl - command-line client for strix_halo_smu, built on libshs. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shs.h"

static const char usage[] =
	"usage: shsctl info\n"
	"       shsctl table [--raw]\n"
	"       shsctl msg mp1|rsmu ID [ARG...]\n"
	"       shsctl co-all STEPS\n"
	"       shsctl stats\n";

static int fail(const char *what, int err)
{
	fprintf(stderr, "shsctl: %s: %s\n", what, strerror(-err));
	return 1;
}

static int parse_u32(const char *s, uint32_t *v)
{
	char *end;
	unsigned long x;

	errno = 0;
	x = strtoul(s, &end, 0);
	if (errno || *end || !*s || x > UINT32_MAX)
		return -EINVAL;
	*v = x;
	return 0;
}

static int cmd_info(void)
{
	struct shs_snapshot s;
	uint32_t pmfw, ifv;
	int fd, err;

	fd = shs_open(0);
	if (fd < 0)
		return fail(SHS_DEV_PATH, fd);

	err = shs_pmfw_version(fd, &pmfw);
	if (!err)
		err = shs_interface_version(fd, &ifv);
	if (!err)
		err = shs_read_table(fd, &s);
	shs_close(fd);
	if (err)
		return fail("query", err);

	printf("pmfw       %u.%u.%u.%u\n", pmfw >> 24, (pmfw >> 16) & 0xff,
	       (pmfw >> 8) & 0xff, pmfw & 0xff);
	printf("interface  %u\n", ifv);
	printf("table      0x%08x, 0x%x bytes\n", s.version, s.size);
	return 0;
}

static int cmd_table(int raw)
{
	struct shs_snapshot s;
	unsigned int i;
	int fd, err;

	fd = shs_open(0);
	if (fd < 0)
		return fail(SHS_DEV_PATH, fd);
	err = shs_read_table(fd, &s);
	shs_close(fd);
	if (err)
		return fail("table", err);

	if (raw)
		return fwrite(s.data, 1, s.size, stdout) == s.size ? 0 : 1;

	printf("time_ns      %llu\n", (unsigned long long)s.time_ns);
	printf("stapm_limit  %.2f W\n", shs_stapm_limit_w(&s));
	printf("slow_limit   %.2f W\n", shs_slow_limit_w(&s));
	printf("skin_limit   %.2f C\n", shs_skin_limit_c(&s));
	printf("tctl         %.2f C\n", shs_tctl_c(&s));
	printf("core  clk_MHz  volt_mV  power_W  temp_C\n");
	for (i = 0; i < SHS_TABLE_CORES; i++)
		printf("%4u  %7.0f  %7.1f  %7.2f  %6.1f\n", i, shs_core_clk_mhz(&s, i),
		       shs_core_volt_mv(&s, i), shs_core_power_w(&s, i), shs_core_temp_c(&s, i));
	return 0;
}

static int cmd_msg(int argc, char **argv)
{
	uint32_t args[SHS_NUM_ARGS] = { 0 }, id, rsp;
	unsigned int mbox;
	int i, fd, err;

	if (argc < 2 || argc > 2 + SHS_NUM_ARGS) {
		fputs(usage, stderr);
		return 2;
	}
	if (!strcmp(argv[0], "mp1"))
		mbox = SHS_MBOX_MP1;
	else if (!strcmp(argv[0], "rsmu"))
		mbox = SHS_MBOX_RSMU;
	else
		return fail(argv[0], -EINVAL);
	if (parse_u32(argv[1], &id))
		return fail(argv[1], -EINVAL);
	for (i = 2; i < argc; i++)
		if (parse_u32(argv[i], &args[i - 2]))
			return fail(argv[i], -EINVAL);

	/* The module decides what each message may do; a writable fd lets it allow set class. */
	fd = shs_open(1);
	if (fd < 0)
		return fail(SHS_DEV_PATH, fd);
	err = shs_msg(fd, mbox, id, args, &rsp);
	shs_close(fd);
	if (err)
		return fail("msg", err);

	printf("rsp 0x%02x args", rsp);
	for (i = 0; i < SHS_NUM_ARGS; i++)
		printf(" 0x%08x", args[i]);
	putchar('\n');
	return rsp == SHS_RSP_OK ? 0 : 3;
}

static int cmd_co_all(const char *arg)
{
	char *end;
	uint32_t rsp;
	long steps;
	int fd, err;

	errno = 0;
	steps = strtol(arg, &end, 10);
	if (errno || *end || !*arg)
		return fail(arg, -EINVAL);

	fd = shs_open(1);
	if (fd < 0)
		return fail(SHS_DEV_PATH, fd);
	err = shs_co_all(fd, (int)steps, &rsp);
	shs_close(fd);
	if (err)
		return fail("co-all", err);

	printf("co-all %+ld (0x%05x): rsp 0x%02x\n", steps, shs_co_encode((int)steps), rsp);
	return rsp == SHS_RSP_OK ? 0 : 3;
}

static int cmd_stats(void)
{
	struct shs_send_stat st;
	unsigned int i;
	int fd, n = 0;

	fd = open(SHS_SEND_STATS_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(SHS_SEND_STATS_PATH, -errno);

	printf("mbox  msg   admitted  refused  last_pid  last_arg0   last_ns\n");
	for (i = 0; read(fd, &st, sizeof(st)) == sizeof(st); i++) {
		if (!st.admitted && !st.refused)
			continue;
		printf("%-4s  0x%02x  %8u  %7u  %8d  0x%08x  %llu\n", i < 256 ? "mp1" : "rsmu",
		       i % 256, st.admitted, st.refused, st.pid, st.args[0],
		       (unsigned long long)st.time_ns);
		n++;
	}
	close(fd);
	if (!n)
		puts("(no sends)");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "info"))
		return cmd_info();
	if (argc == 2 && !strcmp(argv[1], "table"))
		return cmd_table(0);
	if (argc == 3 && !strcmp(argv[1], "table") && !strcmp(argv[2], "--raw"))
		return cmd_table(1);
	if (argc >= 2 && !strcmp(argv[1], "msg"))
		return cmd_msg(argc - 2, argv + 2);
	if (argc == 3 && !strcmp(argv[1], "co-all"))
		return cmd_co_all(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "stats"))
		return cmd_stats();

	fputs(usage, stderr);
	return 2;
}
