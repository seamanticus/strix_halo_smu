# strix_halo_smu

SMU mailbox transport for AMD Strix Halo on the ASUS ROG Flow Z13 GZ302EA, as an out-of-tree Linux module. It is a serialized, gated path to the MP1 and RSMU mailboxes plus the PM table. Policy (undervolt values, power limits, re-apply on resume) belongs in userspace.

## Supported hardware

The module binds only when every check passes, and refuses to load otherwise:

- CPU: AMD family 0x1A model 0x70; root complex 1022:1507 at 0000:00:00.0
- DMI: vendor `ASUSTeK COMPUTER INC.`, board `GZ302EA`
- ACPI: `\_SB.AM00` and `\_SB.ALIB` present
- Firmware contract, verified on hardware:

| BIOS interface (MP1 0x03) | PM table version (RSMU 0x06) | Table size | ALIB SSDT `AmdTable` revision |
|---|---|---|---|
| 25 | 0x0064020C | 0x1028 | 2 |

Verified on BIOS GZ302EA.311, PMFW 10.100.2.0. A new BIOS needs a new verified tuple.

## Build and load

```sh
make                      # needs headers for the running kernel (CONFIG_AMD_NODE=y)
sudo insmod strix_halo_smu.ko
sudo dmesg | grep strix_halo_smu
```

Do not load it alongside ryzen_smu or any other tool that drives the same mailboxes.

## Interface

`/dev/strix_halo_smu` (mode 0600; opening needs `CAP_SYS_RAWIO`). The definitions are in `strix_halo_smu.h`.

- `SHS_IOC_MSG`, `struct shs_msg {mailbox, msg, args[6], rsp}`: one complete transaction. It returns 0 when the SMU answered; `rsp` holds the raw code (0x01 OK, 0xFC busy, 0xFD prerequisite, 0xFE unknown, 0xFF failed).
- `SHS_IOC_TABLE`, `struct shs_table {buf, size, version, time_ns}`: copies the PM table snapshot together with its `CLOCK_MONOTONIC` completion time. A buffer smaller than the table returns `ENOSPC` with `size` set; the table is 0x1028 bytes on the verified contract.
- `/sys/class/misc/strix_halo_smu/pm_table`: the same snapshot, read-only.
- `/sys/class/misc/strix_halo_smu/send_stats` (root only): one `struct shs_send_stat` per (mailbox, msg), at index `mailbox * 256 + msg`.

### PM table

- RSMU 0x65 refreshes the table at most once every 55 ms. Reads inside that window get the same snapshot, or the error from the last refresh.
- Values are averages over the time since the previous refresh, not instantaneous. Poll steadily (about 10 Hz) when measuring.
- Canary: the table's Tctl (offset 0xB38) is compared with the SMN Tctl sensor (0x59800) after every refresh that comes at most 110 ms after the previous one. A difference above 3 C disables the table (`EIO`) until reload.

### Message gate

Every message ID is looked up in the catalogue, `shs_catalogue[]` in `strix_halo_smu.c`:

| Class | Examples | Requirement |
|---|---|---|
| read | MP1 0x01-0x03, RSMU 0x06, 0x66 | none |
| set | MP1 0x14-0x18, 0x23 (power limits), 0x4B/0x4C (Curve Optimizer) | fd opened for writing; not locked down (`LOCKDOWN_MSR`) |
| safeguard | MP1 0x05/0x07, 0x19-0x1F, 0x33, 0x34, 0x4A, 0x63; RSMU 0x17-0x1C, 0x37-0x3E, 0x89, 0x92 | as set, plus `allow_safeguard_writes=1` at load |
| table | RSMU 0x65 | only through the rate-limited refresh (`EPERM`) |
| deny | MP1 0x25/0x26 (the BIOS passes a GPU BAR address through them) | never forwarded (`EPERM`) |
| uncatalogued | anything else | `allow_unlisted=1` at load; otherwise `ENOMSG` |

- Each state-changing send is logged in full (all six args, caller, pid) and taints the kernel `CPU_OUT_OF_SPEC`.
- Both gate parameters are load-time only (0400) and closed by default.
- Send budget: each mailbox admits a burst of 16 messages, refilled at one per second, across all callers and classes. Calls beyond that fail with `EAGAIN` before reaching the SMU. The budget bounds how long userspace can hold `\_SB.AM00` against the BIOS, and how fast the log grows.

### Concurrency with the BIOS

ALIB drives the MP1 mailbox asynchronously, on EC events, AC changes and profile changes. Every MP1 transaction holds the AML mutex `\_SB.AM00` that ALIB itself uses. The response timeout is 50 ms. On a timeout the module drains the message for up to 100 ms while still holding the lock, then refuses all further sends (`EIO`) until reload.

## Verified messages

| Message | Encoding | Verified |
|---|---|---|
| MP1 0x01 / 0x02 / 0x03 | test (arg + 1) / PMFW version / interface version | phase 2 |
| RSMU 0x06 / 0x66 / 0x65 | table version+size / table base (lo, hi) / transfer (arg 3) | phase 3 |
| MP1 0x4C CO all-core | 20-bit two's complement steps (-5 = 0xFFFFB); 0 restores the default | phase 6: -5 gave -6.8 mV at +32 MHz on one core; 0 returned to baseline within 0.5 mV |

Other set and safeguard entries are catalogued from BIOS and tool evidence but have not been exercised on hardware.

## Userspace

`user/` holds `libshs` (`shs.h`, `libshs.c`) and the `shsctl` command line client. Build with `make -C user`.

- The library is a thin layer: one call per transaction, the table snapshot, named fields of table 0x0064020C (they return NAN for any other version), and `shs_co_all()`, an encoder for the only set-class message verified on hardware. It holds no policy.
- `shsctl info`: PMFW and interface version, table version and size.
- `shsctl table [--raw]`: limits, Tctl and the per-core clock, voltage, power and temperature; `--raw` writes the binary table to stdout.
- `shsctl msg mp1|rsmu ID [ARG...]`: a raw transaction. The module's gate decides what is allowed; exit code 3 means the SMU answered something other than OK.
- `shsctl co-all STEPS`: CO all-core; 0 restores the default.
- `shsctl stats`: the non-empty `send_stats` records.

## Tests

Run from this directory with the module loaded with default parameters:

- `tests/test_phase5.py`: interface and gate. Sends read-class messages only; everything else is refused before reaching the SMU.
- `tests/test_m2_window.py`: table averaging and the canary window. Reads only.
- `tests/test_phase6.py [--control]`: CO -5 then 0, under 1-core and all-core load, each load starting at matched Tctl. Run it on AC and at idle; it aborts without sending if idle Tctl is above 45 C or 1-core boost is below 4.5 GHz. `--control` sends nothing. Without it, the kernel stays tainted until reboot.

## License

MIT (see `LICENSE`), except the kernel module source, `strix_halo_smu.c`, which is dual `GPL-2.0 OR MIT`. The module calls kernel functions exported to GPL-compatible modules only (`amd_smn_read/write`, `dmi_match`, `region_intersects`), and the kernel accepts MIT only as `Dual MIT/GPL`. It may still be used under MIT.
