< Previous: [Open questions](08-unknowns.md) | [Manual](README.md) | Next: [Sources](10-sources.md) >

---

# 9. Test suite

Every claim marked **verified** in this manual comes from one of these scripts. They are plain Python 3 with no dependencies, and talk to the module directly through `ioctl`, so they also show how to use the interface without `libshs`.

## 9.1 General rules

- Run from the repository root, as root, with the module loaded with **default parameters** (both gates closed), unless a script says otherwise.
- **Run on AC power, at idle.** Close browsers, benchmarks and anything else that uses the CPU. A background job ruins a measurement (one CO run was discarded because an AI benchmark held the power budget).
- Redirect the output to a file: `sudo python3 tests/X.py > X.txt`. The output is the evidence.
- Scripts that send state-changing messages **taint the kernel until reboot** and set everything back to default on every exit path, including Ctrl-C.
- Every comparison that matters is made against a **no-send control run** with the same sequence, so that temperature drift is measured instead of assumed.

## 9.2 Read-only scripts

| Script | What it proves | Run |
|---|---|---|
| `tests/test_phase5.py` | The interface and gate: read-class messages work; every other class is refused before reaching hardware; the send budget admits 16, then `EAGAIN`; the table is served with a timestamp; no taint. Aborts if a gate parameter is open. 29 checks. | `sudo python3 tests/test_phase5.py` |
| `tests/test_m2_window.py` | Table averaging ([7.8](07-findings.md#78-table-averaging-and-the-canary)): compares table Tctl with k10temp at read time and averaged over the window, at gaps from 55 ms to 4 s, after an all-core load; also checks the canary never trips. | `sudo python3 tests/test_m2_window.py` |
| `tests/capture_table.py` | A long capture of the full table plus every temperature sensor at 10 Hz: idle, all-core load, cool-down. Writes `capture-<profile>.bin` and `.txt`. | `sudo python3 tests/capture_table.py [IDLE_S LOAD_S COOL_S]` |
| `tests/analyze_cap.py` | Offline analysis of a capture: which cap binds under load, what table word `0x000` follows (correlation with lag search across every word and sensor), what it settles at. Identified `0x000` as the STT cap ([7.2](07-findings.md#72-what-limits-sustained-power-stt)). | `python3 tests/analyze_cap.py capture-PROFILE.bin` |
| `tests/test_co_per_core.py map` | The CPU-to-core mapping ([7.6](07-findings.md#76-cpu-to-core-mapping)). | `sudo python3 tests/test_co_per_core.py map` |

## 9.3 State-changing scripts

Each sends only values that lower a limit or a voltage by a small amount, and restores defaults on exit.

| Script | What it sends | What it proves |
|---|---|---|
| `tests/test_phase6.py` | MP1 `0x4C` -5, then 0 | All-core CO effect and its reset, under single-core and all-core load, each load starting at the same idle Tctl. Aborts without sending if idle Tctl is above 45 C or single-core boost is below 4.5 GHz. `--control` sends nothing. |
| `tests/test_co_per_core.py control CORE` | nothing | Drift baseline for the per-core runs. |
| `tests/test_co_per_core.py ccd CORE` | MP1 `0x4B` -10 on CORE with the CCD-split encoding, then 0 | Per-core CO on the target core, measured against a neighbour in the same CCD. |
| `tests/test_co_per_core.py flat CORE` | MP1 `0x4B` with the flat encoding | Kept to show the SMU refusing it for cores 8-15. |
| `tests/test_power_limits.py control` | nothing; 120 s load | Baseline: which limit binds and when. |
| `tests/test_power_limits.py fast` | MP1 `0x15` 45000 mW before a 60 s load | Fast limit enforcement. |
| `tests/test_power_limits.py slow` | MP1 `0x16` 30000 mW, 30 s into a 90 s load | Slow limit enforcement. |
| `tests/test_power_limits.py stapm` | MP1 `0x14` 20000 mW before a 300 s load | STAPM's lack of effect. |

`test_power_limits.py` restores limits by cycling the power profile, which makes `amd_pmf` send its own values. With `--no-pmf` (boot with `modprobe.blacklist=amd_pmf`), the restore is MP1 only, and a reboot is the clean way back. It runs in whatever profile is current and names it in the output.

## 9.4 Recorded results

The research notes and raw outputs from the development machine (BIOS 311, PMFW 10.100.2.0) are summarised throughout [chapter 7](07-findings.md), with the numbers each script produced. If you run the scripts on your machine and get different numbers, please open an issue with the output file and `cat /sys/class/dmi/id/bios_version`.

---

< Previous: [Open questions](08-unknowns.md) | [Manual](README.md) | Next: [Sources](10-sources.md) >
