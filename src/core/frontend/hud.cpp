// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <map>
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
    if (!in.empty() && in[0] == '=') {
        std::string error;
        b.constant = false;
        b.is_expr = true;
        if (!b.node.Parse(in.substr(1), error)) {
            LOG_WARNING(Frontend, "HUD expression '{}': {}", in.substr(1), error);
            return false;
        }
        return true;
    }
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

FxLayers& FxLayers::Instance() {
    static FxLayers instance;
    return instance;
}

void FxLayers::Ask(const std::string& name, float x, float y, float w, float h, bool active) {
    std::scoped_lock lock{mutex};
    pending[name] = Request{name, x, y, w, h, active, std::chrono::steady_clock::now()};
}

std::vector<FxLayers::Request> FxLayers::TakeRequests() {
    std::scoped_lock lock{mutex};
    std::vector<Request> out;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending.begin(); it != pending.end();) {
        if (now - it->second.asked > std::chrono::milliseconds(250)) {
            it = pending.erase(it);
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
    return out;
}

void FxLayers::Store(const std::string& name, Image image) {
    std::scoped_lock lock{mutex};
    store[name] = {std::make_shared<const Image>(std::move(image)), ++counter};
}

std::shared_ptr<const Image> FxLayers::Get(const std::string& name) const {
    std::scoped_lock lock{mutex};
    const auto it = store.find(name);
    return it == store.end() ? nullptr : it->second.first;
}

u64 FxLayers::Version(const std::string& name) const {
    std::scoped_lock lock{mutex};
    const auto it = store.find(name);
    return it == store.end() ? 0 : it->second.second;
}

Captures& Captures::Instance() {
    static Captures instance;
    return instance;
}

void Captures::Ask(const std::string& name, float x, float y, float w, float h) {
    std::scoped_lock lock{mutex};
    pending[name] = Request{name, x, y, w, h};
}

std::vector<Captures::Request> Captures::TakeRequests() {
    std::scoped_lock lock{mutex};
    std::vector<Request> out;
    for (auto& [name, r] : pending) {
        out.push_back(r);
    }
    pending.clear();
    return out;
}

void Captures::Store(const std::string& name, Image image) {
    std::scoped_lock lock{mutex};
    store[name] = {std::make_shared<const Image>(std::move(image)), ++counter};
}

std::shared_ptr<const Image> Captures::Get(const std::string& name) const {
    std::scoped_lock lock{mutex};
    const auto it = store.find(name);
    return it == store.end() ? nullptr : it->second.first;
}

u64 Captures::Version(const std::string& name) const {
    std::scoped_lock lock{mutex};
    const auto it = store.find(name);
    return it == store.end() ? 0 : it->second.second;
}

void Captures::Clear() {
    std::scoped_lock lock{mutex};
    pending.clear();
    store.clear();
}

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
namespace {
std::string Format(const std::string& tmpl, const std::vector<Value>& values);
}

Value Binding::Eval(const Expr::Context& ctx) const {
    if (is_expr) {
        return node.Eval(ctx);
    }
    return Value(Read(ctx.system));
}

s64 Binding::Read(Core::System& system) const {
    s64 v = 0;
    if (is_expr) {
        static const std::map<std::string, Value> no_vars;
        static const std::vector<std::string> no_lines;
        Expr::Context ctx{system, no_vars,
                          [](const std::string&) -> const std::vector<std::string>& {
                              return no_lines;
                          }};
        v = node.Eval(ctx).n;
    } else if (probe) {
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

bool Hud::ParseLet(const std::string& line, HudDef& def, std::string& error) {
    const auto eq = line.find('=');
    if (line.rfind("let ", 0) != 0 || eq == std::string::npos) {
        error = "expected let name = expression";
        return false;
    }
    std::string name = line.substr(4, eq - 4);
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](unsigned char c) { return std::isspace(c); }),
               name.end());
    Expr expr;
    if (!expr.Parse(line.substr(eq + 1), error)) {
        return false;
    }
    def.lets.emplace_back(name, std::move(expr));
    return true;
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
        } else if (kind == "capture") {
            e.type = Element::Type::Capture;
            const std::string q = t.at(i++);
            e.image = q[0] == '"' ? q.substr(1) : q;
            e.x = num(); e.y = num(); e.w = num(); e.h = num();
        } else if (kind == "fx") {
            e.type = Element::Type::Fx;
            const std::string q = t.at(i++);
            e.image = q[0] == '"' ? q.substr(1) : q;
            for (float& v : e.src) {
                v = num();
            }
            e.x = num(); e.y = num(); e.w = num(); e.h = num();
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
            } else if (k == "ox" || k == "oy") {
                if (!ParseBinding(v, k == "ox" ? e.ox : e.oy)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
                e.has_offset = true;
            } else if (k == "fade") {
                if (!ParseBinding(v, e.fade)) {
                    error = "bad binding '" + v + "'";
                    return false;
                }
                e.has_fade = true;
            } else if (k == "fit") {
                e.fit = std::stof(v);
            } else if (k == "crop") {
                std::istringstream cs(v);
                std::string part;
                for (int ci = 0; ci < 4 && std::getline(cs, part, ','); ++ci) {
                    e.crop[ci] = std::stof(part);
                }
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
            } else if (k == "linger") {
                e.linger = std::stoi(v);
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

Hud::Hud() : worker([this] { WorkerLoop(); }) {}

Hud::~Hud() {
    {
        std::scoped_lock lock{job_mutex};
        quit = true;
    }
    job_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void Hud::Flush() {
    std::unique_lock lock{job_mutex};
    idle_cv.wait(lock, [this] { return !pending && !busy; });
}

void Hud::WorkerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{job_mutex};
            job_cv.wait(lock, [this] { return quit || pending.has_value(); });
            if (quit) {
                return;
            }
            job = std::move(*pending);
            pending.reset();
            busy = true;
        }
        if (job.asset_dir != worker_asset_dir) {
            worker_asset_dir = job.asset_dir;
            images.clear();
            font_loaded = false;
            glyphs.clear();
        }
        if (job.clear || !job.defs || job.index >= job.defs->size()) {
            std::scoped_lock lock{canvas_mutex};
            canvas.reset();
            ++canvas_version;
        } else {
            Rasterise((*job.defs)[job.index], job.values, job.visible, job.offsets, job.fades);
        }
        {
            std::scoped_lock lock{job_mutex};
            busy = false;
        }
        idle_cv.notify_all();
    }
}

void Hud::SetDefinition(const std::vector<HudDef>& new_defs, const std::string& dir) {
    defs = new_defs;
    defs_shared = std::make_shared<const std::vector<HudDef>>(new_defs);
    asset_dir = dir;
    lookups.clear();
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
            {
                std::scoped_lock lock{job_mutex};
                Job job;
                job.clear = true;
                job.asset_dir = asset_dir;
                pending = std::move(job);
            }
            job_cv.notify_one();
            return true;
        }
        return false;
    }

    std::map<std::string, Value> vars;
    Expr::Context ctx{system, vars,
                      [this](const std::string& file) -> const std::vector<std::string>& {
                          return GetLookup(file);
                      }};
    for (const auto& [name, expr] : def->lets) {
        vars[name] = expr.Eval(ctx);
    }
    std::vector<std::vector<Value>> values(def->elements.size());
    std::vector<bool> visible(def->elements.size());
    offsets.assign(def->elements.size(), {0.0f, 0.0f});
    fades.assign(def->elements.size(), 1.0f);
    for (size_t i = 0; i < def->elements.size(); ++i) {
        const auto& e = def->elements[i];
        if (e.type == Element::Type::Fx) {
            const bool armed = e.visible.Eval(ctx).Truthy();
            const bool active =
                armed && (e.visible2.constant ? true : e.visible2.Eval(ctx).Truthy());
            visible[i] = active;
            if (armed) {
                FxLayers::Instance().Ask(e.image, e.src[0], e.src[1], e.src[2], e.src[3], active);
            }
            if (active) {
                values[i].push_back(Value(static_cast<s64>(FxLayers::Instance().Version(e.image))));
            }
            continue;
        }
        const bool can_hold = active_profile == last_profile && i < last_visible.size();
        if (can_hold && e.hold.Eval(ctx).Truthy()) {
            visible[i] = last_visible[i];
        } else {
            visible[i] = e.visible.Eval(ctx).Truthy() &&
                         (e.visible2.constant ? true : e.visible2.Eval(ctx).Truthy());
        }
        if (last_true.size() != def->elements.size()) {
            last_true.assign(def->elements.size(), {});
        }
        const auto now = std::chrono::steady_clock::now();
        if (visible[i]) {
            last_true[i] = now;
        } else if (e.linger > 0 && last_true[i].time_since_epoch().count() != 0 &&
                   now - last_true[i] < std::chrono::milliseconds(e.linger)) {
            visible[i] = true; // bridge short drops (state changes between menus)
        }
        if (!visible[i]) {
            continue;
        }
        if (e.has_offset) {
            offsets[i] = {static_cast<float>(e.ox.Eval(ctx).n), static_cast<float>(e.oy.Eval(ctx).n)};
        }
        if (e.has_fade) {
            fades[i] = std::clamp(static_cast<float>(e.fade.Eval(ctx).n) / 100.0f, 0.0f, 1.0f);
        }
        for (const auto& b : e.values) {
            values[i].push_back(b.Eval(ctx));
        }
        if (e.type == Element::Type::Capture) {
            Captures::Instance().Ask(Format(e.image, values[i]), e.x, e.y, e.w, e.h);
            values[i].clear(); // requests don't change the canvas by themselves
        } else if (e.type == Element::Type::Image && !e.image.empty() && e.image[0] == '@') {
            const std::string name = Format(e.image.substr(1), values[i]);
            values[i].push_back(Value(static_cast<s64>(Captures::Instance().Version(name))));
            PersistCapture(name);
        }
    }
    if (active_profile == last_profile && values == last_values && visible == last_visible &&
        offsets == last_offsets && fades == last_fades) {
        return false;
    }
    last_offsets = offsets;
    last_fades = fades;
    last_profile = active_profile;
    last_values = values;
    last_visible = visible;
    {
        std::scoped_lock lock{job_mutex};
        Job job;
        job.defs = defs_shared;
        job.index = static_cast<size_t>(def - defs.data());
        job.values = std::move(values);
        job.visible = std::move(visible);
        job.offsets = offsets;
        job.fades = fades;
        job.asset_dir = asset_dir;
        pending = std::move(job); // only the newest state matters
    }
    job_cv.notify_one();
    return true;
}

void Hud::PersistCapture(const std::string& name) {
    const u64 version = Captures::Instance().Version(name);
    if (version == 0) {
        return;
    }
    auto& st = cache_state[name];
    if (st.version != version) {
        st = {version, 0, false};
        return;
    }
    // save once the capture has been stable for a while (skips mid-animation frames)
    if (st.saved || ++st.seen < 90) {
        return;
    }
    st.saved = true;
    const auto cap = Captures::Instance().Get(name);
    if (!cap || !cap->width) {
        return;
    }
    const std::string dir = asset_dir + "cache/";
    FileUtil::CreateFullPath(dir);
    Frontend::ImageInterface encoder;
    if (encoder.EncodePNG(dir + name + ".png", cap->width, cap->height, cap->pixels)) {
        // the rasteriser reloads it the next time the asset directory changes
    }
}

std::shared_ptr<const Image> Hud::Canvas(u64& version) const {
    std::scoped_lock lock{canvas_mutex};
    version = canvas_version;
    return canvas;
}

namespace {
std::string Format(const std::string& tmpl, const std::vector<Value>& values) {
    std::string s = tmpl;
    for (size_t v = 0; v < values.size(); ++v) {
        const std::string key = "{" + std::to_string(v) + "}";
        const std::string repl = values[v].Text();
        size_t p = 0;
        while ((p = s.find(key, p)) != std::string::npos) {
            s.replace(p, key.size(), repl);
            p += repl.size();
        }
    }
    return s;
}
} // namespace

void Hud::Rasterise(const HudDef& def, const std::vector<std::vector<Value>>& values,
                    const std::vector<bool>& visible,
                    const std::vector<std::pair<float, float>>& offs,
                    const std::vector<float>& fades_in) {
    auto img = std::make_shared<Image>();
    img->width = CanvasWidth;
    img->height = CanvasHeight;
    img->pixels.assign(static_cast<size_t>(CanvasWidth) * CanvasHeight * 4, 0);

    for (size_t i = 0; i < def.elements.size(); ++i) {
        if (!visible[i]) {
            continue;
        }
        Element e = def.elements[i];
        if (i < offs.size()) {
            e.x += offs[i].first;
            e.y += offs[i].second;
        }
        if (i < fades_in.size()) {
            e.opacity *= fades_in[i];
            if (e.opacity <= 0.0f) {
                continue;
            }
        }
        switch (e.type) {
        case Element::Type::Rect:
            DrawRect(*img, e.x, e.y, e.w, e.h, e.color, e.opacity);
            break;
        case Element::Type::Image: {
            if (!e.image.empty() && e.image[0] == '@') {
                const std::string name = Format(e.image.substr(1), values[i]);
                const auto cap = Captures::Instance().Get(name);
                if (cap && cap->width) {
                    DrawImage(*img, *cap, e.x, e.y, e.w, e.h, e.opacity);
                } else if (const Image* disk = GetImage("cache/" + name + ".png")) {
                    // captured in an earlier session
                    DrawImage(*img, *disk, e.x, e.y, e.w, e.h, e.opacity);
                }
                break;
            }
            const std::string file =
                e.image.find('{') != std::string::npos ? Format(e.image, values[i]) : e.image;
            if (const Image* src = GetImage(file)) {
                DrawImage(*img, *src, e.x, e.y, e.w, e.h, e.opacity, e.crop);
            }
            break;
        }
        case Element::Type::Capture:
            break;
        case Element::Type::Fx:
            if (const auto fx = FxLayers::Instance().Get(e.image); fx && fx->width) {
                DrawImage(*img, *fx, e.x, e.y, e.w, e.h, e.opacity);
            }
            break;
        case Element::Type::Bar: {
            DrawRect(*img, e.x, e.y, e.w, e.h, e.color2, e.opacity);
            if (values[i].size() >= 2 && values[i][1].n > 0) {
                const float f = std::clamp(static_cast<float>(values[i][0].n) /
                                               static_cast<float>(values[i][1].n),
                                           0.0f, 1.0f);
                DrawRect(*img, e.x, e.y, e.w * f, e.h, e.color, e.opacity);
            }
            break;
        }
        case Element::Type::Text: {
            std::vector<Value> vals = values[i];
            if (!vals.empty() && !e.lookup.empty() && !vals[0].is_str) {
                const auto& list = GetLookup(e.lookup);
                const s64 idx = vals[0].n;
                vals[0] = Value(idx >= 0 && idx < static_cast<s64>(list.size())
                                    ? list[static_cast<size_t>(idx)]
                                    : std::string{});
            }
            const std::string s = Format(e.text, vals);
            float size = e.size;
            if (e.fit > 0 && LoadFont()) {
                const float w = TextWidth(s, size);
                if (w > e.fit) {
                    size *= e.fit / w;
                }
            }
            if (LoadFont()) {
                // keep the baseline when shrinking to fit
                const float dy = (e.size - size) * 0.8f;
                DrawText(*img, s, e.x, e.y + dy, size, e.align, e.color, e.opacity);
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
                    float opacity, const float* crop) {
    static constexpr float full[4] = {0, 0, 1, 1};
    const float* cr = crop ? crop : full;
    const int x0 = std::max(0, static_cast<int>(x));
    const int y0 = std::max(0, static_cast<int>(y));
    const int x1 = std::min<int>(dst.width, static_cast<int>(x + w));
    const int y1 = std::min<int>(dst.height, static_cast<int>(y + h));
    for (int yy = y0; yy < y1; ++yy) {
        const float v = (cr[1] + ((yy + 0.5f - y) / h) * cr[3]) * src.height - 0.5f;
        const int sy0 = std::clamp(static_cast<int>(std::floor(v)), 0, (int)src.height - 1);
        const int sy1 = std::min(sy0 + 1, (int)src.height - 1);
        const float fy = std::clamp(v - std::floor(v), 0.0f, 1.0f);
        for (int xx = x0; xx < x1; ++xx) {
            const float u = (cr[0] + ((xx + 0.5f - x) / w) * cr[2]) * src.width - 0.5f;
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

float Hud::TextWidth(const std::string& text, float size) {
    const float scale = size / static_cast<float>(font_line);
    float width = 0;
    for (unsigned char ch : text) {
        auto it = glyphs.find(ch);
        if (it != glyphs.end()) {
            width += it->second.advance * scale;
        }
    }
    return width;
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
