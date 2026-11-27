// SPDX-License-Identifier: MIT
/* shsctl - command-line client for strix_halo_smu, built on libshs. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shs.h"

#define PARAMS_PATH	"/sys/module/strix_halo_smu/parameters/"
#define TAINT_PATH	"/proc/sys/kernel/tainted"
#define TAINT_OOS_BIT	2	/* TAINT_CPU_OUT_OF_SPEC, shown as 'S' */
#define WATCH_POLL_MS	100

/* Exit codes: 0 done, 1 error, 2 usage, 3 the SMU answered but not OK. */
enum { EXIT_ERR = 1, EXIT_USAGE = 2, EXIT_SMU = 3 };

struct cmd {
	const char *name;
	const char *synopsis;
	const char *summary;
	const char *details;
};

static const struct cmd cmds[] = {
	{ "info", "info",
	  "firmware versions, table, module gates, taint",
	  "Read-only. Safe at any time.\n" },
	{ "table", "table [--raw]",
	  "one labelled snapshot of the PM table",
	  "Read-only. Values are averages since the previous read, so a single\n"
	  "read after a long pause is smoothed; use 'watch' to follow a load.\n"
	  "--raw writes the 0x1028 table bytes to stdout, for tools.\n"
	  "The limit marked '<- limiting' is the one holding power back now.\n" },
	{ "watch", "watch [SECONDS]",
	  "follow power, limits, temperatures and clocks",
	  "Read-only. Polls the table 10 times a second and prints one line per\n"
	  "second. Runs until Ctrl-C, or for SECONDS.\n" },
	{ "co-all", "co-all STEPS",
	  "Curve Optimizer offset for all cores (0 = default)",
	  "Changes state. Negative STEPS lower the voltage at each clock (undervolt).\n"
	  "Too negative crashes the machine; a reboot clears it. Start at -5 and\n"
	  "go down in steps of 5, testing each. Lost on reboot and on suspend.\n"
	  "Example: shsctl co-all -10    undo: shsctl co-all 0\n" },
	{ "co-core", "co-core CORE STEPS",
	  "Curve Optimizer offset for one core, 0-15 (0 = default)",
	  "Changes state. As co-all, for one core. Core N runs CPUs N and N+16.\n"
	  "Cores 0-7 are CCD 0, 8-15 are CCD 1.\n"
	  "Example: shsctl co-core 3 -10    undo: shsctl co-core 3 0\n" },
	{ "fast-limit", "fast-limit WATTS",
	  "short-term package power limit (MP1 0x15)",
	  "Changes state. Caps power within about a second. amd_pmf sends its own\n"
	  "value again on every power-profile change, so the setting lasts until\n"
	  "then, or until reboot. Example: shsctl fast-limit 45\n" },
	{ "slow-limit", "slow-limit WATTS",
	  "sustained package power limit (MP1 0x16)",
	  "Changes state. As fast-limit, for the averaged limit; it takes effect\n"
	  "within seconds. Example: shsctl slow-limit 30\n" },
	{ "msg", "msg mp1|rsmu ID [ARG...]",
	  "raw transaction, for developers",
	  "Sends message ID with up to six 32-bit arguments (0x.. or decimal) and\n"
	  "prints the response code and the six result words. The module decides\n"
	  "what is allowed; see docs/06-glossary.md before sending anything.\n"
	  "Example: shsctl msg mp1 0x02    (PMFW version, read-only)\n" },
	{ "stats", "stats",
	  "messages sent through the module since load",
	  "Read-only. One row per message ID: admitted and refused counts, last\n"
	  "sender pid, last first argument.\n" },
	{ "help", "help [COMMAND]",
	  "this list, or details of one command", "" },
};

static int dry_run;

static void usage(FILE *f)
{
	unsigned int i;

	fputs("usage: shsctl [-n|--dry-run] COMMAND [ARGS]\n\n", f);
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		fprintf(f, "  %-26s %s\n", cmds[i].synopsis, cmds[i].summary);
	fputs("\n-n, --dry-run   print the message a state-changing command would send, send nothing\n"
	      "Most commands need root. Manual: docs/shsctl.md\n", f);
}

static int cmd_help(const char *name)
{
	unsigned int i;

	if (!name) {
		usage(stdout);
		return 0;
	}
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		if (!strcmp(name, cmds[i].name)) {
			printf("shsctl %s\n  %s\n\n%s", cmds[i].synopsis, cmds[i].summary, cmds[i].details);
			return 0;
		}
	}
	fprintf(stderr, "shsctl: no command '%s'\n", name);
	return EXIT_USAGE;
}

/* Explains a module error in terms of what to do next; set is true for state-changing sends. */
static const char *explain(int err, int set)
{
	switch (-err) {
	case ENOENT:
		return "the module is not loaded (sudo modprobe strix_halo_smu)";
	case EACCES:
		return "permission denied: run as root (sudo shsctl ...)";
	case EPERM:
		return set ? "refused: kernel lockdown forbids hardware tuning, or this message is\n"
			     "  safeguard-class and needs allow_safeguard_writes=1 at module load"
			   : "refused: needs root with CAP_SYS_RAWIO, or the message is never forwarded";
	case ENOMSG:
		return "refused: this message ID is not in the module's catalogue";
	case EBADF:
		return "refused: state-changing messages need the device opened for writing";
	case EAGAIN:
		return "send budget used up (16 messages, then 1 per second); wait and retry";
	case EBUSY:
		return "the BIOS is using the SMU right now; retry in a moment";
	case ETIMEDOUT:
		return "the SMU did not answer in time; the module now refuses sends until reloaded";
	case EIO:
		return "the module stopped serving after a fault (SMU timeout or temperature\n"
		       "  check mismatch); see 'sudo dmesg | grep strix_halo_smu', then reload it";
	case ERANGE:
		return "value out of range";
	case EPROTO:
		return "the SMU answered with an error code";
	default:
		return strerror(-err);
	}
}

static int fail(const char *what, int err, int set)
{
	fprintf(stderr, "shsctl: %s: %s\n", what, explain(err, set));
	return EXIT_ERR;
}

static const char *rsp_name(uint32_t rsp)
{
	switch (rsp) {
	case SHS_RSP_OK:	return "OK";
	case SHS_RSP_BUSY:	return "busy";
	case SHS_RSP_PREREQ:	return "prerequisite not met";
	case SHS_RSP_UNKNOWN:	return "unknown message";
	case SHS_RSP_FAILED:	return "rejected";
	default:		return "unexpected code";
	}
}

static int parse_u32(const char *s, uint32_t *v)
{
	char *end;
	unsigned long x;

	errno = 0;
	x = strtoul(s, &end, 0);
	if (errno || *end || !*s || *s == '-' || x > UINT32_MAX)
		return -EINVAL;
	*v = x;
	return 0;
}

static int parse_int(const char *s, long *v)
{
	char *end;

	errno = 0;
	*v = strtol(s, &end, 10);
	return errno || *end || !*s ? -EINVAL : 0;
}

static int open_dev(int writable, int *fd)
{
	*fd = shs_open(writable);
	return *fd < 0 ? fail(SHS_DEV_PATH, *fd, 0) : 0;
}

static int read_param(const char *name)
{
	char path[128], c = '?';
	FILE *f;

	snprintf(path, sizeof(path), PARAMS_PATH "%s", name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fread(&c, 1, 1, f) != 1)
		c = '?';
	fclose(f);
	return c == 'Y';
}

static int tainted_oos(void)
{
	unsigned long t;
	FILE *f = fopen(TAINT_PATH, "r");
	int n;

	if (!f)
		return -1;
	n = fscanf(f, "%lu", &t);
	fclose(f);
	return n == 1 ? !!(t & (1UL << TAINT_OOS_BIT)) : -1;
}

static const char *yes_no(int v)
{
	return v < 0 ? "?" : v ? "yes" : "no";
}

static int cmd_info(void)
{
	struct shs_snapshot s;
	uint32_t pmfw, ifv;
	int fd, err;

	if (open_dev(0, &fd))
		return EXIT_ERR;
	err = shs_pmfw_version(fd, &pmfw);
	if (!err)
		err = shs_interface_version(fd, &ifv);
	if (!err)
		err = shs_read_table(fd, &s);
	shs_close(fd);
	if (err)
		return fail("query", err, 0);

	printf("SMU firmware (PMFW)    %u.%u.%u.%u\n", pmfw >> 24, (pmfw >> 16) & 0xff,
	       (pmfw >> 8) & 0xff, pmfw & 0xff);
	printf("BIOS interface         %u\n", ifv);
	printf("PM table               version 0x%08x, %u bytes\n", s.version, s.size);
	printf("allow_safeguard_writes %s\n", yes_no(read_param("allow_safeguard_writes")));
	printf("allow_unlisted         %s\n", yes_no(read_param("allow_unlisted")));
	printf("kernel tainted (S)     %s\n", yes_no(tainted_oos()));
	return 0;
}

/* The lowest of the three package power caps, and its name if power sits on it. */
static const char *limiting(const struct shs_snapshot *s, float *cap)
{
	const float v[3] = { shs_stt_cap_w(s), shs_fast_limit_w(s), shs_slow_limit_w(s) };
	static const char *const name[3] = { "stt", "fast", "slow" };
	unsigned int i, k = 0;

	for (i = 1; i < 3; i++)
		if (v[i] < v[k])
			k = i;
	*cap = v[k];
	return fabsf(shs_fast_power_w(s) - v[k]) < 0.5f ? name[k] : NULL;
}

static int read_snapshot(struct shs_snapshot *s)
{
	int fd, err;

	if (open_dev(0, &fd))
		return EXIT_ERR;
	err = shs_read_table(fd, s);
	shs_close(fd);
	return err ? fail("table", err, 0) : 0;
}

static int cmd_table(int raw)
{
	struct shs_snapshot s;
	const char *lim;
	unsigned int i;
	float cap;

	if (read_snapshot(&s))
		return EXIT_ERR;
	if (raw)
		return fwrite(s.data, 1, s.size, stdout) == s.size ? 0 : EXIT_ERR;
	if (s.version != SHS_TABLE_0064020C) {
		fprintf(stderr, "shsctl: table version 0x%08x has no known layout; use --raw\n",
			s.version);
		return EXIT_ERR;
	}

	lim = limiting(&s, &cap);
	printf("package power      %6.1f W\n", shs_fast_power_w(&s));
	printf("  STT cap          %6.1f W%s\n", shs_stt_cap_w(&s),
	       lim && !strcmp(lim, "stt") ? "   <- limiting" : "");
	printf("  fast limit       %6.1f W%s\n", shs_fast_limit_w(&s),
	       lim && !strcmp(lim, "fast") ? "   <- limiting" : "");
	printf("  slow limit       %6.1f W%s\n", shs_slow_limit_w(&s),
	       lim && !strcmp(lim, "slow") ? "   <- limiting" : "");
	printf("skin estimate      %6.1f C   (limit %.1f C)\n", shs_skin_temp_c(&s),
	       shs_skin_limit_c(&s));
	printf("Tctl               %6.1f C\n\n", shs_tctl_c(&s));

	printf("core  ccd  cpus    clock MHz  volt mV  power W  temp C\n");
	for (i = 0; i < SHS_TABLE_CORES; i++)
		printf("%4u  %3u  %2u,%-2u  %9.0f  %7.1f  %7.2f  %6.1f\n", i, i / 8, i, i + 16,
		       shs_core_clk_mhz(&s, i), shs_core_volt_mv(&s, i),
		       shs_core_power_w(&s, i), shs_core_temp_c(&s, i));
	return 0;
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void sleep_until(struct timespec *t, long ms)
{
	t->tv_nsec += ms * 1000000L;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec++;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR)
		;
}

static void watch_line(const struct shs_snapshot *s, double t)
{
	float clk = 0, volt = 0, cap;
	const char *lim = limiting(s, &cap);
	unsigned int i;

	for (i = 0; i < SHS_TABLE_CORES; i++) {
		clk = fmaxf(clk, shs_core_clk_mhz(s, i));
		volt += shs_core_volt_mv(s, i) / SHS_TABLE_CORES;
	}
	printf("%7.1f  %6.1f  %6.1f %-4s  %6.1f  %5.1f/%-5.1f  %7.0f  %7.0f\n", t,
	       shs_fast_power_w(s), cap, lim ? lim : "-", shs_tctl_c(s), shs_skin_temp_c(s),
	       shs_skin_limit_c(s), clk, volt);
	fflush(stdout);
}

static int cmd_watch(const char *secs_s)
{
	struct shs_snapshot s;
	struct timespec next;
	double start, end = 0;
	unsigned int n = 0;
	long secs = 0;
	int fd, err;

	if (secs_s && (parse_int(secs_s, &secs) || secs < 0))
		return fail(secs_s, -EINVAL, 0);
	if (open_dev(0, &fd))
		return EXIT_ERR;
	err = shs_read_table(fd, &s);
	if (!err && s.version != SHS_TABLE_0064020C)
		err = -EPROTO;
	if (err) {
		shs_close(fd);
		return fail("table", err, 0);
	}

	start = now_s();
	if (secs)
		end = start + secs;
	clock_gettime(CLOCK_MONOTONIC, &next);
	printf("   time   power   limit       Tctl   skin/limit    max MHz  mean mV\n");
	for (;;) {
		sleep_until(&next, WATCH_POLL_MS);
		err = shs_read_table(fd, &s);
		if (err)
			break;
		if (++n % (1000 / WATCH_POLL_MS) == 0)
			watch_line(&s, now_s() - start);
		if (end && now_s() >= end)
			break;
	}
	shs_close(fd);
	return err ? fail("table", err, 0) : 0;
}

/* Sends one state-changing MP1 message, or prints it under --dry-run. */
static int send_set(const char *what, unsigned int msg,
		    int (*send)(int fd, void *ctx, uint32_t *rsp), void *ctx, uint32_t arg0)
{
	uint32_t rsp;
	int fd, err;

	if (dry_run) {
		printf("%s: would send mp1 0x%02x arg0 0x%08x\n", what, msg, arg0);
		return 0;
	}
	if (open_dev(1, &fd))
		return EXIT_ERR;
	err = send(fd, ctx, &rsp);
	shs_close(fd);
	if (err)
		return fail(what, err, 1);

	printf("%s: sent mp1 0x%02x arg0 0x%08x, SMU answered 0x%02x (%s)\n", what, msg, arg0,
	       rsp, rsp_name(rsp));
	return rsp == SHS_RSP_OK ? 0 : EXIT_SMU;
}

struct co_req {
	unsigned int core;
	int steps;
};

static int send_co_all(int fd, void *ctx, uint32_t *rsp)
{
	return shs_co_all(fd, ((struct co_req *)ctx)->steps, rsp);
}

static int send_co_core(int fd, void *ctx, uint32_t *rsp)
{
	const struct co_req *r = ctx;

	return shs_co_core(fd, r->core, r->steps, rsp);
}

static int cmd_co_all(const char *steps_s)
{
	struct co_req r = { 0 };
	long steps;

	if (parse_int(steps_s, &steps))
		return fail(steps_s, -EINVAL, 0);
	if (steps < SHS_CO_ALL_MIN || steps > SHS_CO_ALL_MAX)
		return fail(steps_s, -ERANGE, 0);
	r.steps = steps;
	return send_set("co-all", SHS_MP1_CO_ALL_CORE, send_co_all, &r, shs_co_encode(r.steps));
}

static int cmd_co_core(const char *core_s, const char *steps_s)
{
	struct co_req r;
	uint32_t core;
	long steps;

	if (parse_u32(core_s, &core) || core >= SHS_TABLE_CORES)
		return fail(core_s, -ERANGE, 0);
	if (parse_int(steps_s, &steps))
		return fail(steps_s, -EINVAL, 0);
	if (steps < SHS_CO_CORE_MIN || steps > SHS_CO_CORE_MAX)
		return fail(steps_s, -ERANGE, 0);
	r.core = core;
	r.steps = steps;
	return send_set("co-core", SHS_MP1_CO_PER_CORE, send_co_core, &r,
			shs_co_core_encode(r.core, r.steps));
}

static int send_fast(int fd, void *ctx, uint32_t *rsp)
{
	return shs_set_fast_limit(fd, *(uint32_t *)ctx, rsp);
}

static int send_slow(int fd, void *ctx, uint32_t *rsp)
{
	return shs_set_slow_limit(fd, *(uint32_t *)ctx, rsp);
}

static int cmd_limit(const char *what, const char *watts_s)
{
	int fast = !strcmp(what, "fast-limit");
	uint32_t mw;
	char *end;
	double w;
	int ret;

	errno = 0;
	w = strtod(watts_s, &end);
	if (errno || *end || !*watts_s || !(w >= 1.0 && w < 1000.0))
		return fail(watts_s, -ERANGE, 0);
	mw = (uint32_t)lround(w * 1000.0);

	ret = send_set(what, fast ? SHS_MP1_FAST_LIMIT : SHS_MP1_SLOW_LIMIT,
		       fast ? send_fast : send_slow, &mw, mw);
	if (!ret && !dry_run)
		fputs("note: amd_pmf restores its own limit on the next power-profile change\n", stderr);
	return ret;
}

static int cmd_msg(int argc, char **argv)
{
	uint32_t args[SHS_NUM_ARGS] = { 0 }, id, rsp;
	unsigned int mbox;
	int i, fd, err;

	if (argc < 2 || argc > 2 + SHS_NUM_ARGS) {
		cmd_help("msg");
		return EXIT_USAGE;
	}
	if (!strcmp(argv[0], "mp1"))
		mbox = SHS_MBOX_MP1;
	else if (!strcmp(argv[0], "rsmu"))
		mbox = SHS_MBOX_RSMU;
	else
		return fail(argv[0], -EINVAL, 0);
	if (parse_u32(argv[1], &id) || id > 0xff)
		return fail(argv[1], -EINVAL, 0);
	for (i = 2; i < argc; i++)
		if (parse_u32(argv[i], &args[i - 2]))
			return fail(argv[i], -EINVAL, 0);

	if (dry_run) {
		printf("msg: would send %s 0x%02x args", argv[0], id);
		for (i = 0; i < SHS_NUM_ARGS; i++)
			printf(" 0x%08x", args[i]);
		putchar('\n');
		return 0;
	}

	/* The module decides what each message may do; a writable fd lets it allow set class. */
	if (open_dev(1, &fd))
		return EXIT_ERR;
	err = shs_msg(fd, mbox, id, args, &rsp);
	shs_close(fd);
	if (err)
		return fail("msg", err, 1);

	printf("rsp 0x%02x (%s) args", rsp, rsp_name(rsp));
	for (i = 0; i < SHS_NUM_ARGS; i++)
		printf(" 0x%08x", args[i]);
	putchar('\n');
	return rsp == SHS_RSP_OK ? 0 : EXIT_SMU;
}

static int cmd_stats(void)
{
	struct shs_send_stat st;
	unsigned int i;
	int fd, n = 0;

	fd = open(SHS_SEND_STATS_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(SHS_SEND_STATS_PATH, -errno, 0);

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
	const char *c;

	if (argc >= 2 && (!strcmp(argv[1], "-n") || !strcmp(argv[1], "--dry-run"))) {
		dry_run = 1;
		argc--;
		argv++;
	}
	if (argc < 2) {
		usage(stderr);
		return EXIT_USAGE;
	}
	c = argv[1];

	if (!strcmp(c, "help") || !strcmp(c, "-h") || !strcmp(c, "--help"))
		return argc <= 3 ? cmd_help(argc == 3 ? argv[2] : NULL) : EXIT_USAGE;
	if (argc == 2 && !strcmp(c, "info"))
		return cmd_info();
	if (argc == 2 && !strcmp(c, "table"))
		return cmd_table(0);
	if (argc == 3 && !strcmp(c, "table") && !strcmp(argv[2], "--raw"))
		return cmd_table(1);
	if (argc <= 3 && !strcmp(c, "watch"))
		return cmd_watch(argc == 3 ? argv[2] : NULL);
	if (argc == 3 && !strcmp(c, "co-all"))
		return cmd_co_all(argv[2]);
	if (argc == 4 && !strcmp(c, "co-core"))
		return cmd_co_core(argv[2], argv[3]);
	if (argc == 3 && (!strcmp(c, "fast-limit") || !strcmp(c, "slow-limit")))
		return cmd_limit(c, argv[2]);
	if (!strcmp(c, "msg"))
		return cmd_msg(argc - 2, argv + 2);
	if (argc == 2 && !strcmp(c, "stats"))
		return cmd_stats();

	/* A known command with wrong arguments gets its own help. */
	if (!cmd_help(c))
		return EXIT_USAGE;
	usage(stderr);
	return EXIT_USAGE;
}
