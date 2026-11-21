// Necronomicon's fingerprint scenarios (tests/fingerprint.cpp).
#pragma once

namespace
{
// The parameters of Necronomicon 0.1, in its table's order: the random scenarios draw for these.
const std::vector<std::string> order01 { "VCO 1 Freq", "VCO 1 Sub 1", "VCO 1 Sub 2", "VCO 1 Level", "VCO 1 Sub 1 Level", "VCO 1 Sub 2 Level",
    "VCO 2 Freq", "VCO 2 Sub 1", "VCO 2 Sub 2", "VCO 2 Level", "VCO 2 Sub 1 Level", "VCO 2 Sub 2 Level", "VCO 1 Wave", "VCO 2 Wave",
    "Quantize", "Cutoff", "Resonance", "VCF EG Amt", "VCF Attack", "VCF Decay", "EG", "VCA Attack", "VCA Decay", "Volume",
    "Seq 1 Step 1", "Seq 1 Step 2", "Seq 1 Step 3", "Seq 1 Step 4", "Seq 2 Step 1", "Seq 2 Step 2", "Seq 2 Step 3", "Seq 2 Step 4",
    "Seq Oct", "Seq 1 > Osc 1", "Seq 1 > Sub 1", "Seq 1 > Sub 2", "Seq 2 > Osc 2", "Seq 2 > Sub 1", "Seq 2 > Sub 2",
    "Rhythm 1", "Rhythm 1 > Seq 1", "Rhythm 1 > Seq 2", "Rhythm 2", "Rhythm 2 > Seq 1", "Rhythm 2 > Seq 2",
    "Rhythm 3", "Rhythm 3 > Seq 1", "Rhythm 3 > Seq 2", "Rhythm 4", "Rhythm 4 > Seq 1", "Rhythm 4 > Seq 2",
    "Rhythm Logic", "Clock Div", "Run", "Reset", "Next", "Trigger", "Fine Tune" };

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "default", "subharmonic-chords", "polyrhythm-bass", "pwm-pad", "sub-arp", "hold-drone" })
    {
        Scenario s;
        s.name = std::string ("preset-") + key;
        s.preset = key;
        list.push_back (s);
    }
    // The bench's patches.
    Scenario worst;
    worst.name = "bench-worst";
    worst.values = { { "VCO 1 Wave", 1 }, { "VCO 2 Wave", 0 }, { "VCO 1 Sub 2 Level", 0.6 }, { "VCO 2 Sub 1 Level", 0.5 }, { "Seq 1 > Sub 1", 1 },
                     { "Seq 2 > Sub 2", 1 }, { "Rhythm 3 > Seq 1", 1 }, { "Rhythm 4 > Seq 2", 1 }, { "Resonance", 0.8 }, { "EG", 2 }, { "VCO 1 Freq", 0.9 } };
    list.push_back (worst);
    Scenario typical;
    typical.name = "bench-typical";
    typical.values = { { "VCA Decay", 0.35 }, { "Resonance", 0.3 } };
    list.push_back (typical);
    // Random settings with the buttons, notes and knobs moving while it plays.
    for (uint32_t r = 0; r < 24; ++r)
    {
        Scenario s;
        s.name = "random-" + std::to_string (r);
        s.values = randomValues (2000 + r, { "Trigger", "Next", "Reset" }, order01);
        s.seconds = 3.0, s.bpm = 150.0;
        s.events = { { 24000, Event::param, "Trigger", 1.0 }, { 48011, Event::param, "Next", 1.0 }, { 62403, Event::noteOn, "", 64 },
                     { 72000, Event::noteOff, "", 64 }, { 80001, Event::param, "Reset", 1.0 }, { 96005, Event::param, "Run", 1.0 },
                     { 100007, Event::param, "Cutoff", 0.25 }, { 110003, Event::param, "Seq Oct", 2.0 }, { 115201, Event::param, "Clock Div", 7.0 } };
        list.push_back (s);
    }
    // Other block sizes (the same sound as random-0) and other rates.
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        Scenario s = list[8];
        s.name = "random-0-block-" + std::to_string (block);
        s.block = block;
        list.push_back (s);
    }
    for (double rate : { 44100.0, 96000.0 })
    {
        Scenario s;
        s.name = "rate-" + std::to_string ((int) rate);
        s.preset = "sub-arp";
        s.rate = rate;
        s.seconds = 4.0;
        list.push_back (s);
    }
    // Stopped, started again, jumped, a loop's wrap.
    Scenario moves;
    moves.name = "stop-start-jump";
    moves.preset = "polyrhythm-bass";
    moves.seconds = 6.0;
    moves.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
        const double f = (double) at;
        if (at < 48000) return { true, f * bpm / 60.0 / rate };
        if (at < 72000) return { false, 2.0 };
        if (at < 144000) return { true, (f - 72000) * bpm / 60.0 / rate };
        if (at < 200000) return { true, 7.3 + (f - 144000) * bpm / 60.0 / rate };
        return { true, 100.0 / 24000.0 + (f - 200000) * bpm / 60.0 / rate };
    };
    list.push_back (moves);
    // Each wave and quantizer, the sequencers on the subs, XOR, notes played while it runs.
    for (int q = 0; q < 5; ++q)
    {
        Scenario s;
        s.name = "quantize-" + std::to_string (q);
        s.preset = "subharmonic-chords";
        s.values = { { "Quantize", (double) q }, { "VCO 1 Wave", (double) (q % 3) }, { "VCO 2 Wave", (double) ((q + 1) % 3) }, { "Seq Oct", (double) (q % 3) } };
        s.seconds = 4.0;
        list.push_back (s);
    }
    Scenario notes;
    notes.name = "notes-playing";
    notes.preset = "polyrhythm-bass";
    notes.seconds = 4.0;
    for (int i = 0; i < 10; ++i)
    {
        notes.events.push_back ({ (uint64_t) (5000 + i * 17000), Event::noteOn, "", (double) (48 + (i * 7) % 24) });
        notes.events.push_back ({ (uint64_t) (14000 + i * 17000), Event::noteOff, "", (double) (48 + (i * 7) % 24) });
    }
    list.push_back (notes);
    Scenario xorSubs;
    xorSubs.name = "xor-subs";
    xorSubs.preset = "sub-arp";
    xorSubs.values = { { "Rhythm 4", 4 }, { "Rhythm 4 > Seq 2", 1 }, { "Rhythm 2 > Seq 1", 1 }, { "Seq 1 > Osc 1", 1 } };
    xorSubs.events = { { 90000, Event::param, "Rhythm Logic", 0.0 }, { 180000, Event::param, "Rhythm Logic", 1.0 } };
    list.push_back (xorSubs);
    return list;
}
} // namespace
