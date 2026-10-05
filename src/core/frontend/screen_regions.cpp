// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/screen_regions.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace ScreenRegions {

namespace {

constexpr float BottomWidth = 320.0f;
constexpr float BottomHeight = 240.0f;
constexpr float TopWidth = 400.0f;
constexpr float TopHeight = 240.0f;

std::string Trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ParseRect(std::istringstream& in, Rect& r) {
    return static_cast<bool>(in >> r.x >> r.y >> r.w >> r.h);
}

u32 ParseNumber(const std::string& s) {
    return static_cast<u32>(std::stoul(s, nullptr, 0));
}

} // Anonymous namespace

Manager& Manager::Instance() {
    static Manager instance;
    return instance;
}

void Manager::Clear() {
    file_enabled = false;
    space_window = false;
    canvas_w = 1920.0f;
    canvas_h = 1080.0f;
    default_profile.clear();
    overlay_profile.clear();
    profiles.clear();
    rules.clear();
    manual_index = -1;
    overlay_on = false;
    auto_profile.clear();
}

bool Manager::LoadFile(const std::string& path) {
    Clear();
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    enum class Section { Global, Profile, Auto } section = Section::Global;
    Profile* current = nullptr;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string header = Trim(line.substr(1, line.size() - 2));
            const std::string lower = Lower(header);
            if (lower == "auto") {
                section = Section::Auto;
                current = nullptr;
            } else if (lower.rfind("profile", 0) == 0) {
                section = Section::Profile;
                profiles.push_back(Profile{Trim(header.substr(7)), {}, true});
                current = &profiles.back();
            } else {
                LOG_WARNING(Frontend, "screen_regions:{}: unknown section [{}]", line_no, header);
                section = Section::Global;
                current = nullptr;
            }
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            LOG_WARNING(Frontend, "screen_regions:{}: expected key = value", line_no);
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));

        try {
            if (section == Section::Global) {
                if (key == "enabled") {
                    file_enabled = value != "0" && Lower(value) != "false";
                } else if (key == "space") {
                    space_window = Lower(value) == "window";
                } else if (key == "canvas") {
                    std::istringstream in(value);
                    in >> canvas_w >> canvas_h;
                } else if (key == "default_profile") {
                    default_profile = value;
                } else if (key == "overlay_profile" || key == "toggle_profile") {
                    overlay_profile = value;
                }
            } else if (section == Section::Profile && current) {
                if (key == "hide_bottom") {
                    current->hide_bottom = value != "0" && Lower(value) != "false";
                } else if (key == "region") {
                    // region = sx sy sw sh -> dx dy dw dh [opacity=0.9] [touch=0]
                    const auto arrow = value.find("->");
                    if (arrow == std::string::npos) {
                        throw std::invalid_argument("missing ->");
                    }
                    Region region;
                    std::istringstream src(value.substr(0, arrow));
                    std::istringstream dst(value.substr(arrow + 2));
                    if (!ParseRect(src, region.src) || !ParseRect(dst, region.dst)) {
                        throw std::invalid_argument("bad rectangle");
                    }
                    std::string opt;
                    while (dst >> opt) {
                        const auto oeq = opt.find('=');
                        const std::string okey = Lower(opt.substr(0, oeq));
                        const std::string oval =
                            oeq == std::string::npos ? "1" : opt.substr(oeq + 1);
                        if (okey == "opacity") {
                            region.opacity = std::clamp(std::stof(oval), 0.0f, 1.0f);
                        } else if (okey == "touch") {
                            region.touch = oval != "0";
                        }
                    }
                    current->regions.push_back(region);
                }
            } else if (section == Section::Auto && key == "rule") {
                // rule = 0xADDR u8|u16|u32 ==|!=|&|!&|>|< VALUE -> profile
                const auto arrow = value.find("->");
                if (arrow == std::string::npos) {
                    throw std::invalid_argument("missing ->");
                }
                std::istringstream in(value.substr(0, arrow));
                std::string addr, size, op, val;
                if (!(in >> addr >> size >> op >> val)) {
                    throw std::invalid_argument("bad rule");
                }
                Rule rule;
                rule.addr = ParseNumber(addr);
                const std::string lsize = Lower(size);
                rule.size = lsize == "u32" ? 4 : lsize == "u16" ? 2 : 1;
                if (op == "==") rule.op = RuleOp::Eq;
                else if (op == "!=") rule.op = RuleOp::Ne;
                else if (op == "&") rule.op = RuleOp::And;
                else if (op == "!&") rule.op = RuleOp::NotAnd;
                else if (op == ">") rule.op = RuleOp::Gt;
                else if (op == "<") rule.op = RuleOp::Lt;
                else throw std::invalid_argument("bad operator");
                rule.value = ParseNumber(val);
                rule.profile = Trim(value.substr(arrow + 2));
                rules.push_back(rule);
            }
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "screen_regions:{}: {}", line_no, e.what());
        }
    }

    if (default_profile.empty() && !profiles.empty()) {
        default_profile = profiles.front().name;
    }
    LOG_INFO(Frontend, "Screen regions loaded from {}: {} profiles, {} rules, enabled={}", path,
             profiles.size(), rules.size(), file_enabled);
    return true;
}

void Manager::Update(Core::System& system) {
    if (!system.IsPoweredOn()) {
        return;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset) {
        return;
    }

    std::scoped_lock lock{mutex};
    const u64 current_title = process->codeset->program_id;
    const bool title_changed = current_title != title_id;
    const bool check_file = title_changed || reload_requested.exchange(false) ||
                            (++frame_counter % 60) == 0;

    if (check_file) {
        title_id = current_title;
        const std::string path =
            fmt::format("{}screen_regions/{:016X}.ini",
                        FileUtil::GetUserPath(FileUtil::UserPath::LoadDir), title_id);
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            if (!file_path.empty() || title_changed) {
                Clear();
                file_path.clear();
            }
        } else if (title_changed || path != file_path || mtime != file_time) {
            const bool keep_overlay = overlay_on && !title_changed;
            const int keep_manual = title_changed ? -1 : manual_index;
            LoadFile(path);
            file_path = path;
            file_time = mtime;
            overlay_on = keep_overlay;
            manual_index =
                keep_manual < static_cast<int>(profiles.size()) ? keep_manual : -1;
        }
    }

    if (!file_enabled || rules.empty()) {
        auto_profile.clear();
        return;
    }

    // Automatic profile selection: first matching rule wins.
    auto& memory = system.Memory();
    std::string selected;
    for (const auto& rule : rules) {
        if (!memory.IsValidVirtualAddress(*process, rule.addr)) {
            continue;
        }
        u32 v = 0;
        switch (rule.size) {
        case 4:
            v = memory.Read32(rule.addr);
            break;
        case 2:
            v = memory.Read16(rule.addr);
            break;
        default:
            v = memory.Read8(rule.addr);
            break;
        }
        bool match = false;
        switch (rule.op) {
        case RuleOp::Eq: match = v == rule.value; break;
        case RuleOp::Ne: match = v != rule.value; break;
        case RuleOp::And: match = (v & rule.value) != 0; break;
        case RuleOp::NotAnd: match = (v & rule.value) == 0; break;
        case RuleOp::Gt: match = v > rule.value; break;
        case RuleOp::Lt: match = v < rule.value; break;
        }
        if (match) {
            selected = rule.profile;
            break;
        }
    }
    auto_profile = selected;
}

const Profile* Manager::FindProfile(const std::string& name) const {
    for (const auto& p : profiles) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

const Profile* Manager::CurrentProfile() const {
    if (manual_index >= 0 && manual_index < static_cast<int>(profiles.size())) {
        return &profiles[manual_index];
    }
    if (!auto_profile.empty()) {
        if (const auto* p = FindProfile(auto_profile)) {
            return p;
        }
    }
    return FindProfile(default_profile);
}

const Profile* Manager::OverlayProfile() const {
    return overlay_on ? FindProfile(overlay_profile) : nullptr;
}

bool Manager::IsActive() const {
    std::scoped_lock lock{mutex};
    return file_enabled && user_enabled && !profiles.empty();
}

bool Manager::HideBottom() const {
    std::scoped_lock lock{mutex};
    if (!(file_enabled && user_enabled)) {
        return false;
    }
    const auto* overlay = OverlayProfile();
    const auto* current = CurrentProfile();
    return (current && current->hide_bottom) && (!overlay || overlay->hide_bottom);
}

Common::Rectangle<float> Manager::ToFramebuffer(const Layout::FramebufferLayout& layout,
                                                const Rect& r) const {
    float ox, oy, sx, sy;
    if (space_window) {
        ox = 0.0f;
        oy = 0.0f;
        sx = static_cast<float>(layout.width) / canvas_w;
        sy = static_cast<float>(layout.height) / canvas_h;
    } else {
        const auto& top = layout.top_screen;
        ox = static_cast<float>(top.left);
        oy = static_cast<float>(top.top);
        sx = static_cast<float>(top.GetWidth()) / TopWidth;
        sy = static_cast<float>(top.GetHeight()) / TopHeight;
    }
    const float left = ox + r.x * sx;
    const float top = oy + r.y * sy;
    return {left, top, left + r.w * sx, top + r.h * sy};
}

std::vector<DrawRegion> Manager::Resolve(const Layout::FramebufferLayout& layout) const {
    std::scoped_lock lock{mutex};
    std::vector<DrawRegion> out;
    if (!(file_enabled && user_enabled)) {
        return out;
    }
    const auto append = [&](const Profile* profile) {
        if (!profile) {
            return;
        }
        for (const auto& r : profile->regions) {
            const auto dst = ToFramebuffer(layout, r.dst);
            out.push_back(DrawRegion{
                Common::Rectangle<float>{r.src.x / BottomWidth, r.src.y / BottomHeight,
                                         (r.src.x + r.src.w) / BottomWidth,
                                         (r.src.y + r.src.h) / BottomHeight},
                dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, r.opacity, r.touch});
        }
    };
    append(CurrentProfile());
    append(OverlayProfile());
    return out;
}

bool Manager::MapTouch(const Layout::FramebufferLayout& layout, float fb_x, float fb_y,
                       float& touch_x, float& touch_y) const {
    const auto regions = Resolve(layout);
    // Topmost region wins.
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        if (!it->touch || it->w <= 0 || it->h <= 0) {
            continue;
        }
        if (fb_x >= it->x && fb_x < it->x + it->w && fb_y >= it->y && fb_y < it->y + it->h) {
            const float u = (fb_x - it->x) / it->w;
            const float v = (fb_y - it->y) / it->h;
            touch_x = it->src_norm.left + u * (it->src_norm.right - it->src_norm.left);
            touch_y = it->src_norm.top + v * (it->src_norm.bottom - it->src_norm.top);
            return true;
        }
    }
    return false;
}

void Manager::ToggleEnabled() {
    std::scoped_lock lock{mutex};
    user_enabled = !user_enabled;
}

void Manager::ToggleOverlay() {
    std::scoped_lock lock{mutex};
    overlay_on = !overlay_on;
}

void Manager::CycleProfile() {
    std::scoped_lock lock{mutex};
    if (profiles.empty()) {
        return;
    }
    // automatic -> 0 -> 1 -> ... -> n-1 -> automatic
    manual_index = manual_index + 1 >= static_cast<int>(profiles.size()) ? -1 : manual_index + 1;
}

void Manager::Reload() {
    reload_requested = true;
}

std::string Manager::StatusText() const {
    std::scoped_lock lock{mutex};
    if (!file_enabled) {
        return "Screen regions: no profile file for this title";
    }
    if (!user_enabled) {
        return "Screen regions: off";
    }
    const auto* p = CurrentProfile();
    return fmt::format("Screen regions: {}{}{}", manual_index < 0 ? "auto/" : "manual/",
                       p ? p->name : "none", overlay_on ? " + " + overlay_profile : "");
}

} // namespace ScreenRegions
