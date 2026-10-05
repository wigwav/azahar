// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Native HD HUD for Screen Regions.
//
// A HUD is a list of drawing elements (panels, PNG artwork, text, bars) on a
// canvas (default 1920x1080) that the renderer draws over the game image. Element
// values can be bound to guest memory so the HUD shows live game data, drawn at
// display resolution instead of cropped from the 3DS bottom screen. The canvas is
// only re-rasterised when a bound value changes.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace ScreenRegions {

/// RGBA8 image (row-major, top-left origin, straight alpha).
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;
};

/// Value source: a constant or a guest memory read (optionally through a pointer chain).
struct Binding {
    bool constant = true;
    s64 value = 0;
    std::string expr;             ///< address expression, e.g. "[0x00500000]+0x20"
    u32 size = 2;                 ///< 1, 2 or 4 bytes for the final read
    s64 Read(Core::System& system) const;
};

struct Element {
    enum class Type { Rect, Image, Text, Bar } type = Type::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    u32 color = 0xFFFFFFFF;     ///< RRGGBBAA
    u32 color2 = 0x000000A0;    ///< bar background
    float opacity = 1.0f;
    std::string image;          ///< image file (Image)
    std::string text;           ///< text template; {0},{1}.. are replaced by values (Text)
    float size = 32.0f;         ///< text pixel height
    int align = 0;              ///< -1 left, 0 left, 1 centre, 2 right
    std::vector<Binding> values;///< Text: template values; Bar: [value, max]
    Binding visible;            ///< element shown when non-zero (constant 1 by default)
    std::string lookup;         ///< Text: list file; {0} shows line[value] instead of value
};

struct HudDef {
    std::string name;           ///< profile this HUD belongs to
    std::vector<Element> elements;
};

class Hud {
public:
    /// Parses one "hud = ..." element line; returns false on syntax error.
    static bool ParseElement(const std::string& line, Element& out, std::string& error);

    void SetDefinition(const std::vector<HudDef>& defs, const std::string& asset_dir);

    /// Re-evaluates bindings for the active profile and rasterises if anything changed.
    /// Returns true when the canvas changed. Emulation thread only.
    bool Update(Core::System& system, const std::string& active_profile);

    /// Snapshot of the current canvas for the renderer. `version` increments on change.
    std::shared_ptr<const Image> Canvas(u64& version) const;

    static constexpr u32 CanvasWidth = 1920;
    static constexpr u32 CanvasHeight = 1080;

private:
    struct Glyph {
        int x, y, w, h, xoff, yoff, advance;
    };
    bool LoadFont();
    const Image* GetImage(const std::string& file);
    void Rasterise(const HudDef& def, const std::vector<std::vector<s64>>& values,
                   const std::vector<bool>& visible);
    void DrawRect(Image& dst, float x, float y, float w, float h, u32 rgba, float opacity);
    void DrawImage(Image& dst, const Image& src, float x, float y, float w, float h,
                   float opacity);
    void DrawText(Image& dst, const std::string& text, float x, float y, float size, int align,
                  u32 rgba, float opacity);

    std::vector<HudDef> defs;
    std::string asset_dir;
    std::map<std::string, Image> images;
    std::map<std::string, std::vector<std::string>> lookups;
    const std::vector<std::string>& GetLookup(const std::string& file);
    Image font_atlas;
    std::map<int, Glyph> glyphs;
    int font_line = 64;
    bool font_loaded = false;

    std::string last_profile;
    std::vector<std::vector<s64>> last_values;
    std::vector<bool> last_visible;

    mutable std::mutex canvas_mutex;
    std::shared_ptr<Image> canvas;
    u64 canvas_version = 0;
};

} // namespace ScreenRegions
