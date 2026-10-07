// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include "core/frontend/hud.h"
#include "core/memory.h"
#include "video_core/pica/fx_capture.h"
#include "video_core/pica/output_vertex.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/texture/texture_decode.h"

namespace Pica {

namespace {
constexpr int FbW = 240; ///< bottom screen framebuffer: 240 wide, 320 high (rotated LCD)
constexpr int FbH = 320;

using BlendFactor = FramebufferRegs::BlendFactor;
using BlendEquation = FramebufferRegs::BlendEquation;

u64 TextureKey(const TexturingRegs::FullTextureConfig& t) {
    return (static_cast<u64>(t.config.GetPhysicalAddress()) << 32) |
           (static_cast<u64>(t.config.width.Value()) << 20) |
           (static_cast<u64>(t.config.height.Value()) << 8) | static_cast<u64>(t.format);
}

int Wrap(TexturingRegs::TextureConfig::WrapMode mode, int v, int size) {
    switch (mode) {
    case TexturingRegs::TextureConfig::Repeat:
        v %= size;
        return v < 0 ? v + size : v;
    case TexturingRegs::TextureConfig::MirroredRepeat: {
        int p = v % (2 * size);
        if (p < 0)
            p += 2 * size;
        return p < size ? p : 2 * size - 1 - p;
    }
    default:
        return std::clamp(v, 0, size - 1);
    }
}
} // namespace

FxCapture::FxCapture(Memory::MemorySystem& memory_) : memory{memory_} {}

bool FxCapture::IsBottom(const RegsInternal& regs) {
    const auto& fb = regs.framebuffer.framebuffer;
    return fb.width.Value() == FbW && fb.height.Value() == FbH - 1;
}

bool FxCapture::WantsVertices(const RegsInternal& regs) {
    auto& layer = ScreenRegions::FxLayer::Instance();
    return (layer.Learning() || layer.Capturing()) && IsBottom(regs);
}

void FxCapture::Triangle(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                         const OutputVertex& v2) {
    auto& layer = ScreenRegions::FxLayer::Instance();
    const bool learning = layer.Learning();
    const bool capturing = layer.Capturing();
    if ((!learning && !capturing) || !IsBottom(regs)) {
        return;
    }
    const auto textures = regs.texturing.GetTextures();
    const auto& tex = textures[0];
    if (!tex.enabled || tex.config.type != TexturingRegs::TextureConfig::Texture2D) {
        return; // untextured fills are menu panels
    }
    const u64 key = TextureKey(tex);
    if (learning) {
        layer.LearnTexture(key);
        return;
    }
    if (layer.IsUiTexture(key)) {
        return;
    }

    const auto& om = regs.framebuffer.output_merger;
    const auto& ab = om.alpha_blending;
    const bool blend = om.alphablend_enable.Value() != 0;
    const BlendFactor src_f = blend ? ab.factor_source_rgb.Value() : BlendFactor::One;
    const BlendFactor dst_f = blend ? ab.factor_dest_rgb.Value() : BlendFactor::Zero;
    const BlendEquation eq = blend ? ab.blend_equation_rgb.Value() : BlendEquation::Add;
    const bool additive = dst_f == BlendFactor::One;
    const int tw = static_cast<int>(tex.config.width.Value());
    const int th = static_cast<int>(tex.config.height.Value());
    // A big picture blended normally is a portrait or a backdrop (e.g. the partner popping up),
    // not an effect: leave it out.
    if (!additive && tw * th >= 256 * 256) {
        return;
    }
    const u8* texture_data = memory.GetPhysicalPointer(tex.config.GetPhysicalAddress());
    if (!texture_data) {
        return;
    }
    const auto info = Texture::TextureInfo::FromPicaRegister(tex.config, tex.format);

    // Viewport transform (as the software rasteriser does).
    const float hx = f24::FromRaw(regs.rasterizer.viewport_size_x).ToFloat32();
    const float hy = f24::FromRaw(regs.rasterizer.viewport_size_y).ToFloat32();
    const float ox = static_cast<float>(regs.rasterizer.viewport_corner.x);
    const float oy = static_cast<float>(regs.rasterizer.viewport_corner.y);
    struct V {
        float x, y, u, v, r, g, b, a;
    };
    auto conv = [&](const OutputVertex& o) {
        const float w = o.pos.w.ToFloat32();
        const float iw = w != 0.0f ? 1.0f / w : 1.0f;
        return V{(o.pos.x.ToFloat32() * iw + 1.0f) * hx + ox,
                 (o.pos.y.ToFloat32() * iw + 1.0f) * hy + oy,
                 o.tc0.u().ToFloat32(),
                 o.tc0.v().ToFloat32(),
                 std::clamp(o.color.r().ToFloat32(), 0.0f, 1.0f),
                 std::clamp(o.color.g().ToFloat32(), 0.0f, 1.0f),
                 std::clamp(o.color.b().ToFloat32(), 0.0f, 1.0f),
                 std::clamp(o.color.a().ToFloat32(), 0.0f, 1.0f)};
    };
    const V a = conv(v0), b = conv(v1), c = conv(v2);
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) {
        return;
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(FbW - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(FbH - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (x0 > x1 || y0 > y1) {
        return;
    }
    if (accum.empty()) {
        accum.assign(static_cast<size_t>(FbW) * FbH * 4, 0.0f);
    }
    const float inv_area = 1.0f / area;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv_area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            const float u = a.u * w0 + b.u * w1 + c.u * w2;
            const float v = a.v * w0 + b.v * w1 + c.v * w2;
            int s = static_cast<int>(std::floor(u * tw));
            int t = static_cast<int>(std::floor(v * th));
            s = Wrap(tex.config.wrap_s, s, tw);
            t = th - 1 - Wrap(tex.config.wrap_t, t, th);
            const auto texel = Texture::LookupTexture(texture_data, s, t, info);
            const float sr = texel.r() / 255.0f * (a.r * w0 + b.r * w1 + c.r * w2);
            const float sg = texel.g() / 255.0f * (a.g * w0 + b.g * w1 + c.g * w2);
            const float sb = texel.b() / 255.0f * (a.b * w0 + b.b * w1 + c.b * w2);
            const float sa = texel.a() / 255.0f * (a.a * w0 + b.a * w1 + c.a * w2);
            if (sa <= 0.0f && !(additive && src_f == BlendFactor::One)) {
                continue;
            }
            const float k = (src_f == BlendFactor::One) ? 1.0f : sa; // source weight
            float* d = &accum[(static_cast<size_t>(y) * FbW + x) * 4];
            if (eq == BlendEquation::ReverseSubtract || eq == BlendEquation::Subtract) {
                // darkening effect: black with the effect's strength as coverage
                const float dk = std::clamp((sr * 0.3f + sg * 0.59f + sb * 0.11f) * k, 0.0f, 1.0f);
                d[0] *= 1.0f - dk;
                d[1] *= 1.0f - dk;
                d[2] *= 1.0f - dk;
                d[3] = dk + d[3] * (1.0f - dk);
            } else if (additive) {
                d[0] = std::min(1.0f, d[0] + sr * k);
                d[1] = std::min(1.0f, d[1] + sg * k);
                d[2] = std::min(1.0f, d[2] + sb * k);
            } else {
                d[0] = sr * sa + d[0] * (1.0f - sa);
                d[1] = sg * sa + d[1] * (1.0f - sa);
                d[2] = sb * sa + d[2] * (1.0f - sa);
                d[3] = sa + d[3] * (1.0f - sa);
            }
            any = true;
        }
    }
}

void FxCapture::EndBottomFrame() {
    auto& layer = ScreenRegions::FxLayer::Instance();
    const bool capturing = layer.Capturing();
    if (!capturing && !was_capturing) {
        return;
    }
    was_capturing = capturing;
    // Framebuffer space (240 wide, 320 high, row 0 at the bottom of the GL-style framebuffer) to
    // the bottom screen as seen (320x240, row 0 at the top): screen x = fb y, screen y = fb x
    // mirrored.
    std::vector<u8> out(static_cast<size_t>(320) * 240 * 4, 0);
    if (any) {
        // Menus that were never learned (e.g. a state loaded mid-action) cover most of the
        // screen: show nothing rather than a copy of the bottom screen.
        size_t covered = 0;
        for (size_t p = 0; p < accum.size(); p += 4) {
            covered += (accum[p + 3] > 0.05f || accum[p] + accum[p + 1] + accum[p + 2] > 0.15f);
        }
        if (covered * 2 > static_cast<size_t>(FbW) * FbH) {
            any = false;
            std::fill(accum.begin(), accum.end(), 0.0f);
        }
    }
    if (any) {
        for (int sy = 0; sy < 240; ++sy) {
            for (int sx = 0; sx < 320; ++sx) {
                const int fx = FbW - 1 - sy;
                const int fy = sx;
                const float* d = &accum[(static_cast<size_t>(fy) * FbW + fx) * 4];
                u8* o = &out[(static_cast<size_t>(sy) * 320 + sx) * 4];
                for (int ch = 0; ch < 4; ++ch) {
                    o[ch] = static_cast<u8>(std::clamp(d[ch], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }
    layer.Publish(std::move(out), any);
    if (any) {
        std::fill(accum.begin(), accum.end(), 0.0f);
    }
    any = false;
}

} // namespace Pica
