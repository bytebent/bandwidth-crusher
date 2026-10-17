"""Checks the C++ DSP core against the Python reference (reference/bandwidth_fx.py).

usage: python3 tests/compare.py path/to/dsp_cli
"""
import os, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "reference"))
from bandwidth_fx import process

CLI = sys.argv[1]
SR = 48000
TMP = tempfile.mkdtemp()
rng = np.random.default_rng(0)

def test_signal(seconds=6, ch=2):
    t = np.arange(int(SR * seconds)) / SR
    chirp = 0.35 * np.sin(2 * np.pi * (150 + 250 * t) * t)
    gated = 0.3 * np.sin(2 * np.pi * 440 * t) * ((t * 2) % 1 < 0.4)
    kicks = 0.6 * np.exp(-((t % 0.5) * 30)) * np.sin(2 * np.pi * 60 * t)
    hats = 0.08 * rng.standard_normal(t.size) * np.exp(-((t + 0.25) % 0.25) * 80)
    left = chirp + kicks + hats
    right = gated + kicks + 0.7 * hats
    return np.stack([left, right][:ch]).astype(np.float32)

DEF = dict(budget_kbs=40, loop_ms=120, bits=12, randomness=0.3, chunk=1, smooth=0.5, memory=1.0,
           threshold=0, in_gain_db=0, out_gain_db=0, mix=1)

def run_cpp(x, host_block=512, **kw):
    p = dict(DEF); p.update(kw)
    split = p.pop("split", 0); seed = p.pop("seed", 1); sweep = p.pop("sweep", 0)
    inp, out = os.path.join(TMP, "in.f32"), os.path.join(TMP, "out.f32")
    x.astype(np.float32).tofile(inp)
    args = [CLI, inp, out, str(x.shape[0]), str(SR), str(host_block)] + [f"{k}={v}" for k, v in p.items()]
    args += [f"split={split}", f"seed={seed}", f"sweep={sweep}"]
    r = subprocess.run(args, capture_output=True, text=True, check=True)
    y = np.fromfile(out, dtype=np.float32).reshape(x.shape[0], -1)
    info = dict(kv.split("=") for kv in r.stderr.split())
    lat = int(info["latency"])
    y = np.concatenate([y[:, lat:], np.zeros((x.shape[0], lat), np.float32)], axis=1)  # undo latency
    return y, float(info["realtime_factor"])

def run_py(x, **kw):
    p = dict(DEF); p.update(kw)
    split = p.pop("split", 0)
    return process(x, SR, p, stereo_shared=not split, seed=1).astype(np.float32)

rms = lambda a: float(np.sqrt(np.mean(np.square(a))))
fails = 0
def check(name, ok, detail=""):
    global fails
    fails += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

x = test_signal()
lat_cut = 1000  # ignore the very end, which the latency shift pads with zeros

# 1) full budget is transparent (up to 16-bit quantisation)
quiet = (x * 0.5).astype(np.float32)   # stays below the output soft-limiter's knee
for kw in [dict(), dict(chunk=1024), dict(chunk=1024, memory=0.0), dict(smooth=1, chunk=300)]:
    y, _ = run_cpp(quiet, budget_kbs=100000, bits=16, **kw)
    err = np.abs(y - quiet)[:, :-lat_cut].max()
    check(f"transparent at full budget {kw}", err < 1e-3, f"max err {err:.2e}")

# 2) deterministic case matches Python closely: greedy, no ties to break differently
y_c, _ = run_cpp(x, budget_kbs=100000, bits=8)
y_p = run_py(x, budget_kbs=100000, bits=8)
d = np.abs(y_c - y_p)[:, :-lat_cut]
share = float(np.mean(d > 1e-4))
# float vs double can round a sample to the neighbouring 8-bit step; allow that, rarely
check("matches Python at full budget, 8 bit", d.max() <= 1.01/127 and share < 0.01,
      f"max diff {d.max():.2e}, {share*100:.3f}% of samples differ by one step")

# 3) degraded settings: same character as Python (error vs input within 15%)
configs = [dict(budget_kbs=40), dict(budget_kbs=10, randomness=0), dict(budget_kbs=10, randomness=1),
           dict(budget_kbs=15, chunk=1000, loop_ms=400, bits=16), dict(budget_kbs=20, memory=0.3, loop_ms=300),
           dict(budget_kbs=30, split=1), dict(budget_kbs=25, threshold=0.02), dict(budget_kbs=40, loop_ms=900, chunk=64)]
for kw in configs:
    y_c, _ = run_cpp(x, **kw); y_p = run_py(x, **kw)
    ec, ep = rms((y_c - x)[:, :-lat_cut]), rms((y_p - x)[:, :-lat_cut])
    check(f"same character as Python {kw}", abs(ec - ep) / max(ep, 1e-9) < 0.15, f"err C++ {ec:.4f} vs Py {ep:.4f}")

# 4) result does not depend on the host buffer size
base, _ = run_cpp(x, host_block=64, budget_kbs=15, chunk=500, randomness=0.6, memory=0.5)
for hb in (1000, 0):
    y, _ = run_cpp(x, host_block=hb, budget_kbs=15, chunk=500, randomness=0.6, memory=0.5)
    check(f"independent of host buffer size ({hb or 'random'} vs 64)", np.array_equal(y, base))

# 5) automating everything at once stays finite and below 0 dBFS
y, _ = run_cpp(test_signal(12), host_block=0, sweep=1)
check("full automation sweep is finite and limited", np.isfinite(y).all() and np.abs(y).max() < 1.0,
      f"peak {np.abs(y).max():.3f}")

# 6) mono works
y, _ = run_cpp(test_signal(ch=1))
check("mono", y.shape[0] == 1 and np.isfinite(y).all())

# 7) speed in the heaviest settings: 4 s loop, single-sample chunks, half random, stereo
_, rtf = run_cpp(test_signal(10), loop_ms=4000, chunk=1, randomness=0.5, budget_kbs=100)
y_c, _ = run_cpp(x, loop_ms=4000, chunk=1, randomness=0.0, budget_kbs=60); y_p = run_py(x, loop_ms=4000, chunk=1, randomness=0.0, budget_kbs=60)
ec, ep = rms((y_c - x)[:, :-lat_cut]), rms((y_p - x)[:, :-lat_cut])
check("long loop with sampled candidates keeps Python's character", abs(ec - ep) / ep < 0.15, f"err C++ {ec:.4f} vs Py {ep:.4f}")
check("fast enough at heaviest settings (>20x real time)", rtf > 20, f"{rtf:.0f}x real time")
_, rtf2 = run_cpp(test_signal(10))
print(f"info  default settings run at {rtf2:.0f}x real time")

print("\nall passed" if fails == 0 else f"\n{fails} failed")
sys.exit(1 if fails else 0)
