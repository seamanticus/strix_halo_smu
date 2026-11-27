#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Power-limit enforcement under sustained all-core load.

Each experiment: cool to idle, all-core load, optionally one lowered limit sent
over MP1, then restored: the idle value is sent back over MP1 and, with amd_pmf
loaded, every platform-profile handler is cycled and set back to its own profile
so amd_pmf re-sends its SPL/FPPT/SPPT. Table words 0x00-0x3C, summed core power,
mean core clock and Tctl are logged at 10 Hz to <experiment>-<tag>.csv, written
even on abort. Enforcement is judged on the table's power values, not on labels.

  control  no send, 120 s load
  fast     MP1 0x15 = 45000 mW before the load, 60 s load
  slow     MP1 0x16 = 30000 mW 30 s into the load, 90 s load
  stapm    MP1 0x14 = 20000 mW before the load, 300 s load

Only lowered limits are sent. Runs in the current profiles, named in the output.
--no-pmf requires amd_pmf absent (boot with modprobe.blacklist=amd_pmf); the
restore is then MP1 only, and a reboot is the clean way back.
  sudo python3 tests/test_power_limits.py EXPERIMENT [--no-pmf] > OUT.txt
"""
import csv
import ctypes
import fcntl
import glob
import multiprocessing as mp
import os
import statistics as st
import struct
import sys
import time

DEV = "/dev/strix_halo_smu"
PARAMS = "/sys/module/strix_halo_smu/parameters"
HANDLERS = "/sys/class/platform-profile"
MP1 = 0
HOLD_SAMPLES = 10
IDLE_TCTL_MAX_C = 50.0
COOL_TIMEOUT_S = 300
TCTL_ABORT_C = 90.0
TOL_W = 2.0
WORDS = 16
CORES = 16
# Table power values (RyzenAdj generic layout, behaviour checked against k10temp; docs/05-pm-table.md).
W_STAPM_VAL, W_FAST_LIM, W_FAST_VAL, W_SLOW_LIM, W_SLOW_VAL = 1, 2, 3, 4, 5
LIMIT_WORDS = (0, 2, 4, 6)
OFF_CORE_POWER, OFF_CORE_CLK, OFF_TCTL = 0xB90, 0xC50, 0xB38

# name: (msg, lowered mW, send at load second, load seconds, power word judged)
EXPERIMENTS = {
    "control": (None, None, None, 120, None),
    "fast": (0x15, 45000, 0, 60, W_FAST_VAL),
    "slow": (0x16, 30000, 30, 90, W_SLOW_VAL),
    "stapm": (0x14, 20000, 0, 300, W_STAPM_VAL),
}

MSG_FMT = "<II6II"
TABLE_FMT = "<QIIQ"


def iowr(nr, size):
    return (3 << 30) | (size << 16) | (0xB9 << 8) | nr


IOC_MSG = iowr(0, struct.calcsize(MSG_FMT))
IOC_TABLE = iowr(1, struct.calcsize(TABLE_FMT))
libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p)


def sample(fd):
    data = ctypes.create_string_buffer(0x1028)
    buf = ctypes.create_string_buffer(struct.pack(TABLE_FMT, ctypes.addressof(data), 0x1028, 0, 0))
    if libc.ioctl(fd, IOC_TABLE, buf):
        raise OSError(ctypes.get_errno(), "SHS_IOC_TABLE")
    raw = data.raw
    words = struct.unpack_from(f"<{WORDS}f", raw, 0)
    power = sum(struct.unpack_from(f"<{CORES}f", raw, OFF_CORE_POWER))
    clk = st.mean(struct.unpack_from(f"<{CORES}f", raw, OFF_CORE_CLK)) * 1000
    return words, power, clk, struct.unpack_from("<f", raw, OFF_TCTL)[0]


def send(fd, msg, mw, t0):
    buf = bytearray(struct.pack(MSG_FMT, MP1, msg, mw, 0, 0, 0, 0, 0, 0))
    fcntl.ioctl(fd, IOC_MSG, buf)
    rsp = struct.unpack(MSG_FMT, buf)[8]
    print(f"{time.monotonic() - t0:7.2f} s  sent mp1 0x{msg:02x} arg {mw} mW: rsp 0x{rsp:02x}")
    return rsp


def read(path):
    with open(path) as f:
        return f.read().strip()


def handlers():
    """(profile file, handler name, current profile, choices) per platform-profile handler."""
    return [(f"{d}/profile", read(f"{d}/name"), read(f"{d}/profile"), read(f"{d}/choices").split())
            for d in sorted(glob.glob(f"{HANDLERS}/platform-profile-*"))]


def write(path, value):
    with open(path, "w") as f:
        f.write(value)


def held(rows, word, value):
    """True if word reads value for HOLD_SAMPLES consecutive samples, not just passing through."""
    n = 0
    for r in rows:
        n = n + 1 if abs(r[2 + word] - value) < 0.05 else 0
        if n >= HOLD_SAMPLES:
            return True
    return False


def spin(cpu):
    os.sched_setaffinity(0, {cpu})
    while True:
        pass


class Run:
    def __init__(self, fd):
        self.fd, self.rows, self.t0 = fd, [], time.monotonic()
        self.last_limits = None

    def poll(self, seconds, phase, stop_below=None):
        t_end = time.monotonic() + seconds
        while time.monotonic() < t_end:
            words, power, clk, tctl = sample(self.fd)
            now = time.monotonic() - self.t0
            self.rows.append((now, phase, *words, power, clk, tctl))
            limits = tuple(round(words[i], 2) for i in LIMIT_WORDS)
            if self.last_limits is not None and limits != self.last_limits:
                print(f"{now:7.2f} s  limit words 0x00/0x08/0x10/0x18 {self.last_limits} -> {limits} ({phase})")
            self.last_limits = limits
            if tctl > TCTL_ABORT_C:
                raise RuntimeError(f"Tctl {tctl:.1f} C above {TCTL_ABORT_C} C")
            if stop_below is not None and tctl <= stop_below:
                return True
            time.sleep(0.1)
        return stop_below is None

    def window(self, lo, hi):
        return [r for r in self.rows if lo <= r[0] < hi]

    def summarize(self, lo, hi, label):
        win = self.window(lo, hi)
        if not win:
            return None
        med = [st.median(r[2 + i] for r in win) for i in range(WORDS)]
        print(f"{label:>18} [{lo:5.0f}, {hi:5.0f}) s: n={len(win):4d} "
              f"core {st.median(r[2 + WORDS] for r in win):5.1f} W "
              f"clk {st.median(r[3 + WORDS] for r in win):5.0f} MHz "
              f"tctl {st.median(r[4 + WORDS] for r in win):5.1f} C")
        print(" " * 20 + " ".join(f"{i * 4:#06x}={v:6.2f}" for i, v in enumerate(med) if v))
        return med

    def save(self, path):
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "phase", *[f"w{i * 4:#06x}" for i in range(WORDS)],
                        "core_w", "clk_mhz", "tctl_c"])
            w.writerows(self.rows)


def verdict(run, name, low_w, judged, load_t0, send_t, end):
    # 0x0C tracks package power within ~1 s (M2); 0x14 and 0x04 are slow averages, reported only.
    if name == "fast":
        win = run.window(load_t0 + 1, load_t0 + 7)
        power = max(r[2 + W_FAST_VAL] for r in win)
        what = "0x0c peak in the first 7 s"
    else:
        win = run.window(end - 20, end)
        power = st.median(r[2 + W_FAST_VAL] for r in win)
        what = "0x0c median, last 20 s"
    avg = st.median(r[2 + judged] for r in run.window(end - 20, end))
    after = run.window(send_t + 0.5, end)
    landed = [f"{i * 4:#06x}" for i in range(WORDS) if held(after, i, low_w)]
    print(f"words holding {low_w:.2f} W for >= {HOLD_SAMPLES / 10:.0f} s after the send: {', '.join(landed) or 'none'}")
    ok = power <= low_w + TOL_W
    print(f"{'PASS' if ok else 'FAIL'} {what} {power:.2f} W vs limit {low_w:.2f} W "
          f"(averaged word {judged * 4:#06x} last 20 s: {avg:.2f} W)")
    return ok


def main():
    args = sys.argv[1:]
    no_pmf = "--no-pmf" in args
    args = [a for a in args if a != "--no-pmf"]
    if len(args) != 1 or args[0] not in EXPERIMENTS:
        sys.exit(f"usage: {sys.argv[0]} {'|'.join(EXPERIMENTS)} [--no-pmf]")
    name = args[0]
    msg, low, send_at, load_s, judged = EXPERIMENTS[name]
    for p in ("allow_unlisted", "allow_safeguard_writes"):
        with open(f"{PARAMS}/{p}") as f:
            if f.read().strip() != "N":
                sys.exit(f"{p} is open; reload the module with defaults")

    pmf = os.path.isdir("/sys/module/amd_pmf")
    if no_pmf and pmf:
        sys.exit("--no-pmf but amd_pmf is loaded; boot with modprobe.blacklist=amd_pmf. Nothing sent")
    if not no_pmf and not pmf:
        sys.exit("amd_pmf is not loaded; pass --no-pmf for that configuration. Nothing sent")
    hs = handlers()
    for path, hname, prof, choices in hs:
        print(f"handler {hname}: {prof} (choices {' '.join(choices)})")
    other = None
    if pmf:
        # Cycle through a profile every handler offers and none is in, so amd_pmf re-sends.
        common = set.intersection(*(set(h[3]) for h in hs)) if hs else set()
        other = next((c for c in ("performance", "balanced") if c in common
                      and all(h[2] != c for h in hs)), None)
        if other is None:
            sys.exit("no profile to cycle through for the restore; nothing sent")
    tag = f"{name}-{'nopmf' if no_pmf else 'pmf'}-{'-'.join(h[2] for h in hs) or 'noprofile'}"

    fd = os.open(DEV, os.O_RDWR if msg else os.O_RDONLY)
    run, procs, sent, ok = Run(fd), [], False, None
    try:
        if not run.poll(COOL_TIMEOUT_S, "cooling", stop_below=IDLE_TCTL_MAX_C):
            print(f"ABORT Tctl did not reach {IDLE_TCTL_MAX_C} C in {COOL_TIMEOUT_S} s, nothing sent")
            return 2
        run.poll(5, "idle")
        idle = run.window(run.rows[-1][0] - 5, run.rows[-1][0] + 1)
        fast_lim = st.median(r[2 + W_FAST_LIM] for r in idle)
        slow_lim = st.median(r[2 + W_SLOW_LIM] for r in idle)
        print(f"{name}: idle Tctl {st.median(r[4 + WORDS] for r in idle):.1f} C, "
              f"limits 0x08 {fast_lim:.2f} W, 0x10 {slow_lim:.2f} W")
        if msg is not None and low >= min(fast_lim, slow_lim) * 1000:
            print("ABORT the lowered value is not below the current limits, nothing sent")
            return 2
        restore_mw = round((slow_lim if msg == 0x16 else fast_lim) * 1000) if msg else None
        if msg == 0x14 and not pmf:
            # No table word shows the STAPM limit, so its prior value is unknown; lowered is the safe side.
            restore_mw = None
        print(f"amd_pmf {'loaded' if pmf else 'not loaded'}; restore: "
              f"{f'mp1 0x{msg:02x} {restore_mw} mW' if restore_mw else 'none (reboot)' if msg else 'none'}"
              f"{f', then profile cycle through {other}' if other else ''}")

        send_t = None
        if msg is not None and send_at == 0:
            sent = True
            send_t = time.monotonic() - run.t0
            if send(fd, msg, low, run.t0) != 1:
                return 1
        load_t0 = time.monotonic() - run.t0
        procs = [mp.Process(target=spin, args=(c,), daemon=True) for c in range(os.cpu_count())]
        for p in procs:
            p.start()
        if msg is not None and send_at:
            run.poll(send_at, "load")
            sent = True
            send_t = time.monotonic() - run.t0
            if send(fd, msg, low, run.t0) != 1:
                return 1
            run.poll(load_s - send_at, "load+limit")
        else:
            run.poll(load_s, "load+limit" if msg is not None else "load")
        end = load_t0 + load_s
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.join()
        try:
            if sent:
                if restore_mw:
                    send(fd, msg, restore_mw, run.t0)
                if other:
                    for path, _, _, _ in hs:
                        write(path, other)
                    run.poll(3, "restore")
                    for path, _, prof, _ in hs:
                        write(path, prof)
            run.poll(15, "cool")
        finally:
            run.save(f"{tag}.csv")
            os.close(fd)

    run.summarize(0, load_t0, "idle")
    run.summarize(load_t0, load_t0 + 7, "load first 7 s")
    if send_at:
        run.summarize(send_t - 10, send_t, "before send")
        run.summarize(send_t + 10, end, "after send +10 s")
    run.summarize(end - 20, end, "load last 20 s")
    restored = run.summarize(end + 10, end + 20, "after restore")
    if msg is not None:
        ok = verdict(run, name, low / 1000, judged, load_t0, send_t, end)
        back = restored and abs(restored[W_FAST_LIM] - fast_lim) < 0.05 and \
            abs(restored[W_SLOW_LIM] - slow_lim) < 0.05
        print(f"{'PASS' if back else 'FAIL'} limits back at 0x08 {fast_lim:.2f} W, 0x10 {slow_lim:.2f} W")
        ok = ok and back
    return 0 if ok in (None, True) else 1


if __name__ == "__main__":
    sys.exit(main())
