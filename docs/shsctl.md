< Previous: [Sources](10-sources.md) | [Manual](README.md) | Next: [Manual](README.md) >

---

# shsctl user guide

`shsctl` is a small command-line tool to look at, and tune, the power behaviour of your ROG Flow Z13 (GZ302EA). It talks to the `strix_halo_smu` kernel module. This guide assumes nothing beyond knowing how to open a terminal.

**Before you start:** the module must be installed and loaded ([Installation](03-install.md)). Every command needs administrator rights, so each one starts with `sudo`.

## Contents

1. [Safe to try: looking](#1-safe-to-try-looking)
2. [Tuning: what you can change](#2-tuning-what-you-can-change)
3. [Undervolting step by step](#3-undervolting-step-by-step)
4. [Power limits](#4-power-limits)
5. [Undoing changes](#5-undoing-changes)
6. [When settings disappear](#6-when-settings-disappear)
7. [Error messages](#7-error-messages)
8. [Command reference](#8-command-reference)
9. [For the curious](#9-for-the-curious)

## 1. Safe to try: looking

These commands only read. They cannot change anything, and you can run them as often as you like.

### Is everything working?

The outputs below are examples; your numbers will differ.

```sh
sudo shsctl info
```

```
SMU firmware (PMFW)    10.100.2.0
BIOS interface         25
PM table               version 0x0064020c, 4136 bytes
allow_safeguard_writes no
allow_unlisted         no
kernel tainted (S)     no
```

The last line turns to `yes` after the first time you tune something. That is normal; see [9](#tainted).

### A snapshot of what the chip is doing

```sh
sudo shsctl table
```

```
package power        48.2 W
  STT cap            48.3 W   <- limiting
  fast limit         71.0 W
  slow limit         52.0 W
skin estimate        45.9 C   (limit 46.0 C)
Tctl                 82.4 C

core  ccd  cpus    clock MHz  volt mV  power W  temp C
   0    0  0 ,16        4012   1121.0     2.91    80.2
   ...
```

How to read it:

- **package power**: how much power the whole chip (CPU and graphics) uses right now.
- **STT cap, fast limit, slow limit**: the three ceilings on that power. The lowest one wins. `<- limiting` marks the one holding power back now (only shown when the chip is working hard enough to touch it).
  - **STT cap** goes down as the laptop's surface gets warm, to keep it comfortable to hold. On this machine it is usually what limits long workloads.
  - **fast limit** is the ceiling for short bursts.
  - **slow limit** is the ceiling averaged over a few seconds.
- **skin estimate**: how warm the chip thinks the outside of the tablet is. The STT cap keeps it under the limit shown.
- **Tctl**: the chip temperature. It throttles itself at 95 C.
- The core table: each of the 16 cores, with the two Linux CPU numbers it runs, its clock, voltage, power and temperature.

### Watch it live

```sh
sudo shsctl watch          # until Ctrl-C
sudo shsctl watch 60       # for 60 seconds
```

```
   time   power   limit       Tctl   skin/limit    max MHz  mean mV
    1.0    12.3    71.0 -       52.1   38.2/46.0      5103      912
    2.0    68.9    71.0 fast    71.4   38.4/46.0      4521     1204
```

One line per second: power, the lowest limit and its name if it is holding power back, chip temperature, skin estimate against its limit, the fastest core's clock, and the average core voltage. Use it while running a game or a compile to see what holds your machine back.

The chip reports averages since it was last asked, so `watch` asks 10 times a second to keep the numbers current.

## 2. Tuning: what you can change

| What | Command | What it does for you | Risk |
|---|---|---|---|
| Undervolt all cores | `co-all` | Cooler, quieter, often faster under sustained load | Too much makes the machine crash. A reboot fixes it. |
| Undervolt one core | `co-core` | The same, fine-tuned per core | Same |
| Burst power ceiling | `fast-limit` | Caps power spikes: quieter fans, longer battery | Low when lowering |
| Sustained power ceiling | `slow-limit` | Caps power over seconds | Low when lowering |

Nothing you set is permanent. **A reboot always puts everything back to factory settings.**

### Try a command without sending it

Put `-n` (or `--dry-run`) before any tuning command to see exactly what would be sent, without sending it:

```sh
sudo shsctl -n co-all -10
co-all: would send mp1 0x4c arg0 0x000ffff6
```

## 3. Undervolting step by step

Your processor uses a little more voltage than it strictly needs, as a safety margin. **Curve Optimizer** (CO) lets you trim that margin. Less voltage means less heat. Less heat means the chip can boost higher for longer before it hits its temperature or power limits.

Each chip is different. A value that is stable on one Z13 can crash another. The only way to find yours is to test.

**The unit** is "steps". Negative numbers undervolt. Measured on this machine, -5 lowered voltage by about 7 mV under load. 0 is factory.

### A safe way to find your value

1. Save your work. A too-low value can freeze the machine. Nothing is damaged; you will need to hold the power button and reboot.
2. Start small:
   ```sh
   sudo shsctl co-all -5
   co-all: sent mp1 0x4c arg0 0x000ffffb, SMU answered 0x01 (OK)
   ```
   `OK` means the chip accepted it.
3. Test it under load for a while: a long compile, a game, a stress test (`stress-ng --cpu 0 --timeout 10m`, for example). Also test at light load: undervolt crashes often happen at idle or when a single core boosts.
4. If it was stable, go 5 steps further (`-10`, `-15`, ...) and test again.
5. When it crashes, or a program misbehaves, reboot and use a value 5 steps back from the last stable one. That is your margin of safety.

How far you can go depends on your chip. For reference: z13ctl limits its CO setting to 0 to -40, and RyzenAdj issue #398 reports -50 crashing a Strix Halo machine. This project has measured only -5 (all cores) and -10 (one core).

### Per core

Some cores have more margin than others. Once you know your all-core value, you can push the best cores further:

```sh
sudo shsctl co-core 3 -20      # core 3 only
sudo shsctl co-core 3 0        # back to factory for core 3
```

Cores are numbered 0 to 15. `shsctl table` shows which Linux CPUs each core runs (core N runs CPUs N and N+16). To stress one core, pin a load to its CPU, for example `taskset -c 3 stress-ng --cpu 1`.

Per-core and all-core settings both apply. To keep things simple, use one or the other.

### Is it working?

Run the same load before and after, with the machine starting from the same temperature (let it cool for a minute first), and compare in `shsctl watch` or `shsctl table`. With an undervolt you should see a lower voltage at the same clock, or a higher clock at the same power. Temperature itself changes voltage, so a hotter run shows higher voltage even with an undervolt. Always compare runs that start equally cool.

## 4. Power limits

```sh
sudo shsctl fast-limit 45      # bursts up to 45 W
sudo shsctl slow-limit 30      # sustained up to 30 W
```

Values are in watts. Lower values mean a cooler, quieter machine that lasts longer on battery, and runs slower under heavy load.

Things to know:

- **Your desktop's power profile overrides these.** Every time you switch profile (power saver, balanced, performance, or Fn+F5), the system sends its own limits, replacing yours. Set them again after a profile change.
- The defaults in the balanced profile are 71 W fast and 52 W slow.
- On this machine the STT cap (the skin-temperature ceiling) often limits sustained power to about 48 W anyway. A slow limit above that changes little under long loads.
- There is no "STAPM" command on purpose. This firmware accepts STAPM limits and ignores them.

## 5. Undoing changes

| To undo | Command |
|---|---|
| All-core undervolt | `sudo shsctl co-all 0` |
| One core's undervolt | `sudo shsctl co-core CORE 0` |
| Power limits | switch your power profile away and back, or reboot |
| Everything | reboot |

## 6. When settings disappear

The chip forgets settings on its own. That is how the firmware works, and it is also a safety net.

| Event | Undervolt | Power limits |
|---|---|---|
| Reboot or shutdown | lost | lost |
| Sleep (suspend) and wake | lost | not measured; assume lost |
| Power profile change | probably kept (neither the BIOS nor amd_pmf sends CO); not measured | **lost** |
| Plugging or unplugging AC | probably kept, as above | the BIOS sends its own limits on AC events; assume lost |
| Unloading the module | **kept** (until one of the above) | kept |

`shsctl` sets things once. To keep a setting across sleep or profile changes, something has to send it again. You can do that yourself, or use a program that does it automatically. `strix_halo_smu` only provides the safe channel to the chip.

## 7. Error messages

| Message | What it means | What to do |
|---|---|---|
| `the module is not loaded` | `strix_halo_smu` is not running. | `sudo modprobe strix_halo_smu` |
| `permission denied: run as root` | You forgot `sudo`. | `sudo shsctl ...` |
| `refused: kernel lockdown forbids hardware tuning, ...` | Your system is in lockdown mode (usually Secure Boot). Looking works, tuning does not. Or the command needs a safety gate that is closed. | See [lockdown](01-concepts.md#111-lockdown). |
| `refused: this message ID is not in the module's catalogue` | `msg` with a message number the module does not know. | Check the [glossary](06-glossary.md). |
| `send budget used up` | More than 16 commands in quick succession. | Wait a few seconds. |
| `the BIOS is using the SMU right now` | The firmware was busy. Nothing was sent. | Run the command again. |
| `the module stopped serving after a fault` | The chip did not answer once, or the temperature check failed. The module has stopped itself as a precaution. | `sudo dmesg \| grep strix_halo_smu`, then `sudo modprobe -r strix_halo_smu && sudo modprobe strix_halo_smu`. |
| `SMU answered 0xff (rejected)` | The chip refused the value. Nothing changed. | Use a value in the normal range. |
| `value out of range` | A number `shsctl` cannot send (a core above 15, a negative wattage, ...). | Check the command's help: `shsctl help COMMAND`. |

Exit codes, for scripts: `0` done, `1` error, `2` wrong usage, `3` the chip answered but did not accept.

## 8. Command reference

`shsctl help` lists every command; `shsctl help COMMAND` explains one.

| Command | Changes anything? | Description |
|---|---|---|
| `info` | no | Firmware versions, table, module gates, taint flag. |
| `table` | no | One labelled snapshot. |
| `table --raw` | no | The raw 4136 table bytes, for other programs: `sudo shsctl table --raw > table.bin`. |
| `watch [SECONDS]` | no | One line per second, from 10 reads a second. |
| `co-all STEPS` | **yes** | Curve Optimizer for all cores. 0 = factory. |
| `co-core CORE STEPS` | **yes** | Curve Optimizer for one core (0-15). 0 = factory. Steps from -32768 to 32767; the chip rejects what it cannot use. |
| `fast-limit WATTS` | **yes** | Burst power ceiling, 1 to 999 W. |
| `slow-limit WATTS` | **yes** | Sustained power ceiling, 1 to 999 W. |
| `msg mp1\|rsmu ID [ARG...]` | depends | Raw message, for developers. Up to six arguments, decimal or `0x` hex. |
| `stats` | no | Every message sent through the module since it was loaded, with counts and the last sender. |
| `help [COMMAND]` | no | Help. |

Option `-n` / `--dry-run`, before the command: print what a tuning command would send, send nothing.

## 9. For the curious

<a id="tainted"></a>
### What "tainted" means

After your first tuning command, the kernel marks itself `S` ("CPU out of spec") until the next reboot. It changes nothing about how your system runs. It is a note that appears in crash reports, so that whoever reads one knows the processor was tuned. `shsctl info` shows it.

### What is logged

Every tuning command is recorded in the kernel log with its exact values and which program sent it:

```sh
sudo dmesg | grep strix_halo_smu
strix_halo_smu: mp1: set message 0x4c args 000ffffb 00000000 ... by shsctl (pid 4242), tainting CPU_OUT_OF_SPEC
```

### What the module refuses, and why

Some settings could cause lasting harm if set wrong: the chip's maximum temperature, the voltage regulator's current limits, the skin-temperature limit that keeps the tablet safe to hold. The module groups these as **safeguard** messages and refuses them unless it was loaded with `allow_safeguard_writes=1`. `shsctl` offers no shortcut for them. If you know what you are doing, the [glossary](06-glossary.md) documents each one, and `shsctl msg` can send them once the gate is open.

### Mapping commands to messages

| shsctl | SMU message | Argument |
|---|---|---|
| `co-all S` | MP1 `0x4C` | `S` as a 20-bit two's complement number |
| `co-core C S` | MP1 `0x4B` | `(C / 8) << 28 \| (C % 8) << 20 \| (S & 0xFFFF)` |
| `fast-limit W` | MP1 `0x15` | `W x 1000` (milliwatts) |
| `slow-limit W` | MP1 `0x16` | `W x 1000` |
| `table` | RSMU `0x65` via the module's rate-limited refresh | - |
| `info` | MP1 `0x02`, `0x03`; the table | - |

The full technical background is in the [manual](README.md).

---

< Previous: [Sources](10-sources.md) | [Manual](README.md) | Next: [Manual](README.md) >
