< Previous: [PM table](05-pm-table.md) | [Manual](README.md) | Next: [Findings](07-findings.md) >

---

# 6. Message glossary

Every message the module knows. The summary table gives one line per message; each links to a card with the full description: what the message does, what it affects, the argument format, risks, pros, cons, and where the information came from.

## 6.1 How to read it

**Class** decides what the module requires before forwarding ([2.6](02-safety.md#26-every-message-the-catalogue)): **read** (nothing), **set** (writable fd, no lockdown), **safeguard** (as set, plus `allow_safeguard_writes=1`), **table** (only through the table read), **deny** (never).

**Status** says how much is known:

| Status | Meaning |
|---|---|
| **verified** | Sent on a GZ302EA by this project's tests, and its effect measured in the PM table. |
| **no effect** | Sent on a GZ302EA; the SMU accepted it, and no effect could be measured. |
| **BIOS-confirmed** | This machine's own BIOS sends it, with a meaning confirmed by AMD's DPTC parameter map, and at least one tuning tool agrees. Not sent by this project. |
| **multi-source** | Two or more independent tools agree on the meaning. Not sent by this project. |
| **single-source** | One tool lists it. Catalogued only so that it stays behind the safeguard gate; the meaning may be wrong. |

**Source keys** (full list in [chapter 10](10-sources.md)):

- **HW**: this project's measurement on a GZ302EA.
- **BIOS**: the GZ302EA BIOS 311 ACPI code (SSDT1 `A032` DPTC to MP1 map, DSDT).
- **AMD**: AMD-published headers in the Linux kernel.
- **RA**: RyzenAdj `api.c`, Strix Halo case.
- **UX**: Universal x86 Tuning Utility, FT6/FP7/FP8 socket table.
- **ZS**: ZenStates-Core.
- **GH**: G-Helper.
- **#363**: RyzenAdj issue 363, a GZ302EA debug log and table dump.

## 6.2 Summary

### MP1 mailbox

| ID | Name | Class | Status | Sources |
|---|---|---|---|---|
| [`0x01`](#mp1-0x01) | test | read | verified | HW, RA, UX |
| [`0x02`](#mp1-0x02) | get PMFW version | read | verified | HW, AMD, RA |
| [`0x03`](#mp1-0x03) | get interface version | read | verified | HW, AMD |
| [`0x05`](#mp1-0x05) | enable SMU feature | safeguard | single-source | UX |
| [`0x07`](#mp1-0x07) | disable SMU feature | safeguard | single-source | UX |
| [`0x14`](#mp1-0x14) | STAPM limit | set | **no effect** | HW, BIOS, RA, UX |
| [`0x15`](#mp1-0x15) | fast (PPT) limit | set | **verified** | HW, BIOS, RA, UX |
| [`0x16`](#mp1-0x16) | slow (PPT) limit | set | **verified** | HW, BIOS, RA, UX |
| [`0x17`](#mp1-0x17) | slow time constant | set | BIOS-confirmed | BIOS, RA, UX |
| [`0x18`](#mp1-0x18) | STAPM time constant | set | BIOS-confirmed | BIOS, RA, UX |
| [`0x19`](#mp1-0x19) | Tctl temperature limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x1A`](#mp1-0x1a) | VDD TDC current limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x1B`](#mp1-0x1b) | SoC TDC current limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x1C`](#mp1-0x1c) | VDD EDC current limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x1D`](#mp1-0x1d) | SoC EDC current limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x1E`](#mp1-0x1e) | PSI0 current threshold | safeguard | BIOS-confirmed | BIOS, UX |
| [`0x1F`](#mp1-0x1f) | PROCHOT deassertion ramp | safeguard | BIOS-confirmed | BIOS, RA |
| [`0x23`](#mp1-0x23) | APU slow limit | set | BIOS-confirmed | BIOS, RA, UX |
| [`0x25`/`0x26`](#mp1-0x25) | GPU BAR address low/high | **deny** | BIOS only | BIOS |
| [`0x33`](#mp1-0x33) | APU skin temperature limit | safeguard | BIOS-confirmed | BIOS, UX, HW (value read) |
| [`0x34`](#mp1-0x34) | dGPU skin temperature limit | safeguard | BIOS-confirmed | BIOS, UX |
| [`0x4A`](#mp1-0x4a) | STT power limit | safeguard | BIOS-confirmed | BIOS, RA, UX |
| [`0x4B`](#mp1-0x4b) | Curve Optimizer, one core | set | **verified** | HW, RA, UX, ZS, GH |
| [`0x4C`](#mp1-0x4c) | Curve Optimizer, all cores | set | **verified** | HW, RA, UX, ZS, GH |
| [`0x63`](#mp1-0x63) | cHTC temperature limit | safeguard | single-source | UX |

### RSMU mailbox

| ID | Name | Class | Status | Sources |
|---|---|---|---|---|
| [`0x06`](#rsmu-0x06) | get table version and size | read | verified | HW, #363, RA |
| [`0x17`/`0x18`](#rsmu-0x17) | enable / disable overclocking | safeguard | single-source | UX |
| [`0x19`/`0x1A`](#rsmu-0x19) | overclock clock, all / one core | safeguard | single-source | UX |
| [`0x1B`](#rsmu-0x1b) | overclock voltage | safeguard | single-source | UX |
| [`0x1C`](#rsmu-0x1c) | GFX clock force | safeguard | single-source | UX |
| [`0x37`](#rsmu-0x37) | cHTC temperature limit | safeguard | single-source | UX |
| [`0x38`-`0x3B`](#rsmu-0x38) | VRM TDC/EDC current limits (RSMU twins) | safeguard | single-source | UX |
| [`0x3C`/`0x3D`](#rsmu-0x3c) | PSI0 currents (RSMU twins) | safeguard | single-source | UX |
| [`0x3E`](#rsmu-0x3e) | PBO scalar | safeguard | single-source | UX |
| [`0x65`](#rsmu-0x65) | transfer table to memory | table | verified | HW, RA, AMD |
| [`0x66`](#rsmu-0x66) | get table address | read | verified | HW, #363, RA |
| [`0x89`](#rsmu-0x89) | GFX clock | safeguard | single-source (two tools, untested) | RA, UX |
| [`0x92`](#rsmu-0x92) | dGPU skin temperature limit (RSMU twin) | safeguard | single-source | UX |

Messages that are **not** in the catalogue, and why, are in [chapter 8](08-unknowns.md).

## 6.3 MP1 cards

<a id="mp1-0x01"></a>
### MP1 0x01: test

- **Does:** answers with its first argument plus one. Changes nothing.
- **Affects:** nothing.
- **Argument:** any number in `args[0]`; result in `args[0]`.
- **Risks:** none.
- **Pros:** proves the mailbox path works end to end (locks, SMN access, response).
- **Cons:** none.
- **Status:** verified. `0x47` returned `0x48`.
- **Sources:** HW (phase 2); RA and UX list it as the test message for this family.

<a id="mp1-0x02"></a>
### MP1 0x02: get PMFW version

- **Does:** returns the SMU firmware version.
- **Affects:** nothing.
- **Argument:** none. Result `args[0]`, one byte per field: `0x0A640200` = 10.100.2.0.
- **Risks:** none.
- **Pros:** identifies the firmware build in bug reports.
- **Cons:** not part of the contract. Builds change more often than meanings ([2.1](02-safety.md#21-at-load-is-this-the-verified-machine)).
- **Status:** verified.
- **Sources:** HW; AMD `smu_v14_0_0_ppsmc.h` (`PPSMC_MSG_GetPmfwVersion`); RA.

<a id="mp1-0x03"></a>
### MP1 0x03: get interface version

- **Does:** returns the version of the interface between the BIOS/driver and the SMU firmware.
- **Affects:** nothing.
- **Argument:** none. Result `args[0]` = 25 on the verified firmware.
- **Risks:** none.
- **Pros:** the module's firmware contract uses it. A change means AMD changed the interface.
- **Cons:** none.
- **Status:** verified.
- **Sources:** HW; AMD `smu_v14_0_0_ppsmc.h` (`PPSMC_MSG_GetDriverIfVersion`).

<a id="mp1-0x05"></a>
### MP1 0x05: enable SMU feature

- **Does:** turns on SMU features selected by a bitmask.
- **Affects:** potentially any SMU function: thermal throttling, PROCHOT, clock management. Which bit is which on this firmware is not known.
- **Argument:** a feature bitmask (layout unknown here).
- **Risks:** high. Enabling the wrong feature could change behaviour unpredictably.
- **Pros:** none for normal tuning.
- **Cons:** the bitmap is undocumented for this chip; the ID itself comes from one tool.
- **Status:** single-source.
- **Sources:** UX only. Catalogued as safeguard, flagged unverified, so that `allow_unlisted=1` alone cannot reach it.

<a id="mp1-0x07"></a>
### MP1 0x07: disable SMU feature

- **Does:** turns off SMU features selected by a bitmask.
- **Affects:** potentially any SMU protection.
- **Argument:** a feature bitmask (layout unknown here).
- **Risks:** **the most dangerous message in the catalogue.** Disabling thermal or current protection can cause lasting damage (silicon overheating with throttling off).
- **Pros:** none for normal tuning.
- **Cons:** as 0x05.
- **Status:** single-source.
- **Sources:** UX only. Safeguard, flagged unverified.

<a id="mp1-0x14"></a>
### MP1 0x14: STAPM limit

- **Does:** on older AMD laptops, sets the Skin Temperature Aware Power Management limit, a long-term average power budget.
- **Affects:** on this firmware, nothing measurable.
- **Argument:** `args[0]` in milliwatts.
- **Risks:** low. The SMU's thermal and current protections still apply.
- **Pros:** none on this firmware.
- **Cons:** accepted (answer `0x01`) and ignored. Tools that offer a "STAPM" or "sustained" setting on this machine change nothing with it.
- **Status:** **no effect.** 20 W was sent before a 300 s all-core load in four configurations: balanced and low-power profiles, with and without power-profiles-daemon, and with `amd_pmf` blacklisted. Package power never fell toward 20 W in any of them. Sustained power on this machine is governed by STT instead ([7.2](07-findings.md#72-what-limits-sustained-power-stt), [7.3](07-findings.md#73-stapm-is-accepted-but-has-no-effect)).
- **Sources:** HW (`tests/test_power_limits.py stapm`); BIOS (DPTC 0x05 maps to it); RA, UX.

<a id="mp1-0x15"></a>
### MP1 0x15: fast (PPT) limit

- **Does:** sets the fast package power tracking limit: the most power the chip may draw over a short window.
- **Affects:** package power within about one second; table `0x008` shows the new value.
- **Argument:** `args[0]` in milliwatts (45000 = 45 W).
- **Risks:** low when lowering. Raising it lets the chip draw more power in bursts; temperature and current protections still apply.
- **Pros:** an immediate, reliable ceiling on power, so the laptop runs quieter, cooler, and lasts longer on battery.
- **Cons:** `amd_pmf` writes its own value on every power-profile change, so the setting lasts until the next profile change, suspend or reboot ([7.4](07-findings.md#74-amd_pmf-owns-the-fast-and-slow-limits)).
- **Status:** **verified.** 45000 mW held package power at 45.0 W from the first second of an all-core load. Control run without the send: 71 W.
- **Sources:** HW (`tests/test_power_limits.py fast`); BIOS (DPTC 0x06); RA, UX.

<a id="mp1-0x16"></a>
### MP1 0x16: slow (PPT) limit

- **Does:** sets the slow package power tracking limit: the most power averaged over a window of seconds.
- **Affects:** package power within a few seconds; table `0x010`.
- **Argument:** `args[0]` in milliwatts.
- **Risks:** as 0x15.
- **Pros:** limits sustained power while still allowing short bursts up to the fast limit.
- **Cons:** as 0x15, reset by `amd_pmf` on profile change.
- **Status:** **verified.** 30000 mW sent 30 s into a load cut package power from 52 W to 30 W within seconds.
- **Sources:** HW (`tests/test_power_limits.py slow`); BIOS (DPTC 0x07); RA, UX; ryzen_smu PR #50 read it back on a MAX+ 388.

<a id="mp1-0x17"></a>
### MP1 0x17: slow time constant

- **Does:** sets the averaging window of the slow limit.
- **Affects:** how long the chip can stay above the slow limit before it is enforced.
- **Argument:** `args[0]`, in seconds (RA).
- **Risks:** low. A longer window means longer heat soak; still bounded by Tctl and STT.
- **Pros:** tunes how "bursty" the laptop is.
- **Cons:** not measured here; the unit is from RA, not verified.
- **Status:** BIOS-confirmed (DPTC 0x08).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x18"></a>
### MP1 0x18: STAPM time constant

- **Does:** sets the STAPM averaging window.
- **Affects:** given that STAPM has no measurable effect here (0x14), probably nothing.
- **Argument:** `args[0]`, in seconds (RA).
- **Risks:** low.
- **Pros:** none known on this firmware.
- **Cons:** RyzenAdj's Strix Halo code path for this setting has no `break`, so it also sends message `0x4E` (the Dragon Range number) after `0x18`. `0x4E` is not catalogued, so this module refuses that second message (`ENOMSG`).
- **Status:** BIOS-confirmed (DPTC 0x01).
- **Sources:** BIOS; RA (`api.c` `set_stapm_time`), UX.

<a id="mp1-0x19"></a>
### MP1 0x19: Tctl temperature limit

- **Does:** sets the control temperature at which the SMU throttles the chip.
- **Affects:** maximum chip temperature; table `0x058` (95 C by default).
- **Argument:** `args[0]` in degrees C.
- **Risks:** **raising it lets the silicon run hotter than the BIOS allows.** Lowering it is safe (cooler, slower).
- **Pros:** lowering it is the most direct way to keep the chip cooler.
- **Cons:** the BIOS sets it at boot. Silicon-level safety limit.
- **Status:** BIOS-confirmed (DPTC 0x03).
- **Sources:** BIOS; RA (notes that on Strix Halo it is the only temperature control it offers); UX.

<a id="mp1-0x1a"></a>
### MP1 0x1A: VDD TDC current limit

- **Does:** sets the thermal design current limit of the core voltage regulator (VRM): the sustained current it may deliver.
- **Affects:** sustained core current, hence power under heavy load.
- **Argument:** `args[0]` in milliamps (RA).
- **Risks:** **raising it can overheat the voltage regulator on the motherboard.** The VRM's own rating is not known.
- **Pros:** lowering it is a further, current-based power cap.
- **Cons:** the board's VRM capacity is unknown; raising it is guesswork.
- **Status:** BIOS-confirmed (DPTC 0x0B).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x1b"></a>
### MP1 0x1B: SoC TDC current limit

- **Does:** as 0x1A, for the SoC voltage rail (memory controller, fabric, I/O).
- **Affects:** sustained SoC current.
- **Argument:** `args[0]` in milliamps (RA).
- **Risks:** as 0x1A.
- **Pros:** as 0x1A.
- **Cons:** as 0x1A.
- **Status:** BIOS-confirmed (DPTC 0x0E).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x1c"></a>
### MP1 0x1C: VDD EDC current limit

- **Does:** sets the electrical design current limit of the core VRM: the peak current it may deliver.
- **Affects:** peak current, hence the height of boost spikes.
- **Argument:** `args[0]` in milliamps (RA).
- **Risks:** **raising it can exceed the VRM's peak rating.** Too low can cause instability under sudden load.
- **Pros:** lowering it caps spikes.
- **Cons:** as 0x1A.
- **Status:** BIOS-confirmed (DPTC 0x0C).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x1d"></a>
### MP1 0x1D: SoC EDC current limit

- **Does:** as 0x1C, for the SoC rail.
- **Affects:** peak SoC current.
- **Argument:** `args[0]` in milliamps (RA).
- **Risks:** as 0x1C.
- **Pros:** as 0x1C.
- **Cons:** as 0x1A.
- **Status:** BIOS-confirmed (DPTC 0x11).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x1e"></a>
### MP1 0x1E: PSI0 current threshold

- **Does:** sets the current above which the VRM runs all its phases. Below it, the VRM sheds phases to save power at light load.
- **Affects:** VRM efficiency and behaviour under load changes.
- **Argument:** `args[0]` in milliamps (UX).
- **Risks:** **set too high, the VRM runs too few phases at high load: per-phase over-current and voltage ripple.**
- **Pros:** in theory, light-load efficiency.
- **Cons:** no tuning benefit known on this machine.
- **Status:** BIOS-confirmed (DPTC 0x0D).
- **Sources:** BIOS; UX. RA does not list it.

<a id="mp1-0x1f"></a>
### MP1 0x1F: PROCHOT deassertion ramp

- **Does:** controls how quickly power is allowed back up after PROCHOT (the "processor hot" emergency signal from the EC or VRM) is released.
- **Affects:** power right after a thermal emergency.
- **Argument:** `args[0]`; RA: "higher values apply tighter limits after PROCHOT is over".
- **Risks:** a fast ramp can push a machine that just overheated straight back into it.
- **Pros:** none for normal tuning.
- **Cons:** UX lists `0x1F` twice (also as "psi0soc current"); BIOS and RA agree on PROCHOT ramp (DPTC 0x09), so that is used.
- **Status:** BIOS-confirmed.
- **Sources:** BIOS; RA. UX conflicts.

<a id="mp1-0x23"></a>
### MP1 0x23: APU slow limit

- **Does:** a slow power limit for the APU part in systems with a discrete GPU ("A+A" platforms).
- **Affects:** probably little on this machine, which has no discrete GPU. Table `0x018` reads a constant 70 W.
- **Argument:** `args[0]` in milliwatts.
- **Risks:** as 0x16.
- **Pros:** none known here.
- **Cons:** not measured.
- **Status:** BIOS-confirmed (DPTC 0x13).
- **Sources:** BIOS; RA, UX.

<a id="mp1-0x25"></a>
### MP1 0x25 / 0x26: GPU BAR address low / high

- **Does:** the BIOS passes the physical address of a GPU memory region to the SMU through these two, before a pair of messages (0x27/0x28) of unknown meaning.
- **Affects:** unknown. The SMU is given a memory address it may then access.
- **Argument:** low and high 32 bits of a physical address.
- **Risks:** **a memory primitive.** A wrong address could let the SMU read or write arbitrary memory.
- **Pros:** none.
- **Cons:** purpose unknown.
- **Status:** **deny.** Never forwarded, whatever the parameters. Catalogued only so that `allow_unlisted=1` cannot reach them.
- **Sources:** BIOS only (SSDT1 `A119`). No tool sends them.

<a id="mp1-0x33"></a>
### MP1 0x33: APU skin temperature limit

- **Does:** sets the skin temperature the STT controller aims to stay below.
- **Affects:** sustained power: the STT cap (table `0x000`) falls as the skin estimate (`0x06C`) approaches this limit (`0x068`). It is the main control of sustained power on this machine.
- **Argument:** `args[0]` = degrees C x 256 (46 C = 11776), as the BIOS sends it. RA multiplies by 256 too.
- **Risks:** **raising it lets the laptop's surface get hotter. This is the limit that protects your skin, not the silicon.** On a tablet that you hold, this matters.
- **Pros:** lowering it is the most effective way to make the machine run cooler to the touch under sustained load.
- **Cons:** the EC re-sends its own value on profile changes, AC events and EC queries, so a change is temporary.
- **Status:** BIOS-confirmed. Its effect on table `0x068` is confirmed by matching BIOS values (44/46/47 C per profile); the message itself was not sent by this project.
- **Sources:** BIOS (DSDT `AL22`, DPTC 0x22, values per profile); UX. RA says "not implemented on Strix Halo", which is wrong for this BIOS.

<a id="mp1-0x34"></a>
### MP1 0x34: dGPU skin temperature limit

- **Does:** as 0x33, for a discrete GPU's skin zone.
- **Affects:** probably nothing on this machine, which has no discrete GPU.
- **Argument:** degrees C x 256 (by analogy with 0x33).
- **Risks:** it is still a skin limit.
- **Pros:** none here.
- **Cons:** none here.
- **Status:** BIOS-confirmed (DPTC 0x23).
- **Sources:** BIOS; UX. RA: "not implemented on Strix Halo".

<a id="mp1-0x4a"></a>
### MP1 0x4A: STT power limit

- **Does:** sets the power limit used by the skin-temperature controller.
- **Affects:** sustained power through STT. It is what the asus-armoury `ppt_pl1_spl` setting changes on this BIOS (WMI method `0x001200A3` to DPTC 0x2E to MP1 0x4A).
- **Argument:** `args[0]` in milliwatts. BIOS per profile: quiet 35000, balanced 45000, performance 60000.
- **Risks:** raising it allows more heat into the chassis, bounded by the skin limit (0x33).
- **Pros:** the setting desktop tools expose as "sustained power" on this machine.
- **Cons:** the controller settles at its own equilibrium, not exactly at this value (48 W measured with 45 W set). The EC re-sends it on events.
- **Status:** BIOS-confirmed.
- **Sources:** BIOS (DSDT `AL2E`, WMNB); RA (`skin-temp-limit`); UX.

<a id="mp1-0x4b"></a>
### MP1 0x4B: Curve Optimizer, one core

- **Does:** shifts one core's voltage/frequency curve by a number of steps ([1.8](01-concepts.md#18-curve-optimizer-undervolting)).
- **Affects:** that core's voltage at each clock (table `0xBD0 + 4n`) and, when it has power or thermal headroom, its clock (`0xC50 + 4n`).
- **Argument:** `args[0] = (core / 8) << 28 | (core % 8) << 20 | (steps & 0xFFFF)`. Steps are a 16-bit two's complement number: -10 is `0xFFF6`. Core 10 at -10 is `0x1020FFF6`.
- **Risks:** too negative: crashes, or silent computation errors. Positive: more voltage than stock, more heat and wear.
- **Pros:** per-core tuning. Each core in a chip has different margin, so the best cores can go further.
- **Cons:** lost on reboot and on suspend. Needs testing core by core.
- **Status:** **verified for all 16 cores.** Core 2 at -10: -11.7 mV at +68 MHz, core 3 unchanged. Core 10 at -10: +108 MHz at -3.1 mV, core 11 unchanged. Back to 0 restored both. The "flat" encoding `core << 20` that G-Helper and UXTU use is **refused (`0xFF`) for cores 8-15**; it only works for 0-7 ([7.5](07-findings.md#75-curve-optimizer)).
- **Sources:** HW (`tests/test_co_core.py`); ZS (the CCD-split encoding); RA, UX, GH (the message ID, flat encoding).

<a id="mp1-0x4c"></a>
### MP1 0x4C: Curve Optimizer, all cores

- **Does:** shifts every core's voltage/frequency curve by the same number of steps.
- **Affects:** all core voltages and, with headroom, clocks.
- **Argument:** `args[0]` = steps as a 20-bit two's complement number: -5 is `0xFFFFB`.
- **Risks:** as 0x4B. RyzenAdj #398: -50 crashed a Strix Halo machine.
- **Pros:** the simplest undervolt. One number.
- **Cons:** limited by the weakest core. Lost on reboot and suspend.
- **Status:** **verified.** -5 under a fixed single-core load at matched starting temperature: -6.8 mV at +32 MHz; 0 restores the factory curve.
- **Sources:** HW (`tests/test_co_all.py`); RA, UX, ZS, GH.

<a id="mp1-0x63"></a>
### MP1 0x63: cHTC temperature limit

- **Does:** UX names it the cHTC ("hardware thermal control") temperature, a thermal throttle point.
- **Affects:** unknown on this firmware.
- **Argument:** degrees C (UX).
- **Risks:** a thermal limit; raising it could weaken protection.
- **Pros:** none known.
- **Cons:** one source; not sent by this BIOS.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

## 6.4 RSMU cards

<a id="rsmu-0x06"></a>
### RSMU 0x06: get table version and size

- **Does:** returns the PM table version and its size in bytes.
- **Affects:** nothing.
- **Argument:** none. Results: `args[0]` = `0x0064020C`, `args[1]` = `0x1028`.
- **Risks:** none.
- **Pros:** the size comes from the firmware, so the module never truncates the table ([5.1](05-pm-table.md#51-size-why-0x1028-and-not-0xe50)). Part of the contract.
- **Cons:** none.
- **Status:** verified.
- **Sources:** HW; #363 (same values on another GZ302EA); RA (version only).

<a id="rsmu-0x17"></a>
### RSMU 0x17 / 0x18: enable / disable overclocking

- **Does:** UX: switches the SMU's manual overclocking mode on or off.
- **Affects:** unlocks the clock and voltage overrides (0x19-0x1B).
- **Argument:** none known.
- **Risks:** **opens the path to direct voltage control.**
- **Pros:** none for this module's purpose.
- **Cons:** one source.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x19"></a>
### RSMU 0x19 / 0x1A: overclock clock, all cores / one core

- **Does:** UX: forces a fixed clock on all cores or one core.
- **Affects:** clock, with voltage following the curve.
- **Argument:** MHz (UX).
- **Risks:** fixed clocks bypass the SMU's boost management; instability and heat.
- **Pros:** none for this module's purpose.
- **Cons:** one source. UX's MP1 numbers for the same names (0x31/0x32) are STT parameters on this BIOS, which shows UX's table is wrong in places for this chip.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x1b"></a>
### RSMU 0x1B: overclock voltage

- **Does:** UX: sets a fixed core voltage.
- **Affects:** core voltage directly.
- **Argument:** a VID code (UX).
- **Risks:** **overvoltage: the most direct path to silicon damage.**
- **Pros:** none for this module's purpose.
- **Cons:** one source.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x1c"></a>
### RSMU 0x1C: GFX clock force

- **Does:** UX: forces the integrated GPU clock.
- **Affects:** GPU clock, bypassing its power management.
- **Argument:** MHz (UX).
- **Risks:** damage unlikely; instability likely.
- **Pros:** none known.
- **Cons:** one source.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x37"></a>
### RSMU 0x37: cHTC temperature limit

- **Does:** RSMU twin of MP1 0x63.
- **Affects / risks / pros / cons:** as [MP1 0x63](#mp1-0x63).
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x38"></a>
### RSMU 0x38 / 0x39 / 0x3A / 0x3B: VRM current limits (RSMU twins)

- **Does:** UX lists these as RSMU versions of VDD TDC, SoC TDC, VDD EDC and SoC EDC (MP1 0x1A-0x1D).
- **Affects / risks / pros / cons:** as [MP1 0x1A](#mp1-0x1a) to [0x1D](#mp1-0x1d).
- **Status:** single-source. Catalogued as safeguard so that `allow_unlisted=1` cannot be used to bypass the safeguard gate on the MP1 versions.
- **Sources:** UX.

<a id="rsmu-0x3c"></a>
### RSMU 0x3C / 0x3D: PSI0 currents (RSMU twins)

- **Does:** UX: RSMU versions of the PSI0 thresholds for VDD and SoC.
- **Affects / risks / pros / cons:** as [MP1 0x1E](#mp1-0x1e).
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x3e"></a>
### RSMU 0x3E: PBO scalar

- **Does:** UX: Precision Boost Overdrive scalar, which lets the chip hold boost voltage for longer.
- **Affects:** boost voltage residency; affects chip wear (FIT, the SMU's reliability model).
- **Argument:** scalar (UX).
- **Risks:** accelerated wear.
- **Pros:** none for this module's purpose.
- **Cons:** one source.
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

<a id="rsmu-0x65"></a>
### RSMU 0x65: transfer table to memory

- **Does:** asks the SMU to copy its current PM table values into the shared memory region.
- **Affects:** nothing but the copy. Each transfer also starts a new averaging window ([5.2](05-pm-table.md#52-timing-read-this-before-measuring-anything)).
- **Argument:** `args[0] = 3`. Other tools send 0; this firmware expects 3.
- **Risks:** none in itself. Sent too often, it only keeps the SMU busy.
- **Pros:** the only way to read live telemetry.
- **Cons:** the first transfer after boot may answer `0xFD`; the module retries after 10 and 100 ms.
- **Status:** verified. 2-4 ms per transfer at idle, up to 13.8 ms measured.
- **Class:** **table.** Sent only by the module's rate-limited refresh, at most once per 55 ms ([2.8](02-safety.md#28-the-table-refresh-interval)). Sending it through `SHS_IOC_MSG` returns `EPERM`.
- **Sources:** HW; RA; ryzen_smu; PawnIO; ZenMaster.

<a id="rsmu-0x66"></a>
### RSMU 0x66: get table address

- **Does:** returns the physical address of the shared PM table memory.
- **Affects:** nothing.
- **Argument:** none. Results: low 32 bits in `args[0]`, high in `args[1]`.
- **Risks:** none; the module checks that the address is not normal RAM before using it ([2.2](02-safety.md#22-at-load-is-the-table-safe-to-map)).
- **Pros:** none beyond the module's own use.
- **Cons:** none.
- **Status:** verified.
- **Sources:** HW; #363; RA.

<a id="rsmu-0x89"></a>
### RSMU 0x89: GFX clock

- **Does:** sets the integrated GPU clock.
- **Affects:** GPU clock.
- **Argument:** MHz (RA).
- **Risks:** instability likely; damage unlikely.
- **Pros:** none known.
- **Cons:** RA's source marks it "added to debug on Krackan, Strix Point and Strix Halo", meaning untested.
- **Status:** two tools agree, but neither confirms it works on this chip and it was not tested here. Safeguard, flagged unverified.
- **Sources:** RA (`set_gfx_clk`), UX.

<a id="rsmu-0x92"></a>
### RSMU 0x92: dGPU skin temperature limit (RSMU twin)

- **Does:** UX: RSMU version of MP1 0x34.
- **Affects / risks / pros / cons:** as [MP1 0x34](#mp1-0x34).
- **Status:** single-source. Safeguard, flagged unverified.
- **Sources:** UX.

---

< Previous: [PM table](05-pm-table.md) | [Manual](README.md) | Next: [Findings](07-findings.md) >
