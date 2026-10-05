// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Screen Regions: per-game compositing of bottom-screen regions onto the main display.
//
// A per-title definition file (<user>/load/screen_regions/<TITLEID>.ini) lists named profiles.
// Each profile copies rectangles of the live bottom screen to rectangles positioned relative to
// the top screen (or to the whole window), optionally translucent and clickable. Profiles can be
// selected automatically from guest RAM values, cycled with a hotkey, and a designated "overlay"
// profile (e.g. a full map) can be toggled on top with a hotkey.

#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/frontend/hud.h"

namespace Core {
class System;
}

namespace Layout {
struct FramebufferLayout;
}

namespace ScreenRegions {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct Region {
    Rect src;             ///< Bottom-screen pixels (0..320 x 0..240)
    Rect dst;             ///< Destination, in the profile file's coordinate space
    float opacity = 1.0f; ///< 0..1
    bool touch = true;    ///< Mouse clicks inside dst are forwarded as touches at src
    int space = -1;       ///< -1: file default, 0: top-screen space, 1: window canvas space
};

struct Profile {
    std::string name;
    std::vector<Region> regions;
    bool hide_bottom = true; ///< Suppress the normal bottom screen while this profile is active
    bool has_top = false;    ///< Override the top screen placement while active
    Rect top;                ///< Top screen rectangle in window canvas space
};

enum class RuleOp { Eq, Ne, And, NotAnd, Gt, Lt };

struct Rule {
    std::vector<u64> textures; ///< If set: matches when one of these custom textures was drawn
    u32 addr = 0;
    u32 size = 1; ///< 1, 2 or 4 bytes
    RuleOp op = RuleOp::Eq;
    u32 value = 0;
    std::string profile;
};

/// Resolved draw command in framebuffer pixels.
struct DrawRegion {
    Common::Rectangle<float> src_norm; ///< Normalised bottom-screen rect (0..1, origin top-left)
    float x, y, w, h;                  ///< Framebuffer pixels
    float opacity;
    bool touch;
};

class Manager {
public:
    static Manager& Instance();

    /// Called once per presented frame from the renderer (emulation thread).
    void Update(Core::System& system);

    /// Called by the rasterizer whenever a custom (replaced) texture is sampled (emulation thread).
    void NoteTexture(u64 hash);

    bool IsActive() const;
    bool HideBottom() const;

    /// Returns the layout with the active profile's top-screen placement applied.
    Layout::FramebufferLayout Apply(const Layout::FramebufferLayout& layout) const;

    /// Draw list for the given (unmodified) framebuffer layout, in back-to-front order.
    std::vector<DrawRegion> Resolve(const Layout::FramebufferLayout& layout) const;

    /// Maps a framebuffer click to a normalised bottom-screen touch (0..1, origin top-left).
    bool MapTouch(const Layout::FramebufferLayout& layout, float fb_x, float fb_y, float& touch_x,
                  float& touch_y) const;

    // Hotkey actions
    void ToggleEnabled();
    void ToggleOverlay();
    void CycleProfile();
    void Reload();

    std::string StatusText() const;

    /// Recorder (layout research): true when the renderer should hand over the bottom screen.
    bool WantsBottomCapture() const { return capture_pending.load(); }
    /// Renderer hands over the bottom screen as 320x240 RGB8 (top-left origin).
    void SetBottomCapture(std::vector<u8> rgb);

    /// Current HUD canvas (may be null) and its version; plus where to draw it.
    std::shared_ptr<const Image> HudCanvas(u64& version) const;
    Common::Rectangle<float> CanvasRect(const Layout::FramebufferLayout& layout) const;

private:
    Manager() = default;

    bool LoadFile(const std::string& path, const std::string& contents);
    void Clear();
    const Profile* FindProfile(const std::string& name) const;
    const Profile* CurrentProfile() const;
    const Profile* OverlayProfile() const;
    Common::Rectangle<float> ToFramebuffer(const Layout::FramebufferLayout& layout, const Rect& r,
                                           bool window_space) const;
    Layout::FramebufferLayout ApplyLocked(const Layout::FramebufferLayout& layout) const;

    mutable std::mutex mutex;

    // File state
    u64 title_id = 0;
    std::string file_path;
    std::string file_contents;
    u32 frame_counter = 0;
    const void* last_process = nullptr;

    // Definition
    bool file_enabled = false;
    bool space_window = false; ///< false: 400x240 top-screen space; true: canvas_w x canvas_h
    float canvas_w = 1920.0f, canvas_h = 1080.0f;
    std::string default_profile;
    std::string overlay_profile;
    std::vector<Profile> profiles;
    std::vector<Rule> rules;
    std::vector<HudDef> hud_defs;
    Hud hud;

    // Runtime state
    bool user_enabled = true;
    bool overlay_on = false;
    int manual_index = -1; ///< -1 = automatic selection
    std::string auto_profile;
    std::atomic<bool> reload_requested{false};

    // Texture-rule tracking (emulation thread only)
    u32 present_frame = 1;
    std::vector<std::pair<u64, u32>> texture_seen; ///< watched hash -> last frame drawn

    // Recorder: periodically saves guest RAM + the bottom screen while chosen profiles are
    // active, so UI state variables can be located offline ("record = battle,menu 90 400").
    std::vector<std::string> record_profiles;
    u32 record_interval = 90;
    u32 record_max = 0;
    u32 record_count = 0;
    u32 record_last = 0;
    std::atomic<bool> capture_pending{false};
    std::mutex capture_mutex;
    std::vector<u8> capture_rgb;
    bool capture_ready = false;
    void WriteRecord(Core::System& system, const std::string& profile);

};

} // namespace ScreenRegions
