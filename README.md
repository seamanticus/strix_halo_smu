# strix_halo_smu

**A safe, verified channel to the power-management firmware of one laptop: the ASUS ROG Flow Z13 (2025), GZ302EA, with AMD's Ryzen AI MAX+ 395 "Strix Halo".**

Read live power, limits, temperatures, clocks and voltages. Undervolt your cores. Cap package power. Do it through a kernel module that knows exactly which machine it is on, knows what every message it forwards does, and never collides with the BIOS.

```sh
$ sudo shsctl co-all -10
co-all: sent mp1 0x4c arg0 0x000ffff6, SMU answered 0x01 (OK)
$ sudo shsctl watch
   time   power   limit       Tctl   skin/limit    max MHz  mean mV
    1.0    48.1    48.3 stt     81.9   45.9/46.0      4108     1117
```

## Why it exists

Tuning AMD laptops on Linux usually goes through `ryzen_smu`, a generic driver for some 25 chip families. On this machine it has three problems:

1. **It races the BIOS.** This laptop's firmware talks to the same SMU mailbox from ACPI code, on its own schedule (profile changes, AC plug, EC events), with no idle check and no timeout. `ryzen_smu` does not take the firmware's lock, so a message can be overwritten mid-flight, or the BIOS can spin forever and hang the machine.
2. **It truncates the telemetry table** to 3664 bytes. The firmware reports 4136.
3. **It forwards any message number** to the firmware, including ones that disable thermal protection or hand it a memory address.

`strix_halo_smu` takes the Unix route: **do one thing, for one machine, and do it well.** It supports exactly one model and one verified firmware contract, and refuses to load anywhere else. In exchange, every behaviour was read from this machine's own BIOS or measured on it. It is a transport, not a tuner: it decides no values and holds no policy. Values and when to reapply them belong to the program above it.

## Features

- **Identity guard.** Loads only on AMD family 0x1A model 0x70, DMI board `GZ302EA`, root complex `1022:1507`, with the BIOS lock and ALIB present.
- **Firmware contract.** Loads only if the SMU interface version, PM table version and size, and ALIB revision match a verified set.
- **BIOS-safe MP1 access.** Every MP1 transaction holds the firmware's own AML mutex `\_SB.AM00`, with 50 ms bounds, and refuses to send into a busy mailbox.
- **Message catalogue.** Every message number is classed read, set, safeguard, table or deny, from BIOS evidence and hardware tests. Unknown numbers are refused.
- **Two independent load-time gates**, closed by default: `allow_safeguard_writes` (thermal, current and protection limits) and `allow_unlisted` (uncatalogued numbers). They cannot be changed while the module runs.
- **Full PM table** (0x1028 bytes), size taken from the firmware, refreshed at most every 55 ms, shared by all readers, with a `CLOCK_MONOTONIC` timestamp.
- **Temperature canary.** After each refresh, table Tctl is checked against the hardware sensor `k10temp` reads. A mismatch over 3 C disables the table instead of serving misread data.
- **Fault containment.** On an SMU timeout, the module drains under the lock, then refuses all further sends until reloaded.
- **Send budget.** 16 messages per burst, then 1 per second, per mailbox, so no program can starve the BIOS.
- **Audit trail.** Every state-changing send is logged in full (all arguments, sender, pid), taints the kernel `CPU_OUT_OF_SPEC`, and is counted in a root-only `send_stats` file.
- **Lockdown aware.** State-changing messages obey `LOCKDOWN_MSR`, like the kernel's MSR driver.
- **`shsctl`**: a command-line tool with readable output, live monitoring, undervolt and power-limit commands, dry-run, and plain-English errors.
- **`libshs`**: a small C library with named, verified table fields and encoders for verified messages.
- **DKMS packaging** for automatic rebuilds on kernel updates.

## Technical insights

Measured on this machine, and detailed in [the manual](docs/README.md):

- **Sustained power is governed by Skin Temperature Tracking**, not STAPM. Table word `0x000` is the STT power cap: 71 W with skin headroom, settling at 48 W as the skin estimate reaches its 46 C limit. [More](docs/07-findings.md#72-what-limits-sustained-power-stt)
- **STAPM (MP1 0x14) is accepted and ignored** on this firmware, across profiles and without `amd_pmf`. [More](docs/07-findings.md#73-stapm-is-accepted-but-has-no-effect)
- **`amd_pmf` overwrites fast and slow limits** on every power-profile change. [More](docs/07-findings.md#74-amd_pmf-owns-the-fast-and-slow-limits)
- **Per-core Curve Optimizer needs the CCD-split encoding** `(core / 8) << 28 | (core % 8) << 20 | steps`. The flat `core << 20` used by G-Helper and UXTU is refused for cores 8-15. [More](docs/07-findings.md#75-curve-optimizer)
- **The PM table averages** over the time since the previous read. Poll steadily when measuring. [More](docs/05-pm-table.md#52-timing-read-this-before-measuring-anything)
- **The BIOS's `ppt_pl1_spl` sets the STT power limit** (MP1 0x4A), not STAPM. [More](docs/07-findings.md#77-bios-power-and-thermal-values-per-profile)
- **RyzenAdj's Tctl offset (0x5C) is a filtered value** lagging up to 14 C; real Tctl is at `0xB38`. [More](docs/05-pm-table.md#53-offset-map)

## Supported hardware

| Component | Value |
|---|---|
| Machine | ASUS ROG Flow Z13 (2025), board `GZ302EA` |
| CPU | AMD Ryzen AI MAX+ 395, family 0x1A model 0x70 |
| BIOS interface (MP1 0x03) | 25 |
| PM table (RSMU 0x06) | version `0x0064020C`, size `0x1028` |
| ALIB SSDT `AmdTable` revision | 2 |

Verified on BIOS GZ302EA.311 (PMFW 10.100.2.0). A BIOS that changes any contract value makes the module refuse to load until it is verified. See [open questions](docs/08-unknowns.md#85-new-bios-versions) for what to report.

## Installation

You need the headers for your running kernel, a compiler, and `make`. For the permanent install you also need `dkms`. Full details, Secure Boot notes and troubleshooting: [Installation](docs/03-install.md).

### Try it (lost on reboot)

```sh
git clone https://github.com/seamanticus/strix_halo_smu
cd strix_halo_smu
make && make -C user
sudo insmod strix_halo_smu.ko
sudo user/shsctl info
```

> **Nothing persists with `insmod`.** After a reboot the module is not loaded, and every value you set is gone. Unload with `sudo rmmod strix_halo_smu`. Unloading does not undo values already sent; reboot or send the default (for example `shsctl co-all 0`).

### Install for good (DKMS, loads at boot)

```sh
sudo mkdir -p /usr/src/strix_halo_smu-0.1.0
sudo cp Makefile dkms.conf strix_halo_smu.c strix_halo_smu.h /usr/src/strix_halo_smu-0.1.0/
sudo dkms install strix_halo_smu/0.1.0
echo strix_halo_smu | sudo tee /etc/modules-load.d/strix_halo_smu.conf
sudo modprobe strix_halo_smu
sudo install -m 0755 user/shsctl /usr/local/bin/shsctl
```

### Remove

```sh
sudo modprobe -r strix_halo_smu
sudo dkms remove strix_halo_smu/0.1.0 --all
sudo rm -r /usr/src/strix_halo_smu-0.1.0
sudo rm -f /etc/modules-load.d/strix_halo_smu.conf /etc/modprobe.d/strix_halo_smu.conf /usr/local/bin/shsctl
```

Do not run it alongside `ryzen_smu`.

## Usage

```sh
sudo shsctl info                 # firmware, gates, taint
sudo shsctl table                # labelled snapshot: power, limits, temperatures, per-core
sudo shsctl watch                # live, one line per second
sudo shsctl co-all -10           # undervolt all cores by 10 steps; 0 = factory
sudo shsctl co-core 3 -15        # one core
sudo shsctl fast-limit 45        # burst power ceiling, W
sudo shsctl slow-limit 30        # sustained power ceiling, W
sudo shsctl -n co-all -10        # dry run: show what would be sent
shsctl help co-all               # details and safe-use notes per command
```

Everything is lost on reboot. Curve Optimizer is also lost on suspend, and power limits on a power-profile change. Start undervolting at -5 and go down in steps of 5, testing each.

- **New to this?** Read the [shsctl user guide](docs/shsctl.md).
- **Writing a client?** See the [programming interface](docs/04-interface.md) and the [message glossary](docs/06-glossary.md).

## Documentation

The [manual](docs/README.md) covers concepts from scratch, the safety model, the interface, the PM table layout, every catalogued message with its risks and sources, all measurements, open questions, the test suite, and sources.

## Credits

This module stands on the work of the projects that mapped AMD's SMU in the open: [RyzenAdj](https://github.com/FlyGoat/RyzenAdj), [ryzen_smu](https://github.com/amkillam/ryzen_smu) and the [CkNoSFeRaTU fork](https://github.com/CkNoSFeRaTU/ryzen_smu), [Universal x86 Tuning Utility](https://github.com/JamesCJ60/Universal-x86-Tuning-Utility), [ZenStates-Core](https://github.com/irusanov/ZenStates-Core), [G-Helper](https://github.com/seerge/g-helper), [ZenMaster](https://github.com/HorizonUnix/ZenMaster), [PawnIO](https://github.com/namazso/PawnIO.Modules) and [z13ctl](https://github.com/dahui/z13ctl). Thanks also to the GZ302EA owner who posted a full table dump in [RyzenAdj #363](https://github.com/FlyGoat/RyzenAdj/issues/363), and to AMD for the SMU headers published in the Linux kernel. What was taken from each, and what held up, is in [Sources](docs/10-sources.md).

## Disclosure

- **Not affiliated with, endorsed by, or supported by AMD or ASUS.** Names are used only to identify the hardware.
- **Use at your own risk.** Undervolting and changing power limits run hardware outside its factory settings. A too-negative undervolt crashes the machine and can corrupt unsaved data. The safeguard gate exists because some messages can cause lasting damage if misused. No warranty of any kind, as stated in the licences.
- **The kernel is tainted** (`O` for the out-of-tree module, `S` after the first state-changing message) until reboot. Mention it in any kernel bug report.
- **SMU messages and table offsets are undocumented by AMD.** Everything here was reverse-engineered or measured. "Verified" means measured on one GZ302EA, BIOS 311.

## License

MIT (see [`LICENSE`](LICENSE)), except the kernel module source, `strix_halo_smu.c`, which is dual `GPL-2.0 OR MIT`. The module calls kernel functions exported to GPL-compatible modules only (`amd_smn_read/write`, `dmi_match`, `region_intersects`), and the kernel accepts MIT only as `Dual MIT/GPL`. It may still be used under MIT.
