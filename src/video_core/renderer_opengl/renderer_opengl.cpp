// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include <algorithm>
#include "core/frontend/emu_window.h"
#include "core/frontend/screen_regions.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_texture_mailbox.h"
#include "video_core/renderer_opengl/post_processing_opengl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/shader/generator/glsl_shader_gen.h"

#include "video_core/host_shaders/opengl_present_anaglyph_frag.h"
#include "video_core/host_shaders/opengl_present_frag.h"
#include "video_core/host_shaders/opengl_present_interlaced_frag.h"
#include "video_core/host_shaders/opengl_present_vert.h"

namespace OpenGL {

MICROPROFILE_DEFINE(OpenGL_RenderFrame, "OpenGL", "Render Frame", MP_RGB(128, 128, 64));
MICROPROFILE_DEFINE(OpenGL_WaitPresent, "OpenGL", "Wait For Present", MP_RGB(128, 128, 128));

/**
 * Vertex structure that the drawn screen rectangles are composed of.
 */
struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(GLfloat x, GLfloat y, GLfloat u, GLfloat v) {
        position[0] = x;
        position[1] = y;
        tex_coord[0] = u;
        tex_coord[1] = v;
    }

    std::array<GLfloat, 2> position{};
    std::array<GLfloat, 2> tex_coord{};
};

/**
 * Defines a 1:1 pixel ortographic projection matrix with (0,0) on the top-left
 * corner and (width, height) on the lower-bottom.
 *
 * The projection part of the matrix is trivial, hence these operations are represented
 * by a 3x2 matrix.
 *
 * @param flipped Whether the frame should be flipped upside down.
 */
static std::array<GLfloat, 3 * 2> MakeOrthographicMatrix(const float width, const float height,
                                                         bool flipped) {

    std::array<GLfloat, 3 * 2> matrix; // Laid out in column-major order

    // Last matrix row is implicitly assumed to be [0, 0, 1].
    if (flipped) {
        // clang-format off
        matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
        matrix[1] = 0.f;         matrix[3] = 2.f / height;  matrix[5] = -1.f;
        // clang-format on
    } else {
        // clang-format off
        matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
        matrix[1] = 0.f;         matrix[3] = -2.f / height; matrix[5] = 1.f;
        // clang-format on
    }

    return matrix;
}

RendererOpenGL::RendererOpenGL(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : VideoCore::RendererBase{system, window, secondary_window}, pica{pica_},
      rasterizer{system.Memory(), pica, system.CustomTexManager(), *this, driver},
      frame_dumper{system, window} {
    const bool has_debug_tool = driver.HasDebugTool();
    window.mailbox = std::make_unique<OGLTextureMailbox>(has_debug_tool);
    if (secondary_window) {
        secondary_window->mailbox = std::make_unique<OGLTextureMailbox>(has_debug_tool);
    }
    frame_dumper.mailbox = std::make_unique<OGLVideoDumpingMailbox>();
    InitOpenGLObjects();
}

RendererOpenGL::~RendererOpenGL() = default;

void RendererOpenGL::SwapBuffers() {
    system.perf_stats->StartSwap();
    ScreenRegions::Manager::Instance().Update(system);
    // Maintain the rasterizer's state as a priority
    OpenGLState prev_state = OpenGLState::GetCurState();
    state.Apply();
#ifdef ANDROID
    if (secondary_window) {
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#else
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

    render_window.SetupFramebuffer();

    PrepareRendertarget();
    RenderScreenshot();
    isSecondaryWindow = false;
#ifdef HAVE_LIBRETRO
    DrawScreens(render_window.GetFramebufferLayout(), false);
    render_window.SwapBuffers();
#else
    const auto& main_layout = render_window.GetFramebufferLayout();
    RenderToMailbox(main_layout, render_window.mailbox, false);

#ifdef ANDROID
    // On Android, if secondary_window is defined at all,
    // it means we have a second display
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        isSecondaryWindow = true;
        RenderToMailbox(secondary_layout, secondary_window->mailbox, false);
        secondary_window->PollEvents();
    }
#else
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        isSecondaryWindow = true;
        RenderToMailbox(secondary_layout, secondary_window->mailbox, false);
        secondary_window->PollEvents();
    }
#endif

    if (frame_dumper.IsDumping()) {
        try {
            RenderToMailbox(frame_dumper.GetLayout(), frame_dumper.mailbox, true);
        } catch (const OGLTextureMailboxException& exception) {
            LOG_DEBUG(Render_OpenGL, "Frame dumper exception caught: {}", exception.what());
        }
    }
#endif

    system.perf_stats->EndSwap();
    EndFrame();
    prev_state.Apply();
    rasterizer.TickFrame();
}

void RendererOpenGL::RenderScreenshot() {
    if (settings.screenshot_requested.exchange(false)) {
        // Draw this frame to the screenshot framebuffer
        screenshot_framebuffer.Create();
        GLuint old_read_fb = state.draw.read_framebuffer;
        GLuint old_draw_fb = state.draw.draw_framebuffer;
        state.draw.read_framebuffer = state.draw.draw_framebuffer = screenshot_framebuffer.handle;
        state.Apply();

        const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};

        GLuint renderbuffer;
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB8, layout.width, layout.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  renderbuffer);

        DrawScreens(layout, false);

        glReadPixels(0, 0, layout.width, layout.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                     settings.screenshot_bits);

        screenshot_framebuffer.Release();
        state.draw.read_framebuffer = old_read_fb;
        state.draw.draw_framebuffer = old_draw_fb;
        state.Apply();
        glDeleteRenderbuffers(1, &renderbuffer);

        settings.screenshot_complete_callback(true);
    }
}

void RendererOpenGL::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            // Resize the texture to let it be reconfigured
            texture.width = 1;
            texture.height = 1;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer, color_fill);
        }
        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1, color_fill);
    }
}

void RendererOpenGL::RenderToMailbox(const Layout::FramebufferLayout& layout,
                                     std::unique_ptr<Frontend::TextureMailbox>& mailbox,
                                     bool flipped) {
    if (!Settings::values.use_skip_duplicate_frames.GetValue() ||
        Core::PerfStats::game_frames_updated) {
        Frontend::Frame* frame;
        {
            MICROPROFILE_SCOPE(OpenGL_WaitPresent);

            frame = mailbox->GetRenderFrame();

            // Clean up sync objects before drawing

            // INTEL driver workaround. We can't delete the previous render sync object until we are
            // sure that the presentation is done
            if (frame->present_fence) {
                glClientWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
            }

            // delete the draw fence if the frame wasn't presented
            if (frame->render_fence) {
                glDeleteSync(frame->render_fence);
                frame->render_fence = nullptr;
            }

            // wait for the presentation to be done
            if (frame->present_fence) {
                glWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(frame->present_fence);
                frame->present_fence = nullptr;
            }
        }

        {
            MICROPROFILE_SCOPE(OpenGL_RenderFrame);
            // Recreate the frame if the size of the window has changed
            if (layout.width != frame->width || layout.height != frame->height) {
                LOG_DEBUG(Render_OpenGL, "Reloading render frame");
                mailbox->ReloadRenderFrame(frame, layout.width, layout.height);
            }

            state.draw.draw_framebuffer = frame->render.handle;
            state.Apply();
            DrawScreens(layout, flipped);
            // Create a fence for the frontend to wait on and swap this frame to OffTex
            frame->render_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            mailbox->ReleaseRenderFrame(frame);
        }

        if ((secondaryWindowEnabled && isSecondaryWindow) || (!secondaryWindowEnabled)) {
            Core::PerfStats::game_frames_updated = false;
        }
    }
}

/**
 * Loads framebuffer from emulated memory into the active OpenGL texture.
 */
void RendererOpenGL::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye,
                                        const Pica::ColorFill& color_fill) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0)
        right_eye = false;

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (!right_eye ? framebuffer.address_left1 : framebuffer.address_right1)
            : (!right_eye ? framebuffer.address_left2 : framebuffer.address_right2);

    LOG_TRACE(Render_OpenGL, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    int bpp = Pica::BytesPerPixel(framebuffer.color_format);
    std::size_t pixel_stride = framebuffer.stride / bpp;

    // OpenGL only supports specifying a stride in units of pixels, not bytes, unfortunately
    ASSERT(pixel_stride * bpp == framebuffer.stride);

    // Ensure no bad interactions with GL_UNPACK_ALIGNMENT, which by default
    // only allows rows to have a memory alignement of 4.
    ASSERT(pixel_stride % 4 == 0);

    if (color_fill.is_enabled ||
        !rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        u32 width = framebuffer.width;
        u32 height = framebuffer.height;
        u8 fill_pixel[3];
        // Reset the screen info's display texture to its own permanent texture
        screen_info.display_texture = screen_info.texture.resource.handle;
        screen_info.display_texcoords = Common::Rectangle<f32>(0.f, 0.f, 1.f, 1.f);

        rasterizer.FlushRegion(framebuffer_addr, framebuffer.stride * framebuffer.height);

        u8* framebuffer_data = system.Memory().GetPhysicalPointer(framebuffer_addr);

        if (color_fill.is_enabled) {
            memcpy(fill_pixel, color_fill.AsVector().AsArray(), sizeof(fill_pixel));
            framebuffer_data = fill_pixel;
            width = 1;
            height = 1;
            pixel_stride = 0;
        }

        state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
        state.Apply();

        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pixel_stride);

        // Update existing texture
        // TODO: Test what happens on hardware when you change the framebuffer dimensions so that
        //       they differ from the LCD resolution.
        // TODO: Applications could theoretically crash Citra here by specifying too large
        //       framebuffer sizes. We should make sure that this cannot happen.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, screen_info.texture.gl_format,
                        screen_info.texture.gl_type, framebuffer_data);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        state.texture_units[0].texture_2d = 0;
        state.Apply();
    }
}

/**
 * Initializes the OpenGL state and creates persistent objects.
 */
void RendererOpenGL::InitOpenGLObjects() {
    glClearColor(Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(),
                 Settings::values.bg_blue.GetValue(), 1.0f);

    for (std::size_t i = 0; i < samplers.size(); i++) {
        samplers[i].Create();
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_MIN_FILTER,
                            i == 0 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_MAG_FILTER,
                            i == 0 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    ReloadShader(Settings::values.render_3d.GetValue());

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Generate VAO
    vertex_array.Create();

    state.draw.vertex_array = vertex_array.handle;
    state.draw.vertex_buffer = vertex_buffer.handle;
    state.draw.uniform_buffer = 0;
    state.Apply();

    // Attach vertex data to VAO
    glBufferData(GL_ARRAY_BUFFER, sizeof(ScreenRectVertex) * 4, nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, position));
    glVertexAttribPointer(attrib_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, tex_coord));
    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_tex_coord);

    // Allocate textures for each screen
    for (auto& screen_info : screen_infos) {
        screen_info.texture.resource.Create();

        // Allocation of storage is deferred until the first frame, when we
        // know the framebuffer size.

        state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
        state.Apply();

        glActiveTexture(GL_TEXTURE0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        screen_info.display_texture = screen_info.texture.resource.handle;
    }

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

void RendererOpenGL::ReloadShader(Settings::StereoRenderOption render_3d) {
    // Link shaders and get variable locations
    std::string shader_data = fragment_shader_precision_OES;
    if (render_3d == Settings::StereoRenderOption::Anaglyph) {
        if (Settings::values.anaglyph_shader_name.GetValue() == "Dubois (builtin)") {
            shader_data += HostShaders::OPENGL_PRESENT_ANAGLYPH_FRAG;
        } else {
            std::string shader_text = OpenGL::GetPostProcessingShaderCode(
                true, Settings::values.anaglyph_shader_name.GetValue());
            if (shader_text.empty()) {
                // Should probably provide some information that the shader couldn't load
                shader_data += HostShaders::OPENGL_PRESENT_ANAGLYPH_FRAG;
            } else {
                shader_data += shader_text;
            }
        }
    } else if (render_3d == Settings::StereoRenderOption::Interlaced ||
               render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        shader_data += HostShaders::OPENGL_PRESENT_INTERLACED_FRAG;
    } else {
        if (Settings::values.pp_shader_name.GetValue() == "None (builtin)") {
            shader_data += HostShaders::OPENGL_PRESENT_FRAG;
        } else {
            std::string shader_text = OpenGL::GetPostProcessingShaderCode(
                false, Settings::values.pp_shader_name.GetValue());
            if (shader_text.empty()) {
                // Should probably provide some information that the shader couldn't load
                shader_data += HostShaders::OPENGL_PRESENT_FRAG;
            } else {
                shader_data += shader_text;
            }
        }
    }
    shader.Create(HostShaders::OPENGL_PRESENT_VERT, shader_data);
    state.draw.shader_program = shader.handle;
    state.Apply();
    uniform_modelview_matrix = glGetUniformLocation(shader.handle, "modelview_matrix");
    uniform_color_texture = glGetUniformLocation(shader.handle, "color_texture");
    if (render_3d == Settings::StereoRenderOption::Anaglyph ||
        render_3d == Settings::StereoRenderOption::Interlaced ||
        render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        uniform_color_texture_r = glGetUniformLocation(shader.handle, "color_texture_r");
    }
    if (render_3d == Settings::StereoRenderOption::Interlaced ||
        render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        GLuint uniform_reverse_interlaced =
            glGetUniformLocation(shader.handle, "reverse_interlaced");
        if (render_3d == Settings::StereoRenderOption::ReverseInterlaced)
            glUniform1i(uniform_reverse_interlaced, 1);
        else
            glUniform1i(uniform_reverse_interlaced, 0);
    }
    uniform_i_resolution = glGetUniformLocation(shader.handle, "i_resolution");
    uniform_o_resolution = glGetUniformLocation(shader.handle, "o_resolution");
    uniform_layer = glGetUniformLocation(shader.handle, "layer");
    attrib_position = glGetAttribLocation(shader.handle, "vert_position");
    attrib_tex_coord = glGetAttribLocation(shader.handle, "vert_tex_coord");
}

void RendererOpenGL::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer,
                                                 const Pica::ColorFill& color_fill) {
    Pica::PixelFormat format = framebuffer.color_format;
    GLint internal_format{};
    u32 width, height;

    texture.format = format;
    width = texture.width = framebuffer.width;
    height = texture.height = framebuffer.height;
    if (color_fill.is_enabled) {
        width = 1;
        height = 1;
        format = Pica::PixelFormat::RGB8;
    }

    switch (format) {
    case Pica::PixelFormat::RGBA8:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = driver.IsOpenGLES() ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8;
        break;

    case Pica::PixelFormat::RGB8:
        // This pixel format uses BGR since GL_UNSIGNED_BYTE specifies byte-order, unlike every
        // specific OpenGL type used in this function using native-endian (that is, little-endian
        // mostly everywhere) for words or half-words.
        // TODO: check how those behave on big-endian processors.
        internal_format = GL_RGB;

        // GLES Dosen't support BGR , Use RGB instead
        texture.gl_format = driver.IsOpenGLES() ? GL_RGB : GL_BGR;
        texture.gl_type = GL_UNSIGNED_BYTE;
        break;

    case Pica::PixelFormat::RGB565:
        internal_format = GL_RGB;
        texture.gl_format = GL_RGB;
        texture.gl_type = GL_UNSIGNED_SHORT_5_6_5;
        break;

    case Pica::PixelFormat::RGB5A1:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_5_5_5_1;
        break;

    case Pica::PixelFormat::RGBA4:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_4_4_4_4;
        break;

    default:
        UNIMPLEMENTED();
    }

    state.texture_units[0].texture_2d = texture.resource.handle;
    state.Apply();

    glActiveTexture(GL_TEXTURE0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, texture.gl_format,
                 texture.gl_type, nullptr);

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

/**
 * Draws a single texture to the emulator window, rotating the texture to correct for the 3DS's LCD
 * rotation.
 */
void RendererOpenGL::DrawSingleScreen(const ScreenInfo& screen_info, float x, float y, float w,
                                      float h, Layout::DisplayOrientation orientation) {
    const auto& texcoords = screen_info.display_texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_OpenGL, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u32 scale_factor = GetResolutionScaleFactor();
    const GLuint sampler = samplers[Settings::values.filter_mode.GetValue()].handle;
    glUniform4f(uniform_i_resolution, static_cast<float>(screen_info.texture.width * scale_factor),
                static_cast<float>(screen_info.texture.height * scale_factor),
                1.0f / static_cast<float>(screen_info.texture.width * scale_factor),
                1.0f / static_cast<float>(screen_info.texture.height * scale_factor));
    glUniform4f(uniform_o_resolution, h, w, 1.0f / h, 1.0f / w);
    state.texture_units[0].texture_2d = screen_info.display_texture;
    state.texture_units[0].sampler = sampler;
    state.Apply();

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    state.texture_units[0].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.Apply();
}

/**
 * Draws a single texture to the emulator window, rotating the texture to correct for the 3DS's LCD
 * rotation.
 */
void RendererOpenGL::DrawSingleScreenStereo(const ScreenInfo& screen_info_l,
                                            const ScreenInfo& screen_info_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const auto& texcoords = screen_info_l.display_texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_OpenGL, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u32 scale_factor = GetResolutionScaleFactor();
    const GLuint sampler = samplers[Settings::values.filter_mode.GetValue()].handle;
    glUniform4f(uniform_i_resolution,
                static_cast<float>(screen_info_l.texture.width * scale_factor),
                static_cast<float>(screen_info_l.texture.height * scale_factor),
                1.0f / static_cast<float>(screen_info_l.texture.width * scale_factor),
                1.0f / static_cast<float>(screen_info_l.texture.height * scale_factor));
    glUniform4f(uniform_o_resolution, h, w, 1.0f / h, 1.0f / w);
    state.texture_units[0].texture_2d = screen_info_l.display_texture;
    state.texture_units[1].texture_2d = screen_info_r.display_texture;
    state.texture_units[0].sampler = sampler;
    state.texture_units[1].sampler = sampler;
    state.Apply();

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    state.texture_units[0].texture_2d = 0;
    state.texture_units[1].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.texture_units[1].sampler = 0;
    state.Apply();
}

/**
 * Draws the emulated screens to the emulator window.
 */
void RendererOpenGL::DrawScreens(const Layout::FramebufferLayout& layout, bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        // Update background color before drawing
        glClearColor(Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(),
                     Settings::values.bg_blue.GetValue(), 1.0f);
    }

    if (settings.shader_update_requested.exchange(false)) {
        // Update fragment shader before drawing
        shader.Release();
        // Link shaders and get variable locations
        ReloadShader(layout.render_3d_mode);
    }

    // Screen Regions may move the top screen for the active profile.
    const auto region_layout = ScreenRegions::Manager::Instance().Apply(layout);
    const auto& top_screen = region_layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;

    glViewport(0, 0, layout.width, layout.height);
    if (render_window.NeedsClearing()) {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Set projection matrix
    std::array<GLfloat, 3 * 2> ortho_matrix =
        MakeOrthographicMatrix((float)layout.width, (float)layout.height, flipped);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());

    // Bind texture in Texture Unit 0
    glUniform1i(uniform_color_texture, 0);

    const bool stereo_single_screen =
        layout.render_3d_mode == Settings::StereoRenderOption::Anaglyph ||
        layout.render_3d_mode == Settings::StereoRenderOption::Interlaced ||
        layout.render_3d_mode == Settings::StereoRenderOption::ReverseInterlaced;

    // Bind a second texture for the right eye if in Anaglyph mode
    if (stereo_single_screen) {
        glUniform1i(uniform_color_texture_r, 1);
    }

    const bool hide_bottom = ScreenRegions::Manager::Instance().HideBottom();
    glUniform1i(uniform_layer, 0);
    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        glUniform1i(uniform_layer, 0);
        ApplySecondLayerOpacity(layout.bottom_opacity);
        if (!hide_bottom) {
            DrawBottomScreen(layout, bottom_screen);
        }
    } else {
        if (!hide_bottom) {
            DrawBottomScreen(layout, bottom_screen);
        }
        glUniform1i(uniform_layer, 0);
        ApplySecondLayerOpacity(layout.top_opacity);
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }
    ReadProbes();
    if (ScreenRegions::Manager::Instance().HudUnder()) {
        DrawHud(layout);
        DrawScreenRegions(layout);
    } else {
        DrawScreenRegions(layout);
        DrawHud(layout);
    }
    ResetSecondLayerOpacity();
}

/**
 * Samples requested bottom-screen pixels (HUD "pix:" bindings) from the live screen texture.
 */
void RendererOpenGL::ReadProbes() {
    auto& probes = ScreenRegions::Probes::Instance();
    auto& manager = ScreenRegions::Manager::Instance();
    const auto requests = probes.Requests();
    const bool capture = manager.WantsBottomCapture();
    ++probe_frame;
    std::vector<ScreenRegions::Captures::Request> grabs;
    if ((probe_frame % 20) == 0) {
        grabs = ScreenRegions::Captures::Instance().TakeRequests();
    }
    if (!capture && grabs.empty() && (requests.empty() || (probe_frame & 3) != 0)) {
        return;
    }
    const ScreenInfo& info = screen_infos[2];
    if (info.display_texture == 0) {
        return;
    }
    if (probe_fbo.handle == 0) {
        probe_fbo.Create();
    }
    const auto& tc = info.display_texcoords;
    const GLuint prev_read = state.draw.read_framebuffer;
    const GLuint prev_tex = state.texture_units[0].texture_2d;

    state.texture_units[0].texture_2d = info.display_texture;
    state.draw.read_framebuffer = probe_fbo.handle;
    state.Apply();
    glActiveTexture(GL_TEXTURE0);
    GLint tex_w = 0, tex_h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           info.display_texture, 0);
    if (tex_w > 0 && tex_h > 0 &&
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        if (capture) {
            std::vector<u8> full(static_cast<size_t>(tex_w) * tex_h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, tex_w, tex_h, GL_RGBA, GL_UNSIGNED_BYTE, full.data());
            std::vector<u8> rgb(320 * 240 * 3);
            for (u32 y = 0; y < 240; ++y) {
                for (u32 x = 0; x < 320; ++x) {
                    const float fx = (static_cast<float>(x) + 0.5f) / 320.0f;
                    const float fy = (static_cast<float>(y) + 0.5f) / 240.0f;
                    const float s_coord = tc.bottom + (tc.top - tc.bottom) * fy;
                    const float t_coord = tc.left + (tc.right - tc.left) * fx;
                    const GLint px = std::clamp(static_cast<GLint>(s_coord * tex_w), 0, tex_w - 1);
                    const GLint py = std::clamp(static_cast<GLint>(t_coord * tex_h), 0, tex_h - 1);
                    const u8* src = &full[(static_cast<size_t>(py) * tex_w + px) * 4];
                    u8* dst = &rgb[(y * 320 + x) * 3];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
            manager.SetBottomCapture(std::move(rgb));
        }
        for (const auto& g : grabs) {
            // Copy a bottom-screen rectangle at the screen's internal resolution.
            const float sx = static_cast<float>(tex_h) / 320.0f;
            const float sy = static_cast<float>(tex_w) / 240.0f;
            const u32 ow = static_cast<u32>(std::max(1.0f, g.w * sx));
            const u32 oh = static_cast<u32>(std::max(1.0f, g.h * sy));
            // bounding box of the source in texture pixels
            const float s0 = tc.bottom + (tc.top - tc.bottom) * (g.y / 240.0f);
            const float s1 = tc.bottom + (tc.top - tc.bottom) * ((g.y + g.h) / 240.0f);
            const float t0 = tc.left + (tc.right - tc.left) * (g.x / 320.0f);
            const float t1 = tc.left + (tc.right - tc.left) * ((g.x + g.w) / 320.0f);
            const GLint bx0 = std::clamp(static_cast<GLint>(std::min(s0, s1) * tex_w) - 1, 0, tex_w - 1);
            const GLint bx1 = std::clamp(static_cast<GLint>(std::max(s0, s1) * tex_w) + 1, 0, tex_w - 1);
            const GLint by0 = std::clamp(static_cast<GLint>(std::min(t0, t1) * tex_h) - 1, 0, tex_h - 1);
            const GLint by1 = std::clamp(static_cast<GLint>(std::max(t0, t1) * tex_h) + 1, 0, tex_h - 1);
            const GLint bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
            std::vector<u8> box(static_cast<size_t>(bw) * bh * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(bx0, by0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, box.data());
            ScreenRegions::Image out;
            out.width = ow;
            out.height = oh;
            out.pixels.resize(static_cast<size_t>(ow) * oh * 4);
            for (u32 yy = 0; yy < oh; ++yy) {
                for (u32 xx = 0; xx < ow; ++xx) {
                    const float fx = (g.x + (xx + 0.5f) / ow * g.w) / 320.0f;
                    const float fy = (g.y + (yy + 0.5f) / oh * g.h) / 240.0f;
                    const float sc = tc.bottom + (tc.top - tc.bottom) * fy;
                    const float tcv = tc.left + (tc.right - tc.left) * fx;
                    const GLint px = std::clamp(static_cast<GLint>(sc * tex_w) - bx0, 0, bw - 1);
                    const GLint py = std::clamp(static_cast<GLint>(tcv * tex_h) - by0, 0, bh - 1);
                    const u8* src = &box[(static_cast<size_t>(py) * bw + px) * 4];
                    u8* dst = &out.pixels[(static_cast<size_t>(yy) * ow + xx) * 4];
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            }
            u64 sum = 1469598103934665603ULL;
            for (size_t i = 0; i < out.pixels.size(); i += 16) {
                sum = (sum ^ out.pixels[i]) * 1099511628211ULL;
            }
            auto& last = capture_sums[g.name];
            if (last != sum) {
                last = sum;
                ScreenRegions::Captures::Instance().Store(g.name, std::move(out));
            }
        }
        for (const auto& [x, y] : requests) {
            const float fx = (static_cast<float>(x) + 0.5f) / 320.0f;
            const float fy = (static_cast<float>(y) + 0.5f) / 240.0f;
            const float s_coord = tc.bottom + (tc.top - tc.bottom) * fy;
            const float t_coord = tc.left + (tc.right - tc.left) * fx;
            const GLint px = std::clamp(static_cast<GLint>(s_coord * tex_w), 0, tex_w - 1);
            const GLint py = std::clamp(static_cast<GLint>(t_coord * tex_h), 0, tex_h - 1);
            u8 rgba[4]{};
            glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            probes.Set(x, y, (u32{rgba[0]} << 16) | (u32{rgba[1]} << 8) | rgba[2]);
        }
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    state.draw.read_framebuffer = prev_read;
    state.texture_units[0].texture_2d = prev_tex;
    state.Apply();
}

/**
 * Draws the native HD HUD canvas produced by Screen Regions (straight alpha).
 */
void RendererOpenGL::DrawHud(const Layout::FramebufferLayout& layout) {
    auto& regions = ScreenRegions::Manager::Instance();
    u64 version = 0;
    const auto canvas = regions.HudCanvas(version);
    if (!canvas || canvas->pixels.empty()) {
        return;
    }
    if (hud_texture.handle == 0) {
        hud_texture.Create();
        hud_version = ~0ULL;
    }
    state.texture_units[0].texture_2d = hud_texture.handle;
    state.texture_units[0].sampler = samplers[1].handle;
    state.Apply();
    if (version != hud_version) {
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas->width, canvas->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, canvas->pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        hud_version = version;
    }
    const auto r = regions.CanvasRect(layout);
    const std::array<ScreenRectVertex, 4> vertices = {{
        ScreenRectVertex(r.left, r.top, 0.0f, 0.0f),
        ScreenRectVertex(r.right, r.top, 1.0f, 0.0f),
        ScreenRectVertex(r.left, r.bottom, 0.0f, 1.0f),
        ScreenRectVertex(r.right, r.bottom, 1.0f, 1.0f),
    }};
    state.blend.src_rgb_func = GL_SRC_ALPHA;
    state.blend.dst_rgb_func = GL_ONE_MINUS_SRC_ALPHA;
    state.blend.src_a_func = GL_ONE;
    state.blend.dst_a_func = GL_ONE_MINUS_SRC_ALPHA;
    glUniform1i(uniform_layer, 0);
    const float w = r.right - r.left, h = r.bottom - r.top;
    glUniform4f(uniform_i_resolution, static_cast<float>(canvas->width),
                static_cast<float>(canvas->height), 1.0f / canvas->width, 1.0f / canvas->height);
    glUniform4f(uniform_o_resolution, w, h, 1.0f / w, 1.0f / h);
    state.Apply();
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    state.texture_units[0].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.Apply();
}

/**
 * Draws the bottom-screen regions selected by the active Screen Regions profile.
 */
void RendererOpenGL::DrawScreenRegions(const Layout::FramebufferLayout& layout) {
    if (!layout.is_rotated || layout.render_3d_mode != Settings::StereoRenderOption::Off) {
        return;
    }
    const auto regions = ScreenRegions::Manager::Instance().Resolve(layout);
    if (regions.empty()) {
        return;
    }
    const ScreenInfo& screen_info = screen_infos[2];
    const auto& tc = screen_info.display_texcoords;
    const u32 scale_factor = GetResolutionScaleFactor();
    const GLuint sampler = samplers[Settings::values.filter_mode.GetValue()].handle;

    glUniform1i(uniform_layer, 0);
    for (const auto& r : regions) {
        if (r.w <= 0.0f || r.h <= 0.0f || r.opacity <= 0.0f) {
            continue;
        }
        const float u0 = tc.bottom + (tc.top - tc.bottom) * r.src_norm.top;
        const float u1 = tc.bottom + (tc.top - tc.bottom) * r.src_norm.bottom;
        const float v0 = tc.left + (tc.right - tc.left) * r.src_norm.left;
        const float v1 = tc.left + (tc.right - tc.left) * r.src_norm.right;
        const std::array<ScreenRectVertex, 4> vertices = {{
            ScreenRectVertex(r.x, r.y, u0, v0),
            ScreenRectVertex(r.x + r.w, r.y, u0, v1),
            ScreenRectVertex(r.x, r.y + r.h, u1, v0),
            ScreenRectVertex(r.x + r.w, r.y + r.h, u1, v1),
        }};

        if (r.screen) {
            // Screen blend: dst + src * (1 - dst) == src + dst * (1 - src).
            state.blend.src_rgb_func = GL_ONE;
            state.blend.dst_rgb_func = GL_ONE_MINUS_SRC_COLOR;
            state.blend.src_a_func = GL_ONE;
            state.blend.dst_a_func = GL_ONE_MINUS_SRC_ALPHA;
        } else {
            ApplySecondLayerOpacity(r.opacity);
        }
        glUniform4f(uniform_i_resolution,
                    static_cast<float>(screen_info.texture.width * scale_factor),
                    static_cast<float>(screen_info.texture.height * scale_factor),
                    1.0f / static_cast<float>(screen_info.texture.width * scale_factor),
                    1.0f / static_cast<float>(screen_info.texture.height * scale_factor));
        glUniform4f(uniform_o_resolution, r.w, r.h, 1.0f / r.w, 1.0f / r.h);
        state.texture_units[0].texture_2d = screen_info.display_texture;
        state.texture_units[0].sampler = sampler;
        state.Apply();

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    state.texture_units[0].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.Apply();
}

void RendererOpenGL::ApplySecondLayerOpacity(float opacity) {
    state.blend.src_rgb_func = GL_CONSTANT_ALPHA;
    state.blend.src_a_func = GL_CONSTANT_ALPHA;
    state.blend.dst_a_func = GL_ONE_MINUS_CONSTANT_ALPHA;
    state.blend.dst_rgb_func = GL_ONE_MINUS_CONSTANT_ALPHA;
    state.blend.color.alpha = opacity;
}

void RendererOpenGL::ResetSecondLayerOpacity() {
    state.blend.src_rgb_func = GL_ONE;
    state.blend.dst_rgb_func = GL_ZERO;
    state.blend.src_a_func = GL_ONE;
    state.blend.dst_a_func = GL_ZERO;
    state.blend.color.alpha = 0.0f;
}

void RendererOpenGL::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;

    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(screen_infos[eye], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left / 2, top_screen_top,
                         top_screen_width / 2, top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[rightside],
                         static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[rightside],
                         static_cast<float>(top_screen_left + layout.width / 2), top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[rightside],
            static_cast<float>(layout.cardboard.top_screen_right_eye + (layout.width / 2)),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(screen_infos[leftside], screen_infos[rightside], top_screen_left,
                               top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererOpenGL::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {

        DrawSingleScreen(screen_infos[2], bottom_screen_left / 2, bottom_screen_top,
                         bottom_screen_width / 2, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[2], static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width / 2, bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[2], bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[2],
            static_cast<float>(layout.cardboard.bottom_screen_right_eye + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(screen_infos[2], screen_infos[2], bottom_screen_left,
                               bottom_screen_top, bottom_screen_width, bottom_screen_height,
                               orientation);
        break;
    }
    }
}

void RendererOpenGL::TryPresent(int timeout_ms, bool is_secondary) {
    const auto& window = is_secondary ? *secondary_window : render_window;
    const auto& layout = window.GetFramebufferLayout();
    auto frame = window.mailbox->TryGetPresentFrame(timeout_ms);
    if (!frame) {
        LOG_DEBUG(Render_OpenGL, "TryGetPresentFrame returned no frame to present");
        return;
    }

    // Clearing before a full overwrite of a fbo can signal to drivers that they can avoid a
    // readback since we won't be doing any blending
    glClear(GL_COLOR_BUFFER_BIT);

    // Recreate the presentation FBO if the color attachment was changed
    if (frame->color_reloaded) {
        LOG_DEBUG(Render_OpenGL, "Reloading present frame");
        window.mailbox->ReloadPresentFrame(frame, layout.width, layout.height);
    }
    glWaitSync(frame->render_fence, 0, GL_TIMEOUT_IGNORED);
    // INTEL workaround.
    // Normally we could just delete the draw fence here, but due to driver bugs, we can just delete
    // it on the emulation thread without too much penalty
    // glDeleteSync(frame.render_sync);
    // frame.render_sync = 0;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame->present.handle);
    glBlitFramebuffer(0, 0, frame->width, frame->height, 0, 0, layout.width, layout.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Delete the fence if we're re-presenting to avoid leaking fences
    if (frame->present_fence) {
        glDeleteSync(frame->present_fence);
    }

    /* insert fence for the main thread to block on */
    frame->present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void RendererOpenGL::PrepareVideoDumping() {
    auto* mailbox = static_cast<OGLVideoDumpingMailbox*>(frame_dumper.mailbox.get());
    {
        std::scoped_lock lock{mailbox->swap_chain_lock};
        mailbox->quit = false;
    }
    frame_dumper.StartDumping();
}

void RendererOpenGL::CleanupVideoDumping() {
    frame_dumper.StopDumping();
    auto* mailbox = static_cast<OGLVideoDumpingMailbox*>(frame_dumper.mailbox.get());
    {
        std::scoped_lock lock{mailbox->swap_chain_lock};
        mailbox->quit = true;
    }
    mailbox->free_cv.notify_one();
}

} // namespace OpenGL
