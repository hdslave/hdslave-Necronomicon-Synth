#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace necronomicon
{

struct TilesAccess
{
    static constexpr int kTileCount = Screen::groupCount; // 9 tiles (3x3 grid)

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 3;
        if (out_rows) *out_rows = 3;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // 4 RHYTHM WHEELS",
            "02 // CLOCK & LOGIC",
            "03 // SEQUENCER 1",
            "04 // SEQUENCER 2",
            "05 // VCO 1 & SUBS",
            "06 // VCO 2 & SUBS",
            "07 // 6-CHANNEL MIXER",
            "08 // LADDER VCF",
            "09 // VCA & TUNING"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_canvas_t* canvas, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !canvas || !canvas->pixels) return false;
        auto& p = of (plugin);
        const int savedGroup = p.screen.group;
        p.screen.group = (int) tile;
        p.screen.lastGroup = -1;

        melodee_screen_canvas_t c = *canvas;
        c.whole = true;
        const uint64_t pos = p.position.load (std::memory_order_acquire);
        const int colorStep = int ((c.now_ms * 15 + 500) / 1000 % colorCycleSteps);
        const bool drew = p.screen.paint (c, p.values, pos, colorStep);
        if (knobs_out)
            p.screen.knobs (knobs_out);
        p.screen.group = savedGroup;
        return drew;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t* plugin, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        auto& p = of (plugin);
        const int g = (int) tile;

        // Top-right readout (x: 247..320, y: 0..36) -> Rhythm Logic (top) & Clock Div (bottom)
        if (x >= 247 && x < 320 && y >= 0 && y < 36)
        {
            out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
            out_spot->param_id = y < 18 ? ParamId::rhythmLogic : ParamId::clockDiv;
            out_spot->x = 247;
            out_spot->y = (int16_t) (y < 18 ? 0 : 18);
            out_spot->w = 73;
            out_spot->h = 18;
            return true;
        }

        // Sequencer strip (y: 148..175): SEQ 1 (x: 3..161) and SEQ 2 (x: 162..319)
        if (y >= 148 && y < 176)
        {
            for (int s = 0; s < 2; ++s)
            {
                const int bx = 3 + s * 159;
                for (int n = 0; n < 4; ++n)
                {
                    const int left = bx + 51 + n * 25;
                    if (x >= left && x < left + 25)
                    {
                        out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                        out_spot->param_id = (s == 0 ? ParamId::seq1Step1 : ParamId::seq2Step1) + (uint32_t) n;
                        out_spot->x = (int16_t) left;
                        out_spot->y = 152;
                        out_spot->w = 24;
                        out_spot->h = 20;
                        return true;
                    }
                }
            }
        }

        // Middle body (y: 38..147)
        if (y >= 38 && y < 148)
        {
            if (g == 0)
            {
                // 4 Stained-Glass Rhythm Wheels (80px each)
                const int wIdx = std::clamp (x / 80, 0, 3);
                const int wx = wIdx * 80;
                if (y < 126)
                {
                    // Rhythm division /1 .. /16
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = Screen::rhythmId (wIdx, 0);
                    out_spot->x = (int16_t) (wx + 4);
                    out_spot->y = 40;
                    out_spot->w = 72;
                    out_spot->h = 85;
                    return true;
                }
                else
                {
                    // Routing label (UNROUTED / SEQ 1 / SEQ 2 / SEQ 1+2): click cycles routing!
                    out_spot->kind = HDSLAVE_HOTSPOT_ACTION;
                    out_spot->custom_id = 10 + wIdx;
                    out_spot->x = (int16_t) (wx + 4);
                    out_spot->y = 126;
                    out_spot->w = 72;
                    out_spot->h = 20;
                    return true;
                }
            }
            else
            {
                const auto& grp = panels::groups[g];
                const int stepY = grp.count >= 8 ? 11 : (grp.count == 7 ? 13 : 15);
                const int startY = 38 + (grp.count >= 7 ? 18 : 20);
                if (y >= startY - 2 && y < startY + grp.count * stepY)
                {
                    const int rowIdx = std::clamp ((y - (startY - 2)) / stepY, 0, grp.count - 1);
                    const uint32_t id = grp.ids[rowIdx];
                    const auto& param = paramTable[paramIndex (id)];
                    out_spot->kind = param.kind == Kind::button ? HDSLAVE_HOTSPOT_BUTTON : HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = id;
                    out_spot->x = 4;
                    out_spot->y = (int16_t) (startY + rowIdx * stepY - 2);
                    out_spot->w = 312;
                    out_spot->h = (int16_t) stepY;
                    return true;
                }
            }
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int x, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        const int g = (int) tile;
        p.screen.group = g;
        if (y >= 38 && y < 148)
        {
            if (g == 0)
            {
                p.screen.rows[0] = std::clamp (x / 80, 0, 3);
            }
            else
            {
                const auto& grp = panels::groups[g];
                const int stepY = grp.count >= 8 ? 11 : (grp.count == 7 ? 13 : 15);
                const int startY = 38 + (grp.count >= 7 ? 18 : 20);
                if (y >= startY - 2 && y < startY + grp.count * stepY)
                    p.screen.rows[g] = std::clamp ((y - (startY - 2)) / stepY, 0, grp.count - 1);
            }
        }
    }

    static void CLAP_ABI action (const clap_plugin_t* plugin, uint32_t, int32_t custom_id, int32_t, bool shift)
    {
        auto& p = of (plugin);
        if (custom_id >= 10 && custom_id < 14)
        {
            const int wIdx = custom_id - 10;
            p.screen.rows[0] = wIdx;
            const uint32_t id1 = Screen::rhythmId (wIdx, 1);
            const uint32_t id2 = Screen::rhythmId (wIdx, 2);
            const int i1 = paramIndex (id1), i2 = paramIndex (id2);
            const int b1 = p.values[i1].load (std::memory_order_relaxed) >= 0.5f ? 1 : 0;
            const int b2 = p.values[i2].load (std::memory_order_relaxed) >= 0.5f ? 1 : 0;
            const int route = (b1 * 2 + b2 + (shift ? 3 : 1)) & 3;
            p.values[i1].store ((route & 2) ? 1.0f : 0.0f, std::memory_order_relaxed);
            p.values[i2].store ((route & 1) ? 1.0f : 0.0f, std::memory_order_relaxed);
            p.mainChanged.store (true, std::memory_order_release);
            p.outgoing.fetch_or ((uint64_t (1) << i1) | (uint64_t (1) << i2), std::memory_order_release);
            if (p.hostParams && p.hostParams->rescan) p.hostParams->rescan (p.host, CLAP_PARAM_RESCAN_VALUES);
            if (p.hostParams && p.hostParams->request_flush) p.hostParams->request_flush (p.host);
            if (p.host && p.host->request_process) p.host->request_process (p.host);
            repaint (p);
        }
    }
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    176,
};

} // namespace necronomicon
