< Previous: [Installation](03-install.md) | [Manual](README.md) | Next: [PM table](05-pm-table.md) >

---

# 4. Programming interface

This chapter is for people writing a program that uses the module. Everything here is defined in two headers:

- [`strix_halo_smu.h`](../strix_halo_smu.h): the kernel interface (structures, ioctl numbers, message numbers). Works from any language that can call `ioctl`.
- [`user/shs.h`](../user/shs.h): `libshs`, a small C library on top of it, with named table fields and encoders for verified messages.

## 4.1 What the module creates

| Path | Mode | What it is |
|---|---|---|
| `/dev/strix_halo_smu` | `0600` root, needs `CAP_SYS_RAWIO` | The device. All transactions go through `ioctl` on it. |
| `/sys/class/misc/strix_halo_smu/pm_table` | `0444` | The PM table snapshot as a binary file, `0x1028` bytes. |
| `/sys/class/misc/strix_halo_smu/send_stats` | `0400` root | Per-message send counters (4.6). |
| `/sys/module/strix_halo_smu/parameters/allow_safeguard_writes` | `0400` | `Y` or `N` ([2.11](02-safety.md#211-load-parameters)). |
| `/sys/module/strix_halo_smu/parameters/allow_unlisted` | `0400` | `Y` or `N`. |

An **ioctl** ("input/output control") is a system call that sends a command and a pointer to a structure to a device file. The kernel module reads the structure, acts, and writes results back into it. It is how most drivers offer operations that are not plain reads and writes.

## 4.2 Opening the device

```c
int fd = open("/dev/strix_halo_smu", O_RDWR | O_CLOEXEC);   /* O_RDONLY for read-only use */
```

Open it read-only if you only read the table or send read-class messages. The module refuses state-changing messages on a read-only file descriptor (`EBADF`), so a monitoring program cannot change anything even by mistake.

## 4.3 `SHS_IOC_MSG`: one transaction

```c
struct shs_msg {
	__u32 mailbox;              /* SHS_MBOX_MP1 (0) or SHS_MBOX_RSMU (1) */
	__u32 msg;                  /* message number, 0x00-0xFF */
	__u32 args[6];              /* in: arguments; out: results */
	__u32 rsp;                  /* out: SMU response code */
};
#define SHS_IOC_MSG  _IOWR(0xB9, 0x00, struct shs_msg)
```

The call returns 0 when the SMU answered. **Always check `rsp` too**: `0x01` is OK, anything else is the SMU's refusal ([1.3](01-concepts.md#13-mailboxes)). It returns -1 with `errno` set when the module refused or the transaction failed (4.5).

Example: read the firmware version.

```c
struct shs_msg m = { .mailbox = SHS_MBOX_MP1, .msg = SHS_MP1_GET_PMFW_VERSION };

if (ioctl(fd, SHS_IOC_MSG, &m) == 0 && m.rsp == SHS_RSP_OK)
	printf("PMFW %u.%u.%u.%u\n", m.args[0] >> 24, (m.args[0] >> 16) & 0xff,
	       (m.args[0] >> 8) & 0xff, m.args[0] & 0xff);
```

The order of checks inside the module is: mailbox and message number valid, then catalogue class ([2.6](02-safety.md#26-every-message-the-catalogue)), then writable fd, then lockdown, then the send budget ([2.7](02-safety.md#27-every-message-the-send-budget)), then the log and taint, then the transaction. A refusal at any step means nothing reached the hardware.

The module does **not** check argument values. The SMU does that, and answers `0xFF` for values it rejects.

## 4.4 `SHS_IOC_TABLE`: the PM table with its timestamp

```c
struct shs_table {
	__u64 buf;                  /* user pointer to a buffer */
	__u32 size;                 /* in: buffer size; out: table size */
	__u32 version;              /* out: table version, 0x0064020C */
	__u64 time_ns;              /* out: CLOCK_MONOTONIC when the snapshot completed */
};
#define SHS_IOC_TABLE  _IOWR(0xB9, 0x01, struct shs_table)
```

It copies the current snapshot (refreshing it if the last one is 55 ms old or older) and tells you when the snapshot was taken, on the same clock as `clock_gettime(CLOCK_MONOTONIC)`. Use the timestamp to tell a new snapshot from a repeated one, and to compute the averaging window ([5.2](05-pm-table.md#52-timing-read-this-before-measuring-anything)).

If `size` is smaller than the table, the call fails with `ENOSPC` and sets `size` to the size needed.

```c
uint8_t data[0x2000];
struct shs_table t = { .buf = (uintptr_t)data, .size = sizeof(data) };

if (ioctl(fd, SHS_IOC_TABLE, &t) == 0) {
	float tctl;
	memcpy(&tctl, data + 0xB38, 4);
	printf("Tctl %.1f C at %llu ns\n", tctl, (unsigned long long)t.time_ns);
}
```

The sysfs file `pm_table` gives the same bytes without a timestamp, to anyone. It is convenient from a shell or script:

```sh
python3 -c 'import struct; d=open("/sys/class/misc/strix_halo_smu/pm_table","rb").read(); print(struct.unpack_from("<f", d, 0xB38)[0])'
```

Read it in one `read()` of the full size. A read at offset 0 refreshes; reads at later offsets come from the same snapshot.

## 4.5 Error codes

| `errno` | From | Meaning |
|---|---|---|
| `EPERM` | open | Caller lacks `CAP_SYS_RAWIO` (not root). |
| `EACCES` | open | File mode: not root. |
| `EINVAL` | `SHS_IOC_MSG` | Mailbox not 0 or 1, or message number above `0xFF`. |
| `ENOMSG` | `SHS_IOC_MSG` | Message not in the catalogue, and `allow_unlisted` is off. |
| `EPERM` | `SHS_IOC_MSG` | Safeguard class with `allow_safeguard_writes` off; or class table/deny; or lockdown forbids it. |
| `EBADF` | `SHS_IOC_MSG` | State-changing message on a read-only file descriptor. |
| `EAGAIN` | `SHS_IOC_MSG` | Send budget used up. Nothing was sent. Retry later. |
| `EBUSY` | `SHS_IOC_MSG`, table | The BIOS held `\_SB.AM00` for more than 50 ms, or the mailbox was busy outside the module's locks. Nothing was sent. Retry. |
| `ETIMEDOUT` | `SHS_IOC_MSG`, table | The SMU did not answer within 50 ms. The module is now wedged. |
| `EIO` | any | Wedged after a timeout, or the table temperature check failed, or an RSMU table transfer was refused. Reload the module. |
| `ENOSPC` | `SHS_IOC_TABLE` | Buffer too small; `size` holds the needed size. |
| `EFAULT` | any | Bad user pointer. |
| `ENOTTY` | any | Unknown ioctl number. |

## 4.6 `send_stats`

`/sys/class/misc/strix_halo_smu/send_stats` holds 512 records of `struct shs_send_stat`, one per (mailbox, message) pair, at index `mailbox * 256 + msg`:

```c
struct shs_send_stat {
	__u32 admitted;             /* passed every check and went to the SMU */
	__u32 refused;              /* refused by the send budget */
	__u32 args[6];              /* arguments of the last admitted send */
	__s32 pid;                  /* process of the last admitted sender */
	__u32 pad;
	__u64 time_ns;              /* CLOCK_MONOTONIC of the last admitted send */
};
```

It counts messages sent through `SHS_IOC_MSG` only, not the module's own table refreshes. `shsctl stats` prints the non-empty records.

## 4.7 libshs

`libshs` wraps the ioctls and adds named accessors for table fields that were verified on this firmware. It returns 0 or a negative `errno`.

| Function | What it does |
|---|---|
| `shs_open(writable)` / `shs_close(fd)` | Open read-only (0) or read-write (1). |
| `shs_msg(fd, mailbox, msg, args, &rsp)` | One transaction. |
| `shs_pmfw_version`, `shs_interface_version` | Read-class queries; `-EPROTO` if the SMU did not answer OK. |
| `shs_read_table(fd, &snapshot)` | Table, size, version and timestamp in a `struct shs_snapshot`. |
| `shs_stt_cap_w`, `shs_fast_limit_w`, `shs_slow_limit_w`, `shs_fast_power_w` | Power caps and package power, in W. |
| `shs_skin_limit_c`, `shs_skin_temp_c`, `shs_tctl_c` | Temperatures, in C. |
| `shs_core_clk_mhz`, `shs_core_volt_mv`, `shs_core_power_w`, `shs_core_temp_c` | Per-core values, core 0-15. |
| `shs_table_f32(snapshot, offset)` | Any 32-bit float in the table. |
| `shs_set_fast_limit`, `shs_set_slow_limit` | MP1 `0x15` / `0x16`, in mW. |
| `shs_co_all(fd, steps, &rsp)` | MP1 `0x4C`, all-core Curve Optimizer. |
| `shs_co_core(fd, core, steps, &rsp)` | MP1 `0x4B`, one core, with the verified CCD-split encoding. |
| `shs_co_encode`, `shs_co_core_encode` | The raw argument values, for logging or dry runs. |

Field accessors return `NAN` if the table version is not `0x0064020C`, so a program never silently reads a different layout.

Build: `make -C user` builds `libshs.so` and `shsctl`. Link with `-lshs -lm`.

## 4.8 Writing a client: rules of thumb

- **Do not poll faster than 10 Hz.** Faster reads get the same snapshot anyway.
- **Read the table steadily when measuring.** Values average since the last refresh by anyone ([5.2](05-pm-table.md#52-timing-read-this-before-measuring-anything)).
- **Reapply after resume.** Curve Optimizer is lost on suspend. Listen for systemd-logind's `PrepareForSleep(false)` signal and send it again.
- **Reapply after a profile change**, if you set power limits. `amd_pmf` overwrites fast and slow limits every time the platform profile changes ([7.4](07-findings.md#74-amd_pmf-owns-the-fast-and-slow-limits)).
- **Do not send STAPM (`0x14`) expecting an effect**. It is accepted and ignored on this firmware ([7.3](07-findings.md#73-stapm-is-accepted-but-has-no-effect)).
- **Use `shs_co_core_encode`**, or its formula, for per-core CO. The formula other tools use fails for cores 8-15 ([7.5](07-findings.md#75-curve-optimizer)).
- **Keep messages per burst under 16**, or handle `EAGAIN`.
- **Check `rsp`**, not only the return value.

---

< Previous: [Installation](03-install.md) | [Manual](README.md) | Next: [PM table](05-pm-table.md) >
