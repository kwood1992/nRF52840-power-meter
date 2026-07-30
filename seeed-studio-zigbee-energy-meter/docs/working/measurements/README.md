# Power-measurement baselines

Reference captures for the INA219 rig (0.1 Ω shunt in the Pi 3V3 → XIAO BAT path,
`/dev/i2c-1` @ 0x40). Keep this index current when you add a capture — a CSV with
no recorded verdict is worse than no CSV, because someone will diff against it.

## How to cross-check a change

1. Take a capture: `./tools/measure-power.sh <label> <seconds> <hz>`.
2. **Check the POR took** before believing anything — `./tools/xiao-por.sh`
   verifies and refuses on a contaminated baseline. See "reading a capture" below.
3. Compare against the matching row here. The comparison that matters is the
   **median**, not the mean: radio TX spikes reach ~30 mA and drag the mean and
   sd around without saying anything about steady-state draw.

Regenerate any summary line with:

```
python3 - <<'EOF'
import csv,sys
p='docs/working/measurements/<dir>/ina219.csv'
r=sorted(float(x['current_mA']) for x in csv.DictReader(l for l in open(p) if not l.startswith('#')))
n=len(r); print(f"n={n} mean={sum(r)/n:.3f} p50={r[n//2]:.3f} p95={r[int(n*.95)]:.3f} max={r[-1]:.3f}")
EOF
```

## Reading a capture: is the baseline even valid?

The CoreSight `CDBGPWRUP` latch adds a flat ~1.5 mA whenever it hasn't been
cleared by a POR, and `xiao-por.sh` used to fail to clear it silently about half
the time. Classify by median before drawing any conclusion:

| median | verdict | meaning |
|---|---|---|
| < 0.5 mA | **clean** | latch cleared, numbers are trustworthy |
| 1.0 – 2.2 mA | **contaminated** | POR didn't take — discard, re-POR, re-capture |
| > 2.2 mA | **busy** | device scanning/rejoining, latch state unjudgeable |

A clean sleep baseline sits near zero and is dominated by the INA219's ~−0.1 mA
zero offset — i.e. true draw is *below what this rig resolves*. Do not read a
negative mean as an error.

## Captures

| capture | fw | n | mean | p50 | max | verdict |
|---|---|---|---|---|---|---|
| `2026-07-30-103422-with-events` | `8f3ca41+dirty` | 301 | 1.731 | 1.700 | 2.4 | **contaminated** |
| `2026-07-30-144349-postpor-idle` | `b958171` | 3601 | 1.512 | 1.500 | 29.8 | **contaminated** |
| `2026-07-30-145209-pulse-burst` | `b958171+dirty` | 1501 | 1.526 | 1.500 | 5.4 | **contaminated** |
| `2026-07-30-150812-clean-idle-360` | `b958171+dirty` | 3601 | −0.097 | −0.100 | 5.8 | **clean** ← idle reference |
| `../ina219-2026-07-30-150257-por-railcheck.csv` | `b958171+dirty` | 301 | 2.765 | 1.400 | 9.3 | rail transient during POR |
| `../ina219-2026-07-30-150541-postpor2-floor.csv` | `b958171+dirty` | 1201 | −0.097 | −0.100 | 1.4 | **clean** (confirms the above) |
| `../ina219-2026-07-30-151520-clean-pulse-cost.csv` | `b958171+dirty` | 1201 | −0.018 | −0.100 | 3.5 | **clean**, D7 pulse control |

The three contaminated captures are kept deliberately. They are the reference
*shape* of a failed POR — a tight median plateau at ~1.5 mA — and they are what
`tools/tests/test-por-verify.sh` is seeded with. Do not delete them, and do not
cite them as results.

## Reference values (from `2026-07-30-150812-clean-idle-360`)

| quantity | value |
|---|---|
| idle floor, 360 s, n=3601 | −0.097 mA, 95% CI [−0.1035, −0.0898] |
| bus voltage | 3288–3316 mV stable |
| wake catches > 0.4 mA | 11 / 3601 (0.31%), max 5.8 mA |
| device transmissions in 360 s | exactly 1 (t≈296.7 s, 3.2 mA) — matches the 5-min report contract |
| dynamic charge (wake + radio) | 2100 µA·s / 360 s = **5.8 µA equivalent continuous** |
| per-pulse CPU cost | **not resolvable above noise** |

### The pulse-cost trap

Holding the D7 bench GPIO low sinks **+0.242 mA** through the pin's ~13 kΩ
pull-up. In a 50-pulse burst at 71 % low-duty the total delta was +0.204 mA
against +0.172 mA predicted from the sink alone — residual +0.032 mA, about
1 SEM. So the current measured during pulses is *entirely rig artifact*, and the
LPCOMP→PPI→TIMER2 path has no measurable CPU wake cost, as designed.

Any pulse-cost capture must subtract a duty-matched control or it overstates the
cost by roughly 6×.

### What these numbers do and don't settle

They do confirm the sleep architecture: the radio duty cycle is nowhere near the
battery-life limiter at 5.8 µA equivalent.

They do **not** settle battery life. The limiter is the XIAO's static overhead
(LDO + charge IC), which a 0.1 Ω shunt cannot resolve — everything below ~0.1 mA
is buried in the INA219's zero offset. At 20 µA total → 7.1 yr, 50 µA → 2.9 yr,
100 µA → 1.4 yr. Closing that needs a µA-capable instrument (PPK II). Treat
"multi-year" as plausible-but-unverified until then.
