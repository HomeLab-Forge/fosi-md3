"""Design the MD3's EQ presets for the ES9039Q2M programmable interpolation filter.

The DAC's 8x interpolator is a 2x FIR (128 taps, runs at 2*fs) followed by a 4x FIR (32 taps,
runs at 8*fs), each loaded from a coefficient RAM (datasheet v0.2.3, "Customizable Programmable
FIR Filters"). Replacing those coefficients replaces the built-in filter, so each custom preset
here is a full anti-imaging interpolator with an EQ shape folded into its 2x stage.

  * 4x stage: one 32-tap linear-phase lowpass, flat to 0.5*fs, stopband from 1.45*fs (where the
    first image of anything the 2x stage passes, up to its 0.55*fs stopband edge, can land). It scales with fs, so one set serves every rate.
  * 2x stage: a 128-tap linear-phase lowpass, passband to 0.45*fs, stopband from 0.55*fs, whose
    passband follows the preset's EQ curve (divided by the 4x stage's passband droop).

Both are designed by weighted least squares on a dense grid (type II linear phase: symmetric,
even length), solved with the normal equations in pure Python. The EQ's frequency axis is in Hz
while the filter's scales with fs, so every preset is designed separately for each sample-rate
group (44.1/48, 88.2/96, 176.4/192, 352.8/384 kHz), at the group's 48 kHz-family rate; the
44.1 kHz family plays the same coefficients, so its features sit about 8 % lower in frequency.

Every response is kept at or below 0 dB: a boost is realised by cutting the rest of the band,
and the firmware gives the level back with the DAC's volume registers (MAKEUP, in 0.5 dB steps).

Coefficient scale: nothing in the datasheet says what a coefficient of 1.0 is. We assume Q1.22:
the taps of a unity-DC-gain stage sum to 2**UNITY_LOG2. The firmware can rescale at run time
(USB control request EQ_SCALE) so the real scale can be found by ear; see docs/open-firmware.md.

Run from the repository root:  python3 tools/eq_design.py  (writes firmware/main/eq_coeffs.h and
prints the achieved responses).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware" / "main" / "eq_coeffs.h"

N2, N4 = 128, 32          # taps in the 2x and 4x stages
UNITY_LOG2 = 22           # assumed: unity DC gain <=> taps sum to 2**22
COEF_MAX = (1 << 23) - 1  # 24-bit signed
GROUPS = [(48000, "44.1/48 kHz"), (96000, "88.2/96 kHz"), (192000, "176.4/192 kHz"), (384000, "352.8/384 kHz")]
PASS_EDGE, STOP_EDGE = 0.45, 0.55      # 2x stage, in units of fs
PASS4_EDGE, STOP4_EDGE = 0.5, 1.45     # 4x stage, in units of fs (2 fs - the 2x stopband edge)
AUDIBLE_HZ = 20000.0
GATE_DB = 1.0             # a preset whose curve misses its target by more than this (30 Hz..20 kHz) falls back to Flat
GATE_IMAGES_DB = -60.0    # ... and so does one whose worst image (0.55..4 fs) is above this
CURVE_POINTS, CURVE_F_MIN, CURVE_F_MAX = 48, 200.0, 20000.0


def shelf(f: float, fc: float, p: float = 2.0) -> float:
    """0 well below fc, 1 well above, 0.5 at fc; smooth in log frequency."""
    if f <= 0:
        return 0.0
    return 1.0 / (1.0 + (fc / f) ** p)


def bell(f: float, fc: float, octaves: float) -> float:
    if f <= 0:
        return 0.0
    return math.exp(-0.5 * (math.log2(f / fc) / octaves) ** 2)


def tilt(f: float) -> float:
    """log10(f / 1 kHz) above 1 kHz with a soft knee, held from 20 kHz on."""
    if f <= 0:
        return 0.0
    u = math.log10(f / 1000.0)
    k = 0.08
    soft = k * math.log1p(math.exp(u / k)) if u / k < 50 else u
    return min(soft, math.log10(20.0))


# name, note shown on screen, makeup (0.5 dB steps), target in dB (peak 0 dB) or None for the built-in filter,
# and whether the preset is allowed above the 44.1/48 kHz group at all.
PRESETS = [
    ("Flat", "built-in filter F1", 0, None, True),
    ("Bass+", "+3.5 dB below 1 kHz", 7, lambda f: -3.5 * shelf(f, 1000), True),
    ("Bass-", "-3.5 dB below 1 kHz", 0, lambda f: -3.5 * (1 - shelf(f, 1000)), True),
    ("Mid+", "+3 dB around 2 kHz", 6, lambda f: -3.0 + 3.0 * bell(f, 2000, 1.0), False),
    ("Mid-", "-3 dB around 2 kHz", 0, lambda f: -3.0 * bell(f, 2000, 1.0), False),
    ("Treble+", "+3.5 dB above 5 kHz", 7, lambda f: -3.5 * (1 - shelf(f, 5000)), True),
    ("Treble-", "-3.5 dB above 5 kHz", 0, lambda f: -3.5 * shelf(f, 5000), True),
    ("Warm", "-2 dB at 10 kHz", 0, lambda f: -2.0 * tilt(f), True),
    ("Bright", "+1.5 dB from 3 kHz", 3, lambda f: -1.5 * (1 - shelf(f, 3500)), True),
]


# ---------------------------------------------------------------- linear algebra and FIR helpers

def solve(a: list[list[float]], b: list[float]) -> list[float]:
    """Gaussian elimination with partial pivoting (a and b are copied)."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[piv] = m[piv], m[col]
        p = m[col][col]
        if p == 0:
            raise ValueError("singular normal equations")
        for r in range(col + 1, n):
            k = m[r][col] / p
            if k:
                row, prow = m[r], m[col]
                for c in range(col, n + 1):
                    row[c] -= k * prow[c]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        x[r] = (m[r][n] - sum(m[r][c] * x[c] for c in range(r + 1, n))) / m[r][r]
    return x


def basis(n_taps: int, w: float) -> list[float]:
    """Type II amplitude basis: A(w) = sum_k h[k] * 2cos(w((N-1)/2 - k)), k < N/2."""
    half = n_taps // 2
    return [2.0 * math.cos(w * ((n_taps - 1) / 2.0 - k)) for k in range(half)]


def gram(n_taps: int, grid: list[tuple[float, float]]) -> tuple[list[list[float]], list[list[float]]]:
    """Normal matrix for points (w, weight); also returns the basis rows for reuse."""
    half = n_taps // 2
    g = [[0.0] * half for _ in range(half)]
    rows = []
    for w, wt in grid:
        b = basis(n_taps, w)
        rows.append(b)
        for i in range(half):
            bi = wt * b[i]
            gi = g[i]
            for j in range(i, half):
                gi[j] += bi * b[j]
    for i in range(half):
        for j in range(i):
            g[i][j] = g[j][i]
    return g, rows


def ls_design(n_taps: int, grid, desired, g=None, rows=None) -> list[float]:
    """Weighted least squares; grid = [(w, weight)], desired = amplitudes. Returns all n_taps taps."""
    if g is None:
        g, rows = gram(n_taps, grid)
    half = n_taps // 2
    r = [0.0] * half
    for (w, wt), d, b in zip(grid, desired, rows):
        if d:
            for i in range(half):
                r[i] += wt * d * b[i]
    h = solve(g, r)
    return h + h[::-1]


def amplitude(taps, w: float) -> float:
    """Real amplitude of a symmetric FIR at w (rad/sample), sign kept."""
    n = len(taps)
    return sum(t * math.cos(w * ((n - 1) / 2.0 - k)) for k, t in enumerate(taps))


def db(x: float) -> float:
    return 20.0 * math.log10(max(abs(x), 1e-12))


# ---------------------------------------------------------------- the two stages

def design_4x() -> list[float]:
    # x = f / fs; the stage runs at 8 fs, so w = 2 pi x / 8.
    grid, desired = [], []
    for i in range(301):
        x = PASS4_EDGE * i / 300
        grid.append((2 * math.pi * x / 8, 1.0))
        desired.append(1.0)
    for i in range(801):
        x = STOP4_EDGE + (4.0 - STOP4_EDGE) * i / 800
        grid.append((2 * math.pi * x / 8, 30.0))
        desired.append(0.0)
    taps = ls_design(N4, grid, desired)
    dc = sum(taps)
    return [t / dc for t in taps]


def grid_2x(fs: float) -> list[tuple[float, float]]:
    """Points (w at 2 fs, weight). Log-spaced passband so low frequencies count per octave."""
    pts = []
    f_lo, f_hi = 10.0, PASS_EDGE * fs
    n_log = 700
    for i in range(n_log + 1):
        f = f_lo * (f_hi / f_lo) ** (i / n_log)
        wt = 1.0 if f <= AUDIBLE_HZ else 0.05
        pts.append((math.pi * f / fs, wt))
    pts.append((0.0, 5.0))  # pin DC
    for i in range(1, 40):
        # Transition band: barely weighted, only to keep the solution from growing huge taps there.
        x = PASS_EDGE + (STOP_EDGE - PASS_EDGE) * i / 40
        pts.append((math.pi * x, 1e-3))
    for i in range(1201):
        x = STOP_EDGE + (1.0 - STOP_EDGE) * i / 1200
        pts.append((math.pi * x, 4000.0))
    return pts


def composite(c2, c4, f: float, fs: float, scale: float) -> float:
    """Linear amplitude of 2x then 4x at f (Hz), with integer taps divided by scale."""
    return amplitude(c2, math.pi * f / fs) * amplitude(c4, 2 * math.pi * f / (8 * fs)) / (scale * scale)


def image_rejection(c2, c4, fs: float, scale: float) -> float:
    """Worst composite gain (dB) anywhere an image can land: 0.55 fs .. 4 fs."""
    worst = -400.0
    for i in range(1, 2001):
        x = STOP_EDGE + (4.0 - STOP_EDGE) * i / 2000
        worst = max(worst, db(composite(c2, c4, x * fs, fs, scale)))
    return worst


def quantize(taps: list[float]) -> list[int]:
    out = [round(t * (1 << UNITY_LOG2)) for t in taps]
    if max(abs(c) for c in out) > COEF_MAX:
        raise ValueError("coefficient exceeds 24 bits")
    return out


def main() -> int:
    unity = float(1 << UNITY_LOG2)
    print("Designing the 4x stage (32 taps at 8 fs) ...")
    h4 = design_4x()
    c4 = quantize(h4)
    ripple4 = [db(amplitude(c4, 2 * math.pi * (PASS4_EDGE * i / 50) / 8) / unity) for i in range(51)]
    stop4 = max(db(amplitude(c4, 2 * math.pi * (STOP4_EDGE + (4 - STOP4_EDGE) * i / 400) / 8) / unity)
                for i in range(401))
    print(f"  passband 0..0.5 fs: {min(ripple4):+.3f} .. {max(ripple4):+.3f} dB; "
          f"stopband from {STOP4_EDGE} fs: {stop4:.1f} dB")
    print(f"  tap sum {sum(c4)} (unity {1 << UNITY_LOG2}), largest |tap| {max(abs(c) for c in c4)}")

    sets: list[list[int]] = []
    table = []      # [group][preset] -> set index or -1
    curves = []     # [group][preset] -> centi-dB list
    check_hz = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000, 20000]
    curve_hz = [CURVE_F_MIN * (CURVE_F_MAX / CURVE_F_MIN) ** (i / (CURVE_POINTS - 1)) for i in range(CURVE_POINTS)]
    for fs, label in GROUPS:
        print(f"\n=== {label} (designed at fs = {fs} Hz; 2x stage runs at {2 * fs} Hz) ===")
        grid = grid_2x(fs)
        g, rows = gram(N2, grid)
        row_idx, row_curve = [], []
        for name, _note, makeup, target, high_ok in PRESETS:
            if target is None:
                row_idx.append(-1)
                row_curve.append([0] * CURVE_POINTS)
                print(f"  {name:8s} built-in FILTER_SHAPE 0 (no custom coefficients)")
                continue
            if not high_ok and fs != GROUPS[0][0]:
                row_idx.append(-1)
                row_curve.append([0] * CURVE_POINTS)
                print(f"  {name:8s} not offered at this rate (falls back to Flat)")
                continue
            desired = []
            edge = 10 ** (target(PASS_EDGE * fs) / 20)
            for (w, wt) in grid:
                f = w * fs / math.pi
                if f <= PASS_EDGE * fs + 1e-6:
                    a4 = amplitude(h4, 2 * math.pi * f / (8 * fs))
                    desired.append(10 ** (target(f) / 20) / a4)
                elif f < STOP_EDGE * fs:
                    desired.append(edge * (STOP_EDGE * fs - f) / ((STOP_EDGE - PASS_EDGE) * fs))
                else:
                    desired.append(0.0)
            h2 = ls_design(N2, grid, desired, g, rows)
            # Peak gain at or below 0 dB, ripple included.
            peak = max(abs(amplitude(h2, math.pi * f / fs) * amplitude(h4, 2 * math.pi * f / (8 * fs)))
                       for f in [PASS_EDGE * fs * i / 600 for i in range(601)])
            if peak > 1.0:
                h2 = [t / peak for t in h2]
            if max(abs(t) for t in h2) * (1 << UNITY_LOG2) > COEF_MAX:
                print(f"  {name:8s} needs taps beyond 24 bits at this rate -> FALLS BACK TO FLAT")
                row_idx.append(-1)
                row_curve.append([0] * CURVE_POINTS)
                continue
            c2 = quantize(h2)
            err = 0.0
            for i in range(200):
                f = 30.0 * (AUDIBLE_HZ / 30.0) ** (i / 199)
                err = max(err, abs(db(composite(c2, c4, f, fs, unity)) - target(f)))
            rej = image_rejection(c2, c4, fs, unity)
            ok = err <= GATE_DB and rej <= GATE_IMAGES_DB
            got = " ".join(f"{db(composite(c2, c4, f, fs, unity)) + makeup / 2:+5.1f}" for f in check_hz)
            want = " ".join(f"{target(f) + makeup / 2:+5.1f}" for f in check_hz)
            print(f"  {name:8s} makeup {makeup / 2:.1f} dB, max error {err:.2f} dB (30 Hz-20 kHz), "
                  f"images <= {rej:.1f} dB, tap sum {sum(c2)}, max |tap| {max(abs(c) for c in c2)}"
                  f"{'' if ok else '  -> FALLS BACK TO FLAT'}")
            print(f"           target  {want}")
            print(f"           achieved{got}")
            if not ok:
                row_idx.append(-1)
                row_curve.append([0] * CURVE_POINTS)
                continue
            row_idx.append(len(sets))
            sets.append(c2)
            row_curve.append([round(100 * (db(composite(c2, c4, f, fs, unity)) + makeup / 2)) for f in curve_hz])
        table.append(row_idx)
        curves.append(row_curve)
    print("\n  check frequencies (Hz): " + " ".join(f"{f:g}" for f in check_hz))
    write_header(c4, sets, table, curves)
    print(f"\nwrote {HEADER.relative_to(ROOT)}: {len(sets)} 2x-stage sets")
    return 0


def c_array(values: list[int], indent: str, per_line: int = 8) -> str:
    lines = []
    for i in range(0, len(values), per_line):
        lines.append(indent + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(lines)


def write_header(c4, sets, table, curves) -> None:
    out = []
    out.append("// Generated by tools/eq_design.py. Do not edit; change the script and run it again.")
    out.append("// EQ presets for the ES9039Q2M programmable 8x interpolator (2x stage then 4x stage).")
    out.append("// Coefficients assume a unity-DC-gain stage sums to 2^EQ_COEF_UNITY_LOG2 (Q1.22, not")
    out.append("// documented by ESS); the firmware rescales them at run time (eq_set_scale).")
    out.append("#pragma once\n\n#include <stdint.h>\n")
    out.append(f"#define EQ_PRESET_COUNT    {len(PRESETS)}")
    out.append(f"#define EQ_GROUP_COUNT     {len(GROUPS)}")
    out.append(f"#define EQ_FIR2X_TAPS      {N2}")
    out.append(f"#define EQ_FIR4X_TAPS      {N4}")
    out.append(f"#define EQ_COEF_UNITY_LOG2 {UNITY_LOG2}")
    out.append(f"#define EQ_CURVE_POINTS    {CURVE_POINTS}")
    out.append(f"#define EQ_CURVE_F_MIN     {int(CURVE_F_MIN)}")
    out.append(f"#define EQ_CURVE_F_MAX     {int(CURVE_F_MAX)}")
    out.append(f"#define EQ_FIR2X_SETS      {len(sets)}\n")
    out.append("// Preset names and on-screen notes, in menu order. Preset 0 is the built-in filter.")
    out.append("static const char *const k_eq_name[EQ_PRESET_COUNT] = {"
               + ", ".join(f'"{p[0]}"' for p in PRESETS) + "};")
    out.append("static const char *const k_eq_note[EQ_PRESET_COUNT] = {")
    out.append("    " + ", ".join(f'"{p[1]}"' for p in PRESETS) + ",\n};")
    out.append("// Volume make-up per preset, in 0.5 dB steps (applied only while the custom filter is in use).")
    out.append("static const uint8_t k_eq_makeup[EQ_PRESET_COUNT] = {" + ", ".join(str(p[2]) for p in PRESETS) + "};\n")
    out.append("// Sample-rate groups: 0 = 44.1/48 kHz, 1 = 88.2/96, 2 = 176.4/192, 3 = 352.8/384.")
    out.append("// Index into k_eq_fir2x per group and preset; -1 = use the built-in filter (Flat).")
    out.append("static const int8_t k_eq_set[EQ_GROUP_COUNT][EQ_PRESET_COUNT] = {")
    for row in table:
        out.append("    {" + ", ".join(str(v) for v in row) + "},")
    out.append("};\n")
    out.append("// 4x stage, shared by every preset and rate: 32-tap lowpass at 8 fs.")
    out.append(f"static const int32_t k_eq_fir4x[EQ_FIR4X_TAPS] = {{\n{c_array(c4, '    ')}\n}};\n")
    out.append("// 2x stage sets: 128-tap lowpass at 2 fs with the preset's EQ folded in.")
    out.append("static const int32_t k_eq_fir2x[EQ_FIR2X_SETS][EQ_FIR2X_TAPS] = {")
    for s in sets:
        out.append(f"    {{\n{c_array(s, '        ')}\n    }},")
    out.append("};\n")
    out.append("// Response of the quantized taps (2x x 4x) plus make-up, in 0.01 dB, at EQ_CURVE_POINTS")
    out.append("// log-spaced frequencies from EQ_CURVE_F_MIN to EQ_CURVE_F_MAX, per group and preset.")
    out.append("static const int16_t k_eq_curve[EQ_GROUP_COUNT][EQ_PRESET_COUNT][EQ_CURVE_POINTS] = {")
    for g in curves:
        out.append("    {")
        for c in g:
            out.append(f"        {{{', '.join(str(v) for v in c)}}},")
        out.append("    },")
    out.append("};")
    HEADER.write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    sys.exit(main())
