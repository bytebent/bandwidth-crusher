// Bandwidth Crusher — real-time DSP core (no JUCE dependency, so it can be tested on its own).
//
// A loop buffer plays on repeat. The input keeps writing what the loop *should*
// contain (the target). Only a limited number of bytes per second may actually be
// copied into the playing loop, so parts that are not updated keep playing stale
// audio from earlier passes.
//
// This is a port of reference/bandwidth_fx.py. Audio is processed in fixed 10 ms
// control blocks (so the sound does not depend on the host's buffer size), which
// adds 10 ms of latency that the plugin reports to the host.
//
// Real-time rules: everything is allocated in prepare(); process() never allocates,
// locks or does I/O.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bc
{

struct Params
{
    float budgetKbs  = 40.0f;   // bytes per second allowed to change, in kB/s
    float loopMs     = 120.0f;  // loop length
    float bits       = 12.0f;   // bit depth of the stored loop
    float randomness = 0.3f;    // 0 = fix worst errors first, 1 = random
    float chunk      = 1.0f;    // samples per update
    float smooth     = 0.5f;    // chunk edge crossfade, 0..1
    float memory     = 1.0f;    // fraction of stale audio left after 1 s
    float threshold  = 0.0f;    // skip chunks whose mean error is below this
    float inGainDb   = 0.0f;
    float outGainDb  = 0.0f;
    float mix        = 1.0f;
    bool  stereoSplit = false;  // false = one budget shared by all channels
};

// Snapshot of the loop for the UI. Written by the audio thread, read by the UI thread.
struct LoopView
{
    static constexpr int N = 240;
    std::array<std::atomic<float>, N> value {};
    std::array<std::atomic<float>, N> age {};
    std::atomic<float> freshPct { 0.0f }, loopMs { 0.0f }, playhead { 0.0f };
    std::atomic<uint32_t> counter { 0 };   // increments after every new snapshot
};

class Rng
{
public:
    void seed (uint64_t v)
    {
        s = v * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
        if (s == 0) s = 1;
        for (int i = 0; i < 8; ++i) next();
    }
    uint64_t next()
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    float uniform() { return (float) (next() >> 40) * (1.0f / 16777216.0f); }  // [0, 1)
private:
    uint64_t s = 1;
};

inline float dbToLin (float db) { return std::pow (10.0f, db / 20.0f); }

// Pass-through below 0.8, smooth tanh saturation above (peak stays below 1.0).
inline float softLimit (float x)
{
    constexpr float knee = 0.8f, head = 0.2f;
    const float a = std::fabs (x);
    if (a <= knee) return x;
    return std::copysign (knee + head * std::tanh ((a - knee) / head), x);
}

class Crusher
{
public:
    static constexpr int maxChunk = 4096;

    void prepare (double sampleRate, int numChannels, double maxLoopSeconds = 8.0)
    {
        sr = sampleRate;
        ch = std::clamp (numChannels, 1, 2);
        block = std::max (16, (int) std::lround (sr * 0.010));
        maxL = (int) std::ceil (maxLoopSeconds * sr) + 1;

        for (int c = 0; c < 2; ++c)
        {
            target[c].assign ((size_t) maxL, 0.0f);
            buf[c].assign ((size_t) maxL, 0.0f);
            mask[c].assign ((size_t) maxL, 0.0f);
            stamp[c].assign ((size_t) maxL, 0u);
            chunkErr[c].assign ((size_t) maxL, 0.0);
            inFifo[c].assign ((size_t) block, 0.0f);
            outFifo[c].assign ((size_t) block, 0.0f);
            dry[c].assign ((size_t) block, 0.0f);
        }
        scratchF.assign ((size_t) maxL, 0.0f);
        scratchU.assign ((size_t) maxL, 0u);
        scores.assign ((size_t) (2 * maxL), 0.0f);
        candC.assign ((size_t) (2 * maxL), 0);
        candJ.assign ((size_t) (2 * maxL), 0);
        candE.assign ((size_t) (2 * maxL), 0.0);
        pickC.assign ((size_t) (2 * maxL), 0);
        pickJ.assign ((size_t) (2 * maxL), 0);
        win.assign ((size_t) 2 * maxChunk + 4, 0.0f);
        dirtyMax = maxL;
        reset (1);
    }

    int latencySamples() const { return block; }

    // Clears the loop and the random generator. Safe on the audio thread.
    void reset (uint32_t seed)
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (target[c].begin(), target[c].begin() + (std::ptrdiff_t) dirtyMax, 0.0f);
            std::fill (buf[c].begin(), buf[c].begin() + (std::ptrdiff_t) dirtyMax, 0.0f);
            std::fill (mask[c].begin(), mask[c].begin() + (std::ptrdiff_t) dirtyMax, 0.0f);
            std::fill (stamp[c].begin(), stamp[c].begin() + (std::ptrdiff_t) dirtyMax, 0u);
            std::fill (chunkErr[c].begin(), chunkErr[c].begin() + (std::ptrdiff_t) dirtyMax, 0.0);
            std::fill (outFifo[c].begin(), outFifo[c].end(), 0.0f);
            lastOut[c] = 0.0f;
        }
        dirtyMax = 0;
        L = 0; pos = 0; pool = 0.0; fifoPos = 0; curChunk = -1; rollIdx = 0;
        declick = false; firstBlock = true;
        rng.seed (seed);
    }

    // In-place processing of the host buffer. numChannels may be 1 or 2.
    void process (float* const* io, int numChannels, int numSamples, const Params& p)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < ch; ++c)
                inFifo[c][(size_t) fifoPos] = io[std::min (c, numChannels - 1)][i];
            for (int c = 0; c < numChannels; ++c)
                io[c][i] = outFifo[std::min (c, ch - 1)][(size_t) fifoPos];

            if (++fifoPos == block)
            {
                runControlBlock (p);
                fifoPos = 0;
            }
        }
    }

    // Fill a UI snapshot. Costs O(loop length); call it ~30x per second, only while
    // the editor is open.
    void writeView (LoopView& v) const
    {
        if (L <= 0) return;
        const uint32_t now = (uint32_t) clock;
        int fresh = 0;
        for (int b = 0; b < LoopView::N; ++b)
        {
            const int s0 = (int) ((int64_t) b * L / LoopView::N);
            const int s1 = std::max (s0 + 1, (int) ((int64_t) (b + 1) * L / LoopView::N));
            float best = 0.0f;
            uint32_t newest = 0; bool any = false;
            const int stride = std::max (1, (s1 - s0) / 64);   // a display doesn't need every sample
            for (int c = 0; c < ch; ++c)
                for (int i = s0; i < s1 && i < L; i += stride)
                {
                    const float x = buf[c][(size_t) i];
                    if (std::fabs (x) > std::fabs (best)) best = x;
                    const uint32_t st = stamp[c][(size_t) i];
                    if (st != 0 && (! any || (uint32_t) (now - st) < (uint32_t) (now - newest))) { newest = st; any = true; }
                }
            v.value[(size_t) b].store (best, std::memory_order_relaxed);
            v.age[(size_t) b].store (any ? (float) ((uint32_t) (now - newest) / sr) : 1000.0f, std::memory_order_relaxed);
        }
        const int fstride = std::max (1, L / 16384);
        int counted = 0;
        for (int i = 0; i < L; i += fstride, ++counted)
        {
            const uint32_t st = stamp[0][(size_t) i];
            if (st != 0 && (uint32_t) (now - st) < (uint32_t) L) ++fresh;
        }
        v.freshPct.store (100.0f * (float) fresh / (float) counted, std::memory_order_relaxed);
        v.loopMs.store ((float) (1000.0 * L / sr), std::memory_order_relaxed);
        v.playhead.store ((float) pos / (float) L, std::memory_order_relaxed);
        v.counter.fetch_add (1, std::memory_order_release);
    }

private:
    // ---------------------------------------------------------------- helpers
    inline void setTarget (int c, int i, float v)
    {
        float& t = target[c][(size_t) i];
        const float b = buf[c][(size_t) i];
        chunkErr[c][(size_t) (i / curChunk)] += std::fabs (v - b) - std::fabs (t - b);
        t = v;
    }
    inline void setBuf (int c, int i, float v)
    {
        float& b = buf[c][(size_t) i];
        const float t = target[c][(size_t) i];
        chunkErr[c][(size_t) (i / curChunk)] += std::fabs (t - v) - std::fabs (t - b);
        b = v;
    }
    void recomputeErr (int c, int j)
    {
        const int s0 = j * curChunk, s1 = std::min (L, s0 + curChunk);
        double e = 0.0;
        for (int i = s0; i < s1; ++i) e += std::fabs (target[c][(size_t) i] - buf[c][(size_t) i]);
        chunkErr[c][(size_t) j] = e;
    }
    void recomputeAllErr()
    {
        const int n = (L + curChunk - 1) / curChunk;
        for (int c = 0; c < ch; ++c)
            for (int j = 0; j < n; ++j) recomputeErr (c, j);
    }

    void stretchTo (int newL)
    {
        const int oldL = L;
        for (int c = 0; c < ch; ++c)
        {
            for (auto* arr : { &target[c], &buf[c] })
            {
                auto& a = *arr;
                for (int j = 0; j < newL; ++j)
                {
                    const double src = (double) j * oldL / newL;
                    const int i0 = (int) src;
                    const float f = (float) (src - i0);
                    const float x0 = a[(size_t) i0], x1 = a[(size_t) ((i0 + 1) % oldL)];
                    scratchF[(size_t) j] = x0 + f * (x1 - x0);
                }
                std::copy_n (scratchF.begin(), newL, a.begin());
            }
            for (int j = 0; j < newL; ++j)
                scratchU[(size_t) j] = stamp[c][(size_t) std::min (oldL - 1, (int) ((int64_t) j * oldL / newL))];
            std::copy_n (scratchU.begin(), newL, stamp[c].begin());
        }
        pos = (int) ((int64_t) pos * newL / oldL) % newL;
        L = newL;
        dirtyMax = std::max (dirtyMax, L);
        declick = true;
        curChunk = -1;   // forces an error recompute with the new length
    }

    // ---------------------------------------------------------------- processing
    void runControlBlock (const Params& p)
    {
        const float gIn1 = dbToLin (p.inGainDb), gOut1 = dbToLin (p.outGainDb);
        const float mix1 = std::clamp (p.mix, 0.0f, 1.0f);
        if (firstBlock) { gIn0 = gIn1; gOut0 = gOut1; mix0 = mix1; firstBlock = false; }

        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i)
                dry[c][(size_t) i] = inFifo[c][(size_t) i] * (gIn0 + (gIn1 - gIn0) * (float) i / (float) block);

        int off = 0;
        while (off < block)
        {
            const int newL = std::clamp ((int) std::lround (p.loopMs * sr / 1000.0), 8, maxL);
            if (L == 0) { L = newL; dirtyMax = std::max (dirtyMax, L); curChunk = -1; pos = 0; }
            else if (std::abs (newL - L) > std::max (1.0, 0.01 * L)) stretchTo (newL);

            const int m = std::min (block - off, L);
            subBlock (off, m, p, gOut1, mix1);
            off += m;
        }
        gIn0 = gIn1; gOut0 = gOut1; mix0 = mix1;
    }

    void subBlock (int off, int m, const Params& p, float gOut1, float mix1)
    {
        const int bits = std::clamp ((int) std::lround (p.bits), 2, 16);
        const float levels = (float) ((1 << (bits - 1)) - 1);
        const int chunk = std::clamp ((int) std::lround (p.chunk), 1, std::min (L, maxChunk));
        if (chunk != curChunk) { curChunk = chunk; rollIdx = 0; recomputeAllErr(); }
        const int nChunks = (L + chunk - 1) / chunk;

        // 1) The input writes the target at the positions being played now.
        for (int i = 0; i < m; ++i)
        {
            const int at = (pos + i) % L;
            for (int c = 0; c < ch; ++c)
            {
                const float x = std::clamp (dry[c][(size_t) (off + i)], -1.0f, 1.0f);
                setTarget (c, at, std::round (x * levels) / levels);
            }
        }

        // Keep the incremental error sums honest: re-sum a few chunks every block.
        {
            const int n = std::max (1, nChunks / 256);
            for (int k = 0; k < n; ++k)
            {
                rollIdx = (rollIdx + 1) % nChunks;
                for (int c = 0; c < ch; ++c) recomputeErr (c, rollIdx);
            }
        }

        // 2) Spend the byte budget on updating the playing loop.
        const double bytesPerSample = bits / 8.0;
        const double earned = std::max (0.0f, p.budgetKbs) * 1000.0 * ((double) m / sr) / bytesPerSample;
        pool = std::min (pool + earned, std::max (2.0 * earned, 2.0 * chunk * ch));
        const long nSamples = (long) pool;

        const float r = std::clamp (p.randomness, 0.0f, 1.0f);
        const double thr = std::max (0.0f, p.threshold);
        long spent = 0;
        int nPicks = 0;

        const int nGroups = (p.stereoSplit && ch > 1) ? ch : 1;
        for (int g = 0; g < nGroups; ++g)
        {
            const int c0 = nGroups == 1 ? 0 : g, nc = nGroups == 1 ? ch : 1;
            const long budget = nGroups == 1 ? nSamples : nSamples / ch;
            long k = budget / chunk;
            if (k <= 0) continue;

            // Candidate chunks. Small loops: all of them. Very long loops with tiny chunks:
            // the chunks just written by the input (always the most urgent) plus a random
            // sample of the rest, so the cost per block stays bounded.
            // (A random contiguous stretch rather than scattered picks: far kinder to the cache.)
            int nList = 0;
            double emax = 0.0;
            auto add = [&] (int c, int j)
            {
                const double e = chunkErr[c][(size_t) j];
                candC[(size_t) nList] = c; candJ[(size_t) nList] = j; candE[(size_t) nList] = e;
                ++nList; emax = std::max (emax, e);
            };
            const int perChannel = maxCandidates / nc;
            for (int cc = 0; cc < nc; ++cc)
            {
                const int c = c0 + cc;
                if (nChunks <= perChannel)
                {
                    for (int j = 0; j < nChunks; ++j) add (c, j);
                }
                else
                {
                    const int ws = (int) (rng.next() % (uint64_t) nChunks);
                    for (int t = 0, j = ws; t < perChannel; ++t) { add (c, j); if (++j == nChunks) j = 0; }
                    const int j0 = pos / chunk, nHot = (m + chunk - 1) / chunk + 1;
                    for (int t = 0; t < nHot; ++t)
                    {
                        const int j = (j0 + t) % nChunks;
                        if ((j - ws + nChunks) % nChunks >= perChannel) add (c, j);   // not already in the stretch
                    }
                }
            }
            emax /= chunk;
            if (emax <= 1e-9) continue;

            int valid = 0;
            std::array<int, 1024> hist {};
            const double invMax = 1.0 / (emax * chunk), invChunk = 1.0 / chunk;
            for (int t = 0; t < nList; ++t)
            {
                const double err = candE[(size_t) t];
                const double e = err * invChunk;
                float s = -1.0f;
                if (e > thr && e > 1e-9)
                {
                    s = (float) ((1.0 - r) * err * invMax);
                    if (r > 0.0f) s += r * rng.uniform();
                    s = std::min (std::max (s, 0.0f), 0.99999f);
                    ++hist[(size_t) (s * 1024.0f)];
                    ++valid;
                }
                scores[(size_t) t] = s;
            }
            k = std::min<long> (k, valid);
            if (k <= 0) continue;

            // Top-k by histogram: everything above the cut bin, plus enough from the cut bin.
            int cut = 1023; long above = 0;
            while (cut > 0 && above + hist[(size_t) cut] < k) { above += hist[(size_t) cut]; --cut; }
            long fromCut = k - above;
            const int start = (int) (rng.next() % (uint64_t) nList);
            for (int u = 0; u < nList; ++u)
            {
                int t = start + u; if (t >= nList) t -= nList;
                const float s = scores[(size_t) t];
                if (s < 0.0f) continue;
                const int bin = (int) (s * 1024.0f);
                if (bin > cut || (bin == cut && fromCut-- > 0))
                {
                    pickC[(size_t) nPicks] = candC[(size_t) t];
                    pickJ[(size_t) nPicks] = candJ[(size_t) t];
                    ++nPicks;
                }
            }
            spent += k * chunk;
        }

        const uint32_t now = (uint32_t) (clock + (uint64_t) m) | 1u;  // never 0 (0 = never written)
        const int h = (int) std::lround (std::clamp (p.smooth, 0.0f, 1.0f) * chunk / 2.0f);
        if (h >= 1 && chunk >= 4)
        {
            // Written chunk = 1 inside, plus a fade of h samples into the stale audio either side.
            const int wl = chunk + 2 * h;
            for (int t = 0; t < h; ++t) win[(size_t) t] = win[(size_t) (wl - 1 - t)] = ((float) t + 0.5f) / (float) h;
            std::fill_n (win.begin() + h, chunk, 1.0f);

            for (int n = 0; n < nPicks; ++n)
            {
                const int c = pickC[(size_t) n], s0 = pickJ[(size_t) n] * chunk - h;
                for (int t = 0; t < wl; ++t)
                    mask[c][(size_t) (((s0 + t) % L + L) % L)] += win[(size_t) t];
            }
            for (int n = 0; n < nPicks; ++n)
            {
                const int c = pickC[(size_t) n], s0 = pickJ[(size_t) n] * chunk - h;
                for (int t = 0; t < wl; ++t)
                {
                    const int i = ((s0 + t) % L + L) % L;
                    float& w = mask[c][(size_t) i];
                    if (w <= 0.0f) continue;
                    const float ww = std::min (w, 1.0f);
                    const float b = buf[c][(size_t) i];
                    setBuf (c, i, b + ww * (target[c][(size_t) i] - b));
                    if (ww >= 0.5f) stamp[c][(size_t) i] = now;
                    w = 0.0f;
                }
            }
        }
        else
        {
            for (int n = 0; n < nPicks; ++n)
            {
                const int c = pickC[(size_t) n], s0 = pickJ[(size_t) n] * chunk, s1 = std::min (L, s0 + chunk);
                for (int i = s0; i < s1; ++i)
                {
                    setBuf (c, i, target[c][(size_t) i]);
                    stamp[c][(size_t) i] = now;
                }
            }
        }
        pool = std::max (0.0, pool - (double) spent);

        // 3) Play the loop, 4) apply memory decay to what was just played, 5) mix and output.
        const float mem = std::clamp (p.memory, 0.0f, 1.0f);
        const float decay = mem >= 1.0f ? 1.0f : (mem > 0.0f ? std::pow (mem, (float) (L / sr)) : 0.0f);
        const bool applyDecay = mem < 1.0f;
        float offset[2] = { 0.0f, 0.0f };
        int rampLen = 0;
        if (declick)
        {
            for (int c = 0; c < ch; ++c) offset[c] = lastOut[c] - buf[c][(size_t) pos];
            rampLen = std::max (1, std::min (m, (int) (0.004 * sr)));
            declick = false;
        }
        for (int i = 0; i < m; ++i)
        {
            const int at = (pos + i) % L;
            const float frac = (float) (off + i) / (float) block;
            const float gOut = gOut0 + (gOut1 - gOut0) * frac;
            const float mx = mix0 + (mix1 - mix0) * frac;
            for (int c = 0; c < ch; ++c)
            {
                float wet = buf[c][(size_t) at];
                if (i < rampLen) wet += offset[c] * (1.0f - (float) i / (float) rampLen);
                if (applyDecay) setBuf (c, at, buf[c][(size_t) at] * decay);
                const float d = dry[c][(size_t) (off + i)];
                outFifo[c][(size_t) (off + i)] = softLimit (((1.0f - mx) * d + mx * wet) * gOut);
                lastOut[c] = wet;
            }
        }
        pos = (pos + m) % L;
        clock += (uint64_t) m;
    }

    // ---------------------------------------------------------------- state
    double sr = 48000.0;
    int ch = 2, block = 480, maxL = 0, L = 0, pos = 0, fifoPos = 0, curChunk = -1, rollIdx = 0, dirtyMax = 0;
    double pool = 0.0;
    uint64_t clock = 0;
    bool declick = false, firstBlock = true;
    float lastOut[2] {};
    float gIn0 = 1, gOut0 = 1, mix0 = 1;
    Rng rng;

    std::vector<float> target[2], buf[2], mask[2], inFifo[2], outFifo[2], dry[2];
    std::vector<uint32_t> stamp[2];
    std::vector<double> chunkErr[2];
    std::vector<float> scratchF, scores, win;
    std::vector<uint32_t> scratchU;
    std::vector<int> pickC, pickJ, candC, candJ;
    std::vector<double> candE;
    static constexpr int maxCandidates = 32768;
};

} // namespace bc
