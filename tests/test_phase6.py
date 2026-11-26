#!/usr/bin/env python3
"""Phase 6: first state-changing message, MP1 0x4C (Curve Optimizer, all cores).

Sequence: baseline (CO untouched) -> CO_STEPS -> 0, each measured under the same
single-core and all-core load, every load starting from the same idle Tctl.
CO is set back to 0 on any exit path. --control runs the same three states with
no sends, on a read-only fd, to measure drift.
Run on AC: sudo python3 tests/test_phase6.py [--control] > phase6_test.txt
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
MSG_CO_ALL = 0x4C
CO_STEPS = -5
TCTL_ABORT_C = 90.0
# Idle preconditions; run 2 (background benchmark) read 71 C idle and 1.94 GHz 1-core.
IDLE_TCTL_MAX_C = 45.0
BOOST_MIN_MHZ = 4500

# PM table 0x64020C: 16 cores, f32 each (RyzenAdj api.c get_core_*); Tctl as in the module.
CORES = 16
OFF_POWER, OFF_VOLT, OFF_TEMP, OFF_CLK, OFF_TCTL = 0xB90, 0xBD0, 0xC10, 0xC50, 0xB38

MSG_FMT = "<II6II"
TABLE_FMT = "<QIIQ"


def iowr(nr, size):
    return (3 << 30) | (size << 16) | (0xB9 << 8) | nr


IOC_MSG = iowr(0, struct.calcsize(MSG_FMT))
IOC_TABLE = iowr(1, struct.calcsize(TABLE_FMT))
libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)


def co_encode(steps):
    # 20-bit two's complement: G-Helper EncodeCurve, UXTU ConvertHsmpPsmMargin, RyzenAdj #398 (0xFFFE2 = -30).
    return steps & 0xFFFFF


def send_co(fd, steps):
    buf = bytearray(struct.pack(MSG_FMT, MP1, MSG_CO_ALL, co_encode(steps), 0, 0, 0, 0, 0, 0))
    fcntl.ioctl(fd, IOC_MSG, buf)
    rsp = struct.unpack(MSG_FMT, buf)[8]
    print(f"sent mp1 0x4c arg 0x{co_encode(steps):05x} ({steps:+d}): rsp 0x{rsp:02x}")
    return rsp


def table(fd):
    data = ctypes.create_string_buffer(0x1028)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), 0x1028, 0, 0))
    if libc.ioctl(fd, IOC_TABLE, buf):
        raise OSError(ctypes.get_errno(), "SHS_IOC_TABLE")
    raw = data.raw

    def arr(off):
        return struct.unpack_from(f"<{CORES}f", raw, off)

    return arr(OFF_POWER), arr(OFF_VOLT), arr(OFF_TEMP), arr(OFF_CLK), struct.unpack_from("<f", raw, OFF_TCTL)[0]


def spin(cpu):
    os.sched_setaffinity(0, {cpu})
    while True:
        pass


def hold(fd, seconds):
    # Keeps polling: the table averages over the time since the previous transfer.
    t_end = time.monotonic() + seconds
    while time.monotonic() < t_end:
        table(fd)
        time.sleep(0.1)


def cool_to(fd, target, timeout=240.0):
    """Polls (10 Hz, as the table averages) until Tctl is back at the start temperature."""
    t0 = time.monotonic()
    while True:
        tctl = table(fd)[4]
        if tctl <= target:
            return tctl, time.monotonic() - t0
        if time.monotonic() - t0 > timeout:
            print(f"  warning: Tctl {tctl:.1f} C still above {target:.1f} C after {timeout:.0f} s")
            return tctl, time.monotonic() - t0
        time.sleep(0.1)


def measure(fd, label, cpus, target, settle=3.0, dur=8.0):
    start, waited = cool_to(fd, target)
    print(f"  {label}: start Tctl {start:.1f} C after {waited:.0f} s cooling")
    procs = [mp.Process(target=spin, args=(c,), daemon=True) for c in cpus]
    for p in procs:
        p.start()
    samples = []
    try:
        t_end = time.monotonic() + settle + dur
        t_meas = time.monotonic() + settle
        while time.monotonic() < t_end:
            s = table(fd)
            if s[4] > TCTL_ABORT_C:
                raise RuntimeError(f"Tctl {s[4]:.1f} C above {TCTL_ABORT_C} C")
            if time.monotonic() >= t_meas:
                samples.append(s)
            time.sleep(0.1)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.join()

    power = [st.median(s[0][i] for s in samples) for i in range(CORES)]
    volt = [st.median(s[1][i] for s in samples) for i in range(CORES)]
    # Core clock is in GHz in this table version.
    clk = [st.median(s[3][i] for s in samples) * 1000 for i in range(CORES)]
    tctl = st.median(s[4] for s in samples)
    hot = max(range(CORES), key=lambda i: power[i])
    loaded = sorted(range(CORES), key=lambda i: -power[i])[: max(1, len(cpus) // 2)]
    print(f"{label}: n={len(samples)} tctl {tctl:.1f} C | busiest core {hot}: "
          f"{clk[hot]:.0f} MHz {volt[hot] * 1000:.1f} mV {power[hot]:.2f} W | "
          f"loaded cores mean: {st.mean(clk[i] for i in loaded):.0f} MHz "
          f"{st.mean(volt[i] for i in loaded) * 1000:.1f} mV, sum power {sum(power):.1f} W")
    return clk[hot], volt[hot], st.mean(clk[i] for i in loaded), st.mean(volt[i] for i in loaded)


def run_state(fd, name, target):
    single = measure(fd, f"{name:>8} 1-core", [0], target)
    allc = measure(fd, f"{name:>8} all", list(range(os.cpu_count())), target)
    return single[0], single[1], allc[2], allc[3]


def report(name, s, base):
    print(f"{name:>8} vs baseline: 1-core {(s[1] - base[1]) * 1000:+.1f} mV {s[0] - base[0]:+.0f} MHz | "
          f"all-core {(s[3] - base[3]) * 1000:+.1f} mV {s[2] - base[2]:+.0f} MHz")


def main():
    control = "--control" in sys.argv[1:]
    for p in ("allow_unlisted", "allow_safeguard_writes"):
        with open(f"{PARAMS}/{p}") as f:
            if f.read().strip() != "N":
                sys.exit(f"{p} is open; reload the module with defaults")
    if CO_STEPS < -10 or CO_STEPS > 0:
        sys.exit("CO_STEPS outside the approved range")

    # Control opens read-only: the module refuses set-class messages on it (EBADF).
    fd = os.open(DEV, os.O_RDONLY if control else os.O_RDWR)
    co_sent = False
    reset_rsp = None
    try:
        hold(fd, 5)
        idle = st.median(table(fd)[4] for _ in range(10))
        if idle > IDLE_TCTL_MAX_C:
            print(f"ABORT idle Tctl {idle:.1f} C above {IDLE_TCTL_MAX_C} C: machine not idle, nothing sent")
            return 2
        target = idle + 2.0
        print(f"{'control, no sends' if control else f'co {CO_STEPS:+d}'}; "
              f"each load starts at Tctl <= {target:.1f} C")

        base = run_state(fd, "baseline", target)
        # A background load holds the package budget and caps boost; the V/F curve is then not measured.
        if base[0] < BOOST_MIN_MHZ:
            print(f"ABORT baseline 1-core {base[0]:.0f} MHz below {BOOST_MIN_MHZ} MHz: "
                  "boost capped by another load, nothing sent")
            return 2

        if control:
            hold(fd, 1)
            test = run_state(fd, "ctl B", target)
            hold(fd, 1)
            after = run_state(fd, "ctl C", target)
        else:
            co_sent = True
            if send_co(fd, CO_STEPS) != 1:
                print("FAIL SMU did not accept the message")
                return 1
            hold(fd, 1)
            test = run_state(fd, f"co {CO_STEPS:+d}", target)

            co_sent = False
            reset_rsp = send_co(fd, 0)
            hold(fd, 1)
            after = run_state(fd, "co 0", target)
    finally:
        if co_sent:
            send_co(fd, 0)
        os.close(fd)

    report("ctl B" if control else f"co {CO_STEPS:+d}", test, base)
    report("ctl C" if control else "co 0", after, base)
    if reset_rsp is not None:
        print(f"reset rsp 0x{reset_rsp:02x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
