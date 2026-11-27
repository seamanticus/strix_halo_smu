#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Long PM table capture with every temperature sensor the machine exposes. Read-only.

Profile: IDLE_S idle, LOAD_S all-core load, COOL_S cool-down, sampled at 10 Hz
(the table averages over the time since the previous read, so steady polling
keeps windows short). Output, in the current directory:
  capture-<profile>.bin   records: f64 time, f64 per sensor, 0x1028-byte table
  capture-<profile>.txt   one line per sensor, in record order
Run on AC: sudo python3 tests/capture_table.py [IDLE_S LOAD_S COOL_S]
"""
import ctypes
import glob
import multiprocessing as mp
import os
import struct
import sys
import time

DEV = "/dev/strix_halo_smu"
HANDLERS = "/sys/class/platform-profile"
TABLE_FMT = "<QIIQ"
TABLE_SIZE = 0x1028
PERIOD_S = 0.1
TCTL_ABORT_C = 90.0
IOC_TABLE = (3 << 30) | (struct.calcsize(TABLE_FMT) << 16) | (0xB9 << 8) | 1

libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)


def read(path):
    with open(path) as f:
        return f.read().strip()


def sensors():
    """(label, path, scale) for hwmon temperatures, fan speeds and ACPI thermal zones."""
    out = []
    for h in sorted(glob.glob("/sys/class/hwmon/hwmon*")):
        name = read(f"{h}/name")
        for p in sorted(glob.glob(f"{h}/temp*_input")):
            label = p.rsplit("/", 1)[1][:-6]
            if os.path.exists(f"{h}/{label}_label"):
                label = read(f"{h}/{label}_label")
            out.append((f"{name}/{label} C", p, 1000.0))
        for p in sorted(glob.glob(f"{h}/fan*_input")):
            out.append((f"{name}/{p.rsplit('/', 1)[1][:-6]} rpm", p, 1.0))
    for z in sorted(glob.glob("/sys/class/thermal/thermal_zone*")):
        out.append((f"{z.rsplit('/', 1)[1]}:{read(f'{z}/type')} C", f"{z}/temp", 1000.0))
    return out


def sample(paths):
    vals = []
    for _, p, scale in paths:
        try:
            vals.append(int(read(p)) / scale)
        except (OSError, ValueError):
            vals.append(float("nan"))
    return vals


def table(fd):
    data = ctypes.create_string_buffer(TABLE_SIZE)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), TABLE_SIZE, 0, 0))
    if libc.ioctl(fd, IOC_TABLE, buf):
        raise OSError(ctypes.get_errno(), "SHS_IOC_TABLE")
    return data.raw


def spin(cpu):
    os.sched_setaffinity(0, {cpu})
    while True:
        pass


def main():
    idle_s, load_s, cool_s = (float(a) for a in sys.argv[1:4]) if len(sys.argv) == 4 else (120, 600, 480)
    profiles = [read(f"{d}/profile") for d in sorted(glob.glob(f"{HANDLERS}/platform-profile-*"))]
    tag = "-".join(profiles) or "noprofile"
    paths = sensors()
    with open(f"capture-{tag}.txt", "w") as f:
        f.write("".join(f"{label}\n" for label, _, _ in paths))
    print(f"profiles {tag}; {len(paths)} sensors; idle {idle_s:.0f} s, load {load_s:.0f} s, cool {cool_s:.0f} s")

    tctl_off = 0xB38
    fd = os.open(DEV, os.O_RDONLY)
    procs, n = [], 0
    t0 = time.monotonic()
    try:
        with open(f"capture-{tag}.bin", "wb") as out:
            for phase, seconds in (("idle", idle_s), ("load", load_s), ("cool", cool_s)):
                if phase == "load":
                    procs = [mp.Process(target=spin, args=(c,), daemon=True) for c in range(os.cpu_count())]
                    for p in procs:
                        p.start()
                elif procs:
                    for p in procs:
                        p.terminate()
                    for p in procs:
                        p.join()
                    procs = []
                end = time.monotonic() + seconds
                nxt = time.monotonic()
                while time.monotonic() < end:
                    raw = table(fd)
                    vals = sample(paths)
                    out.write(struct.pack(f"<{1 + len(vals)}d", time.monotonic() - t0, *vals) + raw)
                    n += 1
                    tctl = struct.unpack_from("<f", raw, tctl_off)[0]
                    if tctl > TCTL_ABORT_C:
                        raise RuntimeError(f"Tctl {tctl:.1f} C above {TCTL_ABORT_C} C")
                    nxt += PERIOD_S
                    time.sleep(max(0.0, nxt - time.monotonic()))
                print(f"{phase} done at {time.monotonic() - t0:.0f} s, {n} records")
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.join()
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
