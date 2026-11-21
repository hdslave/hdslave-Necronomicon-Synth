// The engine's own checks, below the CLAP API: the rhythm generators' counting against clock-by-clock counting,
// the quantizer's scales, the subharmonics locked to their VCO, and every oscillator output band-limited.
//   unit
#include "engine.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace necronomicon;

void testRhythmCounting()
{
    // pulsesUpTo (by common multiples) against counting each clock, for random routings of up to four generators
    // (divisors 1-16, repeats allowed), both logics, and a range of song places.
    std::mt19937 random (11);
    for (int round = 0; round < 3000; ++round)
    {
        Routing r;
        r.count = (int) (random() % 5);
        for (int i = 0; i < r.count; ++i)
            r.divisor[i] = 1 + (int) (random() % 16);
        r.exclusive = (random() & 1) != 0;
        const int64_t last = round < 2500 ? (int64_t) (random() % 2000) - 3 : (int64_t) (random() % 100000);
        int64_t counted = 0;
        for (int64_t n = 0; n <= last; ++n)
            counted += pulseAt (r, n);
        CHECK (pulsesUpTo (r, last) == counted);
    }
    // XOR: two generators together cancel. 2 and 3 on one sequencer: 2, 3, 4, 8, 9, 10 in the first twelve clocks.
    Routing r;
    r.divisor[0] = 2, r.divisor[1] = 3, r.count = 2, r.exclusive = true;
    const bool want[12] = { false, false, true, true, true, false, false, false, true, true, true, false };
    for (int n = 0; n < 12; ++n)
        CHECK (pulseAt (r, n) == want[n]);
    r.exclusive = false; // OR: 0, 2, 3, 4, 6, 8, 9, 10
    for (int n = 0; n < 12; ++n)
        CHECK (pulseAt (r, n) == (n % 2 == 0 || n % 3 == 0));
}

void testQuantize()
{
    // Off leaves the pitch; 12-ET the nearest semitone; the scales their own notes, in any octave.
    CHECK (quantizePitch (0.3337f, 0) == 0.3337f);
    CHECK (std::fabs (quantizePitch (0.3337f, 1) - 4.0f / 12.0f) < 1e-6f);
    CHECK (std::fabs (quantizePitch (-0.04f, 1) - 0.0f) < 1e-6f);
    // 8-ET: C# rounds to C or D, F# to F or G; never a black key.
    for (float st = -24.0f; st <= 36.0f; st += 0.25f)
    {
        const float q = quantizePitch (st / 12.0f, 2) * 12.0f;
        const int note = ((int) std::lround (q) % 12 + 12) % 12;
        CHECK (std::fabs (q - std::round (q)) < 1e-4f && note != 1 && note != 3 && note != 6 && note != 8 && note != 10);
        CHECK (std::fabs (q - st) <= 1.0f + 1e-4f);
    }
    // 8-JI: the just major scale; a fifth is 3/2, a major third 5/4, the octave above 2/1 times them.
    CHECK (std::fabs (std::exp2 (quantizePitch (7.1f / 12.0f, 4)) - 1.5f) < 1e-5f);
    CHECK (std::fabs (std::exp2 (quantizePitch (1.0f + 3.9f / 12.0f, 4)) - 2.5f) < 1e-5f);
    CHECK (std::fabs (std::exp2 (quantizePitch (-1.0f + 9.2f / 12.0f, 4)) - 5.0f / 6.0f) < 1e-5f);
    // 12-JI: a minor third is 6/5, a tritone 45/32, a minor seventh 16/9.
    CHECK (std::fabs (std::exp2 (quantizePitch (3.0f / 12.0f, 3)) - 1.2f) < 1e-5f);
    CHECK (std::fabs (std::exp2 (quantizePitch (6.0f / 12.0f, 3)) - 45.0f / 32.0f) < 1e-5f);
    CHECK (std::fabs (std::exp2 (quantizePitch (10.0f / 12.0f, 3)) - 16.0f / 9.0f) < 1e-5f);
}

void testSubsLocked()
{
    // A sub at divisor d starts a cycle on every d-th of the VCO's, and only then, whatever the VCO's increment does;
    // a new divisor takes over at the VCO's next cycle and the count restarts if it is past it.
    std::mt19937 random (3);
    for (int wave = 0; wave < 3; ++wave)
        for (int d : { 1, 2, 3, 7, 16 })
        {
            Vco o;
            o.wanted[0] = d, o.wanted[1] = 16;
            o.takeDivisors();
            const float level[3] = { 1.0f, 1.0f, 1.0f };
            int vcoCycles = 0, subCycles = 0;
            float before = 0.0f, now = 0.0f;
            bool locked = true, bounded = true;
            float lastSub = 0.0f;
            for (int i = 0; i < 200000; ++i)
            {
                const float dt = 0.001f + 0.04f * (float) (random() % 1000) / 1000.0f;
                const float p = o.phase;
                const float out = stepVco (o, wave, dt, level, before, now);
                bounded &= std::isfinite (out) && std::fabs (out) <= 3.0f && std::fabs (before) < 4.0f && std::fabs (now) < 4.0f;
                before = now = 0.0f;
                const float sub = ((float) o.count[0] + o.phase) * o.inv[0];
                if (o.phase < p) // the VCO began a cycle
                {
                    ++vcoCycles;
                    if (sub < lastSub)
                        ++subCycles, locked &= vcoCycles % d == 0;
                }
                else
                    locked &= sub >= lastSub;
                lastSub = sub;
            }
            CHECK (locked && bounded && subCycles == vcoCycles / d);
        }
    // A divisor change: from 16 at count 10 to 4, the sub restarts at the VCO's next cycle.
    Vco o;
    o.wanted[0] = o.wanted[1] = 16;
    o.takeDivisors();
    o.count[0] = 10;
    o.phase = 0.99f;
    o.wanted[0] = 4;
    const float level[3] = { 0.0f, 1.0f, 0.0f };
    float before = 0.0f, now = 0.0f;
    stepVco (o, 2, 0.02f, level, before, now);
    CHECK (o.divisor[0] == 4 && o.count[0] == 0 && std::fabs (o.phase - 0.01f) < 1e-6f);
    // The saw's jump from 11/16 to 0 was corrected: -2 * 11/16 at half a sample ago.
    CHECK (std::fabs (before - 0.5f * (-2.0f * 11.0f / 16.0f) * 0.25f) < 1e-5f);
}
// The energy away from the harmonics of f0 in a held tone of one output (VCO 1 or one of its subs at divisor 3, each
// wave), in dB under the total: aliasing. A Blackman-Harris window over 65,536 frames at 48 kHz; a harmonic's bins
// are those within 6 of it.
double offHarmonic (int wave, int output, float knob)
{
    static Engine e; // large: not on the stack
    e = Engine {};
    for (uint32_t i = 0; i < paramCount; ++i)
        e.set (paramTable[i].id, paramTable[i].def);
    e.prepare (48000.0, 128);
    using namespace ParamId;
    const uint32_t levels[6] = { vco1Level, vco1Sub1Level, vco1Sub2Level, vco2Level, vco2Sub1Level, vco2Sub2Level };
    for (int i = 0; i < 6; ++i)
        e.set (levels[i], i == output ? 0.6f : 0.0f);
    for (uint32_t id = rhythm1Seq1; id <= rhythm4Seq2; ++id)
        e.set (id, 0.0f);
    e.set (vco1Wave, (float) wave), e.set (vco1Sub1, 3.0f), e.set (vco1Sub2, 3.0f), e.set (quantize, 0.0f), e.set (vco1Freq, knob);
    e.set (cutoff, 1.0f), e.set (resonance, 0.0f), e.set (vcfEgAmt, 0.0f), e.set (egMode, 2.0f);
    constexpr size_t n = 65536, settle = 8192;
    std::vector<float> l (n + settle), r (n + settle);
    e.render (l.data(), r.data(), (uint32_t) l.size());
    using C = std::complex<double>;
    std::vector<C> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * M_PI * (double) i / (double) (n - 1);
        a[i] = l[settle + i] * (0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t) - 0.01168 * std::cos (3.0 * t));
    }
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const C w (std::cos (-2.0 * M_PI / (double) len), std::sin (-2.0 * M_PI / (double) len));
        for (size_t i = 0; i < n; i += len)
        {
            C u (1.0);
            for (size_t k = 0; k < len / 2; ++k, u *= w)
            {
                const C x = a[i + k], y = a[i + k + len / 2] * u;
                a[i + k] = x + y, a[i + k + len / 2] = x - y;
            }
        }
    }
    // A sub repeats every 3 of the VCO's cycles, and so does the pulse (its width follows SUB 1).
    const double f0 = vcoHz (knob) / (output == 0 && wave != 1 ? 1.0 : 3.0), bin = 48000.0 / (double) n;
    double total = 0.0, off = 0.0;
    for (size_t k = 2; k < n / 2; ++k)
    {
        const double p = std::norm (a[k]), f = (double) k * bin, h = std::round (f / f0);
        total += p;
        if (h < 1.0 || std::fabs (f - h * f0) > 6.0 * bin)
            off += p;
    }
    return 10.0 * std::log10 (off / total);
}

void testBandLimited()
{
    // Each wave of VCO 1 and of its subs, at 1 kHz and 3.2 kHz: what is not a harmonic stays 45 dB down. (A pulse
    // edge found against the width at the sample's end, not its start, was counted twice: -26 dB.)
    for (float knob : { 0.5f, 0.9f })
        for (int wave = 0; wave < 3; ++wave)
            for (int output = 0; output < 3; ++output)
            {
                const double db = offHarmonic (wave, output, knob);
                if (std::getenv ("NECRONOMICON_DEBUG"))
                    std::printf ("debug alias: %.0f Hz wave %d output %d: %.1f dB\n", vcoHz (knob), wave, output, db);
                CHECK (db < -45.0);
            }
}
} // namespace

int main()
{
    testRhythmCounting();
    testQuantize();
    testSubsLocked();
    testBandLimited();
    std::printf ("unit: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
