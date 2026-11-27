< Previous: [Programming interface](04-interface.md) | [Manual](README.md) | Next: [Message glossary](06-glossary.md) >

---

# 5. PM table

The PM table is the SMU's own report on what it is doing. On the verified firmware it is version `0x0064020C`, **4136 bytes** (`0x1028`), almost entirely little-endian 32-bit floats.

## 5.1 Size: why 0x1028 and not 0xE50

Every other tool hard-codes the table size per version. ryzen_smu and ZenMaster use `0xE50` for this version and cut the table short. The SMU itself reports the size in the second result of RSMU `0x06`: `0x1028` here, the same as the public GZ302EA dump attached to RyzenAdj issue #363 (exactly 4136 bytes). The module uses the SMU's value, checked against the contract. Everything from `0xE50` up to `0x1028` is hidden from ryzen_smu users.

## 5.2 Timing: read this before measuring anything

**Table values are averages over the time since the previous transfer, not snapshots of one instant.**

This was measured by comparing the table's Tctl against the kernel's `k10temp` sensor at different read gaps from 55 ms to 4 s:

| Compared against | Error at 55 ms gap | Error at 4 s gap |
|---|---|---|
| k10temp at the moment of the read | 0.11 C RMS | 1.95 C RMS (max 3.55 C) |
| k10temp averaged over the gap | 0.05 C RMS | 0.06 C RMS |

So a single read after a long pause shows a smoothed value: a 10 s load spike read once after 30 s looks like a third of its height.

Consequences:

- **To follow a changing value, read steadily**, about 10 times a second. `shsctl watch` does this.
- **Another program's reads shorten your window.** The window is "since the previous transfer by anyone", because all readers share one snapshot.
- **Use the timestamp** from `SHS_IOC_TABLE` to know the window length: it is the difference between two consecutive snapshot times.
- Reads closer than 55 ms apart return the same snapshot ([2.8](02-safety.md#28-the-table-refresh-interval)).

## 5.3 Offset map

"Verified" means measured on a GZ302EA by this project's scripts. "Plausible" means the value is physically reasonable and matches the label in another project, but was not tested here. All values are 32-bit floats.

### Power and limits

| Offset | Meaning | Unit | Status | Evidence |
|---|---|---|---|---|
| `0x000` | **STT power cap**: the power budget the skin-temperature controller allows now | W | verified | 20-minute capture: 71 W while the skin estimate is below its limit, falls as it approaches, settled at 48 W with skin at 45.96 C (limit 46 C); every top correlate is a temperature (r about -0.99). RyzenAdj calls it "STAPM limit", which is wrong for this firmware ([7.2](07-findings.md#72-what-limits-sustained-power-stt)). |
| `0x004` | slow-moving power average | W | measured, not named | rises steadily under load, about 50 W at the end of a 10-minute load |
| `0x008` | **fast limit** (PPT fast) | W | verified | moves to exactly the value sent with MP1 `0x15`; `amd_pmf` writes it on every profile change |
| `0x00C` | **package power** | W | verified | under load equals the lowest of `0x000`, `0x008`, `0x010` (median difference 0.00 W across every run); responds within about 1 s |
| `0x010` | **slow limit** (PPT slow) | W | verified | moves to the value sent with MP1 `0x16`; also verified on a MAX+ 388 by ryzen_smu PR #50 |
| `0x014` | further-averaged power | W | measured, not named | smoother than `0x004` |
| `0x018` | APU slow limit | W | plausible | constant 70.0 W; RyzenAdj label |
| `0x058` | Tctl limit | C | plausible | 95 C; RyzenAdj label, matches the BIOS Tctl limit |
| `0x05C` | filtered temperature | C | measured, **not Tctl** | RyzenAdj labels it Tctl; it lags by up to 14 C under load (correlation 0.81) |
| `0x068` | **skin temperature limit** (STT APU) | C | verified | 46.0 C in balanced, the value the BIOS sends with MP1 `0x33`; also 46.0 in the public dump |
| `0x06C` | **skin temperature estimate** | C | verified | what the STT controller holds below `0x068`; settled at 45.96 C under sustained load |

### Temperature

| Offset | Meaning | Unit | Status | Evidence |
|---|---|---|---|---|
| `0xB38` | **Tctl** (control temperature) | C | verified | correlation 1.000 with `k10temp`, mean difference 0.09 C over 1274 samples; the module's canary uses it ([2.3](02-safety.md#23-always-the-temperature-check-canary)) |

### Per core (16 values, core `n` at `base + 4 * n`)

| Base | Meaning | Unit | Status | Evidence |
|---|---|---|---|---|
| `0xB90` | core power | W | verified | sums to about 40 W sustained, about 70 W peak; follows the core under load |
| `0xBD0` | core voltage | V | verified | the Curve Optimizer verification signal: moved by the expected amount on the targeted core only ([7.5](07-findings.md#75-curve-optimizer)) |
| `0xC10` | core temperature | C | plausible | per-core; the hottest core does not equal Tctl (correlation 0.96) |
| `0xC50` | core clock | **GHz** | verified | moves with load and with CO on the targeted core |

`libshs` converts voltage to mV and clock to MHz.

### Other regions

| Region | Pattern | Status |
|---|---|---|
| `0x054C` / `0x0550` / `0x0558` | GFX voltage / temperature / clock | plausible (RyzenAdj labels, idle values reasonable) |
| `0x06E4` | SOCCLK ladder, 8 entries | verified against the kernel's `pp_dpm_socclk` |
| `0x0704` | FCLK ladder, 8 entries | verified against `pp_dpm_fclk` |
| `0x0030`-`0x0064` | seven more limit/value pairs | unlabelled; TDC/EDC/thermal by analogy to Strix Point |
| `0x0D50` + 4n | 16 values about 90-99, low on the busy core | likely per-core sleep residency (%); unverified |
| `0x0DD0`, `0x0E10`, `0x0E50` + 4n | constants 5.13 / 1.99 / 128 | likely per-core fused limits; unverified. `0xE50` is exactly where ryzen_smu stops reading. |
| `0x0344`, `0x0C14`-`0x0C2C`, `0x0FE8`, `0x0054` | temperature-like (correlation 0.96-0.98 with Tctl) | unlabelled |

Offsets not listed are unknown. They are not exposed by name in `libshs`; read them with `shs_table_f32` if you are investigating.

## 5.4 How the labels were checked

1. **Public dump.** The GZ302EA dump attached to RyzenAdj #363 was decoded with RyzenAdj's labels, and each value checked for plausibility (an idle core at 0.7 V, 2 GHz, 0.1 W).
2. **Kernel oracles.** `k10temp` for Tctl; amdgpu's `pp_dpm_*` files for the clock ladders.
3. **BIOS values.** The BIOS's ACPI code contains the exact skin limit and power limits it sends per profile. Matching table values identify the offsets.
4. **Differential tests.** Send one message, change one thing, see which offset moves and by how much, with a no-send control run to measure drift ([chapter 9](09-tests.md)).
5. **Long correlation.** A 20-minute capture of every table word against every temperature sensor in the machine, with lag search ([9](09-tests.md)).

---

< Previous: [Programming interface](04-interface.md) | [Manual](README.md) | Next: [Message glossary](06-glossary.md) >
