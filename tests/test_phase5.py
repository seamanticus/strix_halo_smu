#!/usr/bin/env python3
"""Phase 5 interface test for strix_halo_smu. Sends read-class messages only.

Every other case is refused by the module's gate before any hardware access;
the script aborts if either gate parameter is open, so that stays true.
Run: sudo python3 tests/test_phase5.py > phase5_test.txt
"""
import ctypes
import errno
import fcntl
import glob
import os
import struct
import sys
import time

DEV = "/dev/strix_halo_smu"
SYSFS = "/sys/class/misc/strix_halo_smu/pm_table"
STATS = "/sys/class/misc/strix_halo_smu/send_stats"
STAT_FMT = "<II6IiIQ"
PARAMS = "/sys/module/strix_halo_smu/parameters"
MP1, RSMU = 0, 1
TCTL_OFF = 0xB38
TAINT_CPU_OUT_OF_SPEC = 1 << 2


def iowr(nr, size):
    return (3 << 30) | (size << 16) | (0xB9 << 8) | nr


MSG_FMT = "<II6II"
TABLE_FMT = "<QIIQ"
IOC_MSG = iowr(0, struct.calcsize(MSG_FMT))
IOC_TABLE = iowr(1, struct.calcsize(TABLE_FMT))

fails = 0


def check(name, ok, detail=""):
    global fails
    fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}")


def msg(fd, mbox, m, args=()):
    a = list(args) + [0] * (6 - len(args))
    buf = bytearray(struct.pack(MSG_FMT, mbox, m, *a, 0))
    try:
        fcntl.ioctl(fd, IOC_MSG, buf)
    except OSError as e:
        return e.errno, None, None
    v = struct.unpack(MSG_FMT, buf)
    return 0, v[8], v[2:8]


def expect_errno(fd, name, mbox, m, want):
    err, _, _ = msg(fd, mbox, m)
    check(name, err == want, f"errno {errno.errorcode.get(err, err)} want {errno.errorcode[want]}")


libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)


def table(fd, size=0x2000):
    # libc directly: fcntl.ioctl drops the kernel's output when the call fails (ENOSPC).
    data = ctypes.create_string_buffer(size)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), size, 0, 0))
    err = 0 if libc.ioctl(fd, IOC_TABLE, buf) == 0 else ctypes.get_errno()
    _, sz, ver, t = struct.unpack(TABLE_FMT, buf.raw[:struct.calcsize(TABLE_FMT)])
    return err, sz, ver, t, data.raw[:sz]


def k10temp_mc():
    for h in glob.glob("/sys/class/hwmon/hwmon*"):
        with open(f"{h}/name") as f:
            if f.read().strip() == "k10temp":
                with open(f"{h}/temp1_input") as f:
                    return int(f.read())
    return None


def main():
    for p in ("allow_unlisted", "allow_safeguard_writes"):
        with open(f"{PARAMS}/{p}") as f:
            if f.read().strip() != "N":
                sys.exit(f"{p} is open; reload the module with defaults")

    with open("/proc/sys/kernel/tainted") as f:
        taint0 = int(f.read())

    fd = os.open(DEV, os.O_RDONLY)

    err, rsp, a = msg(fd, MP1, 0x01, [0x47])
    check("mp1 0x01 test", err == 0 and rsp == 1 and a[0] == 0x48, f"rsp {rsp} arg0 {a and hex(a[0])}")
    err, rsp, a = msg(fd, MP1, 0x02)
    check("mp1 0x02 pmfw", err == 0 and rsp == 1 and a[0] == 0x0A640200, f"{a and hex(a[0])}")
    err, rsp, a = msg(fd, MP1, 0x03)
    check("mp1 0x03 interface", err == 0 and rsp == 1 and a[0] == 25, f"{a and a[0]}")
    err, rsp, a = msg(fd, RSMU, 0x06)
    check("rsmu 0x06 table", err == 0 and rsp == 1 and a[:2] == (0x0064020C, 0x1028), f"{a and (hex(a[0]), hex(a[1]))}")
    err, rsp, a = msg(fd, RSMU, 0x66)
    check("rsmu 0x66 base", err == 0 and rsp == 1 and a[0] | a[1] << 32 == 0x205E0C0000, f"{a and hex(a[0] | a[1] << 32)}")

    # Refused by the gate; nothing reaches the SMU.
    expect_errno(fd, "unlisted mp1 0xc0", MP1, 0xC0, errno.ENOMSG)
    expect_errno(fd, "unlisted rsmu 0xaf", RSMU, 0xAF, errno.ENOMSG)
    expect_errno(fd, "safeguard mp1 0x19", MP1, 0x19, errno.EPERM)
    expect_errno(fd, "safeguard rsmu 0x1b", RSMU, 0x1B, errno.EPERM)
    expect_errno(fd, "rsmu 0x65 via msg", RSMU, 0x65, errno.EPERM)
    expect_errno(fd, "set mp1 0x4c on O_RDONLY", MP1, 0x4C, errno.EBADF)
    expect_errno(fd, "bad mailbox", 2, 0x01, errno.EINVAL)
    expect_errno(fd, "msg > 0xff", MP1, 0x101, errno.EINVAL)
    expect_errno(fd, "deny mp1 0x25", MP1, 0x25, errno.EPERM)
    expect_errno(fd, "deny mp1 0x26", MP1, 0x26, errno.EPERM)

    # Send budget: burst 16 per mailbox, 1/s refill. 3 MP1 sends above; gate refusals cost nothing.
    n = 0
    while n < 40:
        err, rsp, _ = msg(fd, MP1, 0x01, [n])
        if err:
            break
        n += 1
    check("mp1 budget exhausts with EAGAIN", err == errno.EAGAIN and 12 <= n <= 14, f"{n} admitted, then {errno.errorcode.get(err, err)}")
    err, rsp, a = msg(fd, RSMU, 0x06)
    check("rsmu budget independent of mp1", err == 0 and rsp == 1)
    time.sleep(1.1)
    err, rsp, _ = msg(fd, MP1, 0x01, [0x47])
    check("mp1 budget refills after 1 s", err == 0 and rsp == 1)

    with open(STATS, "rb") as f:
        stats = f.read()
    rec = struct.calcsize(STAT_FMT)
    check("send_stats size", len(stats) == 512 * rec, f"{len(stats)} bytes")
    adm, ref, a0, *_rest, pid, _pad, t = struct.unpack_from(STAT_FMT, stats, (MP1 * 256 + 0x01) * rec)
    check("send_stats mp1 0x01", adm >= n + 2 and ref >= 1 and a0 == 0x47 and pid == os.getpid(),
          f"admitted {adm} refused {ref} arg0 {hex(a0)} pid {pid}")
    adm25, ref25, *_ = struct.unpack_from(STAT_FMT, stats, (MP1 * 256 + 0x25) * rec)
    check("send_stats deny never admitted", adm25 == 0, f"admitted {adm25}")

    err, sz, _, _, _ = table(fd, 16)
    check("table small buffer", err == errno.ENOSPC and sz == 0x1028, f"errno {err} size {hex(sz)}")

    err, sz, ver, t1, d1 = table(fd)
    kt = k10temp_mc()
    tctl = struct.unpack_from("<f", d1, TCTL_OFF)[0] * 1000 if not err else None
    check("table read", err == 0 and sz == 0x1028 and ver == 0x0064020C, f"size {hex(sz)} ver {hex(ver)}")
    check("table time is CLOCK_MONOTONIC", 0 < time.clock_gettime_ns(time.CLOCK_MONOTONIC) - t1 < 10**9)
    check("table tctl vs k10temp", tctl is not None and kt is not None and abs(tctl - kt) < 3000,
          f"table {tctl and round(tctl)} k10temp {kt}")

    err, _, _, t2, _ = table(fd)
    check("second read inside 55 ms reuses snapshot", err == 0 and t2 == t1, f"dt {(t2 - t1) / 1e6:.2f} ms")
    time.sleep(0.06)
    err, _, _, t3, _ = table(fd)
    check("read after 60 ms refreshes", err == 0 and t3 > t1, f"dt {(t3 - t1) / 1e6:.2f} ms")

    with open(SYSFS, "rb") as f:
        s = f.read()
    check("sysfs pm_table", len(s) == 0x1028, f"{len(s)} bytes")

    os.close(fd)
    with open("/proc/sys/kernel/tainted") as f:
        taint1 = int(f.read())
    check("no new CPU_OUT_OF_SPEC taint", not ((taint1 & ~taint0) & TAINT_CPU_OUT_OF_SPEC), f"{taint0} -> {taint1}")

    print(f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
