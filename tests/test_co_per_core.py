#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Per-core Curve Optimizer (MP1 0x4B) on one target core, with a reference core.

  map                  read-only: spin each logical CPU, report the busiest table core
  control CORE         no sends: the three states below, to measure drift
  flat CORE            arg = core << 20 | steps & 0xFFFF (G-Helper, UXTU)
  ccd CORE             arg = (core / 8) << 28 | (core % 8) << 20 | steps & 0xFFFF (ZenStates)

For cores 0-7 both encodings give the same word; they differ for 8-15. Each
experiment: baseline -> CO STEPS on CORE -> 0 on CORE, every state measured as a
1-core load on CORE and then on REF (same CCD), each starting from the same idle
Tctl. On any exit the target is set to 0 per core and all cores to 0 (0x4C).
Run on AC, at idle: sudo python3 tests/test_co_per_core.py MODE [CORE] > OUT.txt
"""
import ctypes
import fcntl
import multiprocessing as mp
import os
import statistics as st
import struct
import sys
import time

DEV = "/dev/strix_halo_smu"
PARAMS = "/sys/module/strix_halo_smu/parameters"
MP1 = 0
MSG_CO_PER_CORE, MSG_CO_ALL = 0x4B, 0x4C
STEPS = -10
CORES = 16
OFF_VOLT, OFF_CLK, OFF_POWER, OFF_TCTL = 0xBD0, 0xC50, 0xB90, 0xB38
IDLE_TCTL_MAX_C = 50.0
TCTL_ABORT_C = 90.0
BOOST_MIN_MHZ = 4500
MOVE_MV, STILL_MV, STILL_MHZ = 5.0, 3.0, 25.0

MSG_FMT = "<II6II"
TABLE_FMT = "<QIIQ"


def iowr(nr, size):
    return (3 << 30) | (size << 16) | (0xB9 << 8) | nr


IOC_MSG = iowr(0, struct.calcsize(MSG_FMT))
IOC_TABLE = iowr(1, struct.calcsize(TABLE_FMT))
libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)


def co_arg(enc, core, steps):
    m = steps & 0xFFFF
    if enc == "flat":
        return (core << 20) | m
    return ((core // 8) << 28) | ((core % 8) << 20) | m


def table(fd):
    data = ctypes.create_string_buffer(0x1028)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), 0x1028, 0, 0))
    if libc.ioctl(fd, IOC_TABLE, buf):
        raise OSError(ctypes.get_errno(), "SHS_IOC_TABLE")
    raw = data.raw

    def arr(off):
        return struct.unpack_from(f"<{CORES}f", raw, off)

    return arr(OFF_VOLT), arr(OFF_CLK), arr(OFF_POWER), struct.unpack_from("<f", raw, OFF_TCTL)[0]


def send(fd, msg, arg, label):
    buf = bytearray(struct.pack(MSG_FMT, MP1, msg, arg, 0, 0, 0, 0, 0, 0))
    fcntl.ioctl(fd, IOC_MSG, buf)
    rsp = struct.unpack(MSG_FMT, buf)[8]
    print(f"sent mp1 0x{msg:02x} arg 0x{arg:08x} ({label}): rsp 0x{rsp:02x}")
    return rsp


def spin(cpu):
    os.sched_setaffinity(0, {cpu})
    while True:
        pass


def hold(fd, seconds):
    t_end = time.monotonic() + seconds
    while time.monotonic() < t_end:
        table(fd)
        time.sleep(0.1)


def cool_to(fd, target, timeout=300.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if table(fd)[3] <= target:
            return
        time.sleep(0.1)
    print(f"  warning: Tctl still above {target:.1f} C after {timeout:.0f} s")


def load_core(fd, cpu, seconds):
    """Spins one logical CPU; returns per-sample (volt, clk, power) arrays and Tctl."""
    p = mp.Process(target=spin, args=(cpu,), daemon=True)
    p.start()
    samples = []
    try:
        t_meas = time.monotonic() + 3.0
        t_end = t_meas + seconds
        while time.monotonic() < t_end:
            s = table(fd)
            if s[3] > TCTL_ABORT_C:
                raise RuntimeError(f"Tctl {s[3]:.1f} C above {TCTL_ABORT_C} C")
            if time.monotonic() >= t_meas:
                samples.append(s)
            time.sleep(0.1)
    finally:
        p.terminate()
        p.join()
    return samples


def busiest(samples):
    power = [st.median(s[2][i] for s in samples) for i in range(CORES)]
    return max(range(CORES), key=lambda i: power[i])


def measure(fd, cpu, core, target, label):
    cool_to(fd, target)
    samples = load_core(fd, cpu, 8.0)
    hot = busiest(samples)
    if hot != core:
        raise RuntimeError(f"cpu {cpu} loaded table core {hot}, not {core}; run 'map' first")
    v = st.median(s[0][core] for s in samples) * 1000
    f = st.median(s[1][core] for s in samples) * 1000
    print(f"  {label:<18} core {core:2d}: {f:5.0f} MHz {v:7.1f} mV (n={len(samples)})")
    return v, f


def do_map(fd):
    for cpu in range(os.cpu_count()):
        samples = load_core(fd, cpu, 2.0)
        print(f"cpu {cpu:2d} -> table core {busiest(samples):2d}")
        hold(fd, 1)


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("map", "control", "flat", "ccd") or (args[0] != "map" and len(args) != 2):
        sys.exit(__doc__)
    for p in ("allow_unlisted", "allow_safeguard_writes"):
        with open(f"{PARAMS}/{p}") as f:
            if f.read().strip() != "N":
                sys.exit(f"{p} is open; reload the module with defaults")

    mode = args[0]
    if mode == "map":
        fd = os.open(DEV, os.O_RDONLY)
        try:
            do_map(fd)
        finally:
            os.close(fd)
        return 0

    core = int(args[1])
    if not 0 <= core < CORES:
        sys.exit("CORE must be 0-15")
    ref = core + 1 if core % 8 != 7 else core - 1
    # Linux numbers the first thread of each core as cpu N on this machine; 'map' checks it.
    cpu, ref_cpu = core, ref
    send_co = mode != "control"
    arg = co_arg(mode, core, STEPS) if send_co else None
    if send_co:
        print(f"{mode} encoding, core {core}, ref {ref}: CO {STEPS:+d} -> 0x{arg:08x}, reset 0x{co_arg(mode, core, 0):08x}")
    else:
        print(f"control, core {core}, ref {ref}: no sends")

    fd = os.open(DEV, os.O_RDWR if send_co else os.O_RDONLY)
    pending = False
    try:
        hold(fd, 5)
        idle = st.median(table(fd)[3] for _ in range(10))
        if idle > IDLE_TCTL_MAX_C:
            print(f"ABORT idle Tctl {idle:.1f} C above {IDLE_TCTL_MAX_C} C, nothing sent")
            return 2
        target = idle + 2.0
        print(f"idle Tctl {idle:.1f} C; every load starts at <= {target:.1f} C")

        res = {}
        for state in ("baseline", "co", "reset"):
            if state == "co" and send_co:
                pending = True
                if send(fd, MSG_CO_PER_CORE, arg, f"core {core} {STEPS:+d}") != 1:
                    print("FAIL SMU did not accept the message")
                    return 1
                hold(fd, 1)
            if state == "reset" and send_co:
                send(fd, MSG_CO_PER_CORE, co_arg(mode, core, 0), f"core {core} 0")
                pending = False
                hold(fd, 1)
            name = state if send_co else {"baseline": "baseline", "co": "ctl B", "reset": "ctl C"}[state]
            print(f"{name}:")
            res[state] = (measure(fd, cpu, core, target, "target"), measure(fd, ref_cpu, ref, target, "reference"))
            if state == "baseline" and res[state][0][1] < BOOST_MIN_MHZ:
                print(f"ABORT target clock {res[state][0][1]:.0f} MHz below {BOOST_MIN_MHZ}: boost capped, nothing sent")
                return 2
    finally:
        if pending:
            send(fd, MSG_CO_PER_CORE, co_arg(mode, core, 0), f"core {core} 0, exit path")
        if send_co:
            send(fd, MSG_CO_ALL, 0, "all cores 0, exit path")
        os.close(fd)

    def delta(state, which):
        (v, f), (v0, f0) = res[state][which], res["baseline"][which]
        return v - v0, f - f0

    for state in ("co", "reset"):
        for which, label in ((0, f"target core {core}"), (1, f"reference core {ref}")):
            dv, df = delta(state, which)
            print(f"{state:>5} vs baseline, {label}: {dv:+6.1f} mV {df:+5.0f} MHz")

    if not send_co:
        return 0
    tv, tf = delta("co", 0)
    rv, rf = delta("co", 1)
    xv, xf = delta("reset", 0)
    moved = tv <= -MOVE_MV or tf >= STILL_MHZ
    still = abs(rv) < STILL_MV and abs(rf) < STILL_MHZ
    back = abs(xv) < STILL_MV and abs(xf) < STILL_MHZ
    print(f"{'PASS' if moved else 'FAIL'} target moved (<= -{MOVE_MV:.0f} mV or >= +{STILL_MHZ:.0f} MHz)")
    print(f"{'PASS' if still else 'FAIL'} reference unaffected (< {STILL_MV:.0f} mV, < {STILL_MHZ:.0f} MHz)")
    print(f"{'PASS' if back else 'FAIL'} target back after per-core 0")
    return 0 if moved and still and back else 1


if __name__ == "__main__":
    sys.exit(main())
