< Previous: [Findings](07-findings.md) | [Manual](README.md) | Next: [Test suite](09-tests.md) >

---

# 8. Open questions

What is not known, and what the module does about it. The rule: **a message whose meaning is not established stays out of the catalogue**. It is documented here, and the module refuses it unless loaded with `allow_unlisted=1`.

## 8.1 Messages the BIOS sends, meaning unknown

These appear in this machine's ACPI code, so the SMU accepts them, but no source explains them.

| MP1 | Where the BIOS sends it | What is known | Status |
|---|---|---|---|
| `0x09` | `A133` | nothing | not catalogued |
| `0x11` / `0x12` | `A011` | probably AC/DC mode | not catalogued |
| `0x27` / `0x28` | after `0x25`/`0x26` (GPU BAR address); also `ALIB(0x10)`, `ALIB(0x11)`, `ALIB(0xAA)` | a paired start/stop of something using the GPU memory region, inferred from call order only. An earlier label, "power saving / max performance", had no source and was withdrawn. | not catalogued |
| `0x29` / `0x2A` | `A133` | nothing | not catalogued |
| `0x31`, `0x32`, `0x35`-`0x3E` | DPTC `0x20`, `0x21`, `0x24`-`0x2D` | STT model parameters (filter coefficients, by analogy). UXTU names `0x31`/`0x32` as overclocking messages, which is wrong for this BIOS. | not catalogued; would be safeguard if ever named |
| `0x54`-`0x56`, `0x20` | DPTC `0x12`, `0x30`-`0x32` | nothing | not catalogued |
| `0x61` | `ALIB(0x14)` from `PPS3` (PCIe root port GPP1 power-off) | PCIe link or power state | not catalogued; it was the last message in the MP1 mailbox after boot |

## 8.2 Messages other tools use, untested here

| Message | Tools | Why not catalogued |
|---|---|---|
| RSMU `0xAF`, CO read-back (`GetDldoPsmMargin`) | ZenStates-Core only | One source; the same function returns core 0's value for every core on Cezanne (ZenStates-Core #30). Would be read class once verified to change nothing. |
| PSMU `0xB7`, iGPU Curve Optimizer | UXTU and G-Helper send it | RyzenAdj's Strix Halo code says it is rejected on this architecture. Sources contradict each other. |
| RSMU `0x5D`, CO all-core alternative | G-Helper fallback, ZenStates | MP1 `0x4C` works, so it is not needed. |
| RSMU `0x53` | listed with the above in research | unverified |

## 8.3 Firmware behaviour

- **EC query `_QCF`** sends zero for every STT parameter (skin limit, STT power limit and the model coefficients). Whether zero disables STT or means "keep current", and what makes the EC raise query `0xCF`, are unknown. It matters for thermal safety, so it is the first thing to watch for (table `0x068` falling to 0) if STT ever behaves oddly.
- **Suspend and the table address.** Does the table's physical address survive s2idle? The module reads it once at load. If it changed, the temperature check would catch the mismatch and disable the table rather than serve wrong data.
- **What `0x004` and `0x014` are.** Two power averages over different windows. Not named until there is evidence.

## 8.4 PM table regions

Unlabelled regions are listed in [5.3](05-pm-table.md#other-regions): seven limit/value pairs at `0x030`-`0x064`, per-core residency-like values at `0xD50`, per-core constants at `0xDD0`/`0xE10`/`0xE50`, and several temperature-like values. They stay unnamed in `libshs` until a differential test or an independent source confirms them.

## 8.5 New BIOS versions

A BIOS update that changes any of the four contract values makes the module refuse to load ([2.1](02-safety.md#21-at-load-is-this-the-verified-machine)). Adding a new row to the verified list needs this evidence, collected read-only:

1. The `dmesg` line with PMFW version, interface version, table version and size, and ALIB revision (the module logs it before refusing).
2. A table dump from the new firmware, checked against [5.3](05-pm-table.md#53-offset-map) for the verified offsets.
3. The Tctl canary at `0xB38` passing against `k10temp` under idle and load.
4. The new ACPI tables, checked to see that `A018`/`A032` and the message map are unchanged.

If you have a newer BIOS, the most useful thing you can do is open an issue with item 1.

## 8.6 Not planned

- **Other machines.** Strix Halo appears in other laptops and mini PCs. Each has its own BIOS, its own ALIB, possibly its own lock and its own EC. The checks in chapter 2 are only meaningful for firmware that was actually read. Support for another model means repeating that work for it, as a separate verified entry.
- **Policy in the kernel.** Value ranges, profiles, reapply on resume: these stay in user space.

---

< Previous: [Findings](07-findings.md) | [Manual](README.md) | Next: [Test suite](09-tests.md) >
