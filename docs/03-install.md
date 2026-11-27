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
| Headers for the running kernel | `ls /lib/modules/$(uname -r)/build` exists | Arch: `linux-headers`, `linux-zen-headers`, ... Debian/Ubuntu: `linux-headers-$(uname -r)`. Fedora: `kernel-devel`. |
| Kernel built with `CONFIG_AMD_NODE=y` | `grep AMD_NODE /lib/modules/$(uname -r)/build/.config` | Provides `amd_smn_read/write`. True for all mainstream x86 distribution kernels. |
| Compiler and `make` | `cc --version`, `make --version` | Arch: `base-devel`. Debian/Ubuntu: `build-essential`. |
| Unsigned modules allowed | `cat /sys/kernel/security/lockdown` shows `[none]`, Secure Boot off | With Secure Boot on, sign the module with your own key (machine owner key, MOK) or it is refused. Under lockdown, reading works but tuning does not ([1.11](01-concepts.md#111-lockdown)). |
| DKMS (permanent install only) | `dkms --version` | Arch, Debian, Ubuntu, Fedora: package `dkms`. |
| No ryzen_smu | `lsmod \| grep ryzen_smu` prints nothing | Do not run both. See [2.12](02-safety.md#212-what-the-module-does-not-protect-against). |

## 3.2 Trial: build and load by hand

```sh
git clone https://github.com/seamanticus/strix_halo_smu
cd strix_halo_smu
make                          # builds strix_halo_smu.ko
make -C user                  # builds shsctl and libshs.so
sudo insmod strix_halo_smu.ko
sudo dmesg | grep strix_halo_smu
```

A successful load logs two lines like these:

```
strix_halo_smu: pmfw 10.100.2.0, interface 25, table 0x0064020c size 0x1028, AmdTable rev 2
strix_halo_smu: pm table at 0x..., 0x1028 bytes
```

Try it:

```sh
sudo user/shsctl info
sudo user/shsctl table
```

To open a gate for the trial, pass the parameter to `insmod`:

```sh
sudo insmod strix_halo_smu.ko allow_safeguard_writes=1
```

Unload:

```sh
sudo rmmod strix_halo_smu
```

> **Warning.** With `insmod`, nothing persists. After a reboot the module is not loaded, and every value you sent (power limits, Curve Optimizer) is gone, because the SMU forgets everything on reboot. Unloading the module does **not** undo values already sent; the SMU keeps them until reboot, suspend, or (for power limits) the next power-profile change. Set them back to default first (for example `shsctl co-all 0`) if you want them gone without a reboot.

## 3.3 Permanent install with DKMS

**DKMS** (Dynamic Kernel Module Support) keeps a copy of the source in `/usr/src` and rebuilds the module for every new kernel you install. Without it, a kernel update silently leaves you without the module.

From the repository directory:

```sh
sudo mkdir -p /usr/src/strix_halo_smu-0.1.0
sudo cp Makefile dkms.conf strix_halo_smu.c strix_halo_smu.h /usr/src/strix_halo_smu-0.1.0/
sudo dkms install strix_halo_smu/0.1.0
```

`dkms install` adds, builds and installs the module for the running kernel. Check:

```sh
dkms status strix_halo_smu        # strix_halo_smu/0.1.0, <kernel>, x86_64: installed
modinfo strix_halo_smu            # shows the installed module and its parameters
```

### Load it now

```sh
sudo modprobe strix_halo_smu
```

### Load it at every boot

```sh
echo strix_halo_smu | sudo tee /etc/modules-load.d/strix_halo_smu.conf
```

`systemd-modules-load` reads that directory at boot.

### Set parameters permanently (optional)

Only if you need a gate open at every boot:

```sh
echo "options strix_halo_smu allow_safeguard_writes=1" | sudo tee /etc/modprobe.d/strix_halo_smu.conf
```

Think twice. The gates are closed by default so that a protection can never be weakened by accident.

### Keep ryzen_smu out (optional)

If ryzen_smu is installed for other tools, stop it loading:

```sh
echo "blacklist ryzen_smu" | sudo tee /etc/modprobe.d/no-ryzen_smu.conf
```

### Install shsctl

```sh
make -C user
sudo install -m 0755 user/shsctl /usr/local/bin/shsctl
```

Optional, for programs written against the C library:

```sh
sudo install -m 0644 user/libshs.so /usr/local/lib/libshs.so
sudo install -m 0644 user/shs.h strix_halo_smu.h /usr/local/include/
sudo ldconfig
```

### Updating to a new version

Remove the old version (3.4, first two commands), copy the new source to `/usr/src/strix_halo_smu-<new version>` and run `dkms install strix_halo_smu/<new version>`.

## 3.4 Removing it

Undo every step of 3.3:

```sh
sudo modprobe -r strix_halo_smu                           # unload
sudo dkms remove strix_halo_smu/0.1.0 --all               # remove built modules for all kernels
sudo rm -r /usr/src/strix_halo_smu-0.1.0                  # remove the source copy
sudo rm -f /etc/modules-load.d/strix_halo_smu.conf        # stop loading at boot
sudo rm -f /etc/modprobe.d/strix_halo_smu.conf            # parameters, if you made it
sudo rm -f /usr/local/bin/shsctl                          # the tool
sudo rm -f /usr/local/lib/libshs.so /usr/local/include/shs.h /usr/local/include/strix_halo_smu.h
sudo ldconfig
```

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
| `Key was rejected by service` / `Operation not permitted` on load | Secure Boot or lockdown refuses unsigned modules. | Sign the module, or turn off Secure Boot. |
| `/dev/strix_halo_smu: No such file or directory` | The module is not loaded. | `sudo modprobe strix_halo_smu` |
| `make: *** /lib/modules/.../build: No such file or directory` | Kernel headers missing. | Install the headers package for your kernel (3.1). |
| `rsmu: 0x65 answered 0xfd, retry in 10 ms` | The first table transfer after boot or a long idle was refused once. | Nothing; this is normal and retried automatically. |
| `send budget exhausted` | A program sent more than 16 messages in a burst. | Wait a few seconds. If a program does this constantly, it is misbehaving. |
| `message 0x.. failed (-110), refusing further sends` | The SMU did not answer in 50 ms ([2.5](02-safety.md#25-when-the-smu-does-not-answer)). | Unload and reload the module. Report it if it repeats. |
| `cannot acquire \_SB.AM00` | The BIOS held its lock for more than 50 ms. | Retry; the message was not sent. |

To report a problem, include: `cat /sys/class/dmi/id/bios_version`, the full `dmesg | grep strix_halo_smu` output, and `uname -r`.

---

< Previous: [Safety model](02-safety.md) | [Manual](README.md) | Next: [Programming interface](04-interface.md) >
