#!/usr/bin/env python3
"""Fit the Peloton power -> speed curve from the owner's own ride exports.

    fit CSV...                 least-squares mph = a*x^3 + b*x^2 + c*x + d, x = sqrt(W),
                               on the median speed per watt (equal weight per watt);
                               single cubic and two cubics with a scanned knot
    --check LOG MILES          integrate a dfc_ble.py log (FTMS pow= samples) with the
                               current firmware curve and the new one, compare to MILES
    --coefs a,b,c,d            single-cubic coefficients for --check (default: the fit's result)
    --selftest                 recover a known cubic from rounded synthetic data

CSV columns (one row per second of a ride): t_s,output_w,cadence_rpm,resistance_pct,speed,speed_unit
Pure Python, no numpy.
"""
import argparse, csv, glob, math, re, sys

MPH_PER_KMH = 1.0 / 1.609344
KMH_PER_MPH = 1.609344

# Current firmware curve (PeloMon, CC-BY-NC) — used for comparison in --check only.
PELOMON_LOW = (-0.07605, 0.74063, -0.14023, 0.04660)
PELOMON_HIGH = (0.00087, -0.05685, 2.23594, -1.31158)


def pelomon_mph(w):
    if w <= 0:
        return 0.0
    c = PELOMON_LOW if w < 27.0 else PELOMON_HIGH
    rt = math.sqrt(w)
    v = ((c[0] * rt + c[1]) * rt + c[2]) * rt + c[3]
    return max(v, 0.0)


def cubic_mph(c, w):
    if w <= 0:
        return 0.0
    rt = math.sqrt(w)
    v = ((c[0] * rt + c[1]) * rt + c[2]) * rt + c[3]
    return max(v, 0.0)


def two_piece_mph(lo, hi, knot, w):
    if w <= 0:
        return 0.0
    return cubic_mph(lo if w < knot else hi, w)


# ---------------------------------------------------------------- linear algebra

def solve(A, b):
    n = len(A)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(M[r][col]))
        M[col], M[piv] = M[piv], M[col]
        if abs(M[col][col]) < 1e-300:
            raise ValueError("singular")
        for r in range(n):
            if r != col:
                f = M[r][col] / M[col][col]
                for k in range(col, n + 1):
                    M[r][k] -= f * M[col][k]
    return [M[i][n] / M[i][i] for i in range(n)]


def lstsq_cubic(pairs):
    """pairs: list of (W, mph). Returns (a, b, c, d) for mph = a x^3 + b x^2 + c x + d, x = sqrt(W)."""
    ATA = [[0.0] * 4 for _ in range(4)]
    ATb = [0.0] * 4
    for w, y in pairs:
        x = math.sqrt(w)
        row = (x ** 3, x ** 2, x, 1.0)
        for i in range(4):
            ATb[i] += row[i] * y
            for j in range(4):
                ATA[i][j] += row[i] * row[j]
    return tuple(solve(ATA, ATb))


# ---------------------------------------------------------------- data

def read_csv(path):
    pairs = []
    with open(path, newline="") as f:
        rd = csv.DictReader(f)
        for r in rd:
            try:
                w = float(r.get("output_w") or "")
                s = float(r.get("speed") or "")
            except ValueError:
                continue
            unit = (r.get("speed_unit") or "mph").lower()
            if "km" in unit:
                s *= MPH_PER_KMH
            if w >= 1.0 and s > 0.0:
                pairs.append((w, s))
    return pairs


def stats(pairs, fn):
    res = [(w, y - fn(w)) for w, y in pairs]
    rms = math.sqrt(sum(r * r for _, r in res) / len(res))
    mid = [abs(r) for w, r in res if 20 <= w <= 300]
    mx = max(mid) if mid else max(abs(r) for _, r in res)
    low = [r for w, r in res if w < 40]
    rms_low = math.sqrt(sum(r * r for r in low) / len(low)) if low else 0.0
    return rms, mx, rms_low, res


def monotonic(fn, lo=1, hi=600):
    prev = fn(lo)
    for w in range(lo + 1, hi + 1):
        v = fn(w)
        if v < prev - 1e-6:
            return False, w
        prev = v
    return True, None


def report(name, pairs, fn, coefs_text):
    rms, mx, rms_low, res = stats(pairs, fn)
    ok, at = monotonic(fn)
    print(f"\n== {name} ==")
    print(f"N={len(pairs)}  RMS={rms:.3f} mph  max|res| (20-300 W)={mx:.3f} mph  RMS(<40 W)={rms_low:.3f} mph")
    print("monotonic 1-600 W:", "yes" if ok else f"NO (drops at {at} W)")
    print("predicted mph:", "  ".join(f"{w}W={fn(w):.2f}" for w in (50, 100, 150, 200, 250, 300)))
    bins = {}
    for w, r in res:
        bins.setdefault(int(w // 25) * 25, []).append(r)
    print("residual mean/count per 25 W bin:")
    for b in sorted(bins):
        v = bins[b]
        print(f"  {b:>4}-{b+24:<4} W  mean={sum(v)/len(v):+.3f}  n={len(v)}")
    print(coefs_text)
    return rms, rms_low


def fmt(c):
    return ", ".join(f"{v:.6g}f" for v in c)


def per_watt_table(pairs):
    """Median displayed speed per integer watt: equal weight per watt so the
    sparse high-power end counts as much as the crowded 0-150 W region."""
    import statistics
    byw = {}
    for w, y in pairs:
        byw.setdefault(int(round(w)), []).append(y)
    return [(w, statistics.median(v)) for w, v in sorted(byw.items())]


def table_rms(table, fn, lo=1, hi=10**9):
    r = [m - fn(w) for w, m in table if lo <= w <= hi]
    return math.sqrt(sum(x * x for x in r) / len(r)), max(abs(x) for x in r)


def do_fit(files):
    pairs = []
    for pat in files:
        for p in sorted(glob.glob(pat)) or [pat]:
            n = read_csv(p)
            print(f"{p}: {len(n)} samples")
            pairs.extend(n)
    if len(pairs) < 50:
        print("not enough samples", file=sys.stderr)
        sys.exit(1)
    ws = [w for w, _ in pairs]
    table = per_watt_table(pairs)
    print(f"total {len(pairs)} samples, {min(ws):.0f}-{max(ws):.0f} W, {len(table)} distinct watt values")

    # single cubic, equal weight per watt
    c1 = lstsq_cubic(table)
    f1 = lambda w: cubic_mph(c1, w)
    t1, m1 = table_rms(table, f1)
    report("single cubic in sqrt(W)", pairs, f1,
           f"per-watt table: RMS={t1:.3f} max={m1:.3f} mph\n"
           f"static const float coefs[4] = {{{fmt(c1)}}};")

    # two cubics with a knot: the head unit changes formula at low power
    best = None
    for knot in range(8, 46):
        lo = [p for p in table if p[0] < knot]
        hi = [p for p in table if p[0] >= knot]
        if len(lo) < 5 or len(hi) < 20:
            continue
        cl, ch = lstsq_cubic(lo), lstsq_cubic(hi)
        fn = lambda w, cl=cl, ch=ch, knot=knot: two_piece_mph(cl, ch, knot, w)
        t, m = table_rms(table, fn)
        if best is None or t < best[0]:
            best = (t, m, knot, cl, ch, fn)
    t2, m2, knot, cl, ch, f2 = best
    report(f"two cubics, knot {knot} W", pairs, f2,
           f"per-watt table: RMS={t2:.3f} max={m2:.3f} mph\n"
           f"static const float coefs_low[4]  = {{{fmt(cl)}}};\n"
           f"static const float coefs_high[4] = {{{fmt(ch)}}};  /* knot {knot} W */")
    print("\nreference: current firmware curve on the same data: table RMS=%.3f sample RMS=%.3f"
          % (table_rms(table, pelomon_mph)[0], stats(pairs, pelomon_mph)[0]))
    if t2 < t1 * 0.8:
        print(f"\nrecommended: two cubics, knot {knot} W (table RMS {t2:.3f} vs {t1:.3f} mph)")
        return ("two-piece", f2)
    print(f"\nrecommended: single cubic (table RMS {t1:.3f} mph)")
    return ("single", f1)


# ---------------------------------------------------------------- --check

LOG_RE = re.compile(r"^(\d\d):(\d\d):(\d\d) FTMS .*pow=(\d+)W")


def integrate(path, fn):
    """Distance in miles from the pow= samples, piecewise constant between samples."""
    samples = []
    for line in open(path, errors="replace"):
        m = LOG_RE.match(line)
        if m:
            t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])
            samples.append((t, int(m[4])))
    miles = 0.0
    for (t0, p0), (t1, _) in zip(samples, samples[1:]):
        dt = t1 - t0
        if 0 <= dt <= 5:
            miles += fn(p0) * dt / 3600.0
    return miles, len(samples)


def do_check(log, target, fn):
    old, n = integrate(log, pelomon_mph)
    print(f"{log}: {n} samples")
    print(f"current firmware curve: {old:.3f} mi ({(old/target-1)*100:+.1f} % vs {target} mi)")
    if fn:
        new, _ = integrate(log, fn)
        print(f"new curve:              {new:.3f} mi ({(new/target-1)*100:+.1f} % vs {target} mi, {(new/old-1)*100:+.1f} % vs current)")


# ---------------------------------------------------------------- --selftest

def selftest():
    truth = (0.0012, -0.0631, 2.21, -1.25)
    pairs = []
    for w in range(20, 351):
        for k in range(3):
            y = round(cubic_mph(truth, w) + (k - 1) * 0.03, 1)   # one-decimal display rounding
            pairs.append((w, y))
    c = lstsq_cubic(pairs)
    rms = stats(pairs, lambda w: cubic_mph(c, w))[0]
    worst = max(abs(c[i] - truth[i]) / abs(truth[i]) for i in range(4))
    pred = max(abs(cubic_mph(c, w) - cubic_mph(truth, w)) for w in range(20, 351))
    print(f"selftest: RMS={rms:.4f} mph  worst coef error={worst*100:.2f} %  worst predicted error={pred:.4f} mph")
    ok = rms < 0.05 and pred < 0.05
    print("selftest:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", help="'fit'")
    ap.add_argument("files", nargs="*", help="CSV files or globs")
    ap.add_argument("--check", nargs=2, metavar=("LOG", "MILES"))
    ap.add_argument("--coefs", help="a,b,c,d")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    fn = None
    if a.coefs:
        c = tuple(float(v) for v in a.coefs.split(","))
        fn = lambda w: cubic_mph(c, w)
    if a.cmd == "fit":
        fn = do_fit(a.files)[1]
    elif a.cmd and not a.check:
        ap.error("unknown command")
    if a.check:
        do_check(a.check[0], float(a.check[1]), fn)


if __name__ == "__main__":
    main()
