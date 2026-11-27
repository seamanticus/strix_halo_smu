// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * strix_halo_smu - SMU mailbox transport for AMD Strix Halo on the ASUS ROG
 * Flow Z13 GZ302EA.
 *
 * Serialized MP1/RSMU transactions through /dev/strix_halo_smu, gated by a
 * message catalogue, plus the PM table as a rate-limited snapshot.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <asm/amd/node.h>
#include <asm/processor.h>

#include "strix_halo_smu.h"

#define SHS_CPU_FAMILY		0x1a
#define SHS_CPU_MODEL		0x70
#define SHS_ROOT_DEVICE		0x1507
#define SHS_DMI_VENDOR		"ASUSTeK COMPUTER INC."
#define SHS_DMI_BOARD		"GZ302EA"

/* Firmware-wide SMN lock taken by ALIB (SSDT1 "AmdTable") around MP1 traffic. */
#define SHS_AM00_TIMEOUT_MS	50
static char shs_am00_path[] = "\\_SB.AM00";
static char shs_alib_path[] = "\\_SB.ALIB";
#define SHS_ALIB_TABLE_ID	"AmdTable"

#define SHS_ARGS		SHS_NUM_ARGS
/* Bounds the time AM00 is held while waiting for the SMU to answer. */
#define SHS_RSP_TIMEOUT_US	50000
/* After a timeout, how long the message is drained before its locks are released. */
#define SHS_DRAIN_TIMEOUT_US	100000
/* RSMU 0x65 measured up to 13.8 ms at idle; it is waited on outside AM00. */
#define SHS_TABLE_TIMEOUT_US	100000

/* Minimum PM table refresh interval; see docs/02-safety.md 2.8. */
#define SHS_REFRESH_MS		55
#define SHS_REFRESH_NS		(SHS_REFRESH_MS * NSEC_PER_MSEC)

#define SHS_TCTL_SMN		0x00059800
/* Instantaneous Tctl in the table (M2 capture: r = 1.000 against k10temp); 0x5c is filtered. */
#define SHS_PM_TCTL_OFF		0xb38
/* Worst skew seen under a step load was 1.25 C; see docs/07-findings.md 7.8. */
#define SHS_TCTL_TOLERANCE_MC	3000
/* Longest transfer-to-transfer window the M2 calibration covers (55 ms polling). */
#define SHS_CANARY_WINDOW_NS	(2 * SHS_REFRESH_NS)

static bool allow_unlisted;
module_param(allow_unlisted, bool, 0400);
MODULE_PARM_DESC(allow_unlisted, "Forward message IDs outside the catalogue (taints)");

static bool allow_safeguard_writes;
module_param(allow_safeguard_writes, bool, 0400);
MODULE_PARM_DESC(allow_safeguard_writes, "Forward safeguard-class messages: thermal, current, feature (taints)");

static DEFINE_MUTEX(shs_mp1_lock);
static DEFINE_MUTEX(shs_rsmu_lock);

struct shs_mbox {
	const char *name;
	u32 msg;
	u32 rsp;
	u32 arg;
	bool am00;			/* firmware also drives this mailbox */
	u32 timeout_us;
	struct mutex *lock;		/* one transaction at a time per mailbox */
};

static const struct shs_mbox shs_mp1 = {
	.name = "mp1", .msg = 0x03b10928, .rsp = 0x03b10978, .arg = 0x03b10998,
	.am00 = true, .timeout_us = SHS_RSP_TIMEOUT_US, .lock = &shs_mp1_lock,
};

/* No AML references RSMU (phase 0 scan; M1 pins that BIOS), so AM00 is not taken. */
static const struct shs_mbox shs_rsmu = {
	.name = "rsmu", .msg = 0x03b10a20, .rsp = 0x03b10a80, .arg = 0x03b10a88,
	.am00 = false, .timeout_us = SHS_TABLE_TIMEOUT_US, .lock = &shs_rsmu_lock,
};

/* Firmware contracts verified on hardware; the module binds to nothing else. */
static const struct shs_contract {
	u32 bios_if;
	u32 table_ver;
	u32 table_size;
	u32 alib_rev;
} shs_verified[] = {
	{ 25, 0x0064020c, 0x1028, 2 },
};

static struct {
	struct mutex lock;		/* serializes refresh and the snapshot */
	bool wedged;			/* a transaction was left half-done */
	bool table_mismatch;		/* M2 failed: the table no longer matches the Tctl sensor */
	bool canary_checked;		/* M2 has run at least once */
	int snap_err;			/* outcome of the last refresh */
	void *table;			/* SMU-owned page, written by RSMU 0x65 */
	u8 *snap;			/* last complete copy served to readers */
	size_t size;
	u32 version;
	ktime_t req_time;		/* start of the last refresh; keys the interval */
	ktime_t snap_time;		/* completion of the last refresh; the data age */
} shs;

enum shs_class {
	SHS_READ,		/* no state change */
	SHS_SET,		/* firmware-owned limits and CO; needs a writable fd */
	SHS_SAFEGUARD,		/* can weaken a protection; also needs allow_safeguard_writes */
	SHS_TABLE,		/* reached only through the rate-limited refresh */
	SHS_DENY,		/* never forwarded, whatever the gates */
};

struct shs_entry {
	u8 mailbox;
	u8 msg;
	u8 cls;
	bool verified;		/* false: single-source, catalogued only to keep it gated */
};

/* Inclusion rule and provenance per ID: docs/06-glossary.md. */
static const struct shs_entry shs_catalogue[] = {
	{ SHS_MBOX_MP1, SHS_MP1_TEST,			SHS_READ,	true },
	{ SHS_MBOX_MP1, SHS_MP1_GET_PMFW_VERSION,	SHS_READ,	true },
	{ SHS_MBOX_MP1, SHS_MP1_GET_INTERFACE_VERSION,	SHS_READ,	true },
	{ SHS_MBOX_MP1, SHS_MP1_STAPM_LIMIT,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_FAST_LIMIT,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_SLOW_LIMIT,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_SLOW_TIME,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_STAPM_TIME,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_APU_SLOW_LIMIT,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_CO_PER_CORE,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_CO_ALL_CORE,		SHS_SET,	true },
	{ SHS_MBOX_MP1, SHS_MP1_FEATURE_ENABLE,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_MP1, SHS_MP1_FEATURE_DISABLE,	SHS_SAFEGUARD,	false },
	{ SHS_MBOX_MP1, SHS_MP1_TCTL_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_VDD_TDC_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_SOC_TDC_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_VDD_EDC_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_SOC_EDC_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_PSI0_CURRENT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_PROCHOT_RAMP,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_APU_SKIN_LIMIT,		SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_DGPU_SKIN_LIMIT,	SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_STT_POWER_LIMIT,	SHS_SAFEGUARD,	true },
	{ SHS_MBOX_MP1, SHS_MP1_CHTC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_MP1, SHS_MP1_GPU_BAR_LO,		SHS_DENY,	true },
	{ SHS_MBOX_MP1, SHS_MP1_GPU_BAR_HI,		SHS_DENY,	true },
	{ SHS_MBOX_RSMU, SHS_RSMU_GET_TABLE_VERSION,	SHS_READ,	true },
	{ SHS_MBOX_RSMU, SHS_RSMU_GET_TABLE_BASE,	SHS_READ,	true },
	{ SHS_MBOX_RSMU, SHS_RSMU_TRANSFER_TABLE,	SHS_TABLE,	true },
	{ SHS_MBOX_RSMU, SHS_RSMU_OC_ENABLE,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_OC_DISABLE,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_OC_CLOCK_ALL,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_OC_CLOCK_PER_CORE,	SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_OC_VOLTAGE,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_GFX_CLOCK_FORCE,	SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_CHTC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_VDD_TDC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_SOC_TDC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_VDD_EDC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_SOC_EDC_LIMIT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_PSI0_CURRENT,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_PSI0_SOC_CURRENT,	SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_PBO_SCALAR,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_GFX_CLOCK,		SHS_SAFEGUARD,	false },
	{ SHS_MBOX_RSMU, SHS_RSMU_DGPU_SKIN_LIMIT,	SHS_SAFEGUARD,	false },
};

static int shs_check_identity(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	struct pci_dev *root;
	acpi_handle h;
	bool ok;

	if (c->x86_vendor != X86_VENDOR_AMD || c->x86 != SHS_CPU_FAMILY ||
	    c->x86_model != SHS_CPU_MODEL) {
		pr_err("unsupported CPU family 0x%x model 0x%x\n", c->x86, c->x86_model);
		return -ENODEV;
	}

	if (!dmi_match(DMI_SYS_VENDOR, SHS_DMI_VENDOR) ||
	    !dmi_match(DMI_BOARD_NAME, SHS_DMI_BOARD)) {
		pr_err("unsupported board '%s'\n", dmi_get_system_info(DMI_BOARD_NAME) ?: "?");
		return -ENODEV;
	}

	root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	ok = root && root->vendor == PCI_VENDOR_ID_AMD && root->device == SHS_ROOT_DEVICE;
	pci_dev_put(root);
	if (!ok) {
		pr_err("unexpected root complex\n");
		return -ENODEV;
	}

	if (ACPI_FAILURE(acpi_get_handle(NULL, shs_am00_path, &h)) ||
	    ACPI_FAILURE(acpi_get_handle(NULL, shs_alib_path, &h))) {
		pr_err("firmware lock %s or %s missing\n", shs_am00_path, shs_alib_path);
		return -ENODEV;
	}

	return 0;
}

/* OEM revision of the SSDT carrying ALIB and AM00, or 0 if it is absent. */
static u32 shs_alib_revision(void)
{
	struct acpi_table_header *hdr;
	u32 rev = 0, i;

	for (i = 1; !rev && ACPI_SUCCESS(acpi_get_table(ACPI_SIG_SSDT, i, &hdr)); i++) {
		if (!strncmp(hdr->oem_table_id, SHS_ALIB_TABLE_ID, ACPI_OEM_TABLE_ID_SIZE))
			rev = hdr->oem_revision;
		acpi_put_table(hdr);
	}

	return rev;
}

static int shs_wait_rsp(const struct shs_mbox *mb, u32 *rsp, u32 timeout_us)
{
	ktime_t end = ktime_add_us(ktime_get(), timeout_us);
	int err;

	for (;;) {
		err = amd_smn_read(0, mb->rsp, rsp);
		if (err)
			return err;
		if (*rsp)
			return 0;
		if (ktime_after(ktime_get(), end))
			return -ETIMEDOUT;
		usleep_range(10, 20);
	}
}

/*
 * Writes a message without waiting for it. -EBUSY means the mailbox already
 * had a message in flight and nothing was written.
 */
static int shs_post(const struct shs_mbox *mb, u32 msg, const u32 args[SHS_ARGS])
{
	u32 cur;
	int err, i;

	err = amd_smn_read(0, mb->rsp, &cur);
	if (!err && !cur)
		return -EBUSY;

	if (!err)
		err = amd_smn_write(0, mb->rsp, 0);
	for (i = 0; !err && i < SHS_ARGS; i++)
		err = amd_smn_write(0, mb->arg + 4 * i, args[i]);
	if (!err)
		err = amd_smn_write(0, mb->msg, msg);

	return err;
}

/*
 * One mailbox transaction, under the mailbox's own lock and, for mailboxes the
 * firmware also drives, under AM00. Returns 0 when the SMU answered, with its
 * raw response code in *rsp and its results in args.
 */
static int shs_send(const struct shs_mbox *mb, u32 msg, u32 args[SHS_ARGS], u32 *rsp)
{
	acpi_status st;
	int err, i;

	mutex_lock(mb->lock);

	if (READ_ONCE(shs.wedged)) {
		err = -EIO;
		goto unlock;
	}

	if (mb->am00) {
		st = acpi_acquire_mutex(NULL, shs_am00_path, SHS_AM00_TIMEOUT_MS);
		if (ACPI_FAILURE(st)) {
			pr_warn("%s: cannot acquire %s: %s\n", mb->name, shs_am00_path,
				acpi_format_exception(st));
			err = -EBUSY;
			goto unlock;
		}
	}

	/* Busy here means an agent outside our locks has a message in flight. */
	err = shs_post(mb, msg, args);
	if (err == -EBUSY) {
		pr_warn("%s: mailbox busy outside our locks, not sending 0x%02x\n", mb->name, msg);
		goto out;
	}
	if (!err)
		err = shs_wait_rsp(mb, rsp, mb->timeout_us);

	if (err) {
		WRITE_ONCE(shs.wedged, true);
		pr_err("%s: message 0x%02x failed (%d), refusing further sends\n",
		       mb->name, msg, err);
		/* Still locked: firmware must not write over a message the SMU may be running. */
		if (err == -ETIMEDOUT) {
			ktime_t d0 = ktime_get();
			u32 late = 0;
			int derr = shs_wait_rsp(mb, &late, SHS_DRAIN_TIMEOUT_US);

			pr_err("%s: drain %s: rsp 0x%02x after %lld us more\n", mb->name,
			       derr ? "gave up" : "done", late, ktime_us_delta(ktime_get(), d0));
		}
		goto out;
	}

	for (i = 0; !err && i < SHS_ARGS; i++)
		err = amd_smn_read(0, mb->arg + 4 * i, &args[i]);

out:
	if (mb->am00)
		acpi_release_mutex(NULL, shs_am00_path);
unlock:
	mutex_unlock(mb->lock);
	return err;
}

/* Sends a message that must answer OK; any other response code is -EIO. */
static int shs_query(const struct shs_mbox *mb, u32 msg, u32 args[SHS_ARGS])
{
	u32 rsp = 0;
	int err;

	err = shs_send(mb, msg, args, &rsp);
	if (!err && rsp != SHS_RSP_OK) {
		pr_err("%s: message 0x%02x answered 0x%02x\n", mb->name, msg, rsp);
		err = -EIO;
	}

	return err;
}

static int shs_check_contract(u32 *table_size)
{
	u32 a[SHS_ARGS] = {}, b[SHS_ARGS] = {}, v[SHS_ARGS] = {};
	u32 alib = shs_alib_revision();
	unsigned int i;
	int err;

	err = shs_query(&shs_mp1, SHS_MP1_GET_PMFW_VERSION, v);
	if (!err)
		err = shs_query(&shs_mp1, SHS_MP1_GET_INTERFACE_VERSION, a);
	if (!err)
		err = shs_query(&shs_rsmu, SHS_RSMU_GET_TABLE_VERSION, b);
	if (err)
		return err;

	pr_info("pmfw %u.%u.%u.%u, interface %u, table 0x%08x size 0x%x, %s rev %u\n",
		v[0] >> 24, (v[0] >> 16) & 0xff, (v[0] >> 8) & 0xff, v[0] & 0xff,
		a[0], b[0], b[1], SHS_ALIB_TABLE_ID, alib);

	for (i = 0; i < ARRAY_SIZE(shs_verified); i++) {
		const struct shs_contract *c = &shs_verified[i];

		if (c->bios_if == a[0] && c->table_ver == b[0] &&
		    c->table_size == b[1] && c->alib_rev == alib) {
			shs.version = b[0];
			*table_size = b[1];
			return 0;
		}
	}

	pr_err("firmware contract not verified, refusing to bind\n");
	return -ENODEV;
}

static int shs_map_table(size_t size)
{
	u32 args[SHS_ARGS] = {};
	phys_addr_t base;
	int err;

	err = shs_query(&shs_rsmu, SHS_RSMU_GET_TABLE_BASE, args);
	if (err)
		return err;

	base = ((u64)args[1] << 32) | args[0];

	/* The SMU writes this page by DMA; it must not be memory the kernel owns. */
	if (!base || region_intersects(base, size, IORESOURCE_SYSTEM_RAM,
				       IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("pm table at %pa (0x%zx bytes) overlaps System RAM, refusing\n",
		       &base, size);
		return -EFAULT;
	}

	shs.table = memremap(base, size, MEMREMAP_WB);
	if (!shs.table)
		return -ENOMEM;

	pr_info("pm table at %pa, 0x%zx bytes\n", &base, size);
	return 0;
}

/* IEEE-754 single to thousandths, without the FPU. */
static s64 shs_f32_milli(u32 b)
{
	int e = (b >> 23) & 0xff;
	s64 v = (s64)((b & 0x7fffff) | 0x800000) * 1000;

	if (!e || e == 0xff)
		return 0;

	e -= 150;
	if (e >= 0)
		v = e > 20 ? S32_MAX : v << e;
	else
		v = -e >= 63 ? 0 : v >> -e;

	return (b >> 31) ? -v : v;
}

/* Tctl in milli-C from the SMN register k10temp reads. */
static int shs_smn_tctl(s64 *milli)
{
	u32 r;
	int err;

	err = amd_smn_read(0, SHS_TCTL_SMN, &r);
	if (err)
		return err;

	*milli = (s64)(r >> 21) * 125;
	if ((r & BIT(19)) || (r & GENMASK(17, 16)) == GENMASK(17, 16))
		*milli -= 49000;

	return 0;
}

/*
 * M2: the table's Tctl must track the sensor k10temp reads. A mismatch means the
 * layout no longer is the one verified, so the table stops being served.
 */
static int shs_check_tctl(void)
{
	s64 table, smn;
	int err;

	err = shs_smn_tctl(&smn);
	if (err)
		return err;

	table = shs_f32_milli(get_unaligned_le32(shs.snap + SHS_PM_TCTL_OFF));
	if (abs(table - smn) > SHS_TCTL_TOLERANCE_MC) {
		shs.table_mismatch = true;
		pr_err("pm table tctl %lld mC, sensor %lld mC: layout not verified, table disabled\n",
		       table, smn);
		return -EIO;
	}

	return 0;
}

static int shs_fetch(void)
{
	/* 0x65 is rejected with 0xFD on the first call after boot or a long idle. */
	static const unsigned int retry_ms[] = { 10, 100 };
	ktime_t prev = shs.snap_time;
	unsigned int i;
	u32 rsp = 0;
	int err;

	for (i = 0; ; i++) {
		u32 args[SHS_ARGS] = { 3 };

		err = shs_send(&shs_rsmu, SHS_RSMU_TRANSFER_TABLE, args, &rsp);
		if (err || rsp != SHS_RSP_PREREQ || i == ARRAY_SIZE(retry_ms))
			break;
		pr_info_ratelimited("rsmu: 0x65 answered 0xfd, retry in %u ms\n", retry_ms[i]);
		msleep(retry_ms[i]);
	}
	if (err)
		return err;
	if (rsp != SHS_RSP_OK) {
		pr_err("rsmu: message 0x65 answered 0x%02x\n", rsp);
		return -EIO;
	}

	memcpy(shs.snap, shs.table, shs.size);
	shs.snap_time = ktime_get();

	/* Table values average since the previous transfer; M2 is calibrated on short windows. */
	if (!prev || ktime_to_ns(ktime_sub(shs.snap_time, prev)) > SHS_CANARY_WINDOW_NS)
		return 0;

	shs.canary_checked = true;
	return shs_check_tctl();
}

/*
 * Refreshes the snapshot unless it is younger than the minimum interval. Inside
 * the interval callers get the outcome of the last refresh, failures included.
 */
static int shs_refresh(void)
{
	ktime_t now = ktime_get();

	lockdep_assert_held(&shs.lock);

	if (shs.table_mismatch)
		return -EIO;
	if (shs.req_time && ktime_to_ns(ktime_sub(now, shs.req_time)) < SHS_REFRESH_NS)
		return shs.snap_err;

	shs.req_time = now;
	shs.snap_err = shs_fetch();
	return shs.snap_err;
}

/* A read at offset 0 refreshes; later chunks come from the same snapshot. */
static ssize_t pm_table_read(struct file *f, struct kobject *kobj,
			     const struct bin_attribute *attr, char *buf,
			     loff_t off, size_t count)
{
	ssize_t ret;
	int err;

	if (off >= shs.size)
		return 0;
	count = min_t(size_t, count, shs.size - off);

	mutex_lock(&shs.lock);
	err = off ? shs.snap_err : shs_refresh();
	if (!err)
		memcpy(buf, shs.snap + off, count);
	ret = err ?: count;
	mutex_unlock(&shs.lock);

	return ret;
}

static struct bin_attribute bin_attr_pm_table = __BIN_ATTR_RO(pm_table, 0);

/*
 * Send budget per mailbox, shared by every caller and every class: it bounds
 * how long userspace can keep AM00 from the BIOS and how fast the log grows.
 */
static DEFINE_MUTEX(shs_send_lock);
static struct shs_bucket {
	unsigned int tokens;
	ktime_t refilled;
} shs_bucket[2];
static struct shs_send_stat shs_stats[2 * 256];

static ssize_t send_stats_read(struct file *f, struct kobject *kobj,
			       const struct bin_attribute *attr, char *buf,
			       loff_t off, size_t count)
{
	if (off >= sizeof(shs_stats))
		return 0;
	count = min_t(size_t, count, sizeof(shs_stats) - off);

	mutex_lock(&shs_send_lock);
	memcpy(buf, (u8 *)shs_stats + off, count);
	mutex_unlock(&shs_send_lock);

	return count;
}

/* Root-only: the records name senders' pids and their arguments. */
static const struct bin_attribute bin_attr_send_stats =
	__BIN_ATTR_ADMIN_RO(send_stats, sizeof(shs_stats));

static const struct bin_attribute *const shs_bin_attrs[] = {
	&bin_attr_pm_table,
	&bin_attr_send_stats,
	NULL,
};

static const struct attribute_group shs_group = {
	.bin_attrs = shs_bin_attrs,
};

static const struct attribute_group *shs_groups[] = {
	&shs_group,
	NULL,
};

static const struct shs_entry *shs_lookup(u32 mailbox, u32 msg)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(shs_catalogue); i++)
		if (shs_catalogue[i].mailbox == mailbox && shs_catalogue[i].msg == msg)
			return &shs_catalogue[i];

	return NULL;
}

static bool shs_take_token(struct shs_bucket *b)
{
	const s64 step = (s64)SHS_SEND_INTERVAL_MS * NSEC_PER_MSEC;
	ktime_t now = ktime_get();
	s64 n;

	lockdep_assert_held(&shs_send_lock);

	if (!b->refilled) {
		b->tokens = SHS_SEND_BURST;
		b->refilled = now;
	}

	n = div64_s64(ktime_to_ns(ktime_sub(now, b->refilled)), step);
	if (n >= SHS_SEND_BURST - b->tokens) {
		b->tokens = SHS_SEND_BURST;
		b->refilled = now;
	} else if (n) {
		b->tokens += n;
		b->refilled = ktime_add_ns(b->refilled, n * step);
	}

	if (!b->tokens)
		return false;
	b->tokens--;
	return true;
}

/*
 * Decides whether a message may be sent. On success *what names the class of a
 * state-changing message, or is NULL for read class.
 */
static int shs_gate(struct file *file, const struct shs_msg *m, const char **what)
{
	const struct shs_entry *e = shs_lookup(m->mailbox, m->msg);
	int err;

	*what = NULL;
	if (e && e->cls == SHS_READ)
		return 0;

	if (!e) {
		if (!allow_unlisted)
			return -ENOMSG;
		*what = "unlisted";
	} else if (e->cls == SHS_TABLE || e->cls == SHS_DENY) {
		return -EPERM;
	} else if (e->cls == SHS_SAFEGUARD) {
		if (!allow_safeguard_writes)
			return -EPERM;
		*what = e->verified ? "safeguard" : "unverified safeguard";
	} else {
		*what = "set";
	}

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	err = security_locked_down(LOCKDOWN_MSR);

	return err;
}

static long shs_ioctl_msg(struct file *file, struct shs_msg __user *um)
{
	const struct shs_mbox *mb;
	struct shs_send_stat *st;
	const char *what;
	struct shs_msg m;
	bool admitted;
	int err;

	if (copy_from_user(&m, um, sizeof(m)))
		return -EFAULT;

	if (m.mailbox == SHS_MBOX_MP1)
		mb = &shs_mp1;
	else if (m.mailbox == SHS_MBOX_RSMU)
		mb = &shs_rsmu;
	else
		return -EINVAL;
	if (m.msg > 0xff)
		return -EINVAL;

	err = shs_gate(file, &m, &what);
	if (err)
		return err;

	st = &shs_stats[m.mailbox * 256 + m.msg];
	mutex_lock(&shs_send_lock);
	admitted = shs_take_token(&shs_bucket[m.mailbox]);
	if (admitted) {
		st->admitted++;
		memcpy(st->args, m.args, sizeof(st->args));
		st->pid = task_tgid_nr(current);
		st->time_ns = ktime_get_ns();
	} else {
		st->refused++;
	}
	mutex_unlock(&shs_send_lock);

	if (!admitted) {
		pr_notice_ratelimited("%s: send budget exhausted, refusing 0x%02x from %s (pid %d)\n",
				      mb->name, m.msg, current->comm, task_tgid_nr(current));
		return -EAGAIN;
	}

	/* Every state-changing send is logged; the budget, not a log ratelimit, bounds volume. */
	if (what) {
		pr_notice("%s: %s message 0x%02x args %08x %08x %08x %08x %08x %08x by %s (pid %d), tainting CPU_OUT_OF_SPEC\n",
			  mb->name, what, m.msg, m.args[0], m.args[1], m.args[2], m.args[3],
			  m.args[4], m.args[5], current->comm, task_tgid_nr(current));
		add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
	}

	err = shs_send(mb, m.msg, m.args, &m.rsp);
	if (err)
		return err;

	return copy_to_user(um, &m, sizeof(m)) ? -EFAULT : 0;
}

static long shs_ioctl_table(struct shs_table __user *ut)
{
	struct shs_table t;
	void *buf;
	int err;

	if (copy_from_user(&t, ut, sizeof(t)))
		return -EFAULT;

	if (t.size < shs.size) {
		t.size = shs.size;
		return copy_to_user(ut, &t, sizeof(t)) ? -EFAULT : -ENOSPC;
	}

	/* Copied out of the lock so a faulting user buffer cannot stall refreshes. */
	buf = kmalloc(shs.size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&shs.lock);
	err = shs_refresh();
	if (!err) {
		memcpy(buf, shs.snap, shs.size);
		t.time_ns = ktime_to_ns(shs.snap_time);
	}
	mutex_unlock(&shs.lock);

	t.size = shs.size;
	t.version = shs.version;
	if (!err && (copy_to_user(u64_to_user_ptr(t.buf), buf, shs.size) ||
		     copy_to_user(ut, &t, sizeof(t))))
		err = -EFAULT;

	kfree(buf);
	return err;
}

static long shs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SHS_IOC_MSG:
		return shs_ioctl_msg(file, uarg);
	case SHS_IOC_TABLE:
		return shs_ioctl_table(uarg);
	default:
		return -ENOTTY;
	}
}

static int shs_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	return 0;
}

static const struct file_operations shs_fops = {
	.owner = THIS_MODULE,
	.open = shs_open,
	.unlocked_ioctl = shs_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice shs_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = KBUILD_MODNAME,
	.fops = &shs_fops,
	.groups = shs_groups,
	.mode = 0600,
};

static void shs_release_table(void)
{
	kfree(shs.snap);
	shs.snap = NULL;
	if (shs.table)
		memunmap(shs.table);
	shs.table = NULL;
}

static int __init shs_init(void)
{
	u32 size;
	int err;

	mutex_init(&shs.lock);

	err = shs_check_identity();
	if (!err)
		err = shs_check_contract(&size);
	if (err)
		return err;

	shs.size = size;
	err = shs_map_table(size);
	if (err)
		return err;

	shs.snap = kzalloc(size, GFP_KERNEL);
	err = shs.snap ? 0 : -ENOMEM;
	if (!err) {
		/* The first transfer has no previous one to bound its window; the second does. */
		mutex_lock(&shs.lock);
		err = shs_refresh();
		if (!err) {
			msleep(SHS_REFRESH_MS);
			err = shs_refresh();
		}
		if (!err && !shs.canary_checked) {
			pr_err("tctl check could not run at load\n");
			err = -EIO;
		}
		mutex_unlock(&shs.lock);
	}
	if (err)
		goto fail;

	bin_attr_pm_table.size = size;
	err = misc_register(&shs_misc);
	if (err)
		goto fail;

	return 0;

fail:
	shs_release_table();
	return err;
}

static void __exit shs_exit(void)
{
	misc_deregister(&shs_misc);
	shs_release_table();
}

module_init(shs_init);
module_exit(shs_exit);

MODULE_DESCRIPTION("SMU mailbox transport for AMD Strix Halo (ASUS GZ302EA)");
MODULE_LICENSE("Dual MIT/GPL");
