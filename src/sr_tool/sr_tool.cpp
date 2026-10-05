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

#include <atomic>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: sr_tool <user_dir> <app> < script\n");
        return 1;
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    FileUtil::SetUserPath(std::string(argv[1]) + "/");

    Settings::values.graphics_api = Settings::GraphicsAPI::Software;
    Settings::values.use_cpu_jit = true;
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
            } else if (cmd == "wfile") {
                // wfile <addr> <path>: copy a host file into guest memory
                std::string a, path;
                in >> a >> path;
                const u32 addr = static_cast<u32>(std::stoul(a, nullptr, 0));
                std::string data;
                FileUtil::ReadFileToString(false, path, data);
                system.Memory().WriteBlock(addr, data.data(), data.size());
                std::printf("wrote %zu bytes at %08x\n", data.size(), addr);
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
