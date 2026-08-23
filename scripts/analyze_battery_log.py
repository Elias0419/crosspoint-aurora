#!/usr/bin/env python3
"""Attribute charge draw to components from battery.csv.

The gauge sits in series with the pack, so every row measures the TOTAL draw.
Attribution is a regression over many intervals: charge spent between two rows
= base * dt + extra_cpu * hi_clock + frontlight * (pct * on_time) + per_refresh
* refreshes + per_page * page_turns + wifi * wifi_time, with light-sleep time
subtracted from the awake term (the CPU is halted there and draws its own,
much smaller, amount).

Nothing here decides how much data is enough -- it reports the conditioning and
the noise floor and lets you look. Two loads that are always on together are
collinear no matter how many rows you collect: the CPU floor and the always-on
GT911 are the standing example, which is why CMD:PWRPROF exists.

Usage:  python scripts/analyze_battery_log.py battery.csv
"""
import argparse
import csv
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover
    print("needs numpy: pip install numpy", file=sys.stderr)
    raise SystemExit(1)

# name -> (column expression, unit of the fitted coefficient)
TERMS = [
    ("awake_mA", "hours awake (dt minus light sleep)", "mA"),
    ("lsleep_mA", "hours in light sleep", "mA"),
    ("cpu_hi_extra_mA", "hours at the high CPU clock", "mA"),
    ("frontlight_mA_per_pct", "hours of frontlight x brightness %", "mA/%"),
    ("wifi_mA", "hours with WiFi up", "mA"),
    ("mAh_per_refresh", "e-paper refreshes", "mAh"),
    ("mAh_per_page_turn", "page turns", "mAh"),
]


def intervals(rows):
    """Consecutive row pairs that describe one continuous stretch on battery."""
    out = []
    for a, b in zip(rows, rows[1:]):
        try:
            ua, ub = int(a["uptime_s"]), int(b["uptime_s"])
        except (KeyError, ValueError):
            continue
        if ub <= ua:
            continue  # reboot between the rows: counters and uptime restart
        if a.get("pg") != "0" or b.get("pg") != "0":
            continue  # on the cable: the gauge reports charge current, not load
        d = lambda k: int(b[k]) - int(a[k])  # noqa: E731
        dt = ub - ua
        lsleep = d("lsleep_ms") / 1000.0 if "lsleep_ms" in a else 0.0
        out.append(
            dict(
                dt=dt,
                dcap=int(a["remcap_mah"]) - int(b["remcap_mah"]),
                awake=max(0.0, dt - lsleep),
                lsleep=lsleep,
                hi=d("hi_clock_ms") / 1000.0,
                fl=int(a["fl_pct"]) * d("fl_on_ms") / 1000.0,
                wifi=d("wifi_on_ms") / 1000.0,
                refresh=d("refreshes"),
                pages=d("page_turns") if "page_turns" in a else 0,
            )
        )
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?", default="battery.csv")
    ap.add_argument("--min-dcap", type=int, default=0, help="drop intervals whose charge drop is below this (mAh)")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.csv)))
    iv = [x for x in intervals(rows) if x["dcap"] >= args.min_dcap]
    print(f"{len(rows)} rows -> {len(iv)} usable intervals on battery")
    if not iv:
        print("nothing to fit: the log is all cable time or all reboots", file=sys.stderr)
        return 1
    total_s = sum(x["dt"] for x in iv)
    total_mah = sum(x["dcap"] for x in iv)
    print(f"covered {total_s / 3600:.2f} h, {total_mah} mAh, mean {total_mah * 3600 / total_s:.1f} mA")

    A = np.array(
        [
            [x["awake"] / 3600, x["lsleep"] / 3600, x["hi"] / 3600, x["fl"] / 3600, x["wifi"] / 3600,
             x["refresh"], x["pages"]]
            for x in iv
        ]
    )
    y = np.array([x["dcap"] for x in iv], float)

    keep = [i for i in range(A.shape[1]) if np.count_nonzero(A[:, i]) > 0]
    dropped = [TERMS[i][0] for i in range(A.shape[1]) if i not in keep]
    if dropped:
        print("\nnever varied, cannot be estimated: " + ", ".join(dropped))
    Ak, names = A[:, keep], [TERMS[i][0] for i in keep]

    s = np.linalg.svd(Ak, compute_uv=False)
    cond = s[0] / s[-1] if s[-1] > 0 else float("inf")
    print(f"\nrows={Ak.shape[0]} unknowns={Ak.shape[1]} rank={np.linalg.matrix_rank(Ak)} cond={cond:.3g}")
    # Quantisation: RemainingCapacity moves in whole mAh, so each dcap is +-0.5.
    noise = np.array([0.5 * 3600 / x["dt"] for x in iv])
    print(f"quantisation noise per interval: {noise.min():.1f}..{noise.max():.1f} mA "
          f"(median {np.median(noise):.1f})")

    sol, *_ = np.linalg.lstsq(Ak, y, rcond=None)
    resid = y - Ak @ sol
    dof = max(1, Ak.shape[0] - Ak.shape[1])
    sigma2 = float(resid @ resid) / dof
    try:
        cov = sigma2 * np.linalg.inv(Ak.T @ Ak)
        se = np.sqrt(np.clip(np.diag(cov), 0, None))
    except np.linalg.LinAlgError:
        se = np.full(len(sol), float("nan"))

    print("\nfit (value +- 1 sigma):")
    for n, v, e in zip(names, sol, se):
        unit = next(u for k, _, u in TERMS if k == n)
        flag = ""
        if v < 0:
            flag = "   <- negative: not physical, the fit is under-determined"
        elif e > abs(v):
            flag = "   <- error exceeds the value"
        print(f"  {n:24s} {v:9.3f} +- {e:7.3f} {unit}{flag}")

    print("\nread this before believing any of it:")
    if cond > 100:
        print(f"  - cond={cond:.3g}: the columns are close to collinear, so the split between them is guesswork.")
    if Ak.shape[0] < 4 * Ak.shape[1]:
        print(f"  - {Ak.shape[0]} intervals for {Ak.shape[1]} unknowns is far too few; aim for tens per unknown.")
    if total_s < 12 * 3600:
        print(f"  - only {total_s / 3600:.1f} h of battery time; a day or two of ordinary reading is the target.")
    print("  - the CPU floor and the GT911 are always on together: no amount of logging separates them.")
    print("    Take those two from CMD:PWRPROF (scripts/power_probe.py --prof) and treat them as known.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
