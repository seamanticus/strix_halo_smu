< Previous: [Concepts](01-concepts.md) | [Manual](README.md) | Next: [Installation](03-install.md) >

---

# 2. Safety model

The module does not try to make tuning safe. A too-low voltage still crashes the machine. What it does is make sure that every message reaches the SMU **intact, alone, on the hardware it was verified on, and only if the message is known**. This chapter lists every check, in the order they run.

## 2.1 At load: is this the verified machine?

The module refuses to load (`modprobe` fails with "No such device") unless all of these match:

| Check | Expected value | Why |
|---|---|---|
| CPU | AMD, family `0x1A`, model `0x70` | Strix Halo. Mailbox addresses and message numbers change between chip generations. |
| DMI system vendor | `ASUSTeK COMPUTER INC.` | The BIOS behaviour below was read from ASUS's firmware. |
| DMI board name | `GZ302EA` | Same, for this model only. |
| Root PCI device `0000:00:00.0` | AMD `1022:1507` | The device the kernel's SMN functions use. |
| ACPI objects | `\_SB.AM00` and `\_SB.ALIB` exist | The firmware lock must exist, or the module could not protect MP1 (2.4). |

Then it asks the firmware about itself. All four values must match a row in the module's list of verified firmware (`shs_verified[]`):

| BIOS interface (MP1 `0x03`) | Table version (RSMU `0x06` arg 0) | Table size (RSMU `0x06` arg 1) | ALIB `AmdTable` revision |
|---|---|---|---|
| 25 | `0x0064020C` | `0x1028` | 2 |

This set of four is the **firmware contract**. It is verified on BIOS GZ302EA.311 with PMFW 10.100.2.0. A BIOS update that keeps all four values keeps working. One that changes any of them makes the module refuse to load until someone has collected evidence that the new firmware behaves the same way and added a new row.

Why these four and not the exact firmware build? AMD versions the interface separately from the build. Message numbers within this chip family stayed the same for years across many firmware builds, and changed only with new silicon ([d5_2 research](10-sources.md#project-research)). The interface version, the table layout version and the BIOS's own ALIB revision are the values that would change if the meaning of a message or a table offset changed.

## 2.2 At load: is the table safe to map?

1. The SMU reports where its table copy lives in physical memory (RSMU `0x66`).
2. The module refuses if that address overlaps ordinary RAM that the kernel hands out. The SMU writes to that memory on its own (by DMA), so it must not be memory that a program might be using.
3. The table is mapped with the size the SMU reported, which must match the contract. Each refresh copies it into a private snapshot, and readers only ever see that snapshot.
4. Two table transfers are made, 55 ms apart, and the temperature check below runs on the second. If it cannot run or fails, the module does not load.

## 2.3 Always: the temperature check (canary)

After every table refresh, the module compares two independent readings of the same temperature:

- **Tctl in the table** (offset `0xB38`), written by the SMU;
- **Tctl from the thermal sensor register** (SMN `0x59800`), the same register the kernel's `k10temp` driver reads.

If they differ by more than 3 C, the table layout is not what the module believes it is (for example, a firmware change the contract did not catch). The module then stops serving the table (every read returns `EIO`) until it is reloaded, so no program makes decisions from misread numbers.

Details that matter:

- The table holds **averages** over the time since the previous refresh. The sensor register is instantaneous. Under a changing load they diverge as the gap between refreshes grows. The check therefore runs only when the previous refresh was at most 110 ms ago. Measured error at those gaps: 0.06 C RMS, worst skew 1.25 C under a step load, so 3 C leaves a 2.4x margin. See [7.8](07-findings.md#78-table-averaging-and-the-canary).
- Offset `0x5C`, which RyzenAdj labels Tctl, is a filtered value that lags by up to 14 C under load. It is not used.

## 2.4 Every MP1 transaction: hold the BIOS lock

The BIOS sends MP1 messages from AML on events it does not announce. Its send routine (`A018` in SSDT1) never checks whether the mailbox is idle, and waits forever for an answer. Two senders at once means one message runs with the other's arguments, or one sender waits forever. A forever-wait inside AML freezes the machine.

The module therefore:

1. Takes its own MP1 mutex (serialising callers of the module).
2. Takes the BIOS's AML mutex `\_SB.AM00`, waiting at most 50 ms. If the BIOS holds it longer, the call fails with `EBUSY` and nothing is sent.
3. Checks that the response register is not zero, meaning no message is in flight. If one is (someone outside both locks is using the mailbox), nothing is sent and the call fails with `EBUSY`.
4. Runs the transaction, waiting at most 50 ms for the answer.
5. Releases both locks.

The 50 ms timeouts keep the BIOS from waiting on the module for long. The longest MP1 answer measured was 12.4 ms.

RSMU transactions use their own mutex. The BIOS never touches RSMU, so `\_SB.AM00` is not taken for them.

## 2.5 When the SMU does not answer

If the SMU does not answer within 50 ms:

1. The module keeps holding `\_SB.AM00` and waits up to another 100 ms for the late answer. Releasing the lock while the SMU may still be running the message would let the BIOS write over it.
2. It then releases the lock and marks itself **wedged**: every later transaction fails with `EIO` before touching hardware.
3. The kernel log says which message timed out and whether the late answer came.

Reload the module to clear it. This path was tested by forcing a zero timeout: the late answer came 3 ms after, the lock was held for 3.3 ms in total, and every later call returned `EIO`.

## 2.6 Every message: the catalogue

Every message number is looked up in a built-in **catalogue**. Each entry has a class:

| Class | What it covers | Requirement to send |
|---|---|---|
| **read** | Queries that change nothing: test, firmware version, interface version, table version and address | none, beyond opening the device |
| **set** | Firmware-owned limits that the SMU still bounds by its own protections: power limits, time constants, Curve Optimizer | device opened for writing; lockdown allows `LOCKDOWN_MSR` |
| **safeguard** | Messages that can weaken a protection against lasting harm: temperature limits, VRM current limits, PROCHOT, skin temperature, SMU feature switches | as set, **plus** the module loaded with `allow_safeguard_writes=1` |
| **table** | RSMU `0x65`, the table transfer | never directly; only through the rate-limited table read (2.8) |
| **deny** | MP1 `0x25`/`0x26`, through which the BIOS hands the SMU a physical memory address | never, whatever the parameters |
| *not listed* | anything else | the module loaded with `allow_unlisted=1` |

Every entry and its evidence is in the [glossary](06-glossary.md). A message enters the catalogue only if the BIOS itself confirms its meaning, or two independent tools agree and it was then verified on hardware here. Known-dangerous messages with only one source are catalogued as **safeguard** anyway, flagged unverified, so that `allow_unlisted=1` alone cannot reach them.

The class check uses the error codes in [4.5](04-interface.md#45-error-codes).

## 2.7 Every message: the send budget

Each mailbox admits a **burst of 16 messages**, then **one more per second**. This is shared by every caller and every class. Beyond it, the call fails with `EAGAIN` before reaching the hardware, and the refusal is counted.

16 was chosen because one BIOS profile change is 15 messages: a client can apply a full set of limits in one go. The budget bounds how long user programs can keep `\_SB.AM00` away from the BIOS, and how fast the kernel log can grow.

## 2.8 The table refresh interval

A table refresh is an RSMU `0x65` transaction. The module refreshes at most **once every 55 ms**. Programs that read faster get the same snapshot (or the same error). Any number of readers share one refresh.

ryzen_smu allows one refresh per millisecond, inherited from a similar line in the kernel's graphics driver. That is pointless for a table that averages over the time between reads, and it keeps the SMU busy. 55 ms allows 18 refreshes per second: enough for 10 Hz monitoring with headroom, measured at 2.7-4.3 ms per refresh (99th percentile).

## 2.9 What a state-changing send leaves behind

Every message of class set, safeguard or not listed:

1. Is logged in full in the kernel log: mailbox, class, message number, all six arguments, the sending program and its process ID.
2. Taints the kernel `CPU_OUT_OF_SPEC` (`S`) until reboot.
3. Is counted in `/sys/class/misc/strix_halo_smu/send_stats`, readable by root only.

The log is not rate-limited, so `dmesg` always shows the complete history. The send budget, not the log, bounds the volume.

## 2.10 Who may use it

- The device `/dev/strix_halo_smu` is mode `0600`, owned by root.
- Opening it also requires the capability `CAP_SYS_RAWIO`, the same one raw hardware access needs. Root has it.
- Opening it read-only allows read-class messages and the table. State-changing messages need the device opened for writing.
- `/sys/class/misc/strix_halo_smu/pm_table` is readable by everyone. Telemetry is not secret, and reads are rate-limited.

## 2.11 Load parameters

| Parameter | Default | Effect |
|---|---|---|
| `allow_safeguard_writes` | `0` | `1` lets safeguard-class messages through (still logged and tainting) |
| `allow_unlisted` | `0` | `1` lets message numbers outside the catalogue through (logged as "unlisted" and tainting) |

Both can only be set when the module is loaded, and cannot be changed while it runs (they are read-only in `/sys/module/strix_halo_smu/parameters/`). To change one, unload and load again. This means opening a gate is always a deliberate, logged act by root, never something a running program can do by itself.

## 2.12 What the module does not protect against

- **Your values.** A Curve Optimizer offset too negative for your chip crashes it. A power limit set high runs the machine hot until the SMU's own protections act. The module forwards what you ask for.
- **Other tools.** If ryzen_smu or another driver drives the same mailbox at the same time, the module detects a busy mailbox and refuses to send (2.4 step 3), but it cannot stop the other tool from colliding with the BIOS. Do not load them together.
- **Persistence.** Nothing the SMU accepts survives a reboot. Curve Optimizer is lost on suspend, and power limits are lost on a power-profile change. That is a property of the firmware, and it is also a safety net.

---

< Previous: [Concepts](01-concepts.md) | [Manual](README.md) | Next: [Installation](03-install.md) >
