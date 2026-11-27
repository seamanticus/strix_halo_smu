< Previous: [Message glossary](06-glossary.md) | [Manual](README.md) | Next: [Open questions](08-unknowns.md) >

---

# 7. Findings

What this project measured on a GZ302EA (BIOS 311, PMFW 10.100.2.0, linux-zen 7.3, AC power). Each section gives the result, the numbers, and the test that produced them ([chapter 9](09-tests.md)). Several of these correct what other tools assume.

## 7.1 The BIOS drives the MP1 mailbox

**Result.** The BIOS sends MP1 messages itself, at times it chooses, without checking whether the mailbox is in use.

**Evidence.** The decompiled ACPI tables of BIOS 311:

- SSDT1 (`AmdTable`, AMD's ALIB) contains all three MP1 mailbox register addresses, and a send routine `A018` that writes them. It takes the AML mutex `\_SB.AM00`, never checks that the mailbox is idle, and waits for the answer with no timeout.
- `A032` maps AMD's DPTC parameter IDs to MP1 messages `0x14`-`0x56`: power limits, time constants, Tctl, VRM currents, STT parameters. Never Curve Optimizer.
- 112 call sites in the DSDT reach it. Triggers: the ASUS throttle-profile WMI method (Fn+F5, `platform_profile`), `ppt_pl1_spl` writes from asus-armoury, AC plug and unplug, EC queries `_QCE` and `_QCF`, and PCIe power-state changes (MP1 `0x61`, found in the mailbox at rest after boot).

**Consequence.** Any tool that drives MP1 without holding `\_SB.AM00` can collide with the BIOS. The BIOS's own routine waits forever, so a collision can hang the machine inside AML. z13ctl recorded an unexplained hard lock on 2026-08-14 that is consistent with this. This module holds `\_SB.AM00` for every MP1 transaction ([2.4](02-safety.md#24-every-mp1-transaction-hold-the-bios-lock)).

The RSMU mailbox is never touched by AML; it was found untouched since boot.

## 7.2 What limits sustained power: STT

**Result.** Sustained power on this machine is governed by Skin Temperature Tracking, not by STAPM or the slow limit. Table offset `0x000` is the STT power cap.

**Evidence.** A 20-minute capture at 10 Hz in the balanced profile: idle, 10 minutes of all-core load, cool-down. All PM table words and every temperature sensor in the machine were recorded.

- `0x000` sits at 71 W while the skin estimate (`0x06C`) is below the skin limit (`0x068`, 46 C).
- Under load it falls as the skin estimate approaches the limit, and settles at **48 W with the skin estimate at 45.96 C**.
- It returns to 71 W about 2 minutes into cool-down.
- Under load, `0x000` was the binding limit **81% of the time** (the slow limit `0x010` the other 17%).
- Every top correlate of `0x000` is a temperature (r about -0.99); the skin estimate at lag 0 gives r = -0.985.

The BIOS sends 45 W as the balanced STT power limit (MP1 `0x4A`). The controller settles at its own equilibrium, 48 W here, not exactly at that parameter.

**Consequence.** To change sustained power on this machine, the relevant controls are the skin limit (MP1 `0x33`) and the STT power limit (MP1 `0x4A`). Both are safeguard class, because the skin limit is the one that protects the person holding the tablet. Fast and slow limits remain effective as hard ceilings.

## 7.3 STAPM is accepted but has no effect

**Result.** MP1 `0x14` (STAPM limit) answers OK and changes nothing that can be measured.

**Evidence.** `tests/test_power_limits.py stapm`: 20 W sent before a 300 s all-core load. In every run the SMU answered `0x01`, no table word took the value 20.0, and package power stayed at 52 W (the slow limit) or the STT cap. Repeated in:

- the balanced profile;
- the low-power profile;
- without power-profiles-daemon;
- with `amd_pmf` blacklisted (`modprobe.blacklist=amd_pmf`), so no other driver could overwrite or shadow it.

**Consequence.** STAPM stays in the catalogue (the BIOS sends it, so its meaning is confirmed), but tools offering a STAPM or "sustained power" setting through `0x14` do nothing on this firmware. The finding in 7.2 explains why: sustained power is STT's job here.

## 7.4 amd_pmf owns the fast and slow limits

**Result.** The kernel's `amd_pmf` driver sends its own fast and slow limits on every power-profile change, overwriting any value sent through MP1.

**Evidence.** `amd_pmf` sends its own fast/slow pair per profile: balanced 71/52 W, performance 86/70 W, low-power 55/40 W. In every power-limit run, cycling the profile made it send the performance limits, then the balanced ones. During 300 s without a profile change, it never re-sent them. With `amd_pmf` blacklisted, the firmware starts from the same 71/52 W, and `0x15`/`0x16` work the same way.

Measured effect of each limit (balanced, AC, all-core load):

| Run | Package power | Clock |
|---|---|---|
| control, no send | 71 W for about 7 s, then 52 W | - |
| fast limit 45000 mW before the load | 45.0 W from the first second | 3532 to 2702 MHz |
| slow limit 30000 mW, 30 s into the load | 52 W to 30 W within seconds | 3161 to 2175 MHz |

**Consequence.** A client that sets fast or slow limits must send them again after each profile change. Under load, package power equals the lowest of `0x000` (STT cap), `0x008` (fast) and `0x010` (slow); the median difference was 0.00 W in every run.

## 7.5 Curve Optimizer

### All cores (MP1 0x4C)

`tests/test_co_all.py`, every load started at the same idle Tctl (44.3 C), compared with a no-send control run:

| State | Single-core load | All-core load |
|---|---|---|
| CO -5 | -6.8 mV at +32 MHz | -4.4 mV at +35 MHz |
| back to 0 | -0.5 mV, -2 MHz | +0.1 mV, -4 MHz |
| control drift (no sends) | +0.9 to +3.5 mV | within +23 MHz |

The SMU spends part of the undervolt margin on clock, as boost does. Sending 0 restores the factory curve. The encoding is a 20-bit two's complement number of steps.

A first run that seemed to show 0 not restoring the voltage was explained by rising temperatures (66-75 C). Voltage rises with temperature on this chip: 1314 mV at about 66 C against 1286 mV at about 52 C for the same 5.15 GHz. Matching the starting temperature fixed it. **Any CO measurement must start each load at the same temperature.**

### One core (MP1 0x4B)

`tests/test_co_core.py`, a target core and a neighbour in the same CCD as reference, each under the same pinned load:

| Core | Encoding | Result |
|---|---|---|
| 2 (CCD 0), -10 | `0x0020FFF6` | -11.7 mV at +68 MHz; core 3 unchanged |
| 10 (CCD 1), -10, flat `core << 20` | `0x00A0FFF6` | **refused, `0xFF`**; the flat reset `0x00A00000` also refused |
| 10 (CCD 1), -10, CCD-split | `0x1020FFF6` | **+108 MHz at -3.1 mV**; core 11 unchanged |
| 10, back to 0 | `0x10200000` | -1.5 mV, -4 MHz: restored |

CCD 1's cores boost below the 5.15 GHz ceiling, so the margin went to clock rather than voltage.

**The encoding is `(core / 8) << 28 | (core % 8) << 20 | (steps & 0xFFFF)`.** The SMU checks the core field: an index above 7 in bits 23-20 is refused, not applied to the wrong core. G-Helper and UXTU use `core << 20`, which works only for cores 0-7 on this chip. ZenStates-Core uses the CCD-split form, which works for all 16.

### Persistence

CO is lost on reboot and on suspend/resume (z13ctl documentation; consistent with RyzenAdj). The module has no resume hook by design. Reapplying is the client's job.

## 7.6 CPU-to-core mapping

`tests/test_co_core.py map`: one logical CPU at a time spun under load, while recording which table core's power and clock rose. **Linux CPU `N` and CPU `N + 16` are the two threads of table core `N`**, for N = 0 to 15. Cores 0-7 are CCD 0, cores 8-15 CCD 1.

## 7.7 BIOS power and thermal values per profile

From the DSDT of BIOS 311 (`AL20`-`AL2E`, sent through ALIB on each profile change):

| Profile | Skin limit (MP1 0x33) | STT power limit (MP1 0x4A) |
|---|---|---|
| quiet | 44.0 C | 35000 mW |
| balanced | 46.0 C | 45000 mW |
| performance | 47.0 C | 60000 mW |
| WMI `0x00060077` (an ASUS mode) | 44.0 C | 25000 mW |
| EC query `_QCF` | **0** | **0** |

The asus-armoury setting `ppt_pl1_spl` (WMI `0x001200A3`) sends `value x 1000` mW to the STT power limit (MP1 `0x4A`), **not** to STAPM. Tools and users that set `ppt_pl1_spl` are moving the STT budget.

`_QCF` sends zero for every STT parameter; what triggers it, and whether zero means "off" or "keep", is unknown ([8](08-unknowns.md)).

## 7.8 Table averaging and the canary

The table holds averages over the time since the previous transfer ([5.2](05-pm-table.md#52-timing-read-this-before-measuring-anything)). This was found when the canary tripped after a 4 s idle gap, following an all-core load: the table said 49.0 C, the sensor 45.6 C. Comparing against k10temp averaged over each gap brought the error to 0.05-0.06 C RMS at every gap from 55 ms to 4 s.

Tctl offsets compared over 1274 samples (k10temp 46-65 C):

| Offset | Correlation with k10temp | Max / mean difference |
|---|---|---|
| `0xB38` | 1.000 | 1.25 / 0.09 C |
| `0x05C` (RyzenAdj "Tctl") | 0.81 | 14.15 / 5.02 C |
| hottest of `0xC10` + 4n | 0.96 | 11.46 / 2.13 C |

## 7.9 Mailbox timing

| Measurement | Value |
|---|---|
| RSMU `0x65` (table transfer), idle | 2-4 ms, max 13.8 ms |
| Table read, served from the cached snapshot | 0.08 ms |
| Table read that refreshes, 99th percentile | 2.7-4.3 ms |
| Longest MP1 answer (while a table transfer overlapped) | 12.4 ms |
| 4 readers at 100 Hz each | 16.8 distinct snapshots/s against an 18.2/s ceiling; every read complete |
| Forced timeout: late answer | 3.0 ms after the timeout; `\_SB.AM00` held 3.3 ms in total |

## 7.10 Other tools on this machine

| Tool | Issue found | Source |
|---|---|---|
| ryzen_smu | Truncates the table to `0xE50` bytes; mailbox retry counter wraps; can report OK when the SMU did not answer (fixes open as PR #61, #68); no `\_SB.AM00`; allows a refresh every 1 ms | ryzen_smu source, PRs |
| RyzenAdj | Labels `0x000` "STAPM limit" and `0x05C` "Tctl" for this table, both wrong here; says skin temperature is not implemented on Strix Halo, but this BIOS programs it; `set_stapm_time` falls through and also sends `0x4E` | RyzenAdj `api.c` |
| G-Helper, UXTU | Per-core CO `core << 20` refused for cores 8-15; UXTU's shared table names MP1 `0x31`/`0x32` as overclock messages, which this BIOS uses for STT parameters | their source, this project's tests |

---

< Previous: [Message glossary](06-glossary.md) | [Manual](README.md) | Next: [Open questions](08-unknowns.md) >
