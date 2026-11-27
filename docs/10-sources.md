< Previous: [Test suite](09-tests.md) | [Manual](README.md) | Next: [shsctl user guide](shsctl.md) >

---

# 10. Sources

Every outside source this project used, what was taken from it, and whether it held up when measured on a GZ302EA. Nothing here was copied into the module; the module is original code. These sources gave the facts that were then checked.

## 10.1 The machine itself

| Source | What it gave | Held up? |
|---|---|---|
| GZ302EA BIOS 311 ACPI tables (DSDT, SSDT1 `AmdTable`, SSDT4, SSDT11, SSDT20), decompiled with `iasl -d` | The MP1 send routine `A018`, the `\_SB.AM00` lock, the DPTC-to-MP1 map `A032`, every event that sends MP1 messages, the STT values per profile, `ppt_pl1_spl` routing | Primary source; the module's lock and catalogue are built on it |
| Kernel interfaces: `k10temp`, amdgpu `pp_dpm_*` and `gpu_metrics`, `/proc/iomem` | Independent readings to check table labels and the table address | Used as oracles |

## 10.2 AMD

| Source | What it gave | Held up? |
|---|---|---|
| `smu_v14_0_0_ppsmc.h`, `smu14_driver_if_v14_0_0.h`, `smu_v14_0_0_pmfw.h` in the Linux kernel (amdgpu) | Response codes, the version messages, the metrics structure | Yes |
| Linux `arch/x86/kernel/amd_node.c` | `amd_smn_read/write` and their lock | Yes |
| Linux `drivers/platform/x86/amd/pmf`, `amd/pmc`, `amd/hsmp` | Which mailboxes the kernel's own AMD drivers use (none overlap); `amd_pmf` sending limits per profile; the "1 ms SMU cycle" | Yes; the `amd_pmf` behaviour confirmed by measurement ([7.4](07-findings.md#74-amd_pmf-owns-the-fast-and-slow-limits)) |
| Linux `smu_cmn.c` | The 1 ms metrics refresh that ryzen_smu inherited | Origin of the 1 ms rate, which this module replaced with 55 ms |
| Linux ACPICA `utxfmutex.c` | `acpi_acquire_mutex` for taking an AML mutex from C | Yes |
| Linux `asus-wmi.c`, `asus-armoury.c` | The WMI methods behind `ppt_pl1_spl` and the throttle profile | Yes, matched against the DSDT |

## 10.3 Tuning tools

| Project | What it gave | Held up? |
|---|---|---|
| [RyzenAdj](https://github.com/FlyGoat/RyzenAdj) (`api.c`, `main.c`, `nb_smu_ops.c`) | Mailbox addresses, message IDs for power limits, Tctl, VRM, STT, CO; PM table labels for Strix Halo | IDs: yes. Labels: `0x000` "STAPM limit" and `0x05C` "Tctl" are wrong for this table; "skin temperature not implemented on Strix Halo" is wrong for this BIOS; `set_stapm_time` falls through to `0x4E` |
| [ryzen_smu](https://github.com/amkillam/ryzen_smu) | The SMN window, the table protocol (`0x06`, `0x66`, `0x65`) | Protocol: yes. Truncates the table to `0xE50`; open mailbox bugs (PR [#61](https://github.com/amkillam/ryzen_smu/pull/61), [#68](https://github.com/amkillam/ryzen_smu/pull/68)); 1 ms refresh floor; no BIOS lock |
| [CkNoSFeRaTU/ryzen_smu](https://github.com/CkNoSFeRaTU/ryzen_smu) fork | Strix Halo table sizes per version (`0x1028`); the binary sysfs attribute for a table larger than 4 KiB | Yes |
| [Universal x86 Tuning Utility](https://github.com/JamesCJ60/Universal-x86-Tuning-Utility) (`RyzenSmu.cs`, FT6/FP7/FP8 table) | Mailbox addresses; most message names; the only source for many safeguard-class IDs | Addresses and shared IDs: yes. The table is Phoenix-derived: MP1 `0x31`/`0x32` named as overclocking are STT parameters on this BIOS; `0x1F` listed twice; per-core CO encoding fails for cores 8-15 |
| [ZenStates-Core](https://github.com/irusanov/ZenStates-Core) (`Strix.cs`, `Phoenix.cs`, `SMU.cs`) | The CCD-split per-core CO encoding; RSMU `0xAF` CO read-back | Encoding: **yes, the only one that works for all 16 cores**. `0xAF`: untested |
| [G-Helper](https://github.com/seerge/g-helper) and [g-helper-linux](https://github.com/utajum/g-helper-linux) | CO message IDs; PSMU `0xB7` iGPU CO | CO all-core: yes. Per-core flat encoding fails for cores 8-15. `0xB7`: contradicted by RyzenAdj, untested |
| [ZenMaster](https://github.com/HorizonUnix/ZenMaster) | The table protocol for Strix Halo | Yes, but uses the truncated `0xE50` size |
| [PawnIO RyzenSMU module](https://github.com/namazso/PawnIO.Modules) | Mailbox addresses, SMN window `0xC4/0xC8` | Addresses: yes. The module uses the kernel's own window instead |
| [z13ctl](https://github.com/dahui/z13ctl) | CO resets on suspend; a hard lock of this machine on 2026-08-14 around an MP1 write through ryzen_smu (cause not proven) | Consistent with the BIOS lock finding ([7.1](07-findings.md#71-the-bios-drives-the-mp1-mailbox)) |

## 10.4 Issues and pull requests

| Link | What it gave |
|---|---|
| [RyzenAdj #363](https://github.com/FlyGoat/RyzenAdj/issues/363) | A GZ302EA debug log (`0x06` answers `0x0064020C` / `0x1028`, the `0x66` address) and a full idle table dump, used to decode the layout before any probing |
| [RyzenAdj #333](https://github.com/FlyGoat/RyzenAdj/issues/333), [PR #334](https://github.com/FlyGoat/RyzenAdj/pull/334) | Strix Halo support in RyzenAdj |
| [RyzenAdj #387](https://github.com/FlyGoat/RyzenAdj/issues/387) | A report of all-core CO being rejected |
| [RyzenAdj #398](https://github.com/FlyGoat/RyzenAdj/issues/398) | CO -50 crashing a Strix Halo machine |
| [ryzen_smu #27](https://github.com/amkillam/ryzen_smu/issues/27), [#43](https://github.com/amkillam/ryzen_smu/issues/43) | Strix Halo initialisation problems |
| [ryzen_smu PR #50](https://github.com/amkillam/ryzen_smu/pull/50) | Table version `0x64010C`; fast/slow limits written and read back on a MAX+ 388 |
| [ZenStates-Core #30](https://github.com/irusanov/ZenStates-Core/issues/30) | CO read-back returning core 0's value for every core on Cezanne |
| [G-Helper #4243](https://github.com/seerge/g-helper/issues/4243) | Curve Optimizer on the 2025 Z13 |
| [strixhalo-homelab #2](https://github.com/deseven/strixhalo-homelab/issues/2) | EC behaviour on Strix Halo machines |

<a id="project-research"></a>
## 10.5 This project's research

The decisions behind the module were made one at a time, each from evidence collected before writing code. The working notes (decision records, raw test outputs, decompiled ACPI tables and the reference copies of the sources above) are kept outside this repository. Their conclusions are in this manual:

| Topic | Chapter |
|---|---|
| Why a new module instead of fixing ryzen_smu | [README](../README.md) |
| The firmware lock and the BIOS's MP1 traffic | [2.4](02-safety.md#24-every-mp1-transaction-hold-the-bios-lock), [7.1](07-findings.md#71-the-bios-drives-the-mp1-mailbox) |
| Which messages to forward, and the class rules | [2.6](02-safety.md#26-every-message-the-catalogue), [6](06-glossary.md), [8](08-unknowns.md) |
| What to do on unverified firmware (the contract) | [2.1](02-safety.md#21-at-load-is-this-the-verified-machine) |
| How thermal control works on this machine | [7.2](07-findings.md#72-what-limits-sustained-power-stt), [7.7](07-findings.md#77-bios-power-and-thermal-values-per-profile) |
| Refresh interval, timeouts, send budget | [2.5](02-safety.md#25-when-the-smu-does-not-answer)-[2.8](02-safety.md#28-the-table-refresh-interval), [7.9](07-findings.md#79-mailbox-timing) |

---

< Previous: [Test suite](09-tests.md) | [Manual](README.md) | Next: [shsctl user guide](shsctl.md) >
