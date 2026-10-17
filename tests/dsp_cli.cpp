// Command-line runner for the DSP core, used by tests/compare.py.
// usage: dsp_cli in.f32 out.f32 channels sampleRate hostBlock [key=value ...]
// Files are raw float32, planar (all of channel 0, then channel 1).
// hostBlock = 0 means random host buffer sizes between 1 and 2048.
#include "../source/CrusherDSP.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 6) { std::fprintf (stderr, "usage\n"); return 1; }
    const int ch = std::atoi (argv[3]);
    const double sr = std::atof (argv[4]);
    const int hostBlock = std::atoi (argv[5]);
    bc::Params p; uint32_t seed = 1; double maxLoop = 8.0;
    for (int a = 6; a < argc; ++a)
    {
        std::string kv = argv[a]; auto eq = kv.find ('='); auto k = kv.substr (0, eq); float v = std::stof (kv.substr (eq + 1));
        if (k == "budget_kbs") p.budgetKbs = v; else if (k == "loop_ms") p.loopMs = v; else if (k == "bits") p.bits = v;
        else if (k == "randomness") p.randomness = v; else if (k == "chunk") p.chunk = v; else if (k == "smooth") p.smooth = v;
        else if (k == "memory") p.memory = v; else if (k == "threshold") p.threshold = v; else if (k == "in_gain_db") p.inGainDb = v;
        else if (k == "out_gain_db") p.outGainDb = v; else if (k == "mix") p.mix = v; else if (k == "split") p.stereoSplit = v > 0.5f;
        else if (k == "seed") seed = (uint32_t) v; else if (k == "sweep") {} else { std::fprintf (stderr, "unknown %s\n", k.c_str()); return 1; }
    }
    bool sweep = false; for (int a = 6; a < argc; ++a) if (std::strncmp (argv[a], "sweep=1", 7) == 0) sweep = true;

    FILE* f = std::fopen (argv[1], "rb"); std::fseek (f, 0, SEEK_END); long bytes = std::ftell (f); std::fseek (f, 0, SEEK_SET);
    const long n = bytes / 4 / ch;
    std::vector<std::vector<float>> x (ch, std::vector<float> (n));
    for (int c = 0; c < ch; ++c) if (std::fread (x[c].data(), 4, (size_t) n, f) != (size_t) n) return 1;
    std::fclose (f);

    bc::Crusher dsp; dsp.prepare (sr, ch, maxLoop); dsp.reset (seed);
    std::mt19937 gen (7); std::uniform_int_distribution<int> dist (1, 2048);
    auto t0 = std::chrono::steady_clock::now();
    bc::LoopView view;
    long at = 0;
    while (at < n)
    {
        int len = hostBlock > 0 ? hostBlock : dist (gen);
        len = (int) std::min<long> (len, n - at);
        bc::Params q = p;
        if (sweep)   // automate everything at once to shake out edge cases
        {
            const float t = (float) at / (float) n;
            q.loopMs = 1.0f + 3999.0f * std::pow (t, 3.0f); q.chunk = 1.0f + 4095.0f * t; q.bits = 2.0f + 14.0f * t;
            q.budgetKbs = 0.5f + 999.5f * (1.0f - t); q.memory = t; q.randomness = 1.0f - t; q.smooth = t;
            q.stereoSplit = t > 0.5f;
        }
        float* ptrs[2] = { x[0].data() + at, x[std::min (1, ch - 1)].data() + at };
        dsp.process (ptrs, ch, len, q);
        at += len;
        if ((at / 1600) != ((at - len) / 1600)) dsp.writeView (view);   // ~30x per second, like the editor
    }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::fprintf (stderr, "latency=%d realtime_factor=%.1f\n", dsp.latencySamples(), (n / sr) / secs);

    f = std::fopen (argv[2], "wb");
    for (int c = 0; c < ch; ++c) std::fwrite (x[c].data(), 4, n, f);
    std::fclose (f);
    return 0;
}
