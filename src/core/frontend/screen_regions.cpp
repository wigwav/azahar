// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <sstream>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include "common/file_util.h"
#include "common/zstd_compression.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/screen_regions.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
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
    record_profiles.clear();
    record_max = 0;
    profiles.clear();
    rules.clear();
    hud_defs.clear();
    hud.SetDefinition({}, "");
    manual_index = -1;
    overlay_on = false;
    auto_profile.clear();
}

bool Manager::LoadFile(const std::string& path, const std::string& contents) {
    Clear();
    std::istringstream file(contents);
    if (!file) {
        return false;
    }

    enum class Section { Global, Profile, Auto, Hud } section = Section::Global;
    Profile* current = nullptr;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        {
            // Comments: lines starting with '#' or ';', or text after " #" / " ;"
            // (colours like #FF0000 inside HUD lines are preceded by a space too, so only
            // treat "# " (hash followed by space) or ';' as a comment marker).
            const auto t = line.find_first_not_of(" \t");
            if (t != std::string::npos && (line[t] == '#' || line[t] == ';')) {
                line.clear();
            }
            const auto c1 = line.find(" # ");
            if (c1 != std::string::npos) {
                line.resize(c1);
            }
            const auto c2 = line.find(" ; ");
            if (c2 != std::string::npos) {
                line.resize(c2);
            }
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
            } else if (lower.rfind("hud", 0) == 0) {
                section = Section::Hud;
                current = nullptr;
                hud_defs.push_back(HudDef{Trim(header.substr(3)), {}});
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

        if (section == Section::Hud) {
            Element element;
            std::string error;
            if (line.rfind("let ", 0) == 0) {
                if (!Hud::ParseLet(line, hud_defs.back(), error)) {
                    LOG_WARNING(Frontend, "screen_regions:{}: {}", line_no, error);
                }
            } else if (Hud::ParseElement(line, element, error)) {
                hud_defs.back().elements.push_back(std::move(element));
            } else {
                LOG_WARNING(Frontend, "screen_regions:{}: {}", line_no, error);
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
                } else if (key == "record") {
                    std::istringstream in(value);
                    std::string names;
                    in >> names >> record_interval >> record_max;
                    record_profiles.clear();
                    std::istringstream ns(names);
                    for (std::string n; std::getline(ns, n, ',');) {
                        if (!Trim(n).empty()) {
                            record_profiles.push_back(Trim(n));
                        }
                    }
                    record_interval = std::max<u32>(record_interval, 10);
                }
            } else if (section == Section::Profile && current) {
                if (key == "hide_bottom") {
                    current->hide_bottom = value != "0" && Lower(value) != "false";
                } else if (key == "hud_under") {
                    current->hud_under = value != "0" && Lower(value) != "false";
                } else if (key == "top") {
                    // top = x y w h  (window canvas space)
                    std::istringstream in(value);
                    if (!ParseRect(in, current->top)) {
                        throw std::invalid_argument("bad top rectangle");
                    }
                    current->has_top = true;
                } else if (key == "region") {
                    // region = sx sy sw sh -> dx dy dw dh [opacity=0.9] [touch=0]
                    const auto arrow = value.find("->");
                    if (arrow == std::string::npos) {
                        throw std::invalid_argument("missing ->");
                    }
                    Region region;
                    region.visible.value = 1; // shown unless an if= binding says otherwise
                    region.visible2.value = 1;
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
                        } else if (okey == "space") {
                            region.space = Lower(oval) == "window" ? 1 : 0;
                        } else if (okey == "blend") {
                            region.screen = Lower(oval) == "screen";
                        } else if (okey == "if") {
                            if (!Hud::ParseValue(oval, region.visible)) {
                                throw std::invalid_argument("bad if= binding");
                            }
                        } else if (okey == "and") {
                            if (!Hud::ParseValue(oval, region.visible2)) {
                                throw std::invalid_argument("bad and= binding");
                            }
                        }
                    }
                    current->regions.push_back(region);
                }
            } else if (section == Section::Auto && key == "rule") {
                // rule = texture HASH[,HASH...] -> profile
                // rule = 0xADDR u8|u16|u32 ==|!=|&|!&|>|< VALUE -> profile
                const auto arrow = value.find("->");
                if (arrow == std::string::npos) {
                    throw std::invalid_argument("missing ->");
                }
                std::istringstream in(value.substr(0, arrow));
                std::string first;
                in >> first;
                Rule rule;
                if (Lower(first) == "texture") {
                    std::string list, item;
                    std::getline(in, list);
                    std::replace(list.begin(), list.end(), ',', ' ');
                    std::istringstream items(list);
                    while (items >> item) {
                        rule.textures.push_back(std::stoull(item, nullptr, 16));
                    }
                    if (rule.textures.empty()) {
                        throw std::invalid_argument("texture rule without hashes");
                    }
                } else {
                    std::string size, op, val;
                    if (!(in >> size >> op >> val)) {
                        throw std::invalid_argument("bad rule");
                    }
                    rule.addr = ParseNumber(first);
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
                }
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
    hud.SetDefinition(hud_defs, fmt::format("{}screen_regions/{:016X}/",
                                            FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                                            title_id));
    texture_seen.clear();
    for (const auto& rule : rules) {
        for (const u64 hash : rule.textures) {
            texture_seen.emplace_back(hash, 0);
        }
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
    ++present_frame;
    const u64 current_title = process->codeset->program_id;
    const bool title_changed = current_title != title_id;
    const bool check_file = title_changed || reload_requested.exchange(false) ||
                            (++frame_counter % 60) == 0;

    if (title_changed || process.get() != last_process) {
        last_process = process.get();
        // Log the process memory map (virtual -> FCRAM offset) once per title. Used to
        // translate addresses found in save-state snapshots into game addresses.
        const u8* fcram = system.Memory().GetFCRAMPointer(0);
        for (const auto& [vaddr, vma] : process->vm_manager.vma_map) {
            if (vma.type != Kernel::VMAType::BackingMemory) {
                continue;
            }
            const u8* ptr = vma.backing_memory.GetPtr();
            if (ptr >= fcram && ptr < fcram + Memory::FCRAM_N3DS_SIZE) {
                LOG_INFO(Frontend, "ScreenRegions memmap: va={:08X} size={:08X} fcram={:08X}",
                         vma.base, vma.size, static_cast<u32>(ptr - fcram));
            }
        }
    }

    if (check_file) {
        title_id = current_title;
        const std::string path =
            fmt::format("{}screen_regions/{:016X}.ini",
                        FileUtil::GetUserPath(FileUtil::UserPath::LoadDir), title_id);
        // Compare file contents rather than timestamps: robust on every platform/filesystem.
        std::string contents;
        const bool exists = FileUtil::Exists(path) &&
                            FileUtil::ReadFileToString(true, path, contents) > 0;
        if (!exists) {
            if (!file_path.empty() || title_changed) {
                Clear();
                file_path.clear();
                file_contents.clear();
            }
        } else if (title_changed || path != file_path || contents != file_contents) {
            const bool keep_overlay = overlay_on && !title_changed;
            const int keep_manual = title_changed ? -1 : manual_index;
            LoadFile(path, contents);
            file_path = path;
            file_contents = std::move(contents);
            overlay_on = keep_overlay;
            manual_index =
                keep_manual < static_cast<int>(profiles.size()) ? keep_manual : -1;
        }
    }

    if (!file_enabled || rules.empty()) {
        auto_profile.clear();
        const Profile* p = file_enabled && user_enabled ? CurrentProfile() : nullptr;
        EvaluateRegions(system);
        hud.Update(system, p ? p->name : std::string{});
        return;
    }

    // Automatic profile selection.
    //  1. Memory rules: the first matching rule wins.
    //  2. Texture rules: the rule whose texture was drawn most recently wins (ties: file order).
    auto& memory = system.Memory();
    std::string selected;
    u32 best_frame = 0;
    for (const auto& rule : rules) {
        if (!rule.textures.empty()) {
            u32 seen = 0;
            for (const u64 hash : rule.textures) {
                for (const auto& [h, frame] : texture_seen) {
                    if (h == hash) {
                        seen = std::max(seen, frame);
                    }
                }
            }
            if (seen > best_frame) {
                best_frame = seen;
                selected = rule.profile;
            }
            continue;
        }
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
    if (record_max > 0 && record_count < record_max) {
        const Profile* rp = CurrentProfile();
        const std::string rname = rp ? rp->name : std::string{};
        const bool wanted = std::find(record_profiles.begin(), record_profiles.end(), rname) !=
                            record_profiles.end();
        bool ready = false;
        {
            std::scoped_lock clock{capture_mutex};
            ready = capture_ready;
        }
        if (ready) {
            if (wanted) {
                WriteRecord(system, rname);
            }
            std::scoped_lock clock{capture_mutex};
            capture_ready = false;
            capture_rgb.clear();
        } else if (wanted && !capture_pending.load() &&
                   present_frame - record_last >= record_interval) {
            record_last = present_frame;
            capture_pending = true;
        }
    }
    EvaluateRegions(system);
    if (user_enabled) {
        const Profile* p = CurrentProfile();
        hud.Update(system, p ? p->name : std::string{});
    } else {
        hud.Update(system, std::string{});
    }
}

void Manager::SetBottomCapture(std::vector<u8> rgb) {
    std::scoped_lock clock{capture_mutex};
    capture_rgb = std::move(rgb);
    capture_ready = true;
    capture_pending = false;
}

void Manager::WriteRecord(Core::System& system, const std::string& profile) {
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return;
    }
    auto& memory = system.Memory();
    std::vector<u8> out;
    auto put32 = [&out](u32 v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>(v >> (8 * i)));
        }
    };
    const char magic[8] = {'S', 'R', 'R', 'E', 'C', '1', 0, 0};
    out.insert(out.end(), magic, magic + 8);
    put32(present_frame);
    char pname[16]{};
    std::copy_n(profile.begin(), std::min<size_t>(profile.size(), 15), pname);
    out.insert(out.end(), pname, pname + 16);
    // Guest memory: game .data/.bss, the whole heap, and the start of linear heap (save data).
    std::vector<std::pair<u32, u32>> ranges{{0x00568000, 0xE5000}};
    for (const auto& [vaddr, vma] : process->vm_manager.vma_map) {
        if (vma.type == Kernel::VMAType::BackingMemory && vma.base >= 0x08000000 &&
            vma.base < 0x10000000) {
            ranges.emplace_back(vma.base, vma.size);
        }
    }
    ranges.emplace_back(0x30C00000, 0x200000);
    put32(static_cast<u32>(ranges.size()));
    for (const auto& [va, size] : ranges) {
        put32(va);
        put32(size);
        const size_t at = out.size();
        out.resize(at + size);
        if (memory.IsValidVirtualAddress(*process, va) &&
            memory.IsValidVirtualAddress(*process, va + size - 1)) {
            memory.ReadBlock(*process, va, out.data() + at, size);
        }
    }
    {
        std::scoped_lock clock{capture_mutex};
        put32(320);
        put32(240);
        capture_rgb.resize(320 * 240 * 3);
        out.insert(out.end(), capture_rgb.begin(), capture_rgb.end());
    }
    const std::string dir =
        fmt::format("{}screen_regions/", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir));
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const std::string path = fmt::format("{}rec_{:016X}_{}.zst", dir, title_id, now);
    ++record_count;
    // Compress and write off the emulation thread so recording doesn't stutter the game.
    std::thread([dir, path, data = std::move(out)]() {
        const auto packed = Common::Compression::CompressDataZSTD(data, 3);
        FileUtil::CreateFullPath(dir);
        FileUtil::IOFile file(path, "wb");
        if (file.IsOpen()) {
            file.WriteBytes(packed.data(), packed.size());
        }
    }).detach();
}

void Manager::EvaluateRegions(Core::System& system) {
    const Profile* current = user_enabled ? CurrentProfile() : nullptr;
    const Profile* overlay = user_enabled ? OverlayProfile() : nullptr;
    for (auto& profile : profiles) {
        if (&profile != current && &profile != overlay) {
            continue;
        }
        for (auto& r : profile.regions) {
            r.shown = r.visible.Read(system) != 0 && r.visible2.Read(system) != 0;
        }
    }
}

void Manager::NoteTexture(u64 hash) {
    for (auto& [h, frame] : texture_seen) {
        if (h == hash) {
            frame = present_frame;
            return;
        }
    }
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

bool Manager::HudUnder() const {
    std::scoped_lock lock{mutex};
    const auto* current = file_enabled && user_enabled ? CurrentProfile() : nullptr;
    return current && current->hud_under;
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
                                                const Rect& r, bool window_space) const {
    float ox, oy, sx, sy;
    if (window_space) {
        // Fit the canvas into the window with a uniform scale (no stretching on
        // ultrawide / 16:10 displays) and centre it.
        const float scale = std::min(static_cast<float>(layout.width) / canvas_w,
                                     static_cast<float>(layout.height) / canvas_h);
        sx = sy = scale;
        ox = (static_cast<float>(layout.width) - canvas_w * scale) * 0.5f;
        oy = (static_cast<float>(layout.height) - canvas_h * scale) * 0.5f;
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

Layout::FramebufferLayout Manager::ApplyLocked(const Layout::FramebufferLayout& layout) const {
    Layout::FramebufferLayout out = layout;
    if (!(file_enabled && user_enabled) || !layout.is_rotated) {
        return out;
    }
    const Profile* src = nullptr;
    if (const auto* overlay = OverlayProfile(); overlay && overlay->has_top) {
        src = overlay;
    } else if (const auto* current = CurrentProfile(); current && current->has_top) {
        src = current;
    }
    if (src) {
        const auto r = ToFramebuffer(layout, src->top, true);
        out.top_screen = Common::Rectangle<u32>{
            static_cast<u32>(std::max(0.0f, r.left)), static_cast<u32>(std::max(0.0f, r.top)),
            static_cast<u32>(std::max(0.0f, r.right)), static_cast<u32>(std::max(0.0f, r.bottom))};
    }
    return out;
}

Layout::FramebufferLayout Manager::Apply(const Layout::FramebufferLayout& layout) const {
    std::scoped_lock lock{mutex};
    return ApplyLocked(layout);
}

std::vector<DrawRegion> Manager::Resolve(const Layout::FramebufferLayout& layout) const {
    std::scoped_lock lock{mutex};
    std::vector<DrawRegion> out;
    if (!(file_enabled && user_enabled)) {
        return out;
    }
    const auto adjusted = ApplyLocked(layout);
    const auto append = [&](const Profile* profile) {
        if (!profile) {
            return;
        }
        for (const auto& r : profile->regions) {
            if (!r.shown) {
                continue;
            }
            const bool window_space = r.space < 0 ? space_window : r.space == 1;
            const auto dst = ToFramebuffer(adjusted, r.dst, window_space);
            out.push_back(DrawRegion{
                Common::Rectangle<float>{r.src.x / BottomWidth, r.src.y / BottomHeight,
                                         (r.src.x + r.src.w) / BottomWidth,
                                         (r.src.y + r.src.h) / BottomHeight},
                dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, r.opacity, r.touch,
                r.screen});
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

namespace ScreenRegions {

std::shared_ptr<const Image> Manager::HudCanvas(u64& version) const {
    return hud.Canvas(version);
}

Common::Rectangle<float> Manager::CanvasRect(const Layout::FramebufferLayout& layout) const {
    std::scoped_lock lock{mutex};
    return ToFramebuffer(layout, Rect{0, 0, canvas_w, canvas_h}, true);
}

} // namespace ScreenRegions
