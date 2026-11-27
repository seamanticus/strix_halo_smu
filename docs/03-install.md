< Previous: [Safety model](02-safety.md) | [Manual](README.md) | Next: [Programming interface](04-interface.md) >

---

# 3. Installation

There are two ways to run the module:

- **Trial** (3.2): build it and load it by hand with `insmod`. Nothing is installed. **Everything is gone after a reboot**: the module is not loaded again, and every setting you made is lost. Use this first.
- **Permanent** (3.3): install it with DKMS so it loads at every boot and is rebuilt automatically when your kernel is updated.

## 3.1 Requirements

| Requirement | How to check | Notes |
|---|---|---|
| ASUS ROG Flow Z13 GZ302EA | `cat /sys/class/dmi/id/board_name` prints `GZ302EA` | Any other machine is refused. |
| Verified firmware | see [2.1](02-safety.md#21-at-load-is-this-the-verified-machine) | Checked at load; the log says if it does not match. |
| Kernel 6.15 or newer | `uname -r` | The module uses `<asm/amd/node.h>` (6.15) and the kernel build system's separate output directory (6.13). |
| Headers for the running kernel | `ls /lib/modules/$(uname -r)/build` exists | Arch: `linux-headers`, `linux-zen-headers`, ... Debian/Ubuntu: `linux-headers-$(uname -r)`. Fedora: `kernel-devel`. |
| Kernel built with `CONFIG_AMD_NODE=y` | `grep AMD_NODE /lib/modules/$(uname -r)/build/.config` | Provides `amd_smn_read/write`. True for all mainstream x86 distribution kernels. |
| Compiler and `make` | `cc --version`, `make --version` | Arch: `base-devel`. Debian/Ubuntu: `build-essential`. |
| Unsigned modules allowed | `cat /sys/kernel/security/lockdown` shows `[none]`, Secure Boot off | With Secure Boot on, the module must be signed by a key the firmware trusts. DKMS signs it with its own machine owner key (`/var/lib/dkms/mok.key`, generated on first use); enrol it once with `sudo mokutil --import /var/lib/dkms/mok.pub` and reboot. Under lockdown, reading works but tuning does not ([1.11](01-concepts.md#111-lockdown)). |
| DKMS (permanent install only) | `dkms --version` | Arch, Debian, Ubuntu, Fedora: package `dkms`. |
| No ryzen_smu | `lsmod \| grep ryzen_smu` prints nothing | Do not run both. See [2.12](02-safety.md#212-what-the-module-does-not-protect-against). |

## 3.2 Trial: build and load by hand

```sh
git clone https://github.com/seamanticus/strix_halo_smu
cd strix_halo_smu
make                                  # module, shsctl and libshs, all into build/
sudo insmod build/module/strix_halo_smu.ko
sudo dmesg | grep strix_halo_smu
```

Everything `make` produces goes under `build/`; the source tree is never touched, and `make clean` simply deletes `build/`:

| Path | What |
|---|---|
| `build/module/strix_halo_smu.ko` | the kernel module (plus the kernel build system's intermediate files) |
| `build/user/shsctl` | the command-line tool |
| `build/user/libshs.so.0` | the C library |

To build against another kernel's headers, pass `KVER=<version>` or `KDIR=<headers path>`.

A successful load logs two lines like these:

```
strix_halo_smu: pmfw 10.100.2.0, interface 25, table 0x0064020c size 0x1028, AmdTable rev 2
strix_halo_smu: pm table at 0x..., 0x1028 bytes
```

Try it:

```sh
sudo build/user/shsctl info
sudo build/user/shsctl table
```

To open a gate for the trial, pass the parameter to `insmod`:

```sh
sudo insmod build/module/strix_halo_smu.ko allow_safeguard_writes=1
```

Unload:

```sh
sudo rmmod strix_halo_smu
```

> **Warning.** With `insmod`, nothing persists. After a reboot the module is not loaded, and every value you sent (power limits, Curve Optimizer) is gone, because the SMU forgets everything on reboot. Unloading the module does **not** undo values already sent; the SMU keeps them until reboot, suspend, or (for power limits) the next power-profile change. Set them back to default first (for example `shsctl co-all 0`) if you want them gone without a reboot.

## 3.3 Permanent install: `make install`

**DKMS** (Dynamic Kernel Module Support) keeps a copy of the source in `/usr/src` and rebuilds the module for every new kernel you install. Without it, a kernel update silently leaves you without the module.

From the repository directory:

```sh
make
sudo make install
```

Build first as yourself, then install as root, so that `build/` stays owned by you. `make install` does, in order:

1. **Checks** that it runs as root, that this machine is a GZ302EA, that `dkms` is installed and that the headers for the running kernel exist. If one is missing, it prints the command that installs it on Arch, Fedora or Debian/Ubuntu (and their derivatives), and stops.
2. **Module, via DKMS:** copies `Kbuild`, `dkms.conf`, `strix_halo_smu.c` and `strix_halo_smu.h` to `/usr/src/strix_halo_smu-<version>`, then `dkms add`, `build` and `install`. An older registration of the same version is removed first, so `make install` is also how you update.
3. **Tools:** `shsctl` to `/usr/local/bin`, `libshs.so.0` (and the `libshs.so` link) to `/usr/local/lib`, `shs.h` and `strix_halo_smu.h` to `/usr/local/include`, then `ldconfig`.
4. **Boot:** writes `/etc/modules-load.d/strix_halo_smu.conf`, so `systemd-modules-load` loads the module at every boot.
5. **Loads it now** with `modprobe` and tells you whether it loaded. If your firmware is not the verified one, the files stay installed and the message points you to `dmesg`.

Options:

| Variable | Default | Effect |
|---|---|---|
| `PREFIX` | `/usr/local` | where tools, library and headers go |
| `DESTDIR` | empty | stage tools, library, headers and the boot file under another root (for packaging) |
| `FORCE=1` | off | skip the GZ302EA board check (for packaging or CI on another machine) |
| `KVER` | running kernel | install for another installed kernel |

The individual steps are also targets of their own: `install-check`, `install-dkms`, `install-tools`, `install-boot`.

Check:

```sh
dkms status strix_halo_smu        # strix_halo_smu/0.1.0, <kernel>, x86_64: installed
modinfo strix_halo_smu            # shows the installed module and its parameters
```

### Set parameters permanently (optional)

Only if you need a gate open at every boot:

```sh
echo "options strix_halo_smu allow_safeguard_writes=1" | sudo tee /etc/modprobe.d/strix_halo_smu.conf
```

Think twice. The gates are closed by default so that a protection can never be weakened by accident.

`make uninstall` leaves this file in place, because it is your configuration, and says so.

### Keep ryzen_smu out (optional)

If ryzen_smu is installed for other tools, stop it loading:

```sh
echo "blacklist ryzen_smu" | sudo tee /etc/modprobe.d/no-ryzen_smu.conf
```

### Updating to a new version

```sh
git pull
make
sudo make uninstall     # removes every installed version
sudo make install
```

## 3.4 Removing it

```sh
sudo make uninstall
```

It unloads the module (if loaded and not in use), removes every DKMS-registered version and its `/usr/src` copy, the boot file, `shsctl`, the library and the headers, and runs `ldconfig`. It keeps `/etc/modprobe.d/strix_halo_smu.conf` if you created one, and prints a note. Pass the same `PREFIX` and `DESTDIR` you installed with.

Reboot to clear any values still held by the SMU and the kernel taint flag.

## 3.5 Troubleshooting

All messages from the module start with `strix_halo_smu:`. Read them with `sudo dmesg | grep strix_halo_smu`.

| What you see | Meaning | What to do |
|---|---|---|
| `insmod: ERROR: could not insert module ...: No such device` | One of the load checks failed. The log says which one. | Read the log line, below. |
| `unsupported CPU family ...` or `unsupported board '...'` | Not a GZ302EA. | The module only supports this machine. |
| `unexpected root complex` | The root PCI device is not `1022:1507`. | Same. |
| `firmware lock \_SB.AM00 or \_SB.ALIB missing` | The BIOS lacks the ACPI objects the module relies on. | A BIOS update changed the ACPI tables. Report it with your BIOS version. |
| `firmware contract not verified, refusing to bind` | One of the four contract values differs. The line before it shows the values found. | Your BIOS is newer (or older) than the verified one. Report the line before it, and your BIOS version, so the firmware can be verified. |
| `pm table ... overlaps System RAM, refusing` | The SMU reported a table address in normal RAM. | Should not happen on the verified firmware. Report it. |
| `pm table tctl ... layout not verified, table disabled` | The temperature check failed. | Reload the module. If it repeats, report it with the log. |
| `tctl check could not run at load` | The second table transfer at load did not come within 110 ms. | Retry loading. Report if it repeats. |
| `Invalid module format` | The module was built for a different kernel. | Rebuild (`make clean && make`), or use DKMS. |
| `Key was rejected by service` / `Operation not permitted` on load | Secure Boot or lockdown refuses unsigned modules. | Enrol the DKMS key (3.1, "Unsigned modules allowed"), or turn off Secure Boot. |
| `/dev/strix_halo_smu: No such file or directory` | The module is not loaded. | `sudo modprobe strix_halo_smu` |
| `make: *** /lib/modules/.../build: No such file or directory` | Kernel headers missing. | Install the headers package for your kernel (3.1). |
| `rsmu: 0x65 answered 0xfd, retry in 10 ms` | The first table transfer after boot or a long idle was refused once. | Nothing; this is normal and retried automatically. |
| `send budget exhausted` | A program sent more than 16 messages in a burst. | Wait a few seconds. If a program does this constantly, it is misbehaving. |
| `message 0x.. failed (-110), refusing further sends` | The SMU did not answer in 50 ms ([2.5](02-safety.md#25-when-the-smu-does-not-answer)). | Unload and reload the module. Report it if it repeats. |
| `cannot acquire \_SB.AM00` | The BIOS held its lock for more than 50 ms. | Retry; the message was not sent. |

To report a problem, include: `cat /sys/class/dmi/id/bios_version`, the full `dmesg | grep strix_halo_smu` output, and `uname -r`.

---

< Previous: [Safety model](02-safety.md) | [Manual](README.md) | Next: [Programming interface](04-interface.md) >
