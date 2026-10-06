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

#include <chrono>
#include <condition_variable>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/frontend/hud_expr.h"

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

/// Bottom-screen pixel probes: the HUD requests pixels, the renderer fills in their colour
/// (packed 0xRRGGBB) from the live bottom screen each few frames.
class Probes {
public:
    static Probes& Instance();
    void Request(u32 x, u32 y);
    std::vector<std::pair<u32, u32>> Requests() const;
    void Set(u32 x, u32 y, u32 rgb);
    u32 Get(u32 x, u32 y) const;

private:
    mutable std::mutex mutex;
    std::map<u32, u32> values; ///< key = y << 16 | x
};

/// Bottom-screen captures: the HUD asks for a rectangle of the live bottom screen to be kept
/// under a name (e.g. a party member's portrait while it is fully visible); the renderer
/// copies it at display resolution. Images named "@name" in the HUD draw the latest copy.
class Captures {
public:
    struct Request {
        std::string name;
        float x, y, w, h; ///< bottom-screen pixels (320x240)
    };
    static Captures& Instance();
    void Ask(const std::string& name, float x, float y, float w, float h);
    std::vector<Request> TakeRequests();
    void Store(const std::string& name, Image image);
    std::shared_ptr<const Image> Get(const std::string& name) const;
    u64 Version(const std::string& name) const;
    void Clear();

private:
    mutable std::mutex mutex;
    std::map<std::string, Request> pending;
    std::map<std::string, std::pair<std::shared_ptr<const Image>, u64>> store;
    u64 counter = 0;
};

/// Value source: a constant, a guest memory read (optionally through a pointer chain),
/// or a bottom-screen pixel probe ("pix:X,Y"). An optional "<N" / ">N" suffix turns the
/// value into a 0/1 comparison result.
struct Binding {
    bool constant = true;
    s64 value = 0;
    bool probe = false;
    int probe_mode = 0;           ///< 0: luminance ("pix:"), 1: green excess ("pixg:")
    u32 probe_x = 0, probe_y = 0;
    char cmp = 0;                 ///< '<', '>', '=', '!' (not equal), '&' (any bit) or 0
    s64 cmp_value = 0;
    std::string expr;             ///< address expression, e.g. "[0x00500000]+0x20"
    u32 size = 2;                 ///< 1, 2 or 4 bytes for the final read
    bool is_expr = false;         ///< "=EXPR": full expression (see hud_expr.h)
    Expr node;
    s64 Read(Core::System& system) const;
    Value Eval(const Expr::Context& ctx) const;
};

struct Element {
    enum class Type { Rect, Image, Text, Bar, Capture } type = Type::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    u32 color = 0xFFFFFFFF;     ///< RRGGBBAA
    u32 color2 = 0x000000A0;    ///< bar background
    float opacity = 1.0f;
    float crop[4] = {0, 0, 1, 1}; ///< Image: source rect as fractions ("crop=x,y,w,h")
    std::string image;          ///< image file (Image)
    std::string text;           ///< text template; {0},{1}.. are replaced by values (Text)
    float size = 32.0f;         ///< text pixel height
    int align = 0;              ///< -1 left, 0 left, 1 centre, 2 right
    std::vector<Binding> values;///< Text: template values; Bar: [value, max]
    Binding visible;            ///< element shown when non-zero (constant 1 by default)
    Binding visible2;           ///< optional second condition ("and=")
    Binding hold;               ///< while non-zero, keep the previous visibility ("hold=")
    int linger = 0;             ///< ms an element stays up after its condition drops ("linger=")
    std::string lookup;         ///< Text: list file; {0} shows line[value] instead of value
    float fit = 0;              ///< Text: shrink to fit this width (0 = off)
    Binding ox, oy;             ///< position offsets from expressions ("ox==EXPR", "oy==EXPR")
    bool has_offset = false;
};

struct HudDef {
    std::string name;           ///< profile this HUD belongs to
    std::vector<Element> elements;
    std::vector<std::pair<std::string, Expr>> lets; ///< "let name = EXPR", evaluated in order
};

class Hud {
public:
    Hud();
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    /// Blocks until the rasteriser has drawn everything requested so far (tools/tests).
    void Flush();

    /// Parses one "hud = ..." element line; returns false on syntax error.
    static bool ParseElement(const std::string& line, Element& out, std::string& error);

    /// Parses "let name = EXPR" into `def`; returns false on syntax error.
    static bool ParseLet(const std::string& line, HudDef& def, std::string& error);

    /// Parses a value binding ("u32:[0x500000]+4", "pix:10,4<50", "1", ...).
    static bool ParseValue(const std::string& text, Binding& out);

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
    void Rasterise(const HudDef& def, const std::vector<std::vector<Value>>& values,
                   const std::vector<bool>& visible,
                   const std::vector<std::pair<float, float>>& offs);

    /// Rasterising a 1920x1080 canvas takes tens of milliseconds, so it runs on a worker
    /// thread; the emulation thread only evaluates bindings and queues the newest state.
    struct Job {
        std::shared_ptr<const std::vector<HudDef>> defs;
        size_t index = 0;
        std::vector<std::vector<Value>> values;
        std::vector<bool> visible;
        std::vector<std::pair<float, float>> offsets;
        std::string asset_dir;
        bool clear = false; ///< publish an empty canvas
    };
    void WorkerLoop();
    std::thread worker;
    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::condition_variable idle_cv;
    std::optional<Job> pending;
    bool busy = false;
    bool quit = false;
    std::string worker_asset_dir;
    std::shared_ptr<const std::vector<HudDef>> defs_shared;
    void DrawRect(Image& dst, float x, float y, float w, float h, u32 rgba, float opacity);
    void DrawImage(Image& dst, const Image& src, float x, float y, float w, float h,
                   float opacity, const float* crop = nullptr);
    void DrawText(Image& dst, const std::string& text, float x, float y, float size, int align,
                  u32 rgba, float opacity);
    float TextWidth(const std::string& text, float size);

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
    std::vector<std::vector<Value>> last_values;
    std::vector<std::pair<float, float>> offsets;
    std::vector<std::pair<float, float>> last_offsets;
    std::vector<bool> last_visible;
    std::vector<std::chrono::steady_clock::time_point> last_true;
    /// Captures persisted to <asset_dir>/cache once stable: name -> (version, updates seen, saved)
    struct CacheState {
        u64 version = 0;
        int seen = 0;
        bool saved = false;
    };
    std::map<std::string, CacheState> cache_state;
    void PersistCapture(const std::string& name);

    mutable std::mutex canvas_mutex;
    std::shared_ptr<Image> canvas;
    u64 canvas_version = 0;
};

} // namespace ScreenRegions
