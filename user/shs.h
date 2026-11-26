/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libshs - userspace access to strix_halo_smu.
 *
 * Thin wrapper over /dev/strix_halo_smu: transactions, the PM table snapshot,
 * named table fields, and encoders for messages verified on hardware. No policy.
 * Functions return 0 or a negative errno unless stated otherwise.
 */
#ifndef SHS_H
#define SHS_H

#include <stddef.h>
#include <stdint.h>

#include "strix_halo_smu.h"

#define SHS_DEV_PATH		"/dev/strix_halo_smu"
#define SHS_SEND_STATS_PATH	"/sys/class/misc/strix_halo_smu/send_stats"

/* Table layout the field accessors know; any other version reads as NAN. */
#define SHS_TABLE_0064020C	0x0064020c
#define SHS_TABLE_MAX		0x2000
#define SHS_TABLE_CORES		16

struct shs_snapshot {
	uint32_t size;
	uint32_t version;
	uint64_t time_ns;		/* CLOCK_MONOTONIC at refresh completion */
	uint8_t data[SHS_TABLE_MAX];
};

/* Returns an fd or -errno. Set-class messages need writable != 0. */
int shs_open(int writable);
void shs_close(int fd);

/*
 * One transaction. Returns 0 when the SMU answered (its code in *rsp, results
 * in args), or -errno from the module: ENOMSG uncatalogued, EPERM safeguard,
 * deny or table class, EBADF set class on a read-only fd, EAGAIN send budget.
 */
int shs_msg(int fd, unsigned int mailbox, unsigned int msg,
	    uint32_t args[SHS_NUM_ARGS], uint32_t *rsp);

/* Read-class queries; -EPROTO when the SMU answers anything but OK. */
int shs_pmfw_version(int fd, uint32_t *version);
int shs_interface_version(int fd, uint32_t *version);

/* Values average over the time since the previous refresh; poll steadily. */
int shs_read_table(int fd, struct shs_snapshot *s);

float shs_table_f32(const struct shs_snapshot *s, size_t off);
float shs_stapm_limit_w(const struct shs_snapshot *s);
float shs_slow_limit_w(const struct shs_snapshot *s);
float shs_skin_limit_c(const struct shs_snapshot *s);
float shs_tctl_c(const struct shs_snapshot *s);
float shs_core_power_w(const struct shs_snapshot *s, unsigned int core);
float shs_core_volt_mv(const struct shs_snapshot *s, unsigned int core);
float shs_core_temp_c(const struct shs_snapshot *s, unsigned int core);
float shs_core_clk_mhz(const struct shs_snapshot *s, unsigned int core);

/* Curve Optimizer, all cores (MP1 0x4C): 20-bit two's complement steps; 0 is the default. */
uint32_t shs_co_encode(int steps);
int shs_co_all(int fd, int steps, uint32_t *rsp);

#endif /* SHS_H */
