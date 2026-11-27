// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "shs.h"

/* Offsets in table 0x0064020C; checked against BIOS values and k10temp, see docs/05-pm-table.md. */
#define OFF_STT_CAP		0x000
#define OFF_FAST_LIMIT		0x008
#define OFF_FAST_POWER		0x00c
#define OFF_SLOW_LIMIT		0x010
#define OFF_SKIN_LIMIT		0x068
#define OFF_SKIN_TEMP		0x06c
#define OFF_TCTL		0xb38
#define OFF_CORE_POWER		0xb90
#define OFF_CORE_VOLT		0xbd0
#define OFF_CORE_TEMP		0xc10
#define OFF_CORE_CLK		0xc50

int shs_open(int writable)
{
	int fd = open(SHS_DEV_PATH, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);

	return fd < 0 ? -errno : fd;
}

void shs_close(int fd)
{
	close(fd);
}

int shs_msg(int fd, unsigned int mailbox, unsigned int msg,
	    uint32_t args[SHS_NUM_ARGS], uint32_t *rsp)
{
	struct shs_msg m = { .mailbox = mailbox, .msg = msg };

	memcpy(m.args, args, sizeof(m.args));
	if (ioctl(fd, SHS_IOC_MSG, &m))
		return -errno;

	memcpy(args, m.args, sizeof(m.args));
	*rsp = m.rsp;
	return 0;
}

static int query(int fd, unsigned int mailbox, unsigned int msg, uint32_t *out)
{
	uint32_t args[SHS_NUM_ARGS] = { 0 };
	uint32_t rsp;
	int err;

	err = shs_msg(fd, mailbox, msg, args, &rsp);
	if (err)
		return err;
	if (rsp != SHS_RSP_OK)
		return -EPROTO;

	*out = args[0];
	return 0;
}

int shs_pmfw_version(int fd, uint32_t *version)
{
	return query(fd, SHS_MBOX_MP1, SHS_MP1_GET_PMFW_VERSION, version);
}

int shs_interface_version(int fd, uint32_t *version)
{
	return query(fd, SHS_MBOX_MP1, SHS_MP1_GET_INTERFACE_VERSION, version);
}

int shs_read_table(int fd, struct shs_snapshot *s)
{
	struct shs_table t = {
		.buf = (uintptr_t)s->data,
		.size = sizeof(s->data),
	};

	if (ioctl(fd, SHS_IOC_TABLE, &t))
		return -errno;

	s->size = t.size;
	s->version = t.version;
	s->time_ns = t.time_ns;
	return 0;
}

float shs_table_f32(const struct shs_snapshot *s, size_t off)
{
	float v;

	if (off + sizeof(v) > s->size)
		return NAN;
	memcpy(&v, s->data + off, sizeof(v));
	return v;
}

static float field(const struct shs_snapshot *s, size_t off)
{
	return s->version == SHS_TABLE_0064020C ? shs_table_f32(s, off) : NAN;
}

static float core_field(const struct shs_snapshot *s, size_t base, unsigned int core)
{
	return core < SHS_TABLE_CORES ? field(s, base + 4 * core) : NAN;
}

float shs_stt_cap_w(const struct shs_snapshot *s)
{
	return field(s, OFF_STT_CAP);
}

float shs_fast_limit_w(const struct shs_snapshot *s)
{
	return field(s, OFF_FAST_LIMIT);
}

float shs_slow_limit_w(const struct shs_snapshot *s)
{
	return field(s, OFF_SLOW_LIMIT);
}

float shs_fast_power_w(const struct shs_snapshot *s)
{
	return field(s, OFF_FAST_POWER);
}

float shs_skin_limit_c(const struct shs_snapshot *s)
{
	return field(s, OFF_SKIN_LIMIT);
}

float shs_skin_temp_c(const struct shs_snapshot *s)
{
	return field(s, OFF_SKIN_TEMP);
}

float shs_tctl_c(const struct shs_snapshot *s)
{
	return field(s, OFF_TCTL);
}

float shs_core_power_w(const struct shs_snapshot *s, unsigned int core)
{
	return core_field(s, OFF_CORE_POWER, core);
}

float shs_core_volt_mv(const struct shs_snapshot *s, unsigned int core)
{
	return core_field(s, OFF_CORE_VOLT, core) * 1000.0f;
}

float shs_core_temp_c(const struct shs_snapshot *s, unsigned int core)
{
	return core_field(s, OFF_CORE_TEMP, core);
}

/* The table stores core clock in GHz. */
float shs_core_clk_mhz(const struct shs_snapshot *s, unsigned int core)
{
	return core_field(s, OFF_CORE_CLK, core) * 1000.0f;
}

static int send1(int fd, unsigned int msg, uint32_t arg, uint32_t *rsp)
{
	uint32_t args[SHS_NUM_ARGS] = { arg };

	return shs_msg(fd, SHS_MBOX_MP1, msg, args, rsp);
}

int shs_set_fast_limit(int fd, uint32_t mw, uint32_t *rsp)
{
	return send1(fd, SHS_MP1_FAST_LIMIT, mw, rsp);
}

int shs_set_slow_limit(int fd, uint32_t mw, uint32_t *rsp)
{
	return send1(fd, SHS_MP1_SLOW_LIMIT, mw, rsp);
}

uint32_t shs_co_encode(int steps)
{
	return (uint32_t)steps & 0xfffff;
}

int shs_co_all(int fd, int steps, uint32_t *rsp)
{
	if (steps < SHS_CO_ALL_MIN || steps > SHS_CO_ALL_MAX)
		return -ERANGE;

	return send1(fd, SHS_MP1_CO_ALL_CORE, shs_co_encode(steps), rsp);
}

/* The SMU refuses a core index above 7 in bits 23-20 (0xFF); the CCD goes in bits 31-28. */
uint32_t shs_co_core_encode(unsigned int core, int steps)
{
	return (core / 8) << 28 | (core % 8) << 20 | ((uint32_t)steps & 0xffff);
}

int shs_co_core(int fd, unsigned int core, int steps, uint32_t *rsp)
{
	if (core >= SHS_TABLE_CORES)
		return -EINVAL;
	if (steps < SHS_CO_CORE_MIN || steps > SHS_CO_CORE_MAX)
		return -ERANGE;

	return send1(fd, SHS_MP1_CO_PER_CORE, shs_co_core_encode(core, steps), rsp);
}
