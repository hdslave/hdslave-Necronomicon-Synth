// Loads Necronomicon.clap as a host does and checks it: description and ports, parameters (the steps' text following
// Seq Oct), state, presets, the clock against the host's transport (timing to the frame, stop, restart, jumps, a loop
// that wraps inside a block), the rhythm generators (polyrhythms, OR and XOR), the sequencers' pitches on the VCOs
// and on the subs' divisors, the quantizer, the EG modes, notes, the pads as steps while Run is on, Reset, Next and
// Trigger, the same output whatever the block size, and bounded output at extreme settings. Writes demo renders as WAV
// files into the output folder.
//   host Necronomicon.clap OUT_FOLDER
#include "wav.h"
#include "../src/melodee_screen.h"
#include <array>
#include <chrono>
#include <thread>

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
const bool debug = std::getenv ("NECRONOMICON_DEBUG") != nullptr;

const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

// The host side: log, and what a preset load and a text change tell (counted); the plug-in must cope without the rest.
int screenRequests = 0, processRequests = 0, flushRequests = 0;
int rescans = 0, textRescans = 0, presetsLoaded = 0, presetErrors = 0, callbacks = 0;
const void* hostExtension (const clap_host*, const char* id)
{
    static const melodee_screen_host_t screen { [] (const void*, uint32_t) { ++screenRequests; } };
    if (std::strcmp (id, MELODEE_SCREEN_CLAP_HOST) == 0) return &screen;
    static const clap_host_log log { [] (const clap_host*, clap_log_severity s, const char* m) { std::printf ("  [plug-in log %d] %s\n", (int) s, m); } };
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) {
                                               rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0;
                                               textRescans += (f & CLAP_PARAM_RESCAN_TEXT) != 0;
                                           },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushRequests; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &log;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "Necronomicon test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++processRequests; }, [] (const clap_host*) { ++callbacks; },
};

// Input events for one process call, in time order.
struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity = 1.0)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    void midi (uint32_t time, uint8_t status, uint8_t a1, uint8_t a2)
    {
        Any a {};
        a.midi = { { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { status, a1, a2 } };
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

// What the plug-in sends back: parameter values it changed itself (the buttons returning to 0).
struct OutEvents
{
    std::vector<std::pair<clap_id, double>> values;
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            static_cast<OutEvents*> (o->ctx)->values.push_back ({ v->param_id, v->value });
        }
        return true;
    } };
};

// The transport a block hears: Melodee's way, the place at the block's start counted on by its frames.
struct Transport
{
    bool playing = true;
    double bpm = 120.0, beats = 0.0;
    bool placed = true; // the host gives a song position (it may be negative: a count-in)
    uint16_t numerator = 0, denominator = 4;
};
const Transport stopped { false, 120.0, 0.0 };

using Values = std::vector<std::pair<std::string, double>>;

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::map<std::string, clap_id> ids;
    double rate = 48000.0;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    clap_id id (const std::string& name)
    {
        const auto it = ids.find (name);
        if (it == ids.end())
        {
            std::printf ("FAIL no parameter named \"%s\"\n", name.c_str());
            ++failures;
            return CLAP_INVALID_ID;
        }
        return it->second;
    }
    // A factory preset by its load key, as a host's preset browser loads it.
    bool loadPreset (const char* key)
    {
        return presets != nullptr && presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key);
    }
    double value (const std::string& name)
    {
        double v = 0.0;
        params->get_value (plugin, id (name), &v);
        return v;
    }
    // The value whose text is `text` (a choice by its name).
    double valueOf (const std::string& name, const std::string& text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text.c_str(), &v));
        return v;
    }
    std::string text (const std::string& name, double v)
    {
        char t[64] = {};
        CHECK (params->value_to_text (plugin, id (name), v, t, sizeof (t)));
        return t;
    }

    // Renders `frames` in blocks of `block`, with `events(frame, frames, list)` adding events for the block starting
    // at `frame`; the transport is asked for each block.
    void render (uint32_t frames, uint32_t block, std::vector<float>& left, std::vector<float>& right,
                 const std::function<Transport (uint64_t)>& transport, const std::function<void (uint64_t, uint32_t, Events&)>& events = {},
                 OutEvents* out = nullptr)
    {
        left.assign (frames, 0.0f);
        right.assign (frames, 0.0f);
        OutEvents ignored;
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            const auto t = transport (at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (t.placed ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0) | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = t.bpm;
            tr.song_pos_beats = (clap_beattime) std::llround (t.beats * (double) CLAP_BEATTIME_FACTOR);
            if (t.numerator)
            {
                tr.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
                tr.tsig_num = t.numerator;
                tr.tsig_denom = t.denominator;
                const double barBeats = 4.0 * t.numerator / t.denominator;
                tr.bar_number = int32_t (std::floor (t.beats / barBeats));
                tr.bar_start = clap_beattime (std::llround (tr.bar_number * barBeats * CLAP_BEATTIME_FACTOR));
            }
            Events e;
            if (events)
                events (at, n, e);
            e.sort();
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            p.out_events = out ? &out->out : &ignored.out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
    }
    void set (Events& e, const std::string& name, double value) { e.param (0, id (name), value); }
    // Parameter changes before anything plays: one silent, stopped block.
    void setup (const Values& values)
    {
        std::vector<float> l, r;
        render (1, 1, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                for (auto& [name, v] : values)
                    set (e, name, v);
        });
    }
};

// Sample positions where a hit starts: where the signal passes `threshold`, in a rise that began after at least
// `gap` samples below `quiet`.
std::vector<long> onsets (const std::vector<float>& x, float threshold = 1e-3f, float quiet = 1e-4f, long gap = 96)
{
    std::vector<long> found;
    long silent = gap;      // samples below quiet so far
    bool armed = true;      // this rise began after a long enough quiet
    bool counted = false;   // and has been counted
    for (long i = 0; i < (long) x.size(); ++i)
    {
        const float a = std::fabs (x[(size_t) i]);
        if (a < quiet)
        {
            ++silent;
            continue;
        }
        if (silent > 0) // a rise starts here
            armed = silent >= gap, counted = false;
        silent = 0;
        if (armed && ! counted && a > threshold)
            found.push_back (i), counted = true;
    }
    return found;
}

Transport playingFrom (double startBeats, double bpm, double rate, uint64_t at)
{
    return { true, bpm, startBeats + (double) at * bpm / 60.0 / rate };
}

// A clean wave's frequency over [from, to) seconds: its rising zero crossings (each after the wave was below a little
// hysteresis), placed between samples by interpolation, counted over the time from the first to the last.
double hz (const std::vector<float>& x, double from, double to, double rate = 48000.0)
{
    const size_t a = (size_t) (from * rate), b = std::min (x.size(), (size_t) (to * rate));
    bool armed = false;
    double first = -1.0, last = -1.0;
    long n = 0;
    for (size_t i = a + 1; i < b; ++i)
    {
        if (x[i] < -0.002f)
            armed = true;
        if (armed && x[i - 1] <= 0.0f && x[i] > 0.0f)
        {
            const double t = (double) (i - 1) + (double) x[i - 1] / ((double) x[i - 1] - (double) x[i]);
            if (first < 0.0)
                first = t;
            last = t, ++n, armed = false;
        }
    }
    return n < 2 ? 0.0 : (double) (n - 1) * rate / (last - first);
}
double rms (const std::vector<float>& l, size_t from, size_t n)
{
    double s = 0.0;
    for (size_t i = from; i < from + n && i < l.size(); ++i)
        s += (double) l[i] * l[i];
    return std::sqrt (s / (double) n);
}

// Every rhythm generator off both sequencers, then `extra`.
Values noRhythms (Values extra = {})
{
    Values v;
    for (int g = 1; g <= 4; ++g)
        for (int s = 1; s <= 2; ++s)
            v.push_back ({ "Rhythm " + std::to_string (g) + " > Seq " + std::to_string (s), 0.0 });
    v.insert (v.end(), extra.begin(), extra.end());
    return v;
}

// A short, clean hit on every clock of Rhythm 1 (into Seq 1): VCO 1's first sub alone, square, with the VCO taken 5
// octaves down by its steps and divided by 16 (half a hertz: a steady level within a hit), the filter open, the VCA
// fast with the shortest decay, eighth-note clocks.
Values clickPatch (Instance& a, Values extra = {})
{
    Values v = noRhythms ({ { "VCO 1 Freq", 0.0 }, { "VCO 1 Level", 0.0 }, { "VCO 1 Sub 1 Level", 1.0 }, { "VCO 1 Sub 1", 16 },
        { "VCO 1 Sub 2 Level", 0.0 }, { "VCO 2 Level", 0.0 }, { "VCO 2 Sub 1 Level", 0.0 }, { "VCO 2 Sub 2 Level", 0.0 },
        { "VCO 1 Wave", a.valueOf ("VCO 1 Wave", "Square") }, { "Quantize", a.valueOf ("Quantize", "Off") },
        { "Seq Oct", a.valueOf ("Seq Oct", "+/-5") }, { "Seq 1 > Osc 1", 1.0 }, { "Seq 1 > Sub 1", 0.0 }, { "Seq 1 > Sub 2", 0.0 },
        { "Seq 1 Step 1", -1.0 }, { "Seq 1 Step 2", -1.0 }, { "Seq 1 Step 3", -1.0 }, { "Seq 1 Step 4", -1.0 },
        { "Cutoff", 1.0 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "VCA Attack", 0.0 }, { "VCA Decay", 0.0 },
        { "EG", a.valueOf ("EG", "On") }, { "Volume", 0.8 }, { "Rhythm 1", 1.0 }, { "Rhythm 1 > Seq 1", 1.0 },
        { "Clock Div", a.valueOf ("Clock Div", "1/8") } });
    v.insert (v.end(), extra.begin(), extra.end());
    return v;
}

// A held tone to measure: VCO 1 alone, square, at C5 (523.25 Hz, the knob a quarter up), the filter open, EG: Hold so
// the VCA stays open, no rhythm generator routed (the steps move only by Next, Reset or the song's place).
Values dronePatch (Instance& a, Values extra = {})
{
    Values v = noRhythms ({ { "VCO 1 Freq", 0.25 }, { "VCO 1 Level", 1.0 }, { "VCO 1 Sub 1 Level", 0.0 }, { "VCO 1 Sub 2 Level", 0.0 },
        { "VCO 2 Level", 0.0 }, { "VCO 2 Sub 1 Level", 0.0 }, { "VCO 2 Sub 2 Level", 0.0 }, { "VCO 1 Wave", a.valueOf ("VCO 1 Wave", "Square") },
        { "VCO 2 Wave", a.valueOf ("VCO 2 Wave", "Square") }, { "Quantize", a.valueOf ("Quantize", "12-ET") },
        { "Seq Oct", a.valueOf ("Seq Oct", "+/-2") }, { "Seq 1 Step 1", 0.0 }, { "Seq 1 Step 2", 0.5 }, { "Seq 1 Step 3", 7.0 / 24.0 },
        { "Seq 1 Step 4", -0.5 }, { "Cutoff", 1.0 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "EG", a.valueOf ("EG", "Hold") },
        { "Volume", 0.6 } });
    v.insert (v.end(), extra.begin(), extra.end());
    return v;
}
// Seq 1's four steps in dronePatch: C5, C6, G5, C4.
constexpr double c5 = 523.2511, c6 = 1046.5023, g5 = 783.9909, c4 = 261.6256;

bool near (double got, double want, double share) { return std::fabs (got - want) <= want * share; }

void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (d != nullptr && std::string (d->name) == "hdslave Necronomicon" && std::string (d->id) == "com.hdslave.necronomicon");
    bool instrument = false;
    for (const char* const* f = d->features; f && *f; ++f)
        instrument |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0;
    CHECK (instrument);

    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, true) == 0 && audio->count (a.plugin, false) == 1);
    clap_audio_port_info port {};
    CHECK (audio && audio->get (a.plugin, 0, false, &port) && port.channel_count == 2);
    CHECK (notes && notes->count (a.plugin, true) == 1 && notes->count (a.plugin, false) == 0);
}

void testParameters()
{
    Instance a;
    const uint32_t count = a.params->count (a.plugin);
    CHECK (count == 58);
    std::map<clap_id, int> seen;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (++seen[info.id] == 1 && info.id < count);
        CHECK (info.min_value <= info.default_value && info.default_value <= info.max_value);
        double v = 0.0;
        CHECK (a.params->get_value (a.plugin, info.id, &v) && v == info.default_value);
        char text[64] = {};
        CHECK (a.params->value_to_text (a.plugin, info.id, v, text, sizeof (text)) && text[0] != 0);
        double back = -1e9;
        CHECK (a.params->text_to_value (a.plugin, info.id, text, &back));
        // Text round trips to the same value, within the display's precision.
        CHECK (std::fabs (back - v) <= 0.02 * (info.max_value - info.min_value) + 1e-9);
        if (debug && std::fabs (back - v) > 0.02 * (info.max_value - info.min_value) + 1e-9)
            std::printf ("debug text: %s %g reads \"%s\", back %g\n", info.name, v, text, back);
    }
    for (const char* name : { "VCO 1 Freq", "VCO 1 Wave", "VCO 1 Sub 1", "VCO 1 Sub 2", "VCO 2 Freq", "VCO 2 Wave", "VCO 2 Sub 1", "VCO 2 Sub 2",
                              "VCO 1 Level", "VCO 1 Sub 1 Level", "VCO 1 Sub 2 Level", "VCO 2 Level", "VCO 2 Sub 1 Level", "VCO 2 Sub 2 Level",
                              "Cutoff", "Resonance", "VCF EG Amt", "VCF Attack", "VCF Decay", "VCA Attack", "VCA Decay", "EG", "Volume",
                              "Quantize", "Seq Oct", "Seq 1 Step 1", "Seq 1 Step 4", "Seq 2 Step 1", "Seq 2 Step 4", "Seq 1 > Osc 1",
                              "Seq 1 > Sub 1", "Seq 1 > Sub 2", "Seq 2 > Osc 2", "Seq 2 > Sub 1", "Seq 2 > Sub 2", "Rhythm 1", "Rhythm 4",
                              "Rhythm 1 > Seq 1", "Rhythm 4 > Seq 2", "Rhythm Logic", "Clock Div", "Run", "Reset", "Next", "Trigger", "Fine Tune" })
        CHECK (a.ids.count (name) == 1);
    // The panel's ranges, as text.
    CHECK (a.text ("VCO 1 Freq", 0.0) == "261.6 Hz" && a.text ("VCO 1 Freq", 1.0) == "4186 Hz");
    CHECK (std::fabs (a.valueOf ("VCO 1 Freq", "1047 Hz") - 0.5) < 2e-4); // to the hertz: about 0.2 cent here
    CHECK (a.text ("VCO 1 Sub 1", 16.0) == "1/16" && a.valueOf ("VCO 1 Sub 1", "1/5") == 5.0 && a.valueOf ("VCO 1 Sub 1", "7") == 7.0);
    CHECK (a.text ("VCF Attack", 0.0) == "1.0 ms" && a.text ("VCF Attack", 0.05) == "1.6 ms" && a.text ("VCF Attack", 1.0) == "10.00 s");
    CHECK (a.text ("VCA Decay", 0.0) == "5.0 ms" && a.text ("VCA Decay", 0.4) == "105 ms" && a.text ("VCA Decay", 1.0) == "10.00 s");
    CHECK (a.text ("Cutoff", 0.0) == "20.0 Hz" && a.text ("Fine Tune", -1.0) == "-50 ct");
    // A step reads in semitones at Seq Oct's range: +7 at +/-2, and the same knob +17.5 at +/-5. The host is asked
    // to read the text again (on the main thread) when Seq Oct moves.
    CHECK (a.text ("Seq 1 Step 2", 7.0 / 24.0) == "+7.0 st");
    const int calls = callbacks, texts = textRescans;
    a.setup ({ { "Seq Oct", a.valueOf ("Seq Oct", "+/-5") } });
    CHECK (callbacks > calls);
    a.plugin->on_main_thread (a.plugin);
    CHECK (textRescans == texts + 1);
    CHECK (a.text ("Seq 1 Step 2", 7.0 / 24.0) == "+17.5 st" && std::fabs (a.valueOf ("Seq 1 Step 2", "+12 st") - 0.2) < 1e-6);
    a.plugin->on_main_thread (a.plugin); // nothing new: no second rescan
    CHECK (textRescans == texts + 1);
}

void testState()
{
    std::vector<float> l1, r1, l2, r2;
    Instance a;
    a.setup ({ { "VCO 1 Freq", 0.31 }, { "VCO 2 Sub 2", 11 }, { "Seq 2 Step 3", -0.4 }, { "Rhythm 3", 13 }, { "Rhythm 3 > Seq 2", 1.0 },
               { "Rhythm Logic", 1.0 }, { "Quantize", 4.0 }, { "VCO 1 Wave", 1.0 }, { "Fine Tune", 0.3 } });
    std::vector<uint8_t> saved;
    clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
        auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
        v->insert (v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
        return (int64_t) n;
    } };
    CHECK (a.state->save (a.plugin, &os));
    Instance b;
    struct Reader { const std::vector<uint8_t>* v; size_t at; } reader { &saved, 0 };
    clap_istream is { &reader, [] (const clap_istream* s, void* to, uint64_t n) -> int64_t {
        auto* r = static_cast<Reader*> (s->ctx);
        const size_t k = std::min<size_t> ((size_t) n, r->v->size() - r->at);
        std::memcpy (to, r->v->data() + r->at, k);
        r->at += k;
        return (int64_t) k;
    } };
    CHECK (b.state->load (b.plugin, &is));
    for (auto& [name, id] : a.ids)
        CHECK (a.value (name) == b.value (name));
    b.setup ({}); // the same one-frame start as a's
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    a.render (48000, 128, l1, r1, t);
    b.render (48000, 128, l2, r2, t);
    CHECK (l1 == l2 && r1 == r2);
    // A truncated or foreign state is refused.
    std::vector<uint8_t> junk { 'J', 'U', 'N', 'K' };
    Reader bad { &junk, 0 };
    clap_istream isBad { &bad, is.read };
    CHECK (! b.state->load (b.plugin, &isBad));
}

// The factory presets: listed by preset discovery as a host's browser reads them, loaded by key through
// preset-load, each setting every value (the demos check each one's values and sound).
struct Listed { std::string name, key; bool ours = false; uint32_t flags = 0; };
std::vector<Listed> listPresets (int& locations)
{
    std::vector<Listed> out;
    locations = -1;
    const auto* f = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (f != nullptr && entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == (const void*) f);
    if (f == nullptr || f->count (f) != 1)
        return out;
    const auto* d = f->get_descriptor (f, 0);
    CHECK (d != nullptr && f->get_descriptor (f, 1) == nullptr);
    struct Index
    {
        std::vector<clap_preset_discovery_location> found;
        clap_preset_discovery_indexer indexer {};
    } index;
    index.indexer = { CLAP_VERSION, "tests", "", "", "1", &index,
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
                      [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
                          static_cast<Index*> (i->indexer_data)->found.push_back (*l);
                          return true;
                      },
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
                      [] (const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; } };
    CHECK (f->create (f, &index.indexer, "com.example.other") == nullptr);
    const auto* provider = f->create (f, &index.indexer, d->id);
    CHECK (provider != nullptr && provider->init (provider));
    if (provider == nullptr)
        return out;
    locations = (int) index.found.size();
    CHECK (locations == 1 && index.found[0].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && index.found[0].location == nullptr);
    struct Receive
    {
        clap_preset_discovery_metadata_receiver r {};
        static std::vector<Listed>& of (const clap_preset_discovery_metadata_receiver* r) { return *static_cast<std::vector<Listed>*> (r->receiver_data); }
    } receive;
    using R = Receive;
    receive.r.receiver_data = &out;
    receive.r.on_error = [] (const clap_preset_discovery_metadata_receiver*, int32_t, const char*) { CHECK (false); };
    receive.r.begin_preset = [] (const clap_preset_discovery_metadata_receiver* r, const char* name, const char* key) {
        R::of (r).push_back ({ name ? name : "", key ? key : "" });
        return true;
    };
    receive.r.add_plugin_id = [] (const clap_preset_discovery_metadata_receiver* r, const clap_universal_plugin_id* id) {
        R::of (r).back().ours = std::string (id->abi) == "clap" && std::string (id->id) == "com.hdslave.necronomicon";
    };
    receive.r.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_flags = [] (const clap_preset_discovery_metadata_receiver* r, uint32_t flags) { R::of (r).back().flags = flags; };
    receive.r.add_creator = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_description = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_timestamps = [] (const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {};
    receive.r.add_feature = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.add_extra_info = [] (const clap_preset_discovery_metadata_receiver*, const char*, const char*) {};
    CHECK (! provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere.preset", &receive.r));
    CHECK (out.empty());
    CHECK (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receive.r));
    provider->destroy (provider);
    return out;
}

void testPresets()
{
    int locations = 0;
    const auto listed = listPresets (locations);
    std::vector<std::string> names, keys;
    for (const auto& p : listed)
    {
        names.push_back (p.name), keys.push_back (p.key);
        CHECK (p.ours && (p.flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0);
    }
    CHECK ((names == std::vector<std::string> { "Default", "Subharmonic Chords", "Polyrhythm Bass", "PWM Pad", "Sub Arp", "Hold Drone" }));
    CHECK ((keys == std::vector<std::string> { "default", "subharmonic-chords", "polyrhythm-bass", "pwm-pad", "sub-arp", "hold-drone" }));

    Instance a;
    CHECK (a.presets != nullptr);
    // Unknown keys and kinds are refused, and the host hears why.
    const int errors = presetErrors;
    CHECK (! a.loadPreset ("no-such-preset") && ! a.loadPreset (nullptr));
    CHECK (! a.presets->from_location (a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere", "sub-arp"));
    CHECK (presetErrors == errors + 3);
    // Default sets everything back, whatever was turned before; the host is told the values and text moved.
    a.setup ({ { "Volume", 0.1 }, { "Cutoff", 0.2 }, { "VCO 1 Sub 2", 9 }, { "Seq 2 Step 2", -0.5 }, { "Clock Div", 7.0 }, { "Seq Oct", 2.0 } });
    const int rescanned = rescans, texts = textRescans, loaded = presetsLoaded;
    CHECK (a.loadPreset ("default"));
    CHECK (rescans == rescanned + 1 && textRescans == texts + 1 && presetsLoaded == loaded + 1);
    for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
    {
        clap_param_info info {};
        a.params->get_info (a.plugin, i, &info);
        double v = -1.0;
        a.params->get_value (a.plugin, info.id, &v);
        CHECK (v == info.default_value);
    }
    // Loaded while playing: the next block plays it, exactly as if each value had come as an event at that block's
    // first frame (here Hold Drone and its 1/8 clock, over the 1/32 set before).
    std::vector<float> l, r, l2, r2;
    Instance b, c;
    for (Instance* i : { &b, &c })
        i->setup ({ { "Clock Div", i->valueOf ("Clock Div", "1/32") } });
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    b.render (128, 128, l, r, t);
    c.render (128, 128, l2, r2, t);
    CHECK (b.loadPreset ("hold-drone") && b.value ("Clock Div") == b.valueOf ("Clock Div", "1/8"));
    auto later = [] (uint64_t at) { return playingFrom (128.0 / 24000.0, 120.0, 48000.0, at); };
    b.render (96000, 128, l, r, later);
    c.render (96000, 128, l2, r2, later, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 0)
            for (uint32_t i = 0; i < c.params->count (c.plugin); ++i)
            {
                clap_param_info info {};
                c.params->get_info (c.plugin, i, &info);
                e.param (0, info.id, b.value (info.name)); // (the buttons at 0: no press)
            }
    });
    CHECK (l == l2 && rms (l, 24000, 24000) > 0.01);
}

void testTiming()
{
    // 120 BPM, eighth-note clocks, Rhythm 1 at 1: a hit every 12,000 frames at 48 kHz; the first at frame 0 of Play.
    std::vector<float> l, r;
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
    auto hits = onsets (l);
    if (debug)
    {
        std::printf ("debug: %zu hits:", hits.size());
        for (size_t i = 0; i < hits.size() && i < 20; ++i) std::printf (" %ld", hits[i]);
        std::printf ("\n");
    }
    CHECK (hits.size() == 16);
    // From the second hit on (the first starts with the filter and the mix empty, a few frames slower to rise), to
    // the frame.
    for (size_t i = 2; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 12000) <= 1);
    CHECK (! hits.empty() && hits[0] < 48); // within the attack of frame 0
    CHECK (l == r);

    // Rhythm 1 at 3, sixteenth notes at 90 BPM: 8,000 frames a clock, a hit every 24,000.
    Instance b;
    b.setup (clickPatch (b, { { "Clock Div", b.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 3 } }));
    b.render (48000 * 3, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 90.0, 48000.0, at); });
    hits = onsets (l);
    CHECK (hits.size() == 6);
    for (size_t i = 2; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 24000) <= 1);

    // Other rates: 44.1 and 96 kHz keep the musical timing.
    for (double rate : { 44100.0, 96000.0 })
    {
        Instance c (rate);
        c.setup (clickPatch (c));
        c.render ((uint32_t) rate * 2, 128, l, r, [rate] (uint64_t at) { return playingFrom (0.0, 120.0, rate, at); });
        hits = onsets (l, 1e-3f, 1e-4f, (long) (rate / 500));
        CHECK (hits.size() == 8);
        for (size_t i = 2; i < hits.size(); ++i)
            CHECK (std::labs (hits[i] - hits[1] - (long) std::llround ((i - 1) * rate / 4)) <= 1);
        CHECK (! hits.empty() && hits[0] < (long) (rate / 1000));
    }
}

// The clocks (0-based) a run's hits fell on, `clockFrames` apart.
std::vector<long> clocksOf (const std::vector<long>& hits, long clockFrames)
{
    std::vector<long> out;
    for (long h : hits)
        out.push_back ((h + clockFrames / 4) / clockFrames);
    return out;
}

void testRhythms()
{
    std::vector<float> l, r;
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    // 3 against 4: Rhythm 1 at 3 into Seq 1, Rhythm 2 at 4 into Seq 2. Each sequencer's steps start the EGs, so the
    // hits fall on every multiple of 3 or of 4 (sixteenths: 6,000 frames a clock).
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 3 }, { "Rhythm 2", 4 }, { "Rhythm 2 > Seq 2", 1.0 } }));
        a.render (6000 * 36, 128, l, r, t);
        const auto got = clocksOf (onsets (l), 6000);
        std::vector<long> want;
        for (long n = 0; n < 36; ++n)
            if (n % 3 == 0 || n % 4 == 0)
                want.push_back (n);
        if (debug) { std::printf ("debug 3:4:"); for (long c : got) std::printf (" %ld", c); std::printf ("\n"); }
        CHECK (got == want);
    }
    // One sequencer with Rhythm 1 at 2 and Rhythm 2 at 3: OR on every multiple of either; XOR only where one alone
    // fires (2, 3, 4, 8, 9, 10, ...: never on the multiples of 6, so not on the downbeat either).
    for (bool exclusive : { false, true })
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 2 }, { "Rhythm 2", 3 }, { "Rhythm 2 > Seq 1", 1.0 },
                                  { "Rhythm Logic", a.valueOf ("Rhythm Logic", exclusive ? "XOR" : "OR") } }));
        a.render (6000 * 36, 128, l, r, t);
        const auto got = clocksOf (onsets (l), 6000);
        std::vector<long> want;
        for (long n = 0; n < 36; ++n)
            if (exclusive ? ((n % 2 == 0) != (n % 3 == 0)) : (n % 2 == 0 || n % 3 == 0))
                want.push_back (n);
        if (debug) { std::printf ("debug %s:", exclusive ? "XOR" : "OR"); for (long c : got) std::printf (" %ld", c); std::printf ("\n"); }
        CHECK (got == want);
    }
    // A generator routed nowhere plays nothing; Run: Stop silences the sequencers while the host plays.
    {
        Instance a;
        a.setup (clickPatch (a, { { "Rhythm 1 > Seq 1", 0.0 } }));
        a.render (48000, 128, l, r, t);
        CHECK (onsets (l).empty());
        Instance b;
        b.setup (clickPatch (b, { { "Run", 0.0 } }));
        b.render (48000, 128, l, r, t);
        CHECK (onsets (l).empty());
    }
}

void testStopRestartJump()
{
    std::vector<float> l, r;
    // Plays 1 s, stops for 1 s, plays again from the top: hits stop with the transport and start again on frame 0 of
    // the second Play.
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 3, 128, l, r, [] (uint64_t at) {
        if (at < 48000) return playingFrom (0.0, 120.0, 48000.0, at);
        if (at < 96000) return Transport { false, 120.0, 2.0 };
        return playingFrom (0.0, 120.0, 48000.0, at - 96000);
    });
    auto hits = onsets (l);
    CHECK (hits.size() == 8);
    for (long h : hits)
        CHECK (h < 48000 || h >= 96000);
    CHECK (hits.size() == 8 && hits[4] - 96000 < 48 && hits[4] >= 96000);

    // A jump into the middle of a clock waits for the next one; a jump to just after a clock (inside one block, as
    // when a loop wraps between two blocks) plays it at once.
    Instance b;
    b.setup (clickPatch (b));
    constexpr uint64_t jump = 128 * 100; // a block boundary
    b.render (48000 * 2, 128, l, r, [] (uint64_t at) {
        if (at < jump) return playingFrom (0.0, 120.0, 48000.0, at);
        return playingFrom (3.3, 120.0, 48000.0, at - jump); // the next clock is at beat 3.5: 0.2 beat on
    });
    hits = onsets (l);
    const long expected = (long) jump + 4800;
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= (long) jump && h < expected - 10; }) == hits.end());
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= expected && h < expected + 48; }) != hits.end());

    Instance c;
    c.setup (clickPatch (c));
    constexpr uint64_t wrap = 128 * 40;
    c.render (48000, 128, l, r, [] (uint64_t at) {
        if (at < wrap) return playingFrom (1.0, 120.0, 48000.0, at);
        return playingFrom (100.0 / 24000.0, 120.0, 48000.0, at - wrap); // 100 frames past the top
    });
    hits = onsets (l);
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= (long) wrap && h < (long) wrap + 48; }) != hits.end());

    // A host with no song position (the flag left out): the clocks count from Play itself.
    Instance d;
    d.setup (clickPatch (d));
    d.render (48000 * 2, 128, l, r, [] (uint64_t) { return Transport { true, 120.0, 0.0, false }; });
    hits = onsets (l);
    CHECK (hits.size() == 8);
    for (size_t i = 2; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 12000) <= 1);

    // The song's place sets the steps: Rhythm 1 at 1 into Seq 1 on quarter-note clocks, Play landing at beat 2.2
    // (clock 2 played, so step 3) holds G5 until beat 3, then step 4 (C4).
    Instance e;
    e.setup (dronePatch (e, { { "Rhythm 1", 1 }, { "Rhythm 1 > Seq 1", 1.0 }, { "Clock Div", e.valueOf ("Clock Div", "1/4") } }));
    e.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (2.2, 120.0, 48000.0, at); });
    if (debug) std::printf ("debug jump: %.1f %.1f\n", hz (l, 0.05, 0.35), hz (l, 0.45, 0.85));
    CHECK (near (hz (l, 0.05, 0.35), g5, 0.005) && near (hz (l, 0.45, 0.85), c4, 0.005));
}

// One hit at `at` and no second one on its tail (a doubled clock, a "flam"): the peak over the 400 frames after the
// first 150 stays well under the hit's own.
bool singleHit (const std::vector<float>& l, long at)
{
    float first = 0.0f, after = 0.0f;
    for (long i = at; i < at + 100; ++i) first = std::max (first, std::fabs (l[(size_t) i]));
    for (long i = at + 150; i < at + 550; ++i) after = std::max (after, std::fabs (l[(size_t) i]));
    if (debug) std::printf ("debug hit at %ld: %.4f then %.4f\n", at, first, after);
    return first > 0.05f && after < 0.2f * first;
}

void testLoopsAndClockChanges()
{
    std::vector<float> l, r;
    // A 4-beat loop at 110 BPM whose end falls inside a block (frame 104,727.3), the host telling of the wrap only at
    // the next block as Melodee does: the clocks run on to the loop's end, which is its start.
    constexpr double perFrame = 110.0 / 60.0 / 48000.0, loopEnd = 4.0 / perFrame;
    auto looping = [] (uint64_t at) { return Transport { true, 110.0, std::fmod ((double) at * perFrame, 4.0) }; };
    // Every clock plays (Rhythm 1 at 1): the downbeat is one hit, not the loop's end and then the loop's start again.
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") } }));
        a.render ((uint32_t) loopEnd + 24000, 128, l, r, looping);
        CHECK (singleHit (l, (long) loopEnd + 1));
        const auto hits = onsets (l);
        CHECK (std::count_if (hits.begin(), hits.end(), [] (long h) { return h > loopEnd - 100 && h < loopEnd + 6000; }) == 1);
    }
    // Rhythm 1 at 3: the clock at the loop's end (the 16th) plays nothing, but the downbeat does: it plays when the
    // host tells of the wrap, at most a block late. The same when the host's blocks come in parts smaller than that
    // (Melodee plays a plug-in in parts between its steps, and tells the song's place only at its own blocks).
    for (uint32_t part : { 128u, 32u })
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 3 } }));
        auto parts = [] (uint64_t at) {
            const uint64_t block = at / 128 * 128; // Melodee's block: the place at its start, counted on through it
            return Transport { true, 110.0, std::fmod ((double) block * perFrame, 4.0) + (double) (at - block) * perFrame };
        };
        a.render ((uint32_t) loopEnd + 24000, part, l, r, parts);
        const auto hits = onsets (l);
        const auto down = std::find_if (hits.begin(), hits.end(), [] (long h) { return h > loopEnd - 100; });
        if (debug) std::printf ("debug wrap part %u: %ld\n", part, down == hits.end() ? -1L : *down);
        CHECK (down != hits.end() && *down >= (long) loopEnd && *down < (long) loopEnd + 128 + 60 && singleHit (l, *down - 10));
    }
    // Drift: a tempo change inside a block (120 to 125 BPM, 64 frames before the next block) moves the host's place a
    // few frames from where the clocks counted: the clock played 64 frames ago is not played again, the next comes at
    // the new tempo.
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") } }));
        a.render (48000, 128, l, r, [] (uint64_t at) {
            if (at < 24064) return playingFrom (0.0, 120.0, 48000.0, at);
            return Transport { true, 125.0, 1.0 + (double) (at - 24000) * 125.0 / 60.0 / 48000.0 };
        });
        CHECK (singleHit (l, 24000));
        const auto hits = onsets (l);
        CHECK (std::find_if (hits.begin(), hits.end(), [] (long h) { return std::labs (h - (24000 + 5760 + 10)) <= 3; }) != hits.end());
    }
    // Clock Div changed on a downbeat while playing (1/16 to 1/8 at beat 2): that downbeat still plays.
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") } }));
        a.render (96000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 48000) a.set (e, "Clock Div", a.valueOf ("Clock Div", "1/8"));
        });
        const auto got = clocksOf (onsets (l), 6000);
        if (debug) { std::printf ("debug regrid:"); for (long c : got) std::printf (" %ld", c); std::printf ("\n"); }
        CHECK ((got == std::vector<long> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14 }));
    }
    // Reset and a Clock Div change in one block: the generators restart from the next clock of the new grid. Rhythm 1
    // at 4: sixteenths 0 and 4 (frames 0 and 24,000), Reset and 1/8 at frame 30,720, then eighths 3 and 7 (36,000
    // and 84,000).
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 4 } }));
        a.render (100000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 240) a.set (e, "Reset", 1.0), a.set (e, "Clock Div", a.valueOf ("Clock Div", "1/8"));
        });
        const auto got = clocksOf (onsets (l), 6000);
        if (debug) { std::printf ("debug reset regrid:"); for (long c : got) std::printf (" %ld", c); std::printf ("\n"); }
        CHECK ((got == std::vector<long> { 0, 4, 6, 14 }));
    }
    // A count-in: Play from beat -1. Nothing before the song's top, its first clock on beat 0 (frame 24,000).
    {
        Instance a;
        a.setup (clickPatch (a));
        a.render (72000, 128, l, r, [] (uint64_t at) { return playingFrom (-1.0, 120.0, 48000.0, at); });
        const auto hits = onsets (l);
        CHECK (hits.size() == 4 && hits[0] >= 24000 && hits[0] < 24000 + 48);
        CHECK ((clocksOf (hits, 12000) == std::vector<long> { 2, 3, 4, 5 }));
    }
}

void testBlockSizes()
{
    // The same transport and events heard in blocks of 128, 37, 1 and 4096 frames: the same output, to the bit.
    std::vector<float> l128, r128, l, r;
    auto patch = [] (Instance& a) {
        return Values { { "VCO 1 Wave", 1.0 }, { "VCO 2 Wave", 0.0 }, { "VCO 1 Sub 2 Level", 0.5 }, { "VCO 2 Sub 1 Level", 0.4 }, { "Seq 1 > Sub 1", 1.0 },
                        { "Seq 2 > Sub 2", 1.0 }, { "Rhythm 3 > Seq 1", 1.0 }, { "Rhythm 4 > Seq 2", 1.0 }, { "Rhythm Logic", 1.0 },
                        { "Resonance", 0.7 }, { "Clock Div", a.valueOf ("Clock Div", "1/32") } };
    };
    auto events = [] (Instance* a) {
        return [a] (uint64_t at, uint32_t n, Events& e) {
            for (uint64_t when : { 30000ull, 61111ull }) // a cutoff move and a note inside some block
                if (when >= at && when < at + n)
                {
                    if (when == 30000)
                        e.param ((uint32_t) (when - at), a->id ("Cutoff"), 0.3);
                    else
                        e.note ((uint32_t) (when - at), CLAP_EVENT_NOTE_ON, 67);
                }
        };
    };
    auto t = [] (uint64_t at) { return playingFrom (0.0, 133.0, 48000.0, at); };
    {
        Instance a;
        a.setup (patch (a));
        a.render (96000, 128, l128, r128, t, events (&a));
    }
    for (uint32_t block : { 37u, 1u, 4096u })
    {
        Instance a;
        a.setup (patch (a));
        a.render (96000, block, l, r, t, events (&a));
        CHECK (l == l128 && r == r128);
    }
}

void testPitch()
{
    std::vector<float> l, r;
    // Held tones (EG: Hold, the host stopped), measured by their zero crossings over 0.4 to 2 s.
    auto held = [&] (Values values, double seconds = 1.0, std::function<void (uint64_t, uint32_t, Events&, Instance&)> events = {}) {
        Instance a;
        a.setup (dronePatch (a, values));
        a.render ((uint32_t) (48000 * seconds), 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t n, Events& e) {
            if (events)
                events (at, n, e, a);
        });
        return l;
    };
    // VCO Freq: middle C fully left, the top C fully right, four octaves; 12-ET keeps the knob on semitones, 8-JI on
    // the just major scale, Off leaves it. 3.4 octaves up: 40.8 semitones, so 41 (2,793.8 Hz) in 12-ET, 4/3 of the
    // third C up (2,790.7 Hz) in 8-JI, and 2,761.7 Hz unquantized.
    CHECK (near (hz (held ({ { "VCO 1 Freq", 0.0 } }, 2.5), 0.25, 2.25), c4, 0.003));
    CHECK (near (hz (held ({ { "VCO 1 Freq", 1.0 } }, 2.5), 0.25, 2.25), 4186.009, 0.001));
    {
        const double et = hz (held ({ { "VCO 1 Freq", 0.85 } }, 2.5), 0.25, 2.25);
        const double ji = hz (held ({ { "VCO 1 Freq", 0.85 }, { "Quantize", 4.0 } }, 2.5), 0.25, 2.25);
        const double off = hz (held ({ { "VCO 1 Freq", 0.85 }, { "Quantize", 0.0 } }, 2.5), 0.25, 2.25);
        if (debug) std::printf ("debug quantize: 12-ET %.1f 8-JI %.1f off %.1f\n", et, ji, off);
        CHECK (std::fabs (et - 2793.83) < 1.2 && std::fabs (ji - 2790.70) < 1.2 && std::fabs (off - 2761.73) < 1.2);
    }
    // Fine Tune: +50 cents at full turn.
    CHECK (std::fabs (hz (held ({ { "VCO 1 Freq", 0.0 }, { "Fine Tune", 1.0 } }, 2.5), 0.25, 2.25) - 269.29) < 1.0);

    // Next walks Seq 1's steps (C5, C6, G5, C4) without the EGs; Reset goes back to step 1. Seq 1 > Osc 1 off: none
    // of it reaches VCO 1.
    auto presses = [] (uint64_t at, uint32_t, Events& e, Instance& a) {
        if (at == 128 * 150 || at == 128 * 300 || at == 128 * 301) a.set (e, "Next", 1.0);
        if (at == 128 * 450) a.set (e, "Reset", 1.0);
    };
    held ({}, 2.0, presses);
    const double s1 = hz (l, 0.05, 0.38), s2 = hz (l, 0.45, 0.78), s4 = hz (l, 0.85, 1.18), back = hz (l, 1.25, 1.95);
    if (debug) std::printf ("debug next: %.1f %.1f %.1f %.1f\n", s1, s2, s4, back);
    CHECK (near (s1, c5, 0.005) && near (s2, c6, 0.005) && near (s4, c4, 0.005) && near (back, c5, 0.005));
    held ({ { "Seq 1 > Osc 1", 0.0 } }, 2.0, presses);
    CHECK (near (hz (l, 0.45, 0.78), c5, 0.005) && near (hz (l, 0.85, 1.18), c5, 0.005));

    // The sequencer on a sub moves its divisor, 3 for an octave of step (+/-2 octaves: 0.3 of the knob is 0.6 of an
    // octave, so 2): VCO 1's first sub alone at 1/5 of C5, its steps 1/5, 1/3, 1/7, 1/2. The VCO itself stays.
    auto subSteps = Values { { "VCO 1 Level", 0.0 }, { "VCO 1 Sub 1 Level", 1.0 }, { "VCO 1 Sub 1", 5 }, { "Seq 1 > Osc 1", 0.0 },
                             { "Seq 1 > Sub 1", 1.0 }, { "Seq 1 Step 2", 0.3 }, { "Seq 1 Step 3", -0.3 }, { "Seq 1 Step 4", 0.5 } };
    held (subSteps, 2.0, presses);
    {
        const double a1 = hz (l, 0.05, 0.38), a2 = hz (l, 0.45, 0.78), a4 = hz (l, 0.85, 1.18);
        if (debug) std::printf ("debug sub steps: %.1f %.1f %.1f\n", a1, a2, a4);
        CHECK (near (a1, c5 / 5.0, 0.005) && near (a2, c5 / 3.0, 0.005) && near (a4, c5 / 2.0, 0.005));
    }
    // A sub follows its VCO when the sequencer moves the VCO: Sub 2 at 1/3 under the steps' C5, C6, G5, C4.
    held ({ { "VCO 1 Level", 0.0 }, { "VCO 1 Sub 2 Level", 1.0 }, { "VCO 1 Sub 2", 3 } }, 2.0, presses);
    CHECK (near (hz (l, 0.05, 0.38), c5 / 3.0, 0.005) && near (hz (l, 0.45, 0.78), c6 / 3.0, 0.005) && near (hz (l, 0.85, 1.18), c4 / 3.0, 0.005));
    // VCO 2 with Seq 2 the same way: VCO 2 alone, its steps 0 and +7 (G).
    held ({ { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "VCO 2 Freq", 0.25 }, { "Seq 2 Step 2", 7.0 / 24.0 } }, 2.0, presses);
    CHECK (near (hz (l, 0.05, 0.38), c5, 0.005) && near (hz (l, 0.45, 0.78), g5, 0.005));
    // Saw and the pulse sound at the same pitch as the square.
    for (const char* wave : { "Saw", "Pulse/Saw" })
    {
        Instance a;
        held ({ { "VCO 1 Wave", a.valueOf ("VCO 1 Wave", wave) } }, 1.0);
        CHECK (near (hz (l, 0.1, 0.9), c5, 0.005));
    }
}

void testEnvelopesAndNotes()
{
    std::vector<float> l, r;
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    // EG: Off: the steps start nothing; Trigger still plays the EGs once and returns to 0.
    {
        Instance a;
        a.setup (clickPatch (a, { { "EG", a.valueOf ("EG", "Off") } }));
        OutEvents out;
        a.render (48000, 128, l, r, t, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 100) a.set (e, "Trigger", 1.0);
        }, &out);
        const auto hits = onsets (l);
        CHECK (hits.size() == 1 && hits[0] >= 12800 && hits[0] < 12800 + 48);
        CHECK (std::find (out.values.begin(), out.values.end(), std::pair<clap_id, double> { a.id ("Trigger"), 0.0 }) != out.values.end());
    }
    // EG: Hold, the host stopped: the VCA opens and stays open; back to On, it decays to silence.
    {
        Instance a;
        a.setup (clickPatch (a, { { "EG", a.valueOf ("EG", "Hold") }, { "VCA Decay", 0.3 } }));
        a.render (48000 * 2, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 375) a.set (e, "EG", a.valueOf ("EG", "On"));
        });
        CHECK (rms (l, 4800, 4800) > 0.1 && std::fabs (rms (l, 38400, 4800) - rms (l, 4800, 4800)) < 0.01 && rms (l, 72000, 4800) < 1e-4);
    }
    // A note (stopped): the EGs rise and hold at the top while it is down, then decay. A short VCA decay: silent soon
    // after the note ends.
    {
        Instance a;
        a.setup (clickPatch (a, { { "VCA Decay", 0.3 } }));
        a.render (48000, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, 60);
            if (at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_OFF, 60);
        });
        CHECK (rms (l, 3000, 2400) > 0.1 && std::fabs (rms (l, 20000, 2400) - rms (l, 3000, 2400)) < 0.01 && rms (l, 40000, 2400) < 1e-4);
        // A MIDI note on and off does the same; a choke ends every note.
        Instance b;
        b.setup (clickPatch (b, { { "VCA Decay", 0.3 } }));
        b.render (48000, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.midi (0, 0x90, 60, 100);
            if (at == 128 * 200) e.midi (0, 0x80, 60, 0);
        });
        CHECK (rms (l, 3000, 2400) > 0.1 && rms (l, 40000, 2400) < 1e-4);
        Instance c;
        c.setup (clickPatch (c, { { "VCA Decay", 0.3 } }));
        c.render (48000, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, 60), e.note (0, CLAP_EVENT_NOTE_ON, 64);
            if (at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_CHOKE, -1);
        });
        CHECK (rms (l, 3000, 2400) > 0.1 && rms (l, 40000, 2400) < 1e-4);
        // A note off for key -1 (any key) ends every note too.
        Instance d;
        d.setup (clickPatch (d, { { "VCA Decay", 0.3 } }));
        d.render (48000, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, 60), e.note (0, CLAP_EVENT_NOTE_ON, 67);
            if (at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_OFF, -1);
        });
        CHECK (rms (l, 3000, 2400) > 0.1 && rms (l, 40000, 2400) < 1e-4);
    }
    // Notes move both VCOs from middle C, and the last one stays after it ends: 72 an octave up, then 55 (G3).
    {
        Instance a;
        a.setup (dronePatch (a, { { "VCO 1 Freq", 0.0 }, { "EG", a.valueOf ("EG", "On") }, { "VCA Decay", 1.0 } }));
        a.render (48000 * 2, 128, l, r, [] (uint64_t) { return stopped; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.note (0, CLAP_EVENT_NOTE_ON, 72);
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_OFF, 72), e.note (1, CLAP_EVENT_NOTE_ON, 55);
            if (at == 128 * 400) e.note (0, CLAP_EVENT_NOTE_OFF, 55);
        });
        const double up = hz (l, 0.1, 0.75), g3 = hz (l, 0.85, 1.05), after = hz (l, 1.2, 1.9);
        if (debug) std::printf ("debug notes: %.1f %.1f %.1f\n", up, g3, after);
        CHECK (near (up, c5, 0.005) && near (g3, 195.998, 0.005) && near (after, 195.998, 0.005));
    }
    // Reset while playing: both sequencers to step 1 and the generators counting again from the next clock. Rhythm 1
    // at 4 (sixteenths): hits on clocks 0 and 4; Reset after clock 5; then clocks 6, 10, 14.
    {
        Instance a;
        a.setup (clickPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Rhythm 1", 4 } }));
        a.render (6000 * 16, 128, l, r, t, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 240) a.set (e, "Reset", 1.0); // frame 30,720: after clock 5
        });
        const auto got = clocksOf (onsets (l), 6000);
        CHECK ((got == std::vector<long> { 0, 4, 6, 10, 14 }));
    }
}

// The pads while Run is on (0.2.0): notes 36-51 (pads A01-A16 of a Melodee plug-in track) are the steps: 36-39 Seq
// 1's steps 1-4, 40-43 Seq 2's, 44-51 the same again. A pad shows its step until that sequencer's next step (a clock,
// Reset or Next) without moving it, and starts both EGs as Trigger does; other notes transpose and gate as before;
// with Run off every note does.
constexpr double e3 = 164.8138, e4 = 329.6276, e5 = 659.2551, b3 = 246.9417;

void testStepPads()
{
    std::vector<float> l, r, l2, r2;
    auto still = [] (uint64_t) { return stopped; };
    // A patch whose sound shows every step: both VCOs, Seq 1 on VCO 1's first sub too, 8-JI, no generator routed
    // (only the pads and the buttons move the steps), the host stopped.
    auto stepPatch = [] (Instance& a, Values extra) {
        Values v = noRhythms ({ { "VCO 1 Freq", 0.25 }, { "VCO 2 Freq", 0.3 }, { "VCO 1 Level", 0.6 }, { "VCO 1 Sub 1 Level", 0.5 },
            { "VCO 1 Sub 1", 5 }, { "VCO 1 Sub 2 Level", 0.0 }, { "VCO 2 Level", 0.6 }, { "VCO 2 Sub 1 Level", 0.0 }, { "VCO 2 Sub 2 Level", 0.0 },
            { "Quantize", a.valueOf ("Quantize", "8-JI") }, { "Seq Oct", a.valueOf ("Seq Oct", "+/-2") }, { "Seq 1 > Osc 1", 1.0 },
            { "Seq 1 > Sub 1", 1.0 }, { "Seq 1 Step 1", 0.1 }, { "Seq 1 Step 2", 0.35 }, { "Seq 1 Step 3", -0.2 }, { "Seq 1 Step 4", -0.45 },
            { "Seq 2 Step 1", -0.15 }, { "Seq 2 Step 2", 0.25 }, { "Seq 2 Step 3", 0.05 }, { "Seq 2 Step 4", -0.3 }, { "Cutoff", 0.8 },
            { "VCF EG Amt", 0.2 }, { "VCA Attack", 0.0 }, { "VCA Decay", 0.5 }, { "Volume", 0.7 } });
        v.insert (v.end(), extra.begin(), extra.end());
        return v;
    };
    auto play = [&] (const Values& extra, const std::function<void (uint64_t, Events&, Instance&)>& events, std::vector<float>& out,
                     uint32_t block = 128) {
        Instance a;
        a.setup (stepPatch (a, extra));
        std::vector<float> right;
        a.render (28800, block, out, right, still, [&] (uint64_t at, uint32_t, Events& e) { events (at, e, a); });
    };
    // Each pad against the sequencer playing that step: Next pressed up to it and Trigger, on the pad's frame (the
    // other sequencer's steps all alike, since Next moves it too), after a note (64) has transposed both VCOs. In EG
    // On, Off (the pad starts the EGs as Trigger does) and Hold (it only moves the pitch); a CLAP note and a MIDI one.
    for (int seq = 0; seq < 2; ++seq)
        for (double mode : { 1.0, 0.0, 2.0 }) // EG: On, Off, Hold
            for (int i = 0; i < 4; ++i)
            {
                Values extra { { "EG", mode } };
                for (int k = 1; k <= 4; ++k)
                    extra.push_back ({ "Seq " + std::to_string (2 - seq) + " Step " + std::to_string (k), seq == 0 ? -0.15 : 0.1 });
                auto before = [] (uint64_t at, Events& e) {
                    if (at == 128 * 5) e.note (0, CLAP_EVENT_NOTE_ON, 64);
                    if (at == 128 * 6) e.note (0, CLAP_EVENT_NOTE_OFF, 64);
                };
                std::vector<float> pad, padMidi, sequencer;
                const int16_t key = (int16_t) (36 + 4 * seq + i);
                play (extra, [&] (uint64_t at, Events& e, Instance&) {
                    before (at, e);
                    if (at == 128 * 40) e.note (0, CLAP_EVENT_NOTE_ON, key);
                    if (at == 128 * 60) e.note (0, CLAP_EVENT_NOTE_OFF, key);
                }, pad);
                play (extra, [&] (uint64_t at, Events& e, Instance&) { // pads 44-51 are 36-43 again; as MIDI
                    before (at, e);
                    if (at == 128 * 40) e.midi (0, 0x90, (uint8_t) (key + 8), 100);
                    if (at == 128 * 60) e.midi (0, 0x80, (uint8_t) (key + 8), 0);
                }, padMidi);
                play (extra, [&] (uint64_t at, Events& e, Instance& a) {
                    before (at, e);
                    if (at == 128 * 40)
                    {
                        for (int n = 0; n < i; ++n)
                            a.set (e, "Next", 1.0);
                        a.set (e, "Trigger", 1.0);
                    }
                }, sequencer);
                if (debug && pad != sequencer)
                    std::printf ("debug pad %d (EG %g) differs from its step\n", key, mode);
                CHECK (pad == sequencer && padMidi == sequencer && rms (pad, 5120, 4800) > 0.01);
            }
    // Pads on one frame: two of one sequencer, the later wins (37 then 38 is 38: Next twice and Trigger); one of each,
    // both apply in either order; the EGs start once.
    {
        auto pads = [&] (std::vector<int16_t> keys, std::vector<float>& out) {
            play ({}, [&] (uint64_t at, Events& e, Instance&) {
                if (at == 128 * 40)
                    for (int16_t k : keys)
                        e.note (0, CLAP_EVENT_NOTE_ON, k);
            }, out);
        };
        std::vector<float> both, one;
        Values same;
        for (int k = 1; k <= 4; ++k)
            same.push_back ({ "Seq 2 Step " + std::to_string (k), 0.25 });
        play (same, [&] (uint64_t at, Events& e, Instance&) { if (at == 128 * 40) e.note (0, CLAP_EVENT_NOTE_ON, 37), e.note (0, CLAP_EVENT_NOTE_ON, 38); }, both);
        play (same, [&] (uint64_t at, Events& e, Instance& a) { if (at == 128 * 40) a.set (e, "Next", 1.0), a.set (e, "Next", 1.0), a.set (e, "Trigger", 1.0); }, one);
        CHECK (both == one && onsets (both).size() == 1);
        pads ({ 39, 42 }, both);
        pads ({ 42, 39 }, one);
        CHECK (both == one && onsets (both).size() == 1);
        // Measured: VCO 1 alone, then VCO 2 alone, held (EG: Hold), both pads on one frame: Seq 1's step 4 (C4) and
        // Seq 2's step 3 (+4: E5).
        for (int vco = 1; vco <= 2; ++vco)
        {
            Instance a;
            a.setup (dronePatch (a, { { "VCO 1 Level", vco == 1 ? 1.0 : 0.0 }, { "VCO 2 Level", vco == 2 ? 1.0 : 0.0 }, { "VCO 2 Freq", 0.25 },
                                      { "Seq 2 Step 3", 4.0 / 24.0 } }));
            a.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
                if (at == 128 * 75) e.note (0, CLAP_EVENT_NOTE_ON, 39), e.note (0, CLAP_EVENT_NOTE_ON, 42);
            });
            const double before = hz (l, 0.05, 0.19), after = hz (l, 0.25, 0.95);
            if (debug) std::printf ("debug two pads, VCO %d: %.1f then %.1f\n", vco, before, after);
            CHECK (near (before, c5, 0.005) && near (after, vco == 1 ? c4 : e5, 0.005));
        }
    }
    // Stopped: a pad plays its step without moving the sequencer. Seq 1 on C5; pad 38 (step 3, G5); Next goes to step
    // 2 (C6), not 4; pad 39 (C4); Reset back to step 1 (C5).
    {
        Instance a;
        a.setup (dronePatch (a));
        a.render (96000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 150) e.note (0, CLAP_EVENT_NOTE_ON, 38);
            if (at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_OFF, 38);
            if (at == 128 * 300) a.set (e, "Next", 1.0);
            if (at == 128 * 450) e.note (0, CLAP_EVENT_NOTE_ON, 39), e.note (0, CLAP_EVENT_NOTE_OFF, 39);
            if (at == 128 * 600) a.set (e, "Reset", 1.0);
        });
        const double s1 = hz (l, 0.05, 0.38), p3 = hz (l, 0.45, 0.78), s2 = hz (l, 0.85, 1.18), p4 = hz (l, 1.25, 1.58), back = hz (l, 1.65, 1.95);
        if (debug) std::printf ("debug pads stopped: %.1f %.1f %.1f %.1f %.1f\n", s1, p3, s2, p4, back);
        CHECK (near (s1, c5, 0.005) && near (p3, g5, 0.005) && near (s2, c6, 0.005) && near (p4, c4, 0.005) && near (back, c5, 0.005));
    }
    // Playing (Rhythm 1 into Seq 1 on quarter notes: C5, C6, G5, C4 a half second each): pad 39 (C4) inside the first
    // step holds until the next clock, which plays step 2 as it would have; pad 37 (C6, as MIDI) inside step 3 the same.
    {
        Instance a;
        a.setup (dronePatch (a, { { "Rhythm 1", 1 }, { "Rhythm 1 > Seq 1", 1.0 }, { "Clock Div", a.valueOf ("Clock Div", "1/4") } }));
        a.render (96000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 94) e.note (0, CLAP_EVENT_NOTE_ON, 39);
            if (at == 128 * 100) e.note (0, CLAP_EVENT_NOTE_OFF, 39);
            if (at == 128 * 430) e.midi (0, 0x90, 37, 90), e.midi (0, 0x80, 37, 0);
        });
        const double w[6] = { hz (l, 0.05, 0.24), hz (l, 0.27, 0.49), hz (l, 0.55, 0.95), hz (l, 1.02, 1.14), hz (l, 1.17, 1.49), hz (l, 1.55, 1.95) };
        if (debug) std::printf ("debug pads playing: %.1f %.1f %.1f %.1f %.1f %.1f\n", w[0], w[1], w[2], w[3], w[4], w[5]);
        CHECK (near (w[0], c5, 0.005) && near (w[1], c4, 0.005) && near (w[2], c6, 0.005) && near (w[3], g5, 0.005) && near (w[4], c6, 0.005)
               && near (w[5], c4, 0.005));
    }
    // A pad on Play's frame (its events come before the transport): Play at beat 2.2 puts Seq 1 on step 3 (G5) until
    // beat 3; pad 36 with it shows step 1 (C5) until then, and beat 3 plays step 4 (C4) as without it.
    {
        Instance a;
        a.setup (dronePatch (a, { { "Rhythm 1", 1 }, { "Rhythm 1 > Seq 1", 1.0 }, { "Clock Div", a.valueOf ("Clock Div", "1/4") } }));
        a.render (48000, 128, l, r, [] (uint64_t at) { return at < 1280 ? Transport { false, 120.0, 2.2 } : playingFrom (2.2, 120.0, 48000.0, at - 1280); },
                  [&] (uint64_t at, uint32_t, Events& e) { if (at == 1280) e.note (0, CLAP_EVENT_NOTE_ON, 36); });
        if (debug) std::printf ("debug pad with Play: %.1f %.1f\n", hz (l, 0.05, 0.41), hz (l, 0.45, 0.9));
        CHECK (near (hz (l, 0.05, 0.41), c5, 0.005) && near (hz (l, 0.45, 0.9), c4, 0.005));
        // Play from the top: the clock on Play's frame steps Seq 1 after the pad, so the pad gives way to it at once
        // (the same output as without it); Seq 2, which no generator moves, keeps the pad's step (41: step 2, G5).
        for (int16_t key : { (int16_t) 38, (int16_t) -1 })
        {
            Instance b;
            b.setup (dronePatch (b, { { "Rhythm 1", 1 }, { "Rhythm 1 > Seq 1", 1.0 }, { "Clock Div", b.valueOf ("Clock Div", "1/4") } }));
            b.render (48000, 128, key < 0 ? l2 : l, r, [] (uint64_t at) { return at < 1280 ? stopped : playingFrom (0.0, 120.0, 48000.0, at - 1280); },
                      [&] (uint64_t at, uint32_t, Events& e) { if (at == 1280 && key >= 0) e.note (0, CLAP_EVENT_NOTE_ON, key); });
        }
        CHECK (l == l2);
        Instance c;
        c.setup (dronePatch (c, { { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "VCO 2 Freq", 0.25 }, { "Seq 2 Step 2", 7.0 / 24.0 },
                                  { "Rhythm 1", 1 }, { "Rhythm 1 > Seq 1", 1.0 }, { "Clock Div", c.valueOf ("Clock Div", "1/4") } }));
        c.render (72000, 128, l, r, [] (uint64_t at) { return at < 1280 ? stopped : playingFrom (0.0, 120.0, 48000.0, at - 1280); },
                  [&] (uint64_t at, uint32_t, Events& e) { if (at == 1280) e.note (0, CLAP_EVENT_NOTE_ON, 41); });
        CHECK (near (hz (l, 0.05, 1.45), g5, 0.005));
    }
    // Other notes still transpose (52 and 35, just beside the pads), and the pads keep the transposition: C5; 52 (-8:
    // E4); pad 36 (step 1: E4 still); pad 37 (step 2, an octave up: E5); 35 held (-25 from step 2: B3).
    {
        Instance a;
        a.setup (dronePatch (a));
        a.render (72000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 75) e.note (0, CLAP_EVENT_NOTE_ON, 52);
            if (at == 128 * 150) e.note (0, CLAP_EVENT_NOTE_OFF, 52);
            if (at == 128 * 225) e.note (0, CLAP_EVENT_NOTE_ON, 36);
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_ON, 37);
            if (at == 128 * 375) e.midi (0, 0x90, 35, 100);
        });
        const double w[5] = { hz (l, 0.05, 0.19), hz (l, 0.22, 0.58), hz (l, 0.62, 0.79), hz (l, 0.82, 0.99), hz (l, 1.02, 1.45) };
        if (debug) std::printf ("debug pads and notes: %.1f %.1f %.1f %.1f %.1f\n", w[0], w[1], w[2], w[3], w[4]);
        CHECK (near (w[0], c5, 0.005) && near (w[1], e4, 0.005) && near (w[2], e4, 0.005) && near (w[3], e5, 0.005) && near (w[4], b3, 0.005));
    }
    // Run off: the pads are notes again (40 is -20, E3; 48 an octave down, C4), held as a gate; switched on, a pad
    // (40, Seq 2's step 1) leaves VCO 1 where the note put it.
    {
        Instance a;
        a.setup (dronePatch (a, { { "Run", 0.0 } }));
        a.render (84000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 75) e.note (0, CLAP_EVENT_NOTE_ON, 40);
            if (at == 128 * 150) e.note (0, CLAP_EVENT_NOTE_OFF, 40);
            if (at == 128 * 225) a.set (e, "Run", 1.0);
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_ON, 40), e.note (0, CLAP_EVENT_NOTE_OFF, 40);
            if (at == 128 * 375) a.set (e, "Run", 0.0);
            if (at == 128 * 450) e.note (0, CLAP_EVENT_NOTE_ON, 48);
        });
        const double w[4] = { hz (l, 0.22, 0.58), hz (l, 0.62, 0.78), hz (l, 0.82, 1.18), hz (l, 1.22, 1.7) };
        if (debug) std::printf ("debug run off: %.1f %.1f %.1f %.1f\n", w[0], w[1], w[2], w[3]);
        CHECK (near (w[0], e3, 0.005) && near (w[1], e3, 0.005) && near (w[2], e3, 0.005) && near (w[3], c4, 0.005));
    }
    // Gates (EG On, a short VCA decay, stopped). Run off: note 40 holds the EGs while it is down. Run on: note 52 does
    // too, pad 40 does not (one hit that decays while the key is still down).
    for (const auto& gate : { std::tuple { 0.0, 40, true }, std::tuple { 1.0, 52, true }, std::tuple { 1.0, 40, false } })
    {
        const double run = std::get<0> (gate);
        const int key = std::get<1> (gate);
        const bool holds = std::get<2> (gate);
        Instance a;
        a.setup (clickPatch (a, { { "VCA Decay", 0.3 }, { "Run", run } }));
        a.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) key);
            if (at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_OFF, (int16_t) key);
        });
        if (debug) std::printf ("debug gate run %g key %d: %.4f %.4f %.4f\n", run, key, rms (l, 3000, 2400), rms (l, 20000, 2400), rms (l, 40000, 2400));
        CHECK (rms (l, 1280, 1200) > 0.1 && rms (l, 40000, 2400) < 1e-4);
        CHECK (holds ? std::fabs (rms (l, 20000, 2400) - rms (l, 3000, 2400)) < 0.01 : rms (l, 20000, 2400) < 1e-4);
    }
    // On, on, off of one pad (a pad hit again while its off is pending): two hits, each decaying while it is down; its
    // off ends nothing. Under a held note (60), a pad and its off leave the note's gate held.
    {
        Instance a;
        a.setup (clickPatch (a, { { "VCA Decay", 0.3 } }));
        a.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10 || at == 128 * 100) e.note (0, CLAP_EVENT_NOTE_ON, 37);
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_OFF, 37);
        });
        const auto hits = onsets (l);
        CHECK (hits.size() == 2 && hits[0] < 1280 + 48 && hits[1] >= 12800 && hits[1] < 12800 + 48);
        CHECK (rms (l, 8000, 2400) < 1e-4 && rms (l, 30000, 2400) < 1e-4);
        Instance b;
        b.setup (clickPatch (b, { { "VCA Decay", 0.3 } }));
        b.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, 60);
            if (at == 128 * 100) e.note (0, CLAP_EVENT_NOTE_ON, 37);
            if (at == 128 * 150) e.note (0, CLAP_EVENT_NOTE_OFF, 37), e.midi (0, 0x80, 37, 0), e.note (0, CLAP_EVENT_NOTE_CHOKE, 37);
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_OFF, 60);
        });
        CHECK (rms (l, 3000, 2400) > 0.1 && std::fabs (rms (l, 30000, 2400) - rms (l, 3000, 2400)) < 0.01 && rms (l, 44000, 2400) < 1e-4);
    }
    // A choke and a CC 120/123 storm on every channel while a note and a pad are down: all quiet; the next pad is one
    // hit that decays (nothing held over).
    {
        Instance a;
        a.setup (clickPatch (a, { { "VCA Decay", 0.3 } }));
        a.render (72000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 10) e.note (0, CLAP_EVENT_NOTE_ON, 60), e.note (0, CLAP_EVENT_NOTE_ON, 37);
            if (at == 128 * 100)
            {
                e.note (0, CLAP_EVENT_NOTE_CHOKE, 60), e.note (0, CLAP_EVENT_NOTE_CHOKE, 37);
                e.note (0, CLAP_EVENT_NOTE_OFF, 60), e.note (0, CLAP_EVENT_NOTE_OFF, 37);
                for (uint8_t ch = 0; ch < 16; ++ch)
                    e.midi (0, (uint8_t) (0xb0 | ch), 120, 0), e.midi (0, (uint8_t) (0xb0 | ch), 123, 0);
            }
            if (at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_ON, 45);
        });
        const auto hits = onsets (l);
        CHECK (rms (l, 6000, 2400) > 0.1 && rms (l, 25000, 2400) < 1e-4 && rms (l, 48000, 2400) < 1e-4);
        CHECK (hits.size() == 2 && hits[1] >= 38400 && hits[1] < 38400 + 48);
    }
    // The same output whatever the block size, with pads inside blocks: playing (two pads on one frame, a MIDI pad, a
    // note that transposes, Run off and a pad as a note, Run on again) and stopped.
    {
        auto patch = [] (Instance& a) {
            return Values { { "VCO 1 Wave", 1.0 }, { "VCO 2 Wave", 0.0 }, { "VCO 1 Sub 2 Level", 0.5 }, { "VCO 2 Sub 1 Level", 0.4 }, { "Seq 1 > Sub 1", 1.0 },
                            { "Seq 2 > Sub 2", 1.0 }, { "Rhythm 3 > Seq 1", 1.0 }, { "Rhythm 4 > Seq 2", 1.0 }, { "Resonance", 0.7 },
                            { "Clock Div", a.valueOf ("Clock Div", "1/32") } };
        };
        auto events = [] (Instance* a) {
            return [a] (uint64_t at, uint32_t n, Events& e) {
                for (uint64_t when : { 20001ull, 45000ull, 61111ull, 63000ull, 70000ull, 75000ull, 80000ull, 85000ull, 88000ull, 90000ull })
                    if (when >= at && when < at + n)
                    {
                        const uint32_t t = (uint32_t) (when - at);
                        switch (when)
                        {
                            case 20001: e.note (t, CLAP_EVENT_NOTE_ON, 37); break;
                            case 45000: e.note (t, CLAP_EVENT_NOTE_ON, 39), e.note (t, CLAP_EVENT_NOTE_ON, 42); break;
                            case 61111: e.midi (t, 0x90, 51, 100); break;
                            case 63000: e.midi (t, 0x80, 51, 0); break;
                            case 70000: e.note (t, CLAP_EVENT_NOTE_ON, 67); break;
                            case 75000: e.note (t, CLAP_EVENT_NOTE_OFF, 67); break;
                            case 80000: e.param (t, a->id ("Run"), 0.0); break;
                            case 85000: e.note (t, CLAP_EVENT_NOTE_ON, 40); break;
                            case 88000: e.param (t, a->id ("Run"), 1.0); break;
                            default: e.note (t, CLAP_EVENT_NOTE_ON, 44); break;
                        }
                    }
            };
        };
        auto t = [] (uint64_t at) { return playingFrom (0.0, 133.0, 48000.0, at); };
        std::vector<float> l128, r128, s128;
        auto stoppedRun = [&] (uint32_t block, std::vector<float>& out) {
            Instance a;
            a.setup (stepPatch (a, {}));
            std::vector<float> right;
            a.render (28800, block, out, right, still, [&] (uint64_t at, uint32_t n, Events& e) {
                for (uint64_t when : { 1000ull, 13001ull, 20000ull, 24999ull })
                    if (when >= at && when < at + n)
                    {
                        const uint32_t f = (uint32_t) (when - at);
                        if (when == 13001) e.note (f, CLAP_EVENT_NOTE_ON, 38), e.note (f, CLAP_EVENT_NOTE_ON, 43);
                        else if (when == 20000) e.param (f, a.id ("Next"), 1.0);
                        else e.note (f, CLAP_EVENT_NOTE_ON, when == 1000 ? 41 : 47);
                    }
            });
        };
        {
            Instance a;
            a.setup (patch (a));
            a.render (96000, 128, l128, r128, t, events (&a));
            stoppedRun (128, s128);
        }
        for (uint32_t block : { 37u, 1u, 4096u })
        {
            Instance a;
            a.setup (patch (a));
            a.render (96000, block, l, r, t, events (&a));
            CHECK (l == l128 && r == r128);
            stoppedRun (block, l2);
            CHECK (l2 == s128);
        }
        CHECK (rms (s128, 13001, 4800) > 0.01);
    }
}

void testExtremes()
{
    // Random settings, every one in its range: the output stays a number and within full scale.
    std::mt19937 random (7);
    std::vector<float> l, r;
    for (int round = 0; round < 60; ++round)
    {
        Instance a;
        Values values;
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            const std::string name = info.name;
            if (name == "Trigger" || name == "Next" || name == "Reset")
                continue;
            std::uniform_real_distribution<double> d (info.min_value, info.max_value);
            double v = round < 2 ? (round == 0 ? info.min_value : info.max_value) : d (random);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round (v);
            values.push_back ({ info.name, v });
        }
        a.setup (values);
        a.render (24000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 170.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 50) e.note (3, CLAP_EVENT_NOTE_ON, (int16_t) (24 + round % 80));
        });
        bool finite = true;
        float peak = 0.0f;
        long railed = 0, longest = 0; // samples in a row at the ceiling exactly: what a NaN inside comes out as
        for (float s : l)
        {
            finite &= std::isfinite (s), peak = std::max (peak, std::fabs (s));
            railed = std::fabs (s) == 0.98f ? railed + 1 : 0;
            longest = std::max (longest, railed);
        }
        CHECK (finite && peak <= 1.0f && longest < 64);
    }
}

// A press on the page counts where the same button from the host at the block's first frame does: before the
// transport, so Next pressed on the page as Play starts mid-song does what Next sent with Play does (the song's place
// wins), whichever way it came.
void testPressWithPlay()
{
    auto run = [] (bool fromPage) {
        Instance a;
        CHECK (a.loadPreset ("polyrhythm-bass"));
        std::vector<float> l, r;
        a.render (128 * 8, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; });
        if (fromPage)
        {
            const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
            for (const melodee_screen_input_t e : { melodee_screen_input_t { MELODEE_SCREEN_STEP, 1, 0 },
                                                     melodee_screen_input_t { MELODEE_SCREEN_WHEEL, 4, 0 },
                                                     melodee_screen_input_t { MELODEE_SCREEN_PUSH, 0, 0 } })
                CHECK (screen->input (a.plugin, &e));
        }
        a.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (2.5, 120.0, 48000.0, at); },
                  [&] (uint64_t at, uint32_t, Events& e) { if (! fromPage && at == 0) a.set (e, "Next", 1.0); });
        return l;
    };
    const auto page = run (true), host = run (false);
    CHECK (! page.empty() && page == host);
}

void testScreen()
{
    Instance a;
    const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
    CHECK (screen != nullptr);
    if (!screen) return;
    constexpr unsigned stride = 327;
    constexpr uint32_t sentinel = 0x2468acdf, litKey = 0xffffc59a, paper = 0xfffff2e6;
    std::vector<uint32_t> pixels (stride * 218, sentinel);
    melodee_screen_frame_t c {};
    c.pixels = pixels.data(); c.width = 320; c.height = 218; c.stride = stride; c.area = MELODEE_SCREEN_PAGE;
    for (int k = 0; k < 3; ++k) c.pots[k] = -1, c.pots_live[k] = 1;
    auto paint = [&](bool whole = false) { c.redraw_all = whole ? 1u : 0u; return screen->draw (a.plugin, &c); };
    auto in = [&](uint32_t kind, int v = 0) { melodee_screen_input_t e { kind, v, 0 }; return screen->input (a.plugin, &e); };
    auto knobs = [&] { std::array<uint32_t,3> ids {}; screen->controls (a.plugin, ids.data()); return ids; };
    auto pixel = [&](int x, int y) { return pixels[y * stride + x]; };
    auto step = [&](int lane, int n) { return pixel (58 + lane * 159 + n * 25, 160) == litKey; };
    std::vector<float> l, r;
    auto block = [&](double beat, bool playing, OutEvents* out = nullptr) {
        a.render (128, 128, l, r, [&](uint64_t) { return Transport { playing, 120.0, beat }; }, {}, out);
    };

    CHECK (paint (true) && !paint());
    bool filled = true, padding = true;
    for (int y = 0; y < 218; ++y)
        for (unsigned x = 0; x < stride; ++x)
            if (x < 320) filled &= pixel (x,y) != sentinel;
            else padding &= pixel (x,y) == sentinel;
    CHECK (filled && padding);
    // The static serif and idle rhythm page need no periodic repaint.
    const auto still = pixels;
    const int idleRequests = screenRequests;
    c.time_ms = 1920;
    CHECK (!paint() && screenRequests == idleRequests && pixels == still);
    CHECK (paint (true) && pixels == still);
    auto changed = [&](const std::vector<uint32_t>& prior, int x, int y, int w, int h) {
        for (int j = y; j < y + h; ++j)
        {
            const int start = j * int (stride) + x;
            if (!std::equal (pixels.begin() + start, pixels.begin() + start + w, prior.begin() + start)) return true;
        }
        return false;
    };
    for (int g = 0; g < 4; ++g)
    {
        const int cx = 43 + 78 * g;
        CHECK (!changed (still, cx - 16, 70, 32, 32)); // stopped transport holds the wheel glass
        CHECK (pixel (cx, 60) == paper);
    }
    CHECK (!changed (still, 0, 36, stride, 182)); // the chrome panel stays still on stopped transport
    const auto animated = pixels;
    CHECK (paint (true) && pixels == animated);
    auto routeThird = [&](int first, int second) {
        Events input; OutEvents output;
        a.set (input, "Rhythm 3 > Seq 1", first);
        a.set (input, "Rhythm 3 > Seq 2", second);
        a.params->flush (a.plugin, &input.in, &output.out);
        CHECK (paint());
    };
    routeThird (1, 0);
    CHECK (changed (animated, 183, 70, 32, 32));
    const auto routed = pixels;
    routeThird (1, 1); CHECK (!changed (routed, 183, 70, 32, 32));
    routeThird (0, 1); CHECK (!changed (routed, 183, 70, 32, 32));
    routeThird (0, 0); CHECK (pixels == animated);
    CHECK (!in (99));
    CHECK (!in (MELODEE_SCREEN_STEP, -1));
    CHECK ((knobs() == std::array<clap_id,3> { a.id ("Rhythm 1"), a.id ("Rhythm 1 > Seq 1"), a.id ("Rhythm 1 > Seq 2") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK ((knobs() == std::array<clap_id,3> { a.id ("Rhythm 3"), a.id ("Rhythm 3 > Seq 1"), a.id ("Rhythm 3 > Seq 2") }));
    CHECK (in (MELODEE_SCREEN_PUSH));
    const int asked = processRequests, flushed = flushRequests, rescanned = rescans;
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK (a.value ("Rhythm 3") == 7 && processRequests > asked && flushRequests > flushed && rescans > rescanned);
    OutEvents told; Events empty;
    a.params->flush (a.plugin, &empty.in, &told.out);
    CHECK (std::find (told.values.begin(), told.values.end(), std::make_pair (a.id ("Rhythm 3"), 7.0)) != told.values.end());
    CHECK (in (MELODEE_SCREEN_PUSH));
    paint();
    c.pots[0] = 127, c.pots_live[0] = 0; CHECK (paint() && !paint()); c.pots[0] = -1, c.pots_live[0] = 1;
    c.shift = 1; CHECK (paint()); c.shift = 0; CHECK (paint());

    const int before = screenRequests;
    CHECK (a.loadPreset ("polyrhythm-bass") && screenRequests > before);
    // Clock seven: 3 and 7 advance the first sequencer on 0,3,6,7. 4 and 7 advance the other on 0,4,7.
    block (1.75, true);
    const int whilePlaying = screenRequests;
    CHECK (paint() && step (0,3) && step (1,2) && screenRequests > whilePlaying);
    CHECK (pixel (199,60) == paper && pixel (66,99) == paper);
    c.time_ms += 60; CHECK (paint());
    c.time_ms += 200; paint();
    CHECK (!paint()); // no pixels changed twice at the same timestamp
    // At clock twelve, generators 1 and 2 pulse, and both stars return to the top.
    block (3.0, true); CHECK (paint());
    CHECK (pixel (43,60) == paper && pixel (121,60) == paper);

    // Moving from the rhythm page to CLOCK exposes the original transport buttons.
    CHECK (in (MELODEE_SCREEN_STEP, 1) && knobs()[0] == a.id ("Clock Div"));
    CHECK (in (MELODEE_SCREEN_WHEEL, 4) && knobs()[0] == CLAP_INVALID_ID); // Next
    a.plugin->reset (a.plugin); block (0.0, false); paint();
    CHECK (step (0,0) && step (1,0));
    c.time_ms = 0; paint();
    const auto clockPage = pixels;
    constexpr int eyeX = 166, eyeY = 82, eyeW = 72, eyeH = 28;
    c.time_ms = 2000; CHECK (paint() && changed (clockPage, 192, 88, 26, 19)); // held glance
    c.time_ms = 5400; paint();
    const auto openEye = pixels;
    c.time_ms = 5760; CHECK (paint() && changed (openEye, 192, 88, 26, 19)); // lid shuts
    c.time_ms = 6200; CHECK (paint() && !changed (openEye, eyeX, eyeY, eyeW, eyeH)); // reopens at the same gaze
    CHECK (!changed (clockPage, 0, 36, stride, eyeY - 36) && !changed (clockPage, 0, eyeY + eyeH, stride, 218 - eyeY - eyeH));
    CHECK (!changed (clockPage, 0, eyeY, eyeX, eyeH) && !changed (clockPage, eyeX + eyeW, eyeY, stride - eyeX - eyeW, eyeH));
    CHECK (!paint());
    // Stopped and left alone 4 s on a settings page, the eye rests: no next paint asked for (battery). Opened again, it
    // moves.
    c.time_ms = 20000;
    const int resting = screenRequests;
    paint();
    CHECK (screenRequests == resting);
    paint (true);
    CHECK (screenRequests > resting);
    const int asleep = processRequests;
    CHECK (in (MELODEE_SCREEN_PUSH) && processRequests > asleep);
    std::this_thread::sleep_for (std::chrono::milliseconds (550));
    CHECK (in (MELODEE_SCREEN_PUSH)); // a press never expires: the slept-on one counts too
    block (0.0, false); paint();
    CHECK (step (0,2) && step (1,2));
    CHECK (in (MELODEE_SCREEN_PUSH) && in (MELODEE_SCREEN_PUSH));
    block (0.0, false); paint(); CHECK (step (0,0) && step (1,0));

    // Each non-button parameter remains reachable with a pot somewhere on the custom panels.
    std::vector<clap_id> reachable;
    in (MELODEE_SCREEN_STEP,-1);
    for (int group = 0; group < 9; ++group)
    {
        in (MELODEE_SCREEN_WHEEL,-100);
        for (int row = 0; row < 10; ++row)
        {
            const auto ids = knobs(); reachable.insert (reachable.end(), ids.begin(), ids.end());
            in (MELODEE_SCREEN_WHEEL,1);
        }
        in (MELODEE_SCREEN_STEP,1);
    }
    for (const auto& [name,id] : a.ids)
        if (name != "Reset" && name != "Next" && name != "Trigger")
            CHECK (std::find (reachable.begin(), reachable.end(), id) != reachable.end());
    CHECK (!in (MELODEE_SCREEN_STEP,1));
    c.width = 319; CHECK (!paint (true));
}

void testScreenColorClock()
{
    Instance a;
    a.setup ({{ "Run", 0 }}); // Hold the playheads so the only moving pixels are the unrouted palettes.
    const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
    if (!screen) { CHECK (screen != nullptr); return; }
    std::vector<uint32_t> pixels (320 * 218);
    melodee_screen_frame_t c {};
    c.pixels = pixels.data(); c.width = c.stride = 320; c.height = 218; c.area = MELODEE_SCREEN_PAGE;
    for (int k = 0; k < 3; ++k) c.pots[k] = -1, c.pots_live[k] = 1;
    std::vector<float> l, r;
    auto draw = [&](Transport t, uint32_t frames = 1) {
        a.render (frames, 1024, l, r, [t](uint64_t) { return t; });
        screen->draw (a.plugin, &c);
    };
    draw ({ true, 96, 0 });
    const auto start = pixels;
    const int beforeColor = screenRequests;
    draw ({ true, 96, 8 });
    const auto half = pixels;
    CHECK (screenRequests > beforeColor);
    for (int g = 0; g < 4; ++g)
    {
        const int cx = 43 + 78 * g;
        unsigned changed = 0;
        for (int y = 70; y < 102; ++y)
            for (int x = cx - 16; x < cx + 16; ++x)
                changed += start[320 * y + x] != half[320 * y + x];
        CHECK (g < 2 ? changed == 0 : changed > 900);
    }
    draw ({ false, 96, 0 }); CHECK (pixels == half);
    draw ({ true, 150, 16 }); CHECK (pixels == start);
    draw ({ true, 150, -8 }); CHECK (pixels == half);
    draw ({ true, 120, 6, true, 6, 8 }); CHECK (pixels == half);
    draw ({ true, 120, 12, true, 6, 8 }); CHECK (pixels == start);

    // A tempo-only host supplies no beat position. Change tempo halfway around.
    draw (stopped);
    draw ({ true, 96, 0, false }, 240000); CHECK (pixels == half);
    draw ({ true, 150, 0, false }, 153600); CHECK (pixels == start);
}

void demos (const std::string& folder)
{
    // A few patterns to listen to, 8 s each at 120 BPM: the factory presets. Each preset (loaded by its key) must
    // give exactly the values listed here and the same sound; the files are rendered from the preset.
    struct Demo { const char* file; const char* preset; Values values; };
    std::vector<Demo> list {
        { "demo-default.wav", "default", {} },
        { "demo-subharmonic-chords.wav", "subharmonic-chords", {
            { "Quantize", 3 }, { "VCO 1 Sub 1", 3 }, { "VCO 1 Sub 2", 5 }, { "VCO 2 Freq", 7.0 / 48.0 }, { "VCO 2 Sub 1", 4 }, { "VCO 2 Sub 2", 6 },
            { "VCO 1 Level", 0.5 }, { "VCO 1 Sub 1 Level", 0.6 }, { "VCO 1 Sub 2 Level", 0.5 }, { "VCO 2 Level", 0.45 }, { "VCO 2 Sub 1 Level", 0.5 },
            { "VCO 2 Sub 2 Level", 0.4 }, { "Cutoff", 0.5 }, { "Resonance", 0.25 }, { "VCF EG Amt", 0.35 }, { "VCF Attack", 0.35 }, { "VCF Decay", 0.6 },
            { "VCA Attack", 0.3 }, { "VCA Decay", 0.7 }, { "Seq 1 Step 2", 5.0 / 24.0 }, { "Seq 1 Step 3", 3.0 / 24.0 }, { "Seq 1 Step 4", 7.0 / 24.0 },
            { "Seq 2 Step 2", -2.0 / 24.0 }, { "Seq 2 Step 3", 3.0 / 24.0 }, { "Seq 2 Step 4", -4.0 / 24.0 }, { "Rhythm 1", 8 }, { "Rhythm 2", 6 } } },
        { "demo-polyrhythm-bass.wav", "polyrhythm-bass", {
            { "VCO 1 Sub 1", 2 }, { "VCO 1 Sub 2", 4 }, { "VCO 1 Level", 0.35 }, { "VCO 1 Sub 1 Level", 0.8 }, { "VCO 1 Sub 2 Level", 0.6 },
            { "VCO 2 Freq", 0.0 }, { "VCO 2 Sub 1", 3 }, { "VCO 2 Sub 2", 4 }, { "VCO 2 Level", 0.0 }, { "VCO 2 Sub 1 Level", 0.5 }, { "VCO 2 Sub 2 Level", 0.0 },
            { "Seq 2 > Osc 2", 0.0 }, { "Seq 2 > Sub 1", 1.0 }, { "Cutoff", 0.38 }, { "Resonance", 0.45 }, { "VCF EG Amt", 0.5 }, { "VCF Attack", 0.0 },
            { "VCF Decay", 0.35 }, { "VCA Attack", 0.0 }, { "VCA Decay", 0.45 }, { "Seq Oct", 0 }, { "Seq 1 Step 2", 3.0 / 12.0 },
            { "Seq 1 Step 3", 7.0 / 12.0 }, { "Seq 1 Step 4", -5.0 / 12.0 }, { "Seq 2 Step 2", -0.34 }, { "Seq 2 Step 3", 0.34 }, { "Seq 2 Step 4", 0.0 },
            { "Rhythm 1", 3 }, { "Rhythm 2", 4 }, { "Rhythm 3", 7 }, { "Rhythm 3 > Seq 1", 1.0 }, { "Rhythm 3 > Seq 2", 1.0 } } },
        { "demo-pwm-pad.wav", "pwm-pad", {
            { "VCO 1 Wave", 1 }, { "VCO 2 Wave", 1 }, { "VCO 1 Sub 1", 5 }, { "VCO 1 Sub 2", 2 }, { "VCO 2 Freq", 4.0 / 48.0 }, { "VCO 2 Sub 1", 3 },
            { "VCO 1 Level", 0.6 }, { "VCO 1 Sub 1 Level", 0.3 }, { "VCO 1 Sub 2 Level", 0.4 }, { "VCO 2 Level", 0.55 }, { "VCO 2 Sub 1 Level", 0.3 },
            { "VCO 2 Sub 2 Level", 0.0 }, { "Cutoff", 0.5 }, { "Resonance", 0.3 }, { "VCF EG Amt", 0.3 }, { "VCF Attack", 0.6 }, { "VCF Decay", 0.8 },
            { "VCA Attack", 0.55 }, { "VCA Decay", 0.85 }, { "Seq 1 Step 2", 5.0 / 24.0 }, { "Seq 1 Step 3", -2.0 / 24.0 }, { "Seq 1 Step 4", 3.0 / 24.0 },
            { "Seq 2 Step 2", 3.0 / 24.0 }, { "Seq 2 Step 3", -5.0 / 24.0 }, { "Seq 2 Step 4", 0.0 }, { "Rhythm 1", 16 }, { "Rhythm 2", 12 } } },
        { "demo-sub-arp.wav", "sub-arp", {
            { "VCO 1 Wave", 0 }, { "VCO 2 Wave", 0 }, { "VCO 1 Sub 1", 4 }, { "VCO 1 Sub 2", 6 }, { "VCO 2 Freq", 0.25 }, { "VCO 2 Sub 1", 5 },
            { "VCO 1 Level", 0.2 }, { "VCO 1 Sub 1 Level", 0.7 }, { "VCO 1 Sub 2 Level", 0.5 }, { "VCO 2 Level", 0.0 }, { "VCO 2 Sub 1 Level", 0.6 },
            { "VCO 2 Sub 2 Level", 0.0 }, { "Seq 1 > Osc 1", 0.0 }, { "Seq 1 > Sub 1", 1.0 }, { "Seq 1 > Sub 2", 1.0 }, { "Seq 2 > Osc 2", 0.0 },
            { "Seq 2 > Sub 1", 1.0 }, { "Seq 1 Step 2", 0.33 }, { "Seq 1 Step 3", -0.33 }, { "Seq 1 Step 4", 0.5 }, { "Seq 2 Step 2", 0.5 },
            { "Seq 2 Step 3", -0.5 }, { "Seq 2 Step 4", 0.17 }, { "Cutoff", 0.45 }, { "Resonance", 0.35 }, { "VCF EG Amt", 0.45 }, { "VCF Attack", 0.0 },
            { "VCF Decay", 0.3 }, { "VCA Attack", 0.0 }, { "VCA Decay", 0.35 }, { "Rhythm 1", 2 }, { "Rhythm 2", 3 }, { "Rhythm 3", 5 },
            { "Rhythm 3 > Seq 1", 1.0 }, { "Rhythm Logic", 1 } } },
        { "demo-hold-drone.wav", "hold-drone", {
            { "EG", 2 }, { "Quantize", 3 }, { "VCO 1 Sub 1", 2 }, { "VCO 1 Sub 2", 3 }, { "VCO 2 Freq", 7.0 / 48.0 }, { "VCO 2 Sub 1", 3 }, { "VCO 2 Sub 2", 4 },
            { "VCO 1 Level", 0.45 }, { "VCO 1 Sub 1 Level", 0.5 }, { "VCO 1 Sub 2 Level", 0.4 }, { "VCO 2 Level", 0.4 }, { "VCO 2 Sub 1 Level", 0.4 },
            { "VCO 2 Sub 2 Level", 0.35 }, { "Cutoff", 0.42 }, { "Resonance", 0.6 }, { "VCF EG Amt", 0.0 }, { "Volume", 0.6 },
            { "Seq 1 Step 2", 5.0 / 24.0 }, { "Seq 1 Step 3", -3.0 / 24.0 }, { "Seq 1 Step 4", 2.0 / 24.0 }, { "Seq 2 Step 2", -5.0 / 24.0 },
            { "Seq 2 Step 3", 4.0 / 24.0 }, { "Seq 2 Step 4", 0.0 }, { "Rhythm 1", 16 }, { "Rhythm 2", 12 }, { "Clock Div", 3 } } },
    };
    for (auto& demo : list)
    {
        Instance a, b;
        a.setup (demo.values);
        CHECK (b.loadPreset (demo.preset));
        b.setup ({}); // the same one-frame start as a's
        for (auto& [name, id] : a.ids)
            if (a.value (name) != b.value (name))
            {
                std::printf ("FAIL preset %s: %s is %g, the demo says %g\n", demo.preset, name.c_str(), b.value (name), a.value (name));
                ++failures;
            }
        std::vector<float> l, r, l2, r2;
        auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
        a.render (48000 * 8, 128, l, r, t);
        b.render (48000 * 8, 128, l2, r2, t);
        CHECK (l == l2 && r == r2);
        float peak = 0.0f;
        for (float s : l2)
            peak = std::max (peak, std::fabs (s));
        if (debug)
            std::printf ("debug demo %s: peak %.3f, rms %.4f\n", demo.preset, peak, rms (l2, 0, l2.size()));
        CHECK (peak > 0.05f && peak <= 1.0f);
        const auto path = folder + "/" + demo.file;
        CHECK (writeWav (path.c_str(), l2, r2, 48000));
    }
}
} // namespace

// Verified review fixes (0.2.1): All Sound Off silences an EG: Hold drone (while playing in EG: On, the next
// sequencer step sounds again), the tempo is clamped to a finite 1-1000 BPM (an infinite tempo must not hang
// the render loop), a mid-grid Play lands the same at every block size, a step pad's step does not survive a
// song jump, a page press older than half a second still fires, a state from a newer version is refused, and
// nan/inf text is refused.
void testVerifiedFixes()
{
    std::vector<float> l, r, l2, r2;
    const auto still = [] (uint64_t) { return stopped; };
    // Panic: an EG: Hold drone sounding with no note held. CC 120 and a wildcard choke silence it at once;
    // CC 123 (all notes off) does not touch it. A later note sounds again, and Hold holds it after its off.
    for (int mode = 0; mode < 3; ++mode) // 0: CC 120, 1: wildcard choke, 2: CC 123
    {
        Instance a;
        a.setup (dronePatch (a));
        a.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 100)
            {
                if (mode == 0) e.midi (0, 0xb0, 120, 0);
                else if (mode == 1) e.note (0, CLAP_EVENT_NOTE_CHOKE, -1);
                else e.midi (0, 0xb0, 123, 0);
            }
            if (mode == 0 && at == 128 * 200) e.note (0, CLAP_EVENT_NOTE_ON, 60);
            if (mode == 0 && at == 128 * 300) e.note (0, CLAP_EVENT_NOTE_OFF, 60);
        });
        CHECK (rms (l, 6000, 2400) > 0.05);
        if (mode == 2)
        {
            CHECK (rms (l, 20000, 2400) > 0.05);
            continue;
        }
        bool silent = true;
        for (size_t i = 128 * 100; i < 128 * 200 && i < l.size(); ++i)
            silent &= l[i] == 0.0f && r[i] == 0.0f;
        CHECK (silent);
        if (mode == 0)
            CHECK (rms (l, 30000, 2400) > 0.05 && rms (l, 44000, 2400) > 0.05);
    }
    // Panic while playing in EG: On silences what sounds at once, and the next sequencer step (eighth-note
    // clocks at 120 BPM: frames 0, 12000, 24000, ...) sounds again: the sequencer keeps running.
    {
        Instance a;
        a.setup (clickPatch (a));
        a.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); },
                  [&] (uint64_t at, uint32_t, Events& e) { if (at == 128 * 100) e.midi (0, 0xb0, 120, 0); });
        CHECK (rms (l, 12000, 800) > 0.01);
        bool silent = true;
        for (size_t i = 128 * 100; i < 24000 && i < l.size(); ++i)
            silent &= l[i] == 0.0f && r[i] == 0.0f;
        CHECK (silent);
        CHECK (rms (l, 24000, 2400) > 0.01);
    }
    // Trigger sounds an EG: Hold drone again after a panic (it otherwise does nothing in Hold).
    {
        Instance a;
        a.setup (dronePatch (a));
        a.render (48000, 128, l, r, still, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 100) e.midi (0, 0xb0, 120, 0);
            if (at == 128 * 200) a.set (e, "Trigger", 1.0);
        });
        bool silent = true;
        for (size_t i = 128 * 100; i < 128 * 200 && i < l.size(); ++i)
            silent &= l[i] == 0.0f && r[i] == 0.0f;
        CHECK (silent);
        CHECK (rms (l, 30000, 2400) > 0.05 && rms (l, 44000, 2400) > 0.05);
    }
    // A pad's step ends at a song jump: pad step 3 (G5 in the drone patch) while playing, then a jump; what
    // sounds after the jump is the sequencer's own step 1 (C5), not the pad's step.
    {
        Instance a;
        a.setup (dronePatch (a));
        a.render (96000, 128, l, r,
                  [] (uint64_t at) {
                      return at < 24000 ? Transport { true, 120.0, (double) at * 120.0 / 60.0 / 48000.0 }
                                        : Transport { true, 120.0, 4.3 + (double) (at - 24000) * 120.0 / 60.0 / 48000.0 };
                  },
                  [&] (uint64_t at, uint32_t, Events& e) { if (at == 128 * 50) e.note (0, CLAP_EVENT_NOTE_ON, 38); });
        CHECK (near (hz (l, 0.20, 0.45), g5, 0.01));
        CHECK (near (hz (l, 0.65, 0.95), c5, 0.01));
    }
    // Play landing past a grid line (50 and 200 frames past, eighth-note clocks at 120 BPM): the same output
    // at every block size, matching the 128-frame behaviour.
    for (double past : { 50.0, 200.0 })
    {
        const double start = 0.5 + past * 120.0 / 60.0 / 48000.0;
        std::vector<float> want;
        {
            Instance a;
            a.setup (clickPatch (a));
            a.render (24000, 128, want, r, [&] (uint64_t at) { return playingFrom (start, 120.0, 48000.0, at); });
        }
        CHECK (rms (want, 0, 24000) > 0.001);
        for (uint32_t block : { 1u, 37u, 4096u })
        {
            Instance a;
            a.setup (clickPatch (a));
            a.render (24000, block, l, r, [&] (uint64_t at) { return playingFrom (start, 120.0, 48000.0, at); });
            CHECK (l == want);
        }
    }
    // A state whose version is newer than this build's is refused, and nothing changes.
    {
        Instance a, b;
        a.setup ({ { "Volume", 0.25 }, { "Cutoff", 0.3 } });
        std::vector<uint8_t> saved;
        clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
            auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
            v->insert (v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
            return (int64_t) n;
        } };
        CHECK (a.state->save (a.plugin, &os) && saved.size() > 8);
        saved[4] = 2; // version 2, little-endian as saved
        struct Reader { const std::vector<uint8_t>* v; size_t at; } reader { &saved, 0 };
        clap_istream is { &reader, [] (const clap_istream* s, void* to, uint64_t n) -> int64_t {
            auto* rd = static_cast<Reader*> (s->ctx);
            const size_t k = std::min<size_t> ((size_t) n, rd->v->size() - rd->at);
            std::memcpy (to, rd->v->data() + rd->at, k);
            rd->at += k;
            return (int64_t) k;
        } };
        const double before = b.value ("Volume");
        CHECK (! b.state->load (b.plugin, &is));
        CHECK (b.value ("Volume") == before);
    }
    // Non-finite text is refused, not clamped into a value.
    {
        Instance a;
        double v = -1.0;
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Cutoff"), "nan", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Cutoff"), "inf Hz", &v));
        CHECK (a.params->text_to_value (a.plugin, a.id ("Cutoff"), "1000 Hz", &v) && v > 0.0 && v < 1.0);
    }
    // A page press the host slept on for over half a second still fires exactly once, as the same button
    // (Trigger, stopped) sent by the host at the first frame does.
    {
        auto run = [&] (bool fromPage) {
            Instance a;
            a.setup (clickPatch (a, { { "VCA Decay", 0.3 } }));
            std::vector<float> out, right;
            if (fromPage)
            {
                const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
                for (const melodee_screen_input_t e : { melodee_screen_input_t { MELODEE_SCREEN_STEP, 1, 0 },
                                                         melodee_screen_input_t { MELODEE_SCREEN_WHEEL, 5, 0 },
                                                         melodee_screen_input_t { MELODEE_SCREEN_PUSH, 0, 0 } })
                    CHECK (screen->input (a.plugin, &e));
                std::this_thread::sleep_for (std::chrono::milliseconds (600)); // the host asleep: no process calls
            }
            a.render (24000, 128, out, right, still,
                      [&] (uint64_t at, uint32_t, Events& e) { if (! fromPage && at == 0) a.set (e, "Trigger", 1.0); });
            return out;
        };
        const auto page = run (true), host = run (false);
        CHECK (rms (host, 0, 2400) > 0.01);
        CHECK (page == host);
    }
    // A page press queued before a CLAP reset or a de-/reactivate does not fire after it (a state or preset
    // load already voids queued presses the same way): the queue that fires above stays silent.
    for (int mode = 0; mode < 2; ++mode) // 0: reset, 1: de-/reactivate
    {
        Instance a;
        a.setup (clickPatch (a, { { "VCA Decay", 0.3 } }));
        const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
        for (const melodee_screen_input_t e : { melodee_screen_input_t { MELODEE_SCREEN_STEP, 1, 0 },
                                                 melodee_screen_input_t { MELODEE_SCREEN_WHEEL, 5, 0 },
                                                 melodee_screen_input_t { MELODEE_SCREEN_PUSH, 0, 0 } })
            CHECK (screen->input (a.plugin, &e));
        if (mode == 0)
            a.plugin->reset (a.plugin);
        else
        {
            a.plugin->stop_processing (a.plugin);
            a.plugin->deactivate (a.plugin);
            CHECK (a.plugin->activate (a.plugin, a.rate, 1, 4096));
            CHECK (a.plugin->start_processing (a.plugin));
        }
        std::vector<float> out, right;
        a.render (24000, 128, out, right, still);
        bool silent = true;
        for (size_t i = 0; i < out.size(); ++i)
            silent &= out[i] == 0.0f && right[i] == 0.0f;
        CHECK (silent);
    }
    // Activation refuses a useless sample rate.
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        const clap_plugin* p = factory->create_plugin (factory, &host, d->id);
        CHECK (p != nullptr && p->init (p));
        if (p != nullptr)
        {
            CHECK (! p->activate (p, 0.0, 1, 128));
            CHECK (! p->activate (p, std::numeric_limits<double>::infinity(), 1, 128));
            CHECK (p->activate (p, 48000.0, 1, 128));
            p->deactivate (p);
            p->destroy (p);
        }
    }
    // Tempo: out-of-range values behave as the nearest bound, non-finite ones never reach the clock. The
    // beats advance at the clamped rate while the tempo field carries the hostile value.
    {
        auto at = [&] (double tempoField, double beatsRate) {
            Instance a;
            a.setup (clickPatch (a));
            std::vector<float> out, right;
            a.render (24000, 128, out, right, [&] (uint64_t f) {
                return Transport { true, tempoField, (double) f * beatsRate / 60.0 / 48000.0 };
            });
            return out;
        };
        const auto fast = at (1000.0, 1000.0);
        CHECK (rms (fast, 0, 24000) > 0.001);
        CHECK (at (2000.0, 1000.0) == fast);
        CHECK (at (0.0, 120.0) == at (120.0, 120.0));
        CHECK (at (std::numeric_limits<double>::quiet_NaN(), 120.0) == at (120.0, 120.0));
        CHECK (at (std::numeric_limits<double>::infinity(), 1000.0) == fast); // must terminate
    }
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host Necronomicon.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::printf ("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    entry = static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry"));
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL no clap_entry\n");
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    testScreen();
    testScreenColorClock();
    testPressWithPlay();
    testDescription();
    testParameters();
    testState();
    testPresets();
    testTiming();
    testRhythms();
    testStopRestartJump();
    testLoopsAndClockChanges();
    testBlockSizes();
    testPitch();
    testEnvelopesAndNotes();
    testStepPads();
    testExtremes();
    testVerifiedFixes();
    demos (argv[2]);
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
