[Manual](README.md) | Next: [Concepts](01-concepts.md) >

---

# strix_halo_smu manual

This is the manual for `strix_halo_smu`, a Linux kernel module for one machine: the **ASUS ROG Flow Z13 (2025), model GZ302EA**, with the AMD Ryzen AI MAX+ 395 ("Strix Halo").

The module talks to the processor's power-management firmware (the SMU) safely. It does two things:

1. It shows the firmware's live telemetry table (power, limits, temperatures, clocks, voltages).
2. It forwards tuning messages (power limits, undervolt) through a gate that knows what each message does.

It makes no tuning decisions of its own. Which values to use, and when to apply them again, is up to the program that calls it.

If you only want to tune your laptop, start with the [shsctl user guide](shsctl.md). If you want to understand or build on the module, read the chapters in order. Each one assumes only the chapters before it.

## Chapters

| # | Chapter | What you learn |
|---|---|---|
| 1 | [Concepts](01-concepts.md) | Kernel, module, driver, SMU, mailbox, PM table, ACPI, explained from scratch |
| 2 | [Safety model](02-safety.md) | Every check the module makes, and why it exists |
| 3 | [Installation](03-install.md) | Building it, trying it, installing it for good, removing it, troubleshooting |
| 4 | [Programming interface](04-interface.md) | The device file, the ioctls, sysfs files, error codes, code examples |
| 5 | [PM table](05-pm-table.md) | The telemetry table: layout, timing, how to read it correctly |
| 6 | [Message glossary](06-glossary.md) | Every message the module knows: what it does, risks, pros, cons, sources |
| 7 | [Findings](07-findings.md) | What was measured on this machine, with numbers |
| 8 | [Open questions](08-unknowns.md) | What is not known, and why it is left out |
| 9 | [Test suite](09-tests.md) | The scripts that produced the evidence, and how to run them |
| 10 | [Sources](10-sources.md) | Every outside project and document used, and what was taken from it |
| - | [shsctl user guide](shsctl.md) | The command-line tool, for everyone |

## Conventions in this manual

- Hexadecimal numbers start with `0x`. `0x15` is 21 in decimal.
- "MP1 0x15" means message `0x15` sent to the MP1 mailbox. "RSMU 0x65" means message `0x65` sent to the RSMU mailbox. These are two different message numbering spaces.
- "Table 0x0C" means the 4-byte value at byte offset `0x0C` in the PM table.
- Commands that start with `sudo` need administrator rights.
- **Verified** always means measured on a GZ302EA by this project's test scripts. A claim from another project that has not been measured here is labelled as such, with its source.

---

[Manual](README.md) | Next: [Concepts](01-concepts.md) >
