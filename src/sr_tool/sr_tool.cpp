#include <chrono>
// Headless research tool for Screen Regions: boots a title with the software renderer,
// loads a save state and runs a script (button presses, frame steps, RAM/screen dumps).
//
// usage: sr_tool <user_dir> <app.cxi> < script
//   load <slot>            load save state <user_dir>/states/<tid>.<slot>.cst
//   run <frames>           run N frames
//   hold <btn[+btn..]> <n> hold buttons for n frames, then release
//   tap <btn> [wait]       press 3 frames, then run `wait` frames (default 20)
//   touch <x> <y> <n>      touch bottom screen for n frames
//   dump <file>            write RAM segments + both screens
//   w8|w16|w32 <addr> <v>  poke guest memory
//   quit

#include <cstring>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/thread.h"
#include "core/arm/arm_interface.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/hud.h"
#include "core/frontend/input.h"
#include <fstream>
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/service/service.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/renderer_software.h"

namespace {

std::atomic<u32> g_buttons{0};
std::atomic<bool> g_touch{false};
std::atomic<float> g_tx{0}, g_ty{0};

class ScriptButton final : public Input::ButtonDevice {
public:
    explicit ScriptButton(int code_) : code(code_) {}
    bool GetStatus() const override {
        return (g_buttons.load() >> code) & 1;
    }

private:
    int code;
};

class ScriptButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<ScriptButton>(params.Get("code", 0));
    }
};

class ScriptTouch final : public Input::TouchDevice {
public:
    std::tuple<float, float, bool> GetStatus() const override {
        return {g_tx.load(), g_ty.load(), g_touch.load()};
    }
};

class ScriptTouchFactory final : public Input::Factory<Input::TouchDevice> {
public:
    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<ScriptTouch>();
    }
};

class HeadlessWindow final : public Frontend::EmuWindow {
public:
    HeadlessWindow() {
        // Report a 1x layout so the frontend code paths are happy.
        UpdateCurrentFramebufferLayout(400, 480);
    }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
};

int ButtonCode(const std::string& name) {
    static const std::vector<std::pair<std::string, int>> map{
        {"a", Settings::NativeButton::A},         {"b", Settings::NativeButton::B},
        {"x", Settings::NativeButton::X},         {"y", Settings::NativeButton::Y},
        {"up", Settings::NativeButton::Up},       {"down", Settings::NativeButton::Down},
        {"left", Settings::NativeButton::Left},   {"right", Settings::NativeButton::Right},
        {"l", Settings::NativeButton::L},         {"r", Settings::NativeButton::R},
        {"start", Settings::NativeButton::Start}, {"select", Settings::NativeButton::Select},
        {"zl", Settings::NativeButton::ZL},       {"zr", Settings::NativeButton::ZR},
    };
    for (const auto& [n, c] : map) {
        if (n == name) {
            return c;
        }
    }
    return -1;
}

u32 ParseButtons(const std::string& s) {
    u32 mask = 0;
    std::stringstream ss(s);
    for (std::string b; std::getline(ss, b, '+');) {
        const int c = ButtonCode(b);
        if (c >= 0) {
            mask |= 1u << c;
        } else {
            std::fprintf(stderr, "unknown button %s\n", b.c_str());
        }
    }
    return mask;
}

void RunFrames(Core::System& system, u64 frames) {
    auto& renderer = system.GPU().Renderer();
    for (u64 i = 0; i < frames; ++i) {
        const u64 start = renderer.GetCurrentFrame();
        int guard = 0;
        while (renderer.GetCurrentFrame() == start && guard++ < 100000) {
            const auto result = system.RunLoop();
            if (result != Core::System::ResultStatus::Success) {
                std::fprintf(stderr, "RunLoop status %d\n", static_cast<int>(result));
                return;
            }
        }
    }
}

void PutScreen(std::vector<u8>& out, const SwRenderer::ScreenInfo& info) {
    // Software screen buffers are stored rotated (column-major); emit row-major RGB.
    const u32 w = info.height, h = info.width; // displayed width/height
    auto put32 = [&out](u32 v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    put32(w);
    put32(h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const size_t src = (static_cast<size_t>(y) * info.height + x) * 4;
            if (src + 3 < info.pixels.size()) {
                out.push_back(info.pixels[src + 0]);
                out.push_back(info.pixels[src + 1]);
                out.push_back(info.pixels[src + 2]);
            } else {
                out.insert(out.end(), {0, 0, 0});
            }
        }
    }
}

void Dump(Core::System& system, const std::string& path) {
    auto process = system.Kernel().GetCurrentProcess();
    auto& memory = system.Memory();
    std::vector<u8> out;
    auto put32 = [&out](u32 v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    const char magic[8] = {'S', 'R', 'R', 'A', 'W', '1', 0, 0};
    out.insert(out.end(), magic, magic + 8);
    put32(static_cast<u32>(system.GPU().Renderer().GetCurrentFrame()));
    char pname[16] = "tool";
    out.insert(out.end(), pname, pname + 16);
    std::vector<std::pair<u32, u32>> ranges{{0x00568000, 0xE5000}};
    if (process) {
        for (const auto& [vaddr, vma] : process->vm_manager.vma_map) {
            if (vma.type == Kernel::VMAType::BackingMemory && vma.base >= 0x08000000 &&
                vma.base < 0x10000000) {
                ranges.emplace_back(vma.base, vma.size);
            }
        }
    }
    ranges.emplace_back(0x30000000, 0x3000000);
    ranges.emplace_back(0x1F000000, 0x600000); // VRAM
    put32(static_cast<u32>(ranges.size()));
    for (const auto& [va, size] : ranges) {
        put32(va);
        put32(size);
        const size_t at = out.size();
        out.resize(at + size);
        if (process && memory.IsValidVirtualAddress(*process, va)) {
            memory.ReadBlock(*process, va, out.data() + at, size);
        }
    }
    auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
    if (sw) {
        PutScreen(out, sw->Screen(VideoCore::ScreenId::Bottom));
        PutScreen(out, sw->Screen(VideoCore::ScreenId::TopLeft));
    }
    FileUtil::IOFile f(path, "wb");
    f.WriteBytes(out.data(), out.size());
    std::printf("dumped %s (%zu bytes)\n", path.c_str(), out.size());
    std::fflush(stdout);
}


std::string g_rec_bottom; // 320x240 RGB from a loaded recording
void ServiceCaptures(Core::System& system) {
    auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
    if (!sw) {
        return;
    }
    const auto& info = sw->Screen(VideoCore::ScreenId::Bottom);
    for (const auto& r : ScreenRegions::Captures::Instance().TakeRequests()) {
        ScreenRegions::Image img;
        img.width = static_cast<u32>(r.w);
        img.height = static_cast<u32>(r.h);
        img.pixels.resize(static_cast<size_t>(img.width) * img.height * 4);
        for (u32 y = 0; y < img.height; ++y) {
            for (u32 x = 0; x < img.width; ++x) {
                const u32 sx = static_cast<u32>(r.x) + x, sy = static_cast<u32>(r.y) + y;
                const size_t src = (static_cast<size_t>(sy) * info.height + sx) * 4;
                u8* d = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
                if (src + 3 < info.pixels.size()) {
                    d[0] = info.pixels[src];
                    d[1] = info.pixels[src + 1];
                    d[2] = info.pixels[src + 2];
                }
                d[3] = 255;
            }
        }
        ScreenRegions::Captures::Instance().Store(r.name, std::move(img));
    }
}

/// hud <ini> <assets_dir> <hud_name> <out.rgba>: render a [hud NAME] section over the current state
ScreenRegions::Hud g_hud;

void RenderHud(Core::System& system, const std::string& ini, const std::string& assets,
               const std::string& name, const std::string& out) {
    std::ifstream in(ini);
    std::string line;
    bool inside = false;
    ScreenRegions::HudDef def;
    def.name = name;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        line = line.substr(first);
        if (line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            inside = line == "[hud " + name + "]";
            continue;
        }
        if (!inside) {
            continue;
        }
        std::string error;
        if (line.rfind("let ", 0) == 0) {
            if (!ScreenRegions::Hud::ParseLet(line, def, error)) {
                std::fprintf(stderr, "let error: %s: %s\n", line.c_str(), error.c_str());
            }
            continue;
        }
        ScreenRegions::Element e;
        if (ScreenRegions::Hud::ParseElement(line, e, error)) {
            def.elements.push_back(std::move(e));
        } else {
            std::fprintf(stderr, "element error: %s: %s\n", line.c_str(), error.c_str());
        }
    }
    auto& hud = g_hud;
    hud.SetDefinition({def}, assets + "/");
    hud.Update(system, name);
    {
        // answer pixel probes from the recording (or the software bottom screen)
        auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
        for (const auto& [x, y] : ScreenRegions::Probes::Instance().Requests()) {
            u32 rgb = 0;
            if (g_rec_bottom.size() >= 320 * 240 * 3) {
                const u8* q = reinterpret_cast<const u8*>(g_rec_bottom.data()) + (y * 320 + x) * 3;
                rgb = (u32{q[0]} << 16) | (u32{q[1]} << 8) | q[2];
            } else if (sw) {
                const auto& info = sw->Screen(VideoCore::ScreenId::Bottom);
                const size_t src = (static_cast<size_t>(y) * info.height + x) * 4;
                if (src + 2 < info.pixels.size()) {
                    rgb = (u32{info.pixels[src]} << 16) | (u32{info.pixels[src + 1]} << 8) |
                          info.pixels[src + 2];
                }
            }
            ScreenRegions::Probes::Instance().Set(x, y, rgb);
        }
    }
    hud.Update(system, name);
    ServiceCaptures(system);
    hud.Update(system, name);
    hud.Flush();
    u64 version = 0;
    const auto canvas = hud.Canvas(version);
    FileUtil::IOFile f(out, "wb");
    if (canvas) {
        f.WriteBytes(canvas->pixels.data(), canvas->pixels.size());
    }
    std::printf("hud %zu elements -> %s\n", def.elements.size(), out.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: sr_tool <user_dir> <app> < script\n");
        return 1;
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    {
        Common::Log::Filter filter;
        filter.ParseFilterString(std::getenv("SR_LOG") ? std::getenv("SR_LOG")
                                                       : "*:Info HW.Memory:Critical");
        Common::Log::SetGlobalFilter(filter);
    }
    FileUtil::SetUserPath(std::string(argv[1]) + "/");

    Settings::values.graphics_api = Settings::GraphicsAPI::Software;
    Settings::values.use_cpu_jit = std::getenv("SR_INTERP") == nullptr;
    Settings::values.frame_limit = 0;
    Settings::values.audio_emulation = Settings::AudioEmulation::HLE;
    Input::RegisterFactory<Input::ButtonDevice>("sr", std::make_shared<ScriptButtonFactory>());
    Input::RegisterFactory<Input::TouchDevice>("sr", std::make_shared<ScriptTouchFactory>());
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        Settings::values.current_input_profile.buttons[i] =
            "engine:sr,code:" + std::to_string(i);
    }
    Settings::values.current_input_profile.touch_device = "engine:sr";
    for (const auto& module : Service::service_module_map) {
        Settings::values.lle_modules[module.name] = false;
    }

    HeadlessWindow window;
    auto& system = Core::System::GetInstance();
    const auto status = system.Load(window, argv[2]);
    if (status != Core::System::ResultStatus::Success) {
        std::fprintf(stderr, "Load failed: %d\n", static_cast<int>(status));
        return 2;
    }
    std::printf("loaded\n");
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        try {
            if (cmd.empty() || cmd[0] == '#') {
                continue;
            } else if (cmd == "load") {
                u32 slot = 1;
                in >> slot;
                system.LoadState(slot);
                std::printf("state %u loaded\n", slot);
            } else if (cmd == "save") {
                u32 slot = 3;
                in >> slot;
                system.SaveState(slot);
                std::printf("state %u saved\n", slot);
            } else if (cmd == "pc") {
                auto& core = system.GetRunningCore();
                std::printf("pc=%08x lr=%08x sp=%08x frame=%llu ticks=%llu\n", core.GetPC(),
                            core.GetReg(14), core.GetReg(13),
                            static_cast<unsigned long long>(system.GPU().Renderer().GetCurrentFrame()),
                            static_cast<unsigned long long>(system.CoreTiming().GetTicks()));
            } else if (cmd == "mem") {
                // mem <addr> <size> <path>: raw guest memory to a host file
                std::string a, n, path;
                in >> a >> n >> path;
                const u32 addr = static_cast<u32>(std::stoul(a, nullptr, 0));
                const u32 size = static_cast<u32>(std::stoul(n, nullptr, 0));
                std::vector<u8> buf(size);
                system.Memory().ReadBlock(*system.Kernel().GetCurrentProcess(), addr, buf.data(), size);
                FileUtil::IOFile f(path, "wb");
                f.WriteBytes(buf.data(), buf.size());
            } else if (cmd == "hudbench") {
                // hudbench <n>: after a 'hud' command, run n frames timing Update (eval+raster)
                u64 n = 60;
                std::string prof;
                in >> prof >> n;
                double total = 0, worst = 0;
                int rasters = 0;
                for (u64 k = 0; k < n; ++k) {
                    RunFrames(system, 1);
                    const auto t0 = std::chrono::steady_clock::now();
                    const bool changed = g_hud.Update(system, prof);
                    const double ms = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count();
                    total += ms;
                    worst = std::max(worst, ms);
                    rasters += changed;
                }
                std::printf("hudbench %llu frames: avg %.2f ms, worst %.2f ms, %d rasters\n",
                            static_cast<unsigned long long>(n), total / n, worst, rasters);
            } else if (cmd == "threads") {
                for (u32 c = 0; c < 4; ++c) {
                    for (const auto& t : system.Kernel().GetThreadManager(c).GetThreadList()) {
                        std::printf("core%u tid=%u status=%d pc=%08x lr=%08x prio=%u r0=%08x r4=%08x sp=%08x\n", c,
                                    t->thread_id, static_cast<int>(t->status),
                                    t->context.cpu_registers[15], t->context.cpu_registers[14],
                                    t->current_priority, t->context.cpu_registers[0], t->context.cpu_registers[4], t->context.cpu_registers[13]);
                    }
                }
            } else if (cmd == "run") {
                u64 n = 1;
                in >> n;
                RunFrames(system, n);
            } else if (cmd == "hold") {
                std::string b;
                u64 n = 3;
                in >> b >> n;
                g_buttons = ParseButtons(b);
                RunFrames(system, n);
                g_buttons = 0;
            } else if (cmd == "tap") {
                std::string b;
                u64 wait = 20;
                in >> b >> wait;
                g_buttons = ParseButtons(b);
                RunFrames(system, 3);
                g_buttons = 0;
                RunFrames(system, wait);
            } else if (cmd == "touch") {
                float x = 0, y = 0;
                u64 n = 3;
                in >> x >> y >> n;
                g_tx = x / 320.0f;
                g_ty = y / 240.0f;
                g_touch = true;
                RunFrames(system, n);
                g_touch = false;
            } else if (cmd == "dump") {
                std::string path;
                in >> path;
                Dump(system, path);
            } else if (cmd == "film") {
                // film <prefix> <count> <step>: every <step> frames write both screens plus the
                // battle object block and party units (small, for watching whole turns)
                std::string prefix;
                u64 count = 0, step = 1;
                in >> prefix >> count >> step;
                auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
                auto process = system.Kernel().GetCurrentProcess();
                for (u64 k = 0; k < count; ++k) {
                    RunFrames(system, step);
                    std::vector<u8> out;
                    if (sw) {
                        PutScreen(out, sw->Screen(VideoCore::ScreenId::Bottom));
                        PutScreen(out, sw->Screen(VideoCore::ScreenId::TopLeft));
                    }
                    auto block = [&](u32 va, u32 size) {
                        const size_t at = out.size();
                        out.resize(at + size);
                        if (process && system.Memory().IsValidVirtualAddress(*process, va)) {
                            system.Memory().ReadBlock(*process, va, out.data() + at, size);
                        }
                    };
                    block(0x08650000, 0x12000);
                    block(0x0057113C, 4);
                    block(0x085F8E66, 4 * 0x408);
                    FileUtil::IOFile f(fmt::format("{}{:04}.bin", prefix, k), "wb");
                    f.WriteBytes(out.data(), out.size());
                }
                std::printf("filmed %llu\n", static_cast<unsigned long long>(count));
                std::fflush(stdout);
            } else if (cmd == "w8" || cmd == "w16" || cmd == "w32") {
                std::string a, v;
                in >> a >> v;
                const u32 addr = static_cast<u32>(std::stoul(a, nullptr, 0));
                const u32 val = static_cast<u32>(std::stoul(v, nullptr, 0));
                auto& m = system.Memory();
                if (cmd == "w8")
                    m.Write8(addr, static_cast<u8>(val));
                else if (cmd == "w16")
                    m.Write16(addr, static_cast<u16>(val));
                else
                    m.Write32(addr, val);
            } else if (cmd == "watch") {
                // watch <addr> <size>: log PC/LR of every read/write in [addr, addr+size)
                std::string a, n;
                in >> a >> n;
                const u32 lo = static_cast<u32>(std::stoul(a, nullptr, 0));
                const u32 hi = lo + static_cast<u32>(std::stoul(n, nullptr, 0));
                system.Memory().RegisterWatchpoint(*system.Kernel().GetCurrentProcess(), lo,
                                                   hi - lo);
                Memory::g_watch_hook = [&system, lo, hi](u32 addr, u32 size, bool write) {
                    if (addr + size <= lo || addr >= hi) {
                        return;
                    }
                    auto& core = system.GetRunningCore();
                    std::fprintf(stderr, "WATCH %c %08x %u pc=%08x lr=%08x r0=%08x r1=%08x\n",
                                 write ? 'W' : 'R', addr, size, core.GetPC(), core.GetReg(14),
                                 core.GetReg(0), core.GetReg(1));
                };
            } else if (cmd == "wfile") {
                // wfile <addr> <path>: copy a host file into guest memory
                std::string a, path;
                in >> a >> path;
                const u32 addr = static_cast<u32>(std::stoul(a, nullptr, 0));
                std::string data;
                FileUtil::ReadFileToString(false, path, data);
                system.Memory().WriteBlock(addr, data.data(), data.size());
                std::printf("wrote %zu bytes at %08x\n", data.size(), addr);
            } else if (cmd == "wrec") {
                // wrec <raw recording>: an uncompressed SRREC1 file; writes its memory ranges
                std::string path;
                in >> path;
                std::string d;
                FileUtil::ReadFileToString(false, path, d);
                size_t p = 28;
                auto rd = [&](size_t at) {
                    u32 v;
                    std::memcpy(&v, d.data() + at, 4);
                    return v;
                };
                const u32 n = rd(p);
                p += 4;
                for (u32 i = 0; i < n; ++i) {
                    const u32 va = rd(p), size = rd(p + 4);
                    p += 8;
                    system.Memory().WriteBlock(va, d.data() + p, size);
                    p += size;
                }
                g_rec_bottom.assign(d.begin() + p + 8, d.end());
                std::printf("recording loaded (%u ranges)\n", n);
            } else if (cmd == "hud") {
                std::string ini, assets, name, out;
                in >> ini >> assets >> name >> out;
                RenderHud(system, ini, assets, name, out);
            } else if (cmd == "quit") {
                break;
            } else {
                std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error in '%s': %s\n", line.c_str(), e.what());
        }
        std::printf("ok %s\n", cmd.c_str());
        std::fflush(stdout);
    }
    system.Shutdown();
    return 0;
}
