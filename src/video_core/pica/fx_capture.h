// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {

struct RegsInternal;
struct OutputVertex;

/**
 * CPU copy of the bottom screen's effect draws for the single-screen HUD
 * (see ScreenRegions::FxLayer): menus are learned while the player chooses commands, and
 * everything else drawn on the bottom screen during actions is rasterised into a transparent layer.
 */
class FxCapture {
public:
    explicit FxCapture(Memory::MemorySystem& memory);

    /// True when the current draw targets the bottom screen and should reach Triangle() even if
    /// it could be drawn with hardware shaders.
    bool WantsVertices(const RegsInternal& regs);

    /// One assembled triangle (post vertex/geometry shader).
    void Triangle(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                  const OutputVertex& v2);

    /// A display transfer of the bottom screen happened: publish and restart the layer.
    void EndBottomFrame();

private:
    static bool IsBottom(const RegsInternal& regs);

    Memory::MemorySystem& memory;
    std::vector<float> accum; ///< 240x320 premultiplied RGBA in framebuffer space
    bool any = false;
    bool was_capturing = false;
};

} // namespace Pica
