#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PM table averaging window and Tctl canary. Read-only: table reads and k10temp only.

1. After an all-core load, reads the table at long gaps while cooling; the
   canary must not trip (the table stays served, no EIO).
2. Tests the averaging hypothesis: for each gap, compares the table's Tctl with
   k10temp at read time and with the k10temp mean over the window since the
   previous read (k10temp sampled every ~5 ms in between).
Run: sudo python3 tests/test_table_window.py > table_window.txt
"""
import ctypes
import glob
import multiprocessing as mp
import os
import statistics as st
import struct
import sys
import time

DEV = "/dev/strix_halo_smu"
TABLE_FMT = "<QIIQ"
OFF_TCTL = 0xB38
GAPS = (0.055, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0)

libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)
IOC_TABLE = (3 << 30) | (struct.calcsize(TABLE_FMT) << 16) | (0xB9 << 8) | 1


def k10temp_path():
    for h in glob.glob("/sys/class/hwmon/hwmon*"):
        with open(f"{h}/name") as f:
            if f.read().strip() == "k10temp":
                return f"{h}/temp1_input"
    sys.exit("k10temp not found")


K10 = k10temp_path()


def k10():
    with open(K10) as f:
        return int(f.read()) / 1000


def table_tctl(fd):
    data = ctypes.create_string_buffer(0x1028)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), 0x1028, 0, 0))
    if libc.ioctl(fd, IOC_TABLE, buf):
        return None, ctypes.get_errno()
    return struct.unpack_from("<f", data.raw, OFF_TCTL)[0], 0


def spin(cpu):
    os.sched_setaffinity(0, {cpu})
    while True:
        pass


def heat(seconds):
    procs = [mp.Process(target=spin, args=(c,), daemon=True) for c in range(os.cpu_count())]
    for p in procs:
        p.start()
    time.sleep(seconds)
    for p in procs:
        p.terminate()
    for p in procs:
        p.join()


def window(fd, gap):
    """Sleeps gap while sampling k10temp, then reads the table."""
    samples = []
    t_end = time.monotonic() + gap
    while time.monotonic() < t_end:
        samples.append(k10())
        time.sleep(0.005)
    now = k10()
    tctl, err = table_tctl(fd)
    return tctl, err, now, st.mean(samples) if samples else now


def main():
    fd = os.open(DEV, os.O_RDONLY)
    fails = 0
    print("gap_s  n  inst_err_rms  mean_err_rms  inst_err_max  mean_err_max  eio")
    for gap in GAPS:
        table_tctl(fd)
        heat(10)
        table_tctl(fd)  # window starts at load end, where cooling is fastest
        inst, mean, eio = [], [], 0
        t_end = time.monotonic() + max(8.0, 4 * gap)
        while time.monotonic() < t_end:
            tctl, err, now, avg = window(fd, gap)
            if err:
                eio += 1
                continue
            inst.append(tctl - now)
            mean.append(tctl - avg)

        def rms(v):
            return (sum(x * x for x in v) / len(v)) ** 0.5 if v else float("nan")

        def mx(v):
            return max(map(abs, v)) if v else float("nan")

        print(f"{gap:5.3f} {len(inst):3d}  {rms(inst):12.2f}  {rms(mean):12.2f}  "
              f"{mx(inst):12.2f}  {mx(mean):12.2f}  {eio:3d}")
        fails += eio
    os.close(fd)
    print(f"{'PASS' if not fails else 'FAIL'} table served at every gap ({fails} EIO)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
