// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <sstream>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/hud.h"
#include "core/frontend/image_interface.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace ScreenRegions {

namespace {

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') {
            if (quoted) {
                out.push_back("\"" + cur);
                cur.clear();
            }
            quoted = !quoted;
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

u32 ParseColor(const std::string& s) {
    std::string hex = s[0] == '#' ? s.substr(1) : s;
    if (hex.size() == 6) {
        hex += "FF";
    }
    return static_cast<u32>(std::stoul(hex, nullptr, 16));
}

/// u16:0x08001234   u32:[0x00500000]+0x20   u8:[[0x00500000]+4]+0x1C   or plain number
bool ParseBinding(const std::string& in, Binding& b) {
    std::string s = in;
    const auto cmp = s.find_first_of("<>=!&");
    if (cmp != std::string::npos && cmp > 0) {
        b.cmp = s[cmp];
        b.cmp_value = std::stoll(s.substr(cmp + 1), nullptr, 0);
        s = s.substr(0, cmp);
    }
    if (s.rfind("pix:", 0) == 0 || s.rfind("pixg:", 0) == 0) {
        const auto colon = s.find(':');
        const auto comma = s.find(',');
        b.constant = false;
        b.probe = true;
        b.probe_mode = s[3] == 'g' ? 1 : 0;
        b.probe_x = static_cast<u32>(std::stoul(s.substr(colon + 1, comma - colon - 1)));
        b.probe_y = static_cast<u32>(std::stoul(s.substr(comma + 1)));
        Probes::Instance().Request(b.probe_x, b.probe_y);
        return true;
    }
    const auto colon = s.find(':');
    if (colon == std::string::npos) {
        b.constant = true;
        b.value = std::stoll(s, nullptr, 0);
        return true;
    }
    const std::string type = s.substr(0, colon);
    b.size = type == "u8" ? 1 : type == "u32" ? 4 : 2;
    b.constant = false;
    b.expr = s.substr(colon + 1);
    return !b.expr.empty();
}

/// expr := term (('+'|'-') number)* ; term := number | '[' expr ']' (read u32 at address)
bool EvalExpr(const std::string& e, size_t& pos, Core::System& system,
              const Kernel::Process& process, u32& out) {
    auto& memory = system.Memory();
    u32 v;
    if (pos < e.size() && e[pos] == '[') {
        ++pos;
        u32 inner;
        if (!EvalExpr(e, pos, system, process, inner) || pos >= e.size() || e[pos] != ']') {
            return false;
        }
        ++pos;
        if (!memory.IsValidVirtualAddress(process, inner)) {
            return false;
        }
        v = memory.Read32(inner);
    } else {
        size_t end = e.find_first_of("]+-", pos);
        v = static_cast<u32>(std::stoul(e.substr(pos, end - pos), nullptr, 0));
        pos = end == std::string::npos ? e.size() : end;
    }
    while (pos < e.size() && (e[pos] == '+' || e[pos] == '-')) {
        const char op = e[pos++];
        size_t end = e.find_first_of("]+-", pos);
        const u32 n = static_cast<u32>(std::stoul(e.substr(pos, end - pos), nullptr, 0));
        v = op == '+' ? v + n : v - n;
        pos = end == std::string::npos ? e.size() : end;
    }
    out = v;
    return true;
}

} // Anonymous namespace

Probes& Probes::Instance() {
    static Probes instance;
    return instance;
}

void Probes::Request(u32 x, u32 y) {
    std::scoped_lock lock{mutex};
    values.try_emplace(y << 16 | x, 0);
}

std::vector<std::pair<u32, u32>> Probes::Requests() const {
    std::scoped_lock lock{mutex};
    std::vector<std::pair<u32, u32>> out;
    for (const auto& [key, v] : values) {
        out.emplace_back(key & 0xFFFF, key >> 16);
    }
    return out;
}

void Probes::Set(u32 x, u32 y, u32 rgb) {
    std::scoped_lock lock{mutex};
    values[y << 16 | x] = rgb;
}

u32 Probes::Get(u32 x, u32 y) const {
    std::scoped_lock lock{mutex};
    const auto it = values.find(y << 16 | x);
    return it == values.end() ? 0 : it->second;
}

static s64 ReadRaw(const Binding& b, Core::System& system);

s64 Binding::Read(Core::System& system) const {
    s64 v = 0;
    if (probe) {
        const u32 rgb = Probes::Instance().Get(probe_x, probe_y);
        const s64 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, bl = rgb & 0xFF;
        v = probe_mode == 1 ? g - std::max(r, bl) : (r * 299 + g * 587 + bl * 114) / 1000;
    } else {
        v = ReadRaw(*this, system);
    }
    if (cmp == '<') {
        return v < cmp_value ? 1 : 0;
    }
    if (cmp == '=') {
        return v == cmp_value ? 1 : 0;
    }
    if (cmp == '!') {
        return v != cmp_value ? 1 : 0;
    }
    if (cmp == '&') {
        return (v & cmp_value) != 0 ? 1 : 0;
    }
    if (cmp == '>') {
        return v > cmp_value ? 1 : 0;
    }
    return v;
}

static s64 ReadRaw(const Binding& b, Core::System& system) {
    const auto& constant = b.constant;
    const auto& value = b.value;
    const auto& expr = b.expr;
    const auto& size = b.size;
    if (constant) {
        return value;
    }
    auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return 0;
    }
    u32 addr = 0;
    size_t pos = 0;
    try {
        if (!EvalExpr(expr, pos, system, *process, addr)) {
            return 0;
        }
    } catch (const std::exception&) {
        return 0;
    }
    auto& memory = system.Memory();
    if (!memory.IsValidVirtualAddress(*process, addr)) {
        return 0;
    }
    switch (size) {
    case 1:
        return memory.Read8(addr);
    case 4:
        return memory.Read32(addr);
    default:
        return memory.Read16(addr);
    }
}

bool Hud::ParseValue(const std::string& text, Binding& out) {
    try {
        return ParseBinding(text, out);
    } catch (const std::exception&) {
        return false;
    }
}

bool Hud::ParseElement(const std::string& line, Element& e, std::string& error) {
    try {
        auto t = Tokenize(line);
        if (t.empty()) {
            error = "empty element";
            return false;
        }
        const std::string kind = t[0];
        size_t i = 1;
        auto num = [&]() { return std::stof(t.at(i++)); };
        if (kind == "rect") {
            e.type = Element::Type::Rect;
            e.x = num(); e.y = num(); e.w = num(); e.h = num();
            e.color = ParseColor(t.at(i++));
        } else if (kind == "image") {
            e.type = Element::Type::Image;
            e.x = num(); e.y = num(); e.w = num(); e.h = num();
            e.image = t.at(i++);
        } else if (kind == "text") {
            e.type = Element::Type::Text;
            e.x = num(); e.y = num(); e.size = num();
            e.color = ParseColor(t.at(i++));
            const std::string a = t.at(i++);
            e.align = a == "center" ? 1 : a == "right" ? 2 : 0;
            const std::string q = t.at(i++);
            e.text = q[0] == '"' ? q.substr(1) : q;
        } else if (kind == "bar") {
            e.type = Element::Type::Bar;
            e.x = num(); e.y = num(); e.w = num(); e.h = num();
            e.color = ParseColor(t.at(i++));
            e.color2 = ParseColor(t.at(i++));
        } else {
            error = "unknown element '" + kind + "'";
            return false;
        }
        for (; i < t.size(); ++i) {
            const auto eq = t[i].find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string k = t[i].substr(0, eq);
            const std::string v = t[i].substr(eq + 1);
            if (k == "names" || k == "lookup") {
                e.lookup = v;
            } else if (k == "opacity") {
                e.opacity = std::clamp(std::stof(v), 0.0f, 1.0f);
            } else if (k == "v" || k == "value" || k == "max") {
                Binding b;
                if (!ParseBinding(v, b)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
                e.values.push_back(b);
            } else if (k == "if") {
                if (!ParseBinding(v, e.visible)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
            } else if (k == "and") {
                if (!ParseBinding(v, e.visible2)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
            } else if (k == "hold") {
                if (!ParseBinding(v, e.hold)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
            }
        }
        if (e.visible.constant && e.visible.value == 0) {
            e.visible.value = 1;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void Hud::SetDefinition(const std::vector<HudDef>& new_defs, const std::string& dir) {
    defs = new_defs;
    asset_dir = dir;
    images.clear();
    lookups.clear();
    font_loaded = false;
    last_profile.clear();
    last_values.clear();
}

bool Hud::LoadFont() {
    if (font_loaded) {
        return !glyphs.empty();
    }
    font_loaded = true;
    const Image* atlas = GetImage("font.png");
    if (!atlas) {
        return false;
    }
    font_atlas = *atlas;
    std::string metrics;
    if (FileUtil::ReadFileToString(true, asset_dir + "font.txt", metrics) == 0) {
        return false;
    }
    std::istringstream in(metrics);
    std::string word;
    in >> word >> font_line;
    int id;
    Glyph g;
    while (in >> id >> g.x >> g.y >> g.w >> g.h >> g.xoff >> g.yoff >> g.advance) {
        glyphs[id] = g;
    }
    return !glyphs.empty();
}

const Image* Hud::GetImage(const std::string& file) {
    auto it = images.find(file);
    if (it != images.end()) {
        return it->second.width ? &it->second : nullptr;
    }
    Image img;
    std::string data;
    const std::string path = asset_dir + file;
    if (FileUtil::ReadFileToString(false, path, data) > 0) {
        Frontend::ImageInterface decoder;
        std::vector<u8> pixels;
        if (decoder.DecodePNG(pixels, img.width, img.height,
                              {reinterpret_cast<const u8*>(data.data()), data.size()})) {
            img.pixels = std::move(pixels);
        } else {
            LOG_WARNING(Frontend, "HUD: could not decode {}", path);
            img = {};
        }
    } else {
        LOG_WARNING(Frontend, "HUD: missing asset {}", path);
    }
    auto& slot = images[file] = std::move(img);
    return slot.width ? &slot : nullptr;
}

const std::vector<std::string>& Hud::GetLookup(const std::string& file) {
    auto it = lookups.find(file);
    if (it != lookups.end()) {
        return it->second;
    }
    std::vector<std::string> lines;
    std::string data;
    if (FileUtil::ReadFileToString(true, asset_dir + file, data) > 0) {
        std::istringstream in(data);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
    }
    return lookups[file] = std::move(lines);
}

bool Hud::Update(Core::System& system, const std::string& active_profile) {
    const HudDef* def = nullptr;
    for (const auto& d : defs) {
        if (d.name == active_profile) {
            def = &d;
        }
    }
    if (!def) {
        if (!last_profile.empty()) {
            last_profile.clear();
            std::scoped_lock lock{canvas_mutex};
            canvas.reset();
            ++canvas_version;
            return true;
        }
        return false;
    }

    std::vector<std::vector<s64>> values(def->elements.size());
    std::vector<bool> visible(def->elements.size());
    for (size_t i = 0; i < def->elements.size(); ++i) {
        const auto& e = def->elements[i];
        const bool can_hold = active_profile == last_profile && i < last_visible.size();
        if (can_hold && e.hold.Read(system) != 0) {
            visible[i] = last_visible[i];
        } else {
            visible[i] = e.visible.Read(system) != 0 &&
                         (e.visible2.constant ? true : e.visible2.Read(system) != 0);
        }
        for (const auto& b : e.values) {
            values[i].push_back(b.Read(system));
        }
    }
    if (active_profile == last_profile && values == last_values && visible == last_visible) {
        return false;
    }
    last_profile = active_profile;
    last_values = values;
    last_visible = visible;
    Rasterise(*def, values, visible);
    return true;
}

std::shared_ptr<const Image> Hud::Canvas(u64& version) const {
    std::scoped_lock lock{canvas_mutex};
    version = canvas_version;
    return canvas;
}

void Hud::Rasterise(const HudDef& def, const std::vector<std::vector<s64>>& values,
                    const std::vector<bool>& visible) {
    auto img = std::make_shared<Image>();
    img->width = CanvasWidth;
    img->height = CanvasHeight;
    img->pixels.assign(static_cast<size_t>(CanvasWidth) * CanvasHeight * 4, 0);

    for (size_t i = 0; i < def.elements.size(); ++i) {
        const auto& e = def.elements[i];
        if (!visible[i]) {
            continue;
        }
        switch (e.type) {
        case Element::Type::Rect:
            DrawRect(*img, e.x, e.y, e.w, e.h, e.color, e.opacity);
            break;
        case Element::Type::Image:
            if (const Image* src = GetImage(e.image)) {
                DrawImage(*img, *src, e.x, e.y, e.w, e.h, e.opacity);
            }
            break;
        case Element::Type::Bar: {
            DrawRect(*img, e.x, e.y, e.w, e.h, e.color2, e.opacity);
            if (values[i].size() >= 2 && values[i][1] > 0) {
                const float f = std::clamp(static_cast<float>(values[i][0]) /
                                               static_cast<float>(values[i][1]),
                                           0.0f, 1.0f);
                DrawRect(*img, e.x, e.y, e.w * f, e.h, e.color, e.opacity);
            }
            break;
        }
        case Element::Type::Text: {
            std::string s = e.text;
            for (size_t v = 0; v < values[i].size(); ++v) {
                const std::string key = "{" + std::to_string(v) + "}";
                std::string repl = std::to_string(values[i][v]);
                if (v == 0 && !e.lookup.empty()) {
                    const auto& list = GetLookup(e.lookup);
                    const s64 idx = values[i][v];
                    repl = idx >= 0 && idx < static_cast<s64>(list.size()) ? list[idx] : "";
                }
                size_t p;
                while ((p = s.find(key)) != std::string::npos) {
                    s.replace(p, key.size(), repl);
                }
            }
            if (LoadFont()) {
                DrawText(*img, s, e.x, e.y, e.size, e.align, e.color, e.opacity);
            }
            break;
        }
        }
    }
    std::scoped_lock lock{canvas_mutex};
    canvas = std::move(img);
    ++canvas_version;
}

namespace {
inline void Blend(u8* d, float r, float g, float b, float a) {
    const float ia = 1.0f - a;
    d[0] = static_cast<u8>(r * a + d[0] * ia);
    d[1] = static_cast<u8>(g * a + d[1] * ia);
    d[2] = static_cast<u8>(b * a + d[2] * ia);
    d[3] = static_cast<u8>(std::min(255.0f, a * 255.0f + d[3] * ia));
}
} // namespace

void Hud::DrawRect(Image& dst, float x, float y, float w, float h, u32 rgba, float opacity) {
    const int x0 = std::max(0, static_cast<int>(std::lround(x)));
    const int y0 = std::max(0, static_cast<int>(std::lround(y)));
    const int x1 = std::min<int>(dst.width, static_cast<int>(std::lround(x + w)));
    const int y1 = std::min<int>(dst.height, static_cast<int>(std::lround(y + h)));
    const float r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
    const float a = ((rgba & 0xFF) / 255.0f) * opacity;
    for (int yy = y0; yy < y1; ++yy) {
        u8* row = &dst.pixels[(static_cast<size_t>(yy) * dst.width + x0) * 4];
        for (int xx = x0; xx < x1; ++xx, row += 4) {
            Blend(row, r, g, b, a);
        }
    }
}

void Hud::DrawImage(Image& dst, const Image& src, float x, float y, float w, float h,
                    float opacity) {
    const int x0 = std::max(0, static_cast<int>(x));
    const int y0 = std::max(0, static_cast<int>(y));
    const int x1 = std::min<int>(dst.width, static_cast<int>(x + w));
    const int y1 = std::min<int>(dst.height, static_cast<int>(y + h));
    for (int yy = y0; yy < y1; ++yy) {
        const float v = ((yy + 0.5f - y) / h) * src.height - 0.5f;
        const int sy0 = std::clamp(static_cast<int>(std::floor(v)), 0, (int)src.height - 1);
        const int sy1 = std::min(sy0 + 1, (int)src.height - 1);
        const float fy = std::clamp(v - std::floor(v), 0.0f, 1.0f);
        for (int xx = x0; xx < x1; ++xx) {
            const float u = ((xx + 0.5f - x) / w) * src.width - 0.5f;
            const int sx0 = std::clamp(static_cast<int>(std::floor(u)), 0, (int)src.width - 1);
            const int sx1 = std::min(sx0 + 1, (int)src.width - 1);
            const float fx = std::clamp(u - std::floor(u), 0.0f, 1.0f);
            float c[4];
            for (int k = 0; k < 4; ++k) {
                auto px = [&](int sx, int sy) {
                    return src.pixels[(static_cast<size_t>(sy) * src.width + sx) * 4 + k];
                };
                c[k] = (px(sx0, sy0) * (1 - fx) + px(sx1, sy0) * fx) * (1 - fy) +
                       (px(sx0, sy1) * (1 - fx) + px(sx1, sy1) * fx) * fy;
            }
            u8* d = &dst.pixels[(static_cast<size_t>(yy) * dst.width + xx) * 4];
            Blend(d, c[0], c[1], c[2], (c[3] / 255.0f) * opacity);
        }
    }
}

void Hud::DrawText(Image& dst, const std::string& text, float x, float y, float size, int align,
                   u32 rgba, float opacity) {
    const float scale = size / static_cast<float>(font_line);
    float width = 0;
    for (unsigned char ch : text) {
        auto it = glyphs.find(ch);
        if (it != glyphs.end()) {
            width += it->second.advance * scale;
        }
    }
    float pen = align == 1 ? x - width / 2 : align == 2 ? x - width : x;
    const float r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
    const float a = ((rgba & 0xFF) / 255.0f) * opacity;
    for (unsigned char ch : text) {
        auto it = glyphs.find(ch);
        if (it == glyphs.end()) {
            continue;
        }
        const Glyph& gl = it->second;
        const float gx = pen + gl.xoff * scale;
        const float gy = y + gl.yoff * scale;
        const float gw = gl.w * scale, gh = gl.h * scale;
        const int x0 = std::max(0, static_cast<int>(gx));
        const int y0 = std::max(0, static_cast<int>(gy));
        const int x1 = std::min<int>(dst.width, static_cast<int>(std::ceil(gx + gw)));
        const int y1 = std::min<int>(dst.height, static_cast<int>(std::ceil(gy + gh)));
        for (int yy = y0; yy < y1; ++yy) {
            const float v = gl.y + ((yy + 0.5f - gy) / gh) * gl.h - 0.5f;
            const int sy = std::clamp(static_cast<int>(v), 0, (int)font_atlas.height - 2);
            const float fy = std::clamp(v - sy, 0.0f, 1.0f);
            for (int xx = x0; xx < x1; ++xx) {
                const float u = gl.x + ((xx + 0.5f - gx) / gw) * gl.w - 0.5f;
                const int sx = std::clamp(static_cast<int>(u), 0, (int)font_atlas.width - 2);
                const float fx = std::clamp(u - sx, 0.0f, 1.0f);
                auto al = [&](int px, int py) {
                    return font_atlas.pixels[(static_cast<size_t>(py) * font_atlas.width + px) * 4 + 3] / 255.0f;
                };
                const float cov = (al(sx, sy) * (1 - fx) + al(sx + 1, sy) * fx) * (1 - fy) +
                                  (al(sx, sy + 1) * (1 - fx) + al(sx + 1, sy + 1) * fx) * fy;
                if (cov > 0.0f) {
                    u8* d = &dst.pixels[(static_cast<size_t>(yy) * dst.width + xx) * 4];
                    Blend(d, r, g, b, cov * a);
                }
            }
        }
        pen += gl.advance * scale;
    }
}

} // namespace ScreenRegions
