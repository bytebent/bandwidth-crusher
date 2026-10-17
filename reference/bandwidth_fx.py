"""
Bandwidth-limited loop buffer effect ("Bad Apple" style, but for audio).

A loop buffer plays on repeat. The input keeps writing what the buffer
*should* contain (the target). Only a limited number of bytes per second may
actually be copied into the playing buffer, so un-updated parts keep playing
stale audio from earlier passes.

All parameters are given per control block (default 10 ms), so they can be
automated. Everything is numpy-vectorised per block so it runs fine in Pyodide.
"""
import numpy as np

BLOCK_MS = 10.0


def db_to_lin(db):
    return 10.0 ** (np.asarray(db, dtype=np.float64) / 20.0)


def soft_limit(x, knee=0.8):
    """Pass-through below `knee`, smooth tanh saturation above it (peak < 1.0)."""
    a = np.abs(x)
    over = a > knee
    if not np.any(over):
        return x
    y = x.copy()
    head = 1.0 - knee
    y[over] = np.sign(x[over]) * (knee + head * np.tanh((a[over] - knee) / head))
    return y


def quantize(x, bits):
    """Round to a signed integer grid with `bits` bits."""
    levels = float(2 ** (int(bits) - 1) - 1)
    if levels < 1:
        levels = 1.0
    return np.round(np.clip(x, -1.0, 1.0) * levels) / levels


class BandwidthFX:
    """
    Stateful processor. Call `step(n_blocks)` repeatedly until it returns True,
    then read `self.out` (shape: channels x samples).

    audio:   float array, channels x samples, range about -1..1
    sr:      sample rate
    params:  dict of per-block float arrays (length = n_blocks):
               budget_kbs, loop_ms, bits, randomness, chunk (samples),
               smooth (0..1 chunk edge crossfade), memory (0..1, fraction of
               stale audio left after 1 s), threshold, in_gain_db, out_gain_db, mix
    stereo_shared: True = one budget shared by all channels,
                   False = budget split evenly per channel
    seed:    random seed for repeatable renders
    """

    PARAM_NAMES = ("budget_kbs", "loop_ms", "bits", "randomness", "chunk",
                   "smooth", "memory", "threshold", "in_gain_db", "out_gain_db", "mix")
    # Defaults for params added later, so older callers/presets keep working.
    PARAM_DEFAULTS = {"smooth": 0.5, "memory": 1.0}

    def __init__(self, audio, sr, params, stereo_shared=True, seed=1,
                 block_ms=BLOCK_MS):
        # float32 keeps memory down for long songs (matters in the browser)
        self.x = np.atleast_2d(np.asarray(audio, dtype=np.float32))
        self.ch, self.n = self.x.shape
        self.sr = int(sr)
        self.block = max(16, int(round(self.sr * block_ms / 1000.0)))
        self.n_blocks = int(np.ceil(self.n / self.block))
        self.params = {}
        for k in self.PARAM_NAMES:
            arr = np.asarray(params.get(k, self.PARAM_DEFAULTS.get(k)), dtype=np.float64).ravel()
            if arr.size == 1:
                arr = np.full(self.n_blocks, arr[0])
            if arr.size < self.n_blocks:  # pad with last value
                arr = np.concatenate([arr, np.full(self.n_blocks - arr.size, arr[-1])])
            self.params[k] = arr
        self.shared = bool(stereo_shared)
        self.rng = np.random.default_rng(int(seed))
        self.out = np.zeros_like(self.x)

        self.L = None          # current loop length in samples
        self.target = None     # what the buffer should be (ch x L)
        self.buf = None        # what is actually playing (ch x L)
        self.pos = 0           # play/write pointer inside the loop
        self.pool = 0.0        # unspent sample budget, saved up between blocks
        self.last_out = np.zeros(self.ch)
        self.declick = None    # (offset per channel) applied after a resize
        self.b = 0             # next block index

    # ---- loop buffer management -------------------------------------------
    def _resize(self, new_L):
        if self.L is None:
            self.L = new_L
            self.target = np.zeros((self.ch, new_L))
            self.buf = np.zeros((self.ch, new_L))
            self.pos = 0
            return
        old_L = self.L
        # Resample the loop contents so the pointer keeps its relative phase.
        src = np.linspace(0, old_L, new_L, endpoint=False)
        idx = np.arange(old_L + 1)
        def stretch(a):
            ext = np.concatenate([a, a[:, :1]], axis=1)
            return np.stack([np.interp(src, idx, ext[c]) for c in range(self.ch)])
        self.target = stretch(self.target)
        self.buf = stretch(self.buf)
        self.pos = int(self.pos * new_L / old_L) % new_L
        self.L = new_L
        self.declick = True

    # ---- one control block -------------------------------------------------
    def _process_block(self, b):
        p = {k: v[b] for k, v in self.params.items()}
        s0 = b * self.block
        s1 = min(self.n, s0 + self.block)
        m = s1 - s0

        new_L = max(8, int(round(p["loop_ms"] * self.sr / 1000.0)))
        if self.L is None or abs(new_L - self.L) > max(1, 0.01 * self.L):
            self._resize(new_L)
        L = self.L

        bits = int(np.clip(round(p["bits"]), 2, 16))
        dry = self.x[:, s0:s1] * db_to_lin(p["in_gain_db"])

        # 1) The input writes the *target* at the positions being played now.
        positions = (self.pos + np.arange(m)) % L
        self.target[:, positions] = quantize(dry, bits)

        # 2) Spend the byte budget on updating the playing buffer.
        bytes_per_sample = bits / 8.0
        earned = p["budget_kbs"] * 1000.0 * (m / self.sr) / bytes_per_sample
        chunk = int(np.clip(round(p["chunk"]), 1, L))
        # Save up unspent budget so big chunks still get written at low budgets,
        # but cap it so a quiet stretch can't bank a huge burst for later.
        self.pool = min(self.pool + earned, max(2.0 * earned, 2.0 * chunk * self.ch))
        n_samples = int(self.pool)
        n_chunks = int(np.ceil(L / chunk))
        pad = n_chunks * chunk - L

        err = np.abs(self.target - self.buf)                 # ch x L
        if pad:
            err = np.concatenate([err, np.zeros((self.ch, pad))], axis=1)
        err = err.reshape(self.ch, n_chunks, chunk).mean(axis=2)  # ch x n_chunks

        if self.shared:
            groups = [(list(range(self.ch)), n_samples)]
        else:
            per = n_samples // self.ch
            groups = [([c], per) for c in range(self.ch)]

        # Edge crossfade: a written chunk is always written fully, and its new
        # audio also fades h samples into the stale audio on either side, so
        # there is no hard jump at its edges. (Those fade samples are a small
        # free bonus on top of the budget.)
        h = int(round(np.clip(p["smooth"], 0.0, 1.0) * chunk / 2))
        if h >= 1 and chunk >= 4:
            ramp = (np.arange(h) + 0.5) / h                         # 0 -> 1
            win = np.concatenate([ramp, np.ones(chunk), ramp[::-1]])
            win_off = np.arange(win.size) - h                       # relative to chunk start
        else:
            win = None
        mask = np.zeros((self.ch, L)) if win is not None else None

        r = float(np.clip(p["randomness"], 0.0, 1.0))
        thr = float(max(0.0, p["threshold"]))
        spent = 0
        for chans, budget in groups:
            k = budget // chunk
            if k <= 0:
                continue
            e = err[chans].ravel()                       # candidates: (ch, chunk)
            score = e / (e.max() + 1e-12)
            if r > 0:
                score = (1.0 - r) * score + r * self.rng.random(score.size)
            score[e <= thr] = -1.0                       # already close enough
            score[e <= 1e-9] = -1.0                      # nothing to fix
            valid = int(np.count_nonzero(score >= 0))
            k = min(k, valid)
            if k <= 0:
                continue
            pick = np.argpartition(score, -k)[-k:]
            spent += k * chunk
            ci = pick // n_chunks                         # index into chans
            ki = pick % n_chunks
            for local_c in np.unique(ci):
                c = chans[local_c]
                starts = ki[ci == local_c] * chunk
                if win is None:
                    cols = (starts[:, None] + np.arange(chunk)[None, :]).ravel()
                    cols = cols[cols < L]
                    self.buf[c, cols] = self.target[c, cols]
                else:
                    cols = ((starts[:, None] + win_off[None, :]) % L).ravel()
                    np.add.at(mask[c], cols, np.tile(win, starts.size))
        if mask is not None:
            np.clip(mask, 0.0, 1.0, out=mask)
            self.buf += mask * (self.target - self.buf)
        self.pool = max(0.0, self.pool - spent)

        # 3) Play the buffer at the current positions.
        wet = self.buf[:, positions].copy()

        # 4) Memory: every position fades a little each time it is played.
        #    `memory` = fraction left after 1 second (1.0 = stale audio never fades),
        #    so it behaves the same whatever the loop length.
        mem = float(np.clip(p["memory"], 0.0, 1.0))
        if mem < 1.0:
            g = mem ** (L / self.sr) if mem > 0 else 0.0
            self.buf[:, positions] *= g

        if self.declick:
            offset = self.last_out - wet[:, 0]
            ramp = np.clip(1.0 - np.arange(m) / min(m, max(1, int(0.004 * self.sr))), 0, 1)
            wet += offset[:, None] * ramp[None, :]
            self.declick = None

        self.pos = (self.pos + m) % L
        mix = float(np.clip(p["mix"], 0.0, 1.0))
        y = (1.0 - mix) * dry + mix * wet
        y *= db_to_lin(p["out_gain_db"])
        y = soft_limit(y)
        self.out[:, s0:s1] = y
        self.last_out = wet[:, -1]

    def step(self, n_blocks=200):
        """Process up to n_blocks blocks. Returns True when finished."""
        end = min(self.n_blocks, self.b + n_blocks)
        while self.b < end:
            self._process_block(self.b)
            self.b += 1
        return self.b >= self.n_blocks

    @property
    def progress(self):
        return self.b / max(1, self.n_blocks)

    def render(self):
        while not self.step(1000):
            pass
        return self.out


def process(audio, sr, params, stereo_shared=True, seed=1):
    """Convenience: render everything in one go."""
    return BandwidthFX(audio, sr, params, stereo_shared, seed).render()
