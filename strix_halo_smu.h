/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * strix_halo_smu userspace interface, /dev/strix_halo_smu.
 *
 * Message IDs are the catalogue the module knows; see research/safeguard_survey.md
 * for provenance. Values passed in args are firmware encodings, not checked.
 */
#ifndef _UAPI_STRIX_HALO_SMU_H
#define _UAPI_STRIX_HALO_SMU_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SHS_MBOX_MP1		0
#define SHS_MBOX_RSMU		1

#define SHS_NUM_ARGS		6

/* SMU response codes (AMD ppsmc numbering). */
#define SHS_RSP_OK		0x01
#define SHS_RSP_BUSY		0xfc
#define SHS_RSP_PREREQ		0xfd
#define SHS_RSP_UNKNOWN		0xfe
#define SHS_RSP_FAILED		0xff

/* MP1, read class */
#define SHS_MP1_TEST			0x01	/* answers args[0] + 1 */
#define SHS_MP1_GET_PMFW_VERSION	0x02
#define SHS_MP1_GET_INTERFACE_VERSION	0x03

/* MP1, set class */
#define SHS_MP1_STAPM_LIMIT		0x14
#define SHS_MP1_FAST_LIMIT		0x15
#define SHS_MP1_SLOW_LIMIT		0x16
#define SHS_MP1_SLOW_TIME		0x17
#define SHS_MP1_STAPM_TIME		0x18
#define SHS_MP1_APU_SLOW_LIMIT		0x23
#define SHS_MP1_CO_PER_CORE		0x4b
#define SHS_MP1_CO_ALL_CORE		0x4c

/* MP1, safeguard class: needs allow_safeguard_writes=1 at load */
#define SHS_MP1_FEATURE_ENABLE		0x05	/* single source, unverified */
#define SHS_MP1_FEATURE_DISABLE		0x07	/* single source, unverified */
#define SHS_MP1_TCTL_LIMIT		0x19
#define SHS_MP1_VDD_TDC_LIMIT		0x1a
#define SHS_MP1_SOC_TDC_LIMIT		0x1b
#define SHS_MP1_VDD_EDC_LIMIT		0x1c
#define SHS_MP1_SOC_EDC_LIMIT		0x1d
#define SHS_MP1_PSI0_CURRENT		0x1e
#define SHS_MP1_PROCHOT_RAMP		0x1f
#define SHS_MP1_APU_SKIN_LIMIT		0x33
#define SHS_MP1_DGPU_SKIN_LIMIT		0x34
#define SHS_MP1_STT_POWER_LIMIT		0x4a
#define SHS_MP1_CHTC_LIMIT		0x63	/* single source, unverified */

/* RSMU, read class */
#define SHS_RSMU_GET_TABLE_VERSION	0x06	/* args[0] version, args[1] size */
#define SHS_RSMU_GET_TABLE_BASE		0x66
/* Table transfer: only through SHS_IOC_TABLE or pm_table, which rate-limit it. */
#define SHS_RSMU_TRANSFER_TABLE		0x65

/* RSMU, safeguard class, all single source and unverified */
#define SHS_RSMU_OC_ENABLE		0x17
#define SHS_RSMU_OC_DISABLE		0x18
#define SHS_RSMU_OC_CLOCK_ALL		0x19
#define SHS_RSMU_OC_CLOCK_PER_CORE	0x1a
#define SHS_RSMU_OC_VOLTAGE		0x1b
#define SHS_RSMU_GFX_CLOCK_FORCE	0x1c

/*
 * One complete mailbox transaction. Returns 0 when the SMU answered; rsp then
 * holds its raw response code and args its results.
 */
struct shs_msg {
	__u32 mailbox;			/* SHS_MBOX_* */
	__u32 msg;
	__u32 args[SHS_NUM_ARGS];	/* in: arguments, out: results */
	__u32 rsp;			/* out */
};

/* A copy of the PM table snapshot and when it was taken. */
struct shs_table {
	__u64 buf;			/* user pointer */
	__u32 size;			/* in: buffer size, out: table size */
	__u32 version;			/* out: table version, RSMU 0x06 */
	__u64 time_ns;			/* out: CLOCK_MONOTONIC at refresh completion */
};

#define SHS_IOC_MAGIC		0xb9
#define SHS_IOC_MSG		_IOWR(SHS_IOC_MAGIC, 0x00, struct shs_msg)
#define SHS_IOC_TABLE		_IOWR(SHS_IOC_MAGIC, 0x01, struct shs_table)

#endif /* _UAPI_STRIX_HALO_SMU_H */
