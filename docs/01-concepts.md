< Previous: [Manual](README.md) | [Manual](README.md) | Next: [Safety model](02-safety.md) >

---

# 1. Concepts

This chapter explains the moving parts from scratch. If you write software but have never worked inside an operating system kernel, read it all; the rest of the manual depends on these words.

## 1.1 Kernel, user space, and why a module

A running Linux system has two worlds.

- **User space** is where normal programs run: your shell, your browser, `shsctl`. They cannot touch hardware directly. If one crashes, only that program dies.
- **The kernel** is the core of the operating system. It owns the hardware and gives user programs controlled access to it. Code in the kernel can do anything; a bug there can freeze the whole machine.

A **kernel module** is a piece of kernel code that can be loaded and unloaded while the system runs, like a plug-in. `strix_halo_smu.ko` is one. Once loaded it is part of the kernel, with full hardware access.

A **driver** is kernel code that manages a piece of hardware and offers it to user space through a defined interface. This module is a small driver: its hardware is the SMU mailboxes, and its interface is one device file.

**Out-of-tree** means the module's source is not part of the official Linux source tree, so it is built separately against the headers of your running kernel. When the kernel is updated, the module has to be rebuilt; [DKMS](03-install.md#33-permanent-install-with-dkms) does that automatically.

## 1.2 The SMU

Inside the AMD processor there are several small helper processors with their own firmware. The **SMU** (System Management Unit) is the one that runs power management. Many times a second it decides clock speeds and voltages for every core and the graphics, keeps the chip within its power and temperature limits, and records what it measured.

The SMU's firmware is called **PMFW** (power-management firmware). It ships inside the laptop's BIOS. On the verified machine it is version 10.100.2.0.

The operating system does not set clocks or voltages directly. It sends the SMU *requests*: "limit power to 45 W", "shift this core's voltage curve down". The SMU applies them, inside its own safety limits.

## 1.3 Mailboxes

The SMU receives requests through **mailboxes**. A mailbox is a set of hardware registers:

- a **message** register, where you write the request number (for example `0x15`, "set fast power limit");
- six **argument** registers, where you put the request's values (for example `45000`, in milliwatts);
- a **response** register, where the SMU writes its answer.

One complete exchange is called a **transaction**:

1. Clear the response register.
2. Write the arguments.
3. Write the message number. This starts the request.
4. Wait until the response register is not zero.
5. Read the response code and the arguments (the SMU may write results back into them).

| Response | Name | Meaning |
|---|---|---|
| `0x01` | OK | Done. |
| `0xFC` | busy | The SMU could not take the request now. |
| `0xFD` | prerequisite | Something must happen first (seen once after boot, on the first table transfer). |
| `0xFE` | unknown | The SMU does not know this message number. |
| `0xFF` | failed | The SMU knows the message but rejected the value. |

These codes are AMD's own numbering, from the `smu_v14_0_0_ppsmc.h` header in the Linux graphics driver.

This processor has two mailboxes that matter here:

- **MP1**: power limits, Curve Optimizer, firmware version queries. **The BIOS uses this mailbox too** (see 1.6).
- **RSMU**: the telemetry table. Nothing else on this machine uses it.

A mailbox can hold only one transaction at a time. If two programs write into it at once, the SMU can run one program's message with the other's arguments. Preventing that is the main reason this module exists.

## 1.4 SMN: how the registers are reached

The mailbox registers are not ordinary memory. They live on an internal bus called **SMN** (System Management Network). The kernel reaches SMN through a pair of registers on the processor's root PCI device: write the SMN address to one, read or write the value through the other. Linux has built-in functions for this, `amd_smn_read` and `amd_smn_write`, which serialise access with their own lock. The module uses those and nothing else.

## 1.5 The PM table

Besides the mailboxes, the SMU keeps a block of numbers about itself: the **PM table** (power-management table). On this firmware it is 4136 bytes (`0x1028`), mostly 32-bit floating-point values: package power, each power limit, temperatures, and per-core clock, voltage, power and temperature.

The table lives in the SMU's private memory. To read it:

1. Ask the SMU where its shared copy lives in RAM (RSMU `0x66`). This happens once, at load.
2. Ask the SMU to copy its current values there (RSMU `0x65`, "transfer table").
3. Read the copy.

The layout is not published by AMD. Other projects (RyzenAdj in particular) worked out the meaning of many offsets, and this project measured and corrected several of them. See [chapter 5](05-pm-table.md).

## 1.6 ACPI, ALIB and the firmware lock

**ACPI** is the standard way firmware describes a machine to the operating system. Part of it is code: the BIOS ships small programs written in **AML** (ACPI Machine Language), and the kernel runs them when hardware events happen, such as plugging in AC power, pressing Fn+F5, or a request from the embedded controller (the EC, the small chip that runs the keyboard, fans and battery).

On this laptop, AMD's AML library **ALIB** (in the ACPI table `AmdTable`) sends MP1 messages itself. It sets power and thermal limits whenever the power profile changes, AC is plugged or unplugged, or the EC asks. Those events are asynchronous: they can happen at any moment, while another program is talking to MP1.

ALIB protects its own MP1 traffic with an AML lock (a **mutex**) named `\_SB.AM00`. A mutex is a token that only one holder can have at a time; whoever wants it waits until it is released. The kernel can take the same AML mutex from C code. **This module holds `\_SB.AM00` for every MP1 transaction**, so its messages and the BIOS's messages can never overlap.

Other tools that talk to MP1 (ryzen_smu, and anything built on it) do not take this lock. Their messages can collide with the BIOS's. See [chapter 7.1](07-findings.md#71-the-bios-drives-the-mp1-mailbox).

## 1.7 Power limits in one page

The SMU enforces several power limits at once. Package power can never be higher than the lowest one.

| Limit | Plain meaning | Who sets it on this machine |
|---|---|---|
| **Fast (PPT fast)** | How much power the chip may draw in short bursts. Reacts within about a second. | `amd_pmf` on every power-profile change |
| **Slow (PPT slow)** | How much power over an averaging window of seconds. | `amd_pmf` on every power-profile change |
| **STAPM** | A long-term average limit used on older AMD laptops. | The BIOS, but on this firmware it **has no effect** ([7.3](07-findings.md#73-stapm-is-accepted-but-has-no-effect)) |
| **STT cap** | Skin Temperature Tracking: the SMU estimates how hot the laptop's surface is and lowers power so it stays below a skin limit (46 C in the balanced profile). | The SMU's own control loop, with parameters from the BIOS and EC |

On this machine, the STT cap is what limits sustained power. Fast and slow limits matter for bursts and as a hard ceiling. See [chapter 7.2](07-findings.md#72-what-limits-sustained-power-stt).

**amd_pmf** is the kernel's AMD Platform Management Framework driver. It drives power profiles (the "power saver / balanced / performance" setting in your desktop), and sends its own fast and slow limits every time the profile changes. A limit you set by hand lasts until the next profile change.

## 1.8 Curve Optimizer (undervolting)

For every clock speed, the SMU picks a voltage from a curve stored in the chip. The curve has safety margin built in. **Curve Optimizer** (CO) shifts that curve by a number of *steps*:

- A negative offset (for example -10) asks for less voltage at each clock. The chip then runs cooler and, if power or temperature was the limit, boosts higher. This is called **undervolting**.
- Too negative, and the core no longer gets enough voltage to compute correctly. The result is a crash or, worse, silent wrong results. A reboot clears the setting.
- 0 is the factory curve.

There is one message for all cores at once (MP1 `0x4C`), and one for a single core (MP1 `0x4B`). Measured here: -5 all-core lowered voltage by about 7 mV and raised clock by about 32 MHz under the same load ([7.5](07-findings.md#75-curve-optimizer)).

The SMU forgets CO on reboot and on suspend. Reapplying it is the client program's job.

## 1.9 Cores, CCDs and CPU numbers

The Ryzen AI MAX+ 395 has 16 cores in two **CCDs** (core complex dies), 8 cores each. Each core runs two threads, so Linux shows 32 CPUs.

| Table core | CCD | Linux CPUs |
|---|---|---|
| 0-7 | 0 | N and N+16 (core 0 = CPUs 0 and 16) |
| 8-15 | 1 | N and N+16 (core 15 = CPUs 15 and 31) |

This mapping was measured by loading one CPU at a time and watching which table core changed ([7.6](07-findings.md#76-cpu-to-core-mapping)).

## 1.10 Taint

The kernel keeps a list of **taint** flags: notes that something happened which makes bug reports less trustworthy. Loading an out-of-tree module sets one (`O`). Sending a state-changing SMU message through this module sets `S`, `CPU_OUT_OF_SPEC`: "the processor may be running outside its specification". A taint flag does nothing on its own. It stays set until reboot and appears in crash reports, so a developer can see that the machine was tuned. Read it with `cat /proc/sys/kernel/tainted` (bit 2, value 4, is `S`).

## 1.11 Lockdown

**Kernel lockdown** is a security mode, often enabled together with Secure Boot, that stops even root from changing the running kernel or hardware in low-level ways. The module asks lockdown the same question the kernel's MSR driver does (`LOCKDOWN_MSR`). If lockdown says no, read-only use still works, and every state-changing message is refused.

---

< Previous: [Manual](README.md) | [Manual](README.md) | Next: [Safety model](02-safety.md) >
