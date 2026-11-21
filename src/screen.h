// Liquid Light on Melodee's 320 x 218 canvas. All drawing and navigation run on the main thread.
#pragma once

#include "melodee_screen.h"
#include "params.h"
#include "ui/page.h"
#include "liquid_art.h"
#include <array>
#include <atomic>

namespace necronomicon
{
namespace panels
{
using namespace ParamId;
constexpr uint32_t clock[] = { clockDiv, rhythmLogic, run, reset, next, trigger, quantize, seqOct };
constexpr uint32_t first[] = { seq1Step1, seq1Step2, seq1Step3, seq1Step4, seq1Osc, seq1Sub1, seq1Sub2 };
constexpr uint32_t second[] = { seq2Step1, seq2Step2, seq2Step3, seq2Step4, seq2Osc, seq2Sub1, seq2Sub2 };
constexpr uint32_t voice1[] = { vco1Freq, vco1Sub1, vco1Sub2, vco1Wave };
constexpr uint32_t voice2[] = { vco2Freq, vco2Sub1, vco2Sub2, vco2Wave };
constexpr uint32_t levels[] = { vco1Level, vco1Sub1Level, vco1Sub2Level, vco2Level, vco2Sub1Level, vco2Sub2Level };
constexpr uint32_t filter[] = { cutoff, resonance, vcfEgAmt, vcfAttack, vcfDecay };
constexpr uint32_t amp[] = { egMode, vcaAttack, vcaDecay, volume, fineTune };
struct Group { const char* name; const uint32_t* ids; int count; };
template<size_t N> constexpr Group group (const char* name, const uint32_t (&ids)[N]) { return { name, ids, int (N) }; }
constexpr Group groups[] = { { "RHYTHM", nullptr, 4 }, group ("CLOCK", clock), group ("SEQ 1", first), group ("SEQ 2", second),
    group ("VCO 1", voice1), group ("VCO 2", voice2), group ("MIXER", levels), group ("FILTER", filter), group ("AMP", amp) };
}

struct ScreenEdit
{
    uint32_t id = hdui::noParam;
    float value = 0;
    bool press = false;
};

class Screen
{
    using Raster = hdui::Raster;
    using Ink = hdui::Ink;
    static constexpr int groupCount = int (std::size (panels::groups));
    int group = 0, rows[groupCount] {}, lastGroup = -1, lastRow = -1;
    bool edit = false, lastEdit = false, lastShift = false, fading = false;
    uint64_t lastPos = 0, flashAt = 0;
    int lastFlash = 0;
    melodee_screen_knob_t lastPots[3] {};
    std::array<float, ParamId::count> sampled {};
    hdui::Picture picture;

    static uint32_t rhythmId (int g, int knob)
    {
        constexpr uint32_t first[] = { ParamId::rhythm1, ParamId::rhythm1Seq1, ParamId::rhythm1Seq2 };
        return first[knob] + uint32_t (g);
    }

    static void print (Raster& r, int x, int y, const unsigned char* src, int stride, int width, int height)
    {
        for (int j = 0; j < height; ++j)
            for (int i = 0; i < width; ++i)
            {
                const auto* p = src + 3 * (j * stride + i);
                r.dot (x + i, y + j, 0xff000000u | (uint32_t (p[0]) << 16) | (p[1] << 8) | p[2]);
            }
    }

    static void big (Raster& r, int x, int y, const char* label, Ink color)
    {
        for (const char* s = label; *s; ++s, x += 14)
        {
            const auto& glyph = hdui::font::glyphs[(unsigned char) *s - 32];
            for (int j = 0; j < hdui::font::glyphHeight; ++j)
                for (int k = 0; k < 7; ++k)
                    if (glyph[j] & (1 << k)) r.box (x + k * 2, y + j * 2, 2, 2, color);
        }
    }

    void formatted (uint32_t id, char (&buf)[48]) const
    {
        formatParam (paramTable[paramIndex (id)], sampled[id], buf, sizeof buf, stepOctaves (sampled[ParamId::seqOct]));
    }

    void window (Raster& r, int g, uint64_t pos, int flash) const
    {
        using namespace hdui::ink;
        const int cx = 43 - g * 2, cy = 48;
        const bool chosen = rows[0] == g;
        print (r, cx - 37, 0, art::arches + (chosen ? 74 * 3 : 0), 148, 74, 106);
        const int d = int (sampled[rhythmId (g, 0)]), phase = int ((pos >> 4) % d);
        constexpr int size = 56;
        const auto* lens = art::lens + (d * (d - 1) / 2 + phase) * size * size;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto index = *lens++;
                if (index) r.dot (cx - size / 2 + x, cy - size / 2 + y, art::palette[index]);
            }
        for (int n = 0; n < d; ++n)
        {
            const double a = n * 6.283185307179586 / d;
            const int x = cx + int (std::lround (26.1 * std::sin (a))), y = cy - int (std::lround (26.1 * std::cos (a)));
            if (n == phase)
            {
                const int length = 3 + flash;
                r.box (x - length, y, 2 * length + 1, 1, paper);
                r.box (x, y - length, 1, 2 * length + 1, paper);
                r.box (x - 1, y - 1, 3, 3, paper);
            }
            else r.box (x - 1, y, 2, 1, 0xffd698f3);
        }
        r.box (cx - 33, 78, 66, 25, field);
        if (chosen) r.box (cx - 33, 78, 66, 13, edit ? paper : active);
        char s[20];
        std::snprintf (s, sizeof s, "R%d  /%d", g + 1, d);
        r.text (cx - int (std::strlen (s)) * 7 / 2, 80, s, chosen ? bg : paper);
        const int route = int (sampled[rhythmId (g, 1)]) * 2 + int (sampled[rhythmId (g, 2)]);
        constexpr const char* dest[] = { "UNROUTED", "SEQ 2", "SEQ 1", "SEQ 1+2" };
        r.text (cx - int (std::strlen (dest[route])) * 7 / 2, 94, dest[route], soft);
    }

    void list (Raster& r) const
    {
        using namespace hdui::ink;
        const auto& p = panels::groups[group];
        r.text (8, 4, p.name, active);
        const int row = rows[group], first = std::clamp (row - 3, 0, std::max (0, p.count - 6));
        for (int i = first; i < std::min (p.count, first + 6); ++i)
        {
            const int y = 20 + 15 * (i - first);
            const bool on = i == row;
            const uint32_t id = p.ids[i];
            if (on) { r.box (5, y - 2, 309, 14, card); r.box (5, y - 2, 2, 14, edit ? paper : active); }
            const int index = paramIndex (id);
            if (index < 0) continue;
            r.text (12, y, paramTable[index].name, on ? paper : soft);
            char val[48]; formatted (id, val);
            r.right (307, y, val, on ? active : soft);
        }
    }

    void footer (Raster& r, const melodee_screen_canvas_t& c) const
    {
        using namespace hdui::ink;
        r.box (0, 0, 320, 42, card); r.box (0, 0, 320, 1, rule);
        if (c.shift)
        {
            r.text (8, 7, group ? "WHEEL: Row    PUSH: Edit / press" : "WHEEL: Rhythm   PUSH: Edit divisor", paper);
            r.text (8, 24, edit ? "WHEEL: Value    -/+: Coarse value" : "-/+: Pages      K1-K3: Turn values", soft);
            return;
        }
        uint32_t ids[3]; knobs (ids);
        for (int k = 0; k < 3; ++k)
        {
            const int x = 8 + 106 * k;
            if (k) r.box (x - 7, 6, 1, 29, rule);
            const char tag[] = { 'K', char ('1' + k), 0 };
            r.text (x, 5, tag, active);
            if (ids[k] == hdui::noParam) continue;
            const auto& param = paramTable[paramIndex (ids[k])];
            char label[13], val[48]; formatted (ids[k], val);
            if (group == 0)
            {
                if (k == 0) { std::snprintf (label, sizeof label, "R%c DIV", char ('1' + rows[0])); std::snprintf (val, sizeof val, "/%d", int (sampled[ids[k]])); }
                else { std::strcpy (label, k == 1 ? "TO SEQ 1" : "TO SEQ 2"); std::strcpy (val, sampled[ids[k]] ? "ON" : "OFF"); }
            }
            else std::snprintf (label, sizeof label, "%.11s", param.name);
            r.text (x + 18, 5, label, paper);
            if (std::strlen (val) <= 6) big (r, x, 17, val, paper);
            else r.text (x, 20, val, paper, 6);
            if (!c.knobs[k].engaged && c.knobs[k].position >= 0)
            {
                // The target and the physical pot while Melodee waits for pickup.
                const int target = int (std::lround ((sampled[ids[k]] - param.min) / (param.max - param.min) * 87));
                r.box (x, 37, 88, 1, rule); r.box (x + target, 34, 1, 4, active);
                r.frame (x + c.knobs[k].position * 87 / 127 - 1, 35, 3, 4, paper);
            }
        }
    }

public:
    void knobs (uint32_t ids[3]) const
    {
        const auto& p = panels::groups[group];
        for (int k = 0; k < 3; ++k)
        {
            if (group == 0) { ids[k] = rhythmId (rows[0], k); continue; }
            const int i = rows[group] + k;
            ids[k] = i < p.count && paramTable[paramIndex (p.ids[i])].kind != Kind::button ? p.ids[i] : hdui::noParam;
        }
    }

    bool input (const melodee_screen_input_t& e, const std::atomic<float>* values, ScreenEdit& out)
    {
        if (e.kind > MELODEE_SCREEN_NUDGE) return false;
        const auto& p = panels::groups[group];
        int& row = rows[group];
        if (e.kind == MELODEE_SCREEN_NUDGE && !edit)
        {
            const int next = std::clamp (group + (e.value > 0 ? 1 : -1), 0, groupCount - 1);
            if (next == group) return false;
            group = next; return true;
        }
        if (e.kind == MELODEE_SCREEN_WHEEL && !edit)
        {
            row = int (std::clamp<int64_t> (int64_t (row) + e.value, 0, p.count - 1));
            return true;
        }
        const uint32_t id = group ? p.ids[row] : rhythmId (row, 0);
        const int index = paramIndex (id);
        const auto& param = paramTable[index];
        if (e.kind == MELODEE_SCREEN_PUSH)
        {
            if (param.kind == Kind::button) out = { id, 0, true };
            else edit = !edit;
            return true;
        }
        const float old = values[index].load (std::memory_order_relaxed);
        const float unit = param.kind == Kind::continuous ? (param.max - param.min) /
            (e.kind == MELODEE_SCREEN_NUDGE ? 16.0f : (e.shift ? 1024.0f : 128.0f)) : 1.0f;
        const float value = clampParam (param, old + e.value * unit);
        if (value != old) out = { id, value, false };
        return true;
    }

    bool animating() const { return fading; }

    bool paint (const melodee_screen_canvas_t& c, const std::atomic<float>* values, uint64_t pos)
    {
        using namespace hdui::ink;
        const bool whole = c.whole || lastGroup < 0, page = whole || group != lastGroup;
        const bool selection = page || rows[group] != lastRow || edit != lastEdit;
        if (!page && !group && pos != lastPos) { flashAt = c.now_ms; fading = true; }
        if (page || c.now_ms - flashAt >= 100) fading = false;
        const int flash = fading ? 2 - int ((c.now_ms - flashAt) / 50) : 0;
        bool params = false, pots = false;
        for (uint32_t i = 0; i < paramCount; ++i)
        {
            const float v = values[i].load (std::memory_order_relaxed);
            float& prior = sampled[paramTable[i].id];
            params |= prior != v; prior = v;
        }
        for (int k = 0; k < 3; ++k)
        {
            pots |= c.knobs[k].engaged != lastPots[k].engaged || c.knobs[k].position != lastPots[k].position;
            lastPots[k] = c.knobs[k];
        }
        bool drew = false;
        if (whole || params)
            drew |= picture.region<320,38> (c.pixels, int (c.stride), whole, 0, 0, [&](Raster& r) {
                print (r, 0, 0, art::masthead, 247, 247, 36);
                r.right (312, 7, sampled[ParamId::rhythmLogic] ? "LOGIC XOR" : "LOGIC OR", soft);
                r.right (312, 23, clockDivChoices[int (sampled[ParamId::clockDiv])], paper); r.box (6, 36, 308, 1, rule);
            });
        if (!group && (selection || params || pos != lastPos || flash != lastFlash))
            for (int g = 0; g < 4; ++g)
                drew |= picture.region<80,110> (c.pixels, int (c.stride), whole, g * 80, 38,
                        [&](Raster& r) { window (r, g, pos, flash); });
        if (group && (selection || params))
            drew |= picture.region<320,110> (c.pixels, int (c.stride), whole, 0, 38, [&](Raster& r) { list (r); });
        if (page || ((pos ^ lastPos) & 15))
            drew |= picture.region<320,28> (c.pixels, int (c.stride), whole, 0, 148, [&](Raster& r) {
                for (int s = 0; s < 2; ++s)
                {
                    const int x = 8 + s * 157;
                    r.text (x, 9, s ? "SEQ 2" : "SEQ 1", paper);
                    for (int n = 0; n < 4; ++n)
                    {
                        const bool on = int ((pos >> (2*s)) & 3) == n;
                        const int left = x + 40 + n * 24;
                        r.box (left, 4, 20, 17, on ? active : muted);
                        const char number[] = { char ('1' + n), 0 };
                        r.text (left + 7, 8, number, on ? bg : paper);
                    }
                }
            });
        if (selection || params || pots || c.shift != lastShift)
            drew |= picture.region<320,42> (c.pixels, int (c.stride), whole, 0, 176, [&](Raster& r) { footer (r, c); });
        lastGroup = group; lastRow = rows[group]; lastEdit = edit; lastShift = c.shift;
        lastFlash = flash; lastPos = pos;
        return drew;
    }
};
} // namespace necronomicon
