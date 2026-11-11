// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Imgui.Engine - the `:subsystem` partition.
///
/// ImguiSubsystem: an application-level subsystem that owns the Dear ImGui context and the RHI
/// renderer, and exposes the per-frame hooks an app drives. NewFrame feeds input and the display size
/// and begins the UI frame; the app builds its UI with ImGui:: directly. Where the application has a
/// render subsystem, ImGui is one of its screen overlays, drawn over the game's screens in the shared
/// overlay pass and turned with the window when the swapchain is pre-rotated. An application without
/// one calls Render after its scene, which draws onto the frame's target in a pass of its own. The
/// subsystem has its own ShaderSystem, so it does not depend on the renderer's.

module;
#include "Core/Prelude.h"
#include "imgui.h"

export module imgui.engine:subsystem;

import foundation.core;
import foundation.rhi;
import foundation.runtime;  // Subsystem
import foundation.shell;    // IInputManager / IMouse / IKeyboard / KeyCode / MouseButton
import foundation.graphics; // FrameContext
import foundation.shaders;
import foundation.shaders.system;
import foundation.vfs; // IFileSystem: the application's data mount, which holds the ImGui shaders
import foundation.render.api; // IScreenOverlay, ScreenOverlayView
import engine.render;         // RenderSubsystem: the screen overlays' host
import :renderer;

using namespace foundation::core;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;
namespace shell = foundation::shell;

export namespace imgui
{

    class ImguiSubsystem final : public foundation::runtime::Subsystem, public foundation::render::IScreenOverlay
    {
    public:
        /// Above the game's screens (the GUI draws at 10): a debugging UI stays on top.
        static constexpr i32 kOverlayOrder = 100;

        // `dataFileSystem` is the application's data mount, borrowed. The ImGui shaders resolve
        // from it like every other engine shader.
        ImguiSubsystem(rhi::Device& device, u32 framesInFlight,
                       foundation::vfs::IFileSystem& dataFileSystem) noexcept
            : m_device(&device), m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_dataFileSystem(&dataFileSystem)
        {
        }

        // Begin a UI frame: set the display size (cached from the last Render) and dt, feed input,
        // and call ImGui::NewFrame.
        // Call at the top of the app's update, before any ImGui:: widget calls.
        void NewFrame(shell::IInputManager* input, f32 deltaTime)
        {
            if (!m_ready)
            {
                return;
            }
            // If the previous frame's NewFrame was never matched by a Render()
            // (e.g. BeginFrame returned invalid and OnRenderWindow was skipped),
            // close it now so ImGui::NewFrame doesn't assert.
            if (m_frameOpen)
            {
                ImGui::EndFrame();
                m_frameOpen = false;
            }
            m_drawReady = false;
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(m_width), static_cast<float>(m_height));
            io.DeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);
            FeedInput(io, input);
            ImGui::NewFrame();
            m_frameOpen = true;
        }

        // Finish the UI frame and render it onto the frame's backbuffer, over the scene, for an
        // application whose render subsystem does not draw it as an overlay. Call after the app has
        // rendered its scene to the backbuffer, before present. Does nothing where the overlay pass
        // draws ImGui.
        void Render(foundation::graphics::FrameContext& frame)
        {
            if (!m_ready || m_overlayHost != nullptr)
            {
                return;
            }
            if (!m_frameOpen)
            {
                ImGui::NewFrame();
            } // keep ImGui balanced even if NewFrame was skipped
            m_frameOpen = false;
            ImGui::Render();
            m_width = frame.width;
            m_height = frame.height;
            if (frame.encoder == nullptr || frame.backbufferView == nullptr ||
                frame.window == nullptr)
            {
                return;
            }
            const rhi::TextureFormat fmt = frame.window->Swap()->Format();
            m_renderer->Render(*frame.encoder, frame.backbufferView, fmt, frame.width, frame.height,
                               ImGui::GetDrawData(), frame.frameIndex);
        }

        [[nodiscard]] bool Ready() const noexcept { return m_ready; }

        /// Whether a render subsystem draws ImGui as one of its screen overlays.
        [[nodiscard]] bool IsOverlay() const noexcept { return m_overlayHost != nullptr; }

        // --- IScreenOverlay ---
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return kOverlayOrder; }

        // Finishes the frame's UI (once, however many targets the overlay pass draws this frame) and
        // readies it for the view: lays it out at the window's size from the next frame on.
        void PrepareOverlay(rhi::CommandEncoder& encoder, const foundation::render::ScreenOverlayView& view) override
        {
            m_overlayPrepared = false;
            if (!m_ready)
            {
                return;
            }
            const Float2 window = view.WindowSize();
            m_width = static_cast<u32>(window.x);
            m_height = static_cast<u32>(window.y);
            if (m_frameOpen)
            {
                ImGui::Render();
                m_frameOpen = false;
                m_drawReady = true;
            }
            if (!m_drawReady)
            {
                return; // no NewFrame since the last frame drawn: nothing new to show
            }
            const Float2 target{static_cast<f32>(view.width), static_cast<f32>(view.height)};
            m_overlayPrepared = m_renderer->Prepare(encoder, view.targetFormat, view.depthStencilFormat, target,
                                                    view.WindowToTargetTransform(), ImGui::GetDrawData(),
                                                    view.frameIndex);
        }

        void Render(rhi::RenderPassEncoder& encoder, const foundation::render::ScreenOverlayView& view) override
        {
            if (!m_overlayPrepared)
            {
                return;
            }
            m_overlayPrepared = false;
            m_renderer->Draw(encoder, ImGui::GetDrawData(), view.preRotation, view.WindowSize(), view.width,
                             view.height);
        }

    protected:
        void OnInit() override
        {
            // Resolve the ImGui shaders through the shared ShaderSystemHost over the data mount:
            // cooked WGSL from Shaders/shaders.dpak in a dist or the browser, or DXC over Shaders/
            // during development.
            if (!m_shaderHost.Initialize(*m_device, *m_dataFileSystem))
            {
                return; // no compiler and no shader pack: ImGui stays inert
            }

            IMGUI_CHECKVERSION();
            m_context = ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // no imgui.ini persistence by default
            io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
            io.Fonts->AddFontDefault();
            ImGui::StyleColorsDark();

            m_renderer = MakeUnique<ImguiRenderer>(DefaultAllocator(), *m_device,
                                                   *m_shaderHost.System(), m_framesInFlight);
            if (!m_renderer->Initialize().IsOk())
            {
                m_renderer.Reset();
                return;
            }
            m_ready = true;
        }

        // Draws as a screen overlay where there is a render subsystem (headless and plain
        // applications have none, and call Render).
        void OnReady() override
        {
            foundation::runtime::SubsystemManager* manager = Manager();
            if (!m_ready || manager == nullptr || m_overlayHost != nullptr)
            {
                return;
            }
            if (auto* renderer = manager->GetSubsystem<engine::render::RenderSubsystem>())
            {
                m_overlayHost = renderer;
                m_overlayHost->RegisterOverlay(static_cast<foundation::render::IScreenOverlay*>(this));
            }
        }

        void OnShutdown() override
        {
            if (m_overlayHost != nullptr)
            {
                m_overlayHost->UnregisterOverlay(static_cast<foundation::render::IScreenOverlay*>(this));
                m_overlayHost = nullptr;
            }
            if (m_device != nullptr)
            {
                m_device->WaitIdle();
            }
            m_renderer.Reset();
            if (m_context != nullptr)
            {
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }
            m_shaderHost.Shutdown();
            m_ready = false;
        }

    private:
        void FeedInput(ImGuiIO& io, shell::IInputManager* input)
        {
            if (input == nullptr)
            {
                return;
            }
            if (shell::IMouse* m = input->Mouse())
            {
                io.AddMousePosEvent(m->X(), m->Y());
                io.AddMouseButtonEvent(0, m->IsButtonDown(shell::MouseButton::Left));
                io.AddMouseButtonEvent(1, m->IsButtonDown(shell::MouseButton::Right));
                io.AddMouseButtonEvent(2, m->IsButtonDown(shell::MouseButton::Middle));
                const f32 sx = m->ScrollX(), sy = m->ScrollY();
                if (sx != 0.0f || sy != 0.0f)
                {
                    io.AddMouseWheelEvent(sx, sy);
                }
            }
            if (shell::IKeyboard* k = input->Keyboard())
            {
                using K = shell::KeyCode;
                io.AddKeyEvent(ImGuiMod_Ctrl,
                               k->IsKeyDown(K::LeftCtrl) || k->IsKeyDown(K::RightCtrl));
                io.AddKeyEvent(ImGuiMod_Shift,
                               k->IsKeyDown(K::LeftShift) || k->IsKeyDown(K::RightShift));
                io.AddKeyEvent(ImGuiMod_Alt, k->IsKeyDown(K::LeftAlt) || k->IsKeyDown(K::RightAlt));
                io.AddKeyEvent(ImGuiKey_Tab, k->IsKeyDown(K::Tab));
                io.AddKeyEvent(ImGuiKey_LeftArrow, k->IsKeyDown(K::Left));
                io.AddKeyEvent(ImGuiKey_RightArrow, k->IsKeyDown(K::Right));
                io.AddKeyEvent(ImGuiKey_UpArrow, k->IsKeyDown(K::Up));
                io.AddKeyEvent(ImGuiKey_DownArrow, k->IsKeyDown(K::Down));
                io.AddKeyEvent(ImGuiKey_Enter, k->IsKeyDown(K::Return));
                io.AddKeyEvent(ImGuiKey_Escape, k->IsKeyDown(K::Escape));
                io.AddKeyEvent(ImGuiKey_Backspace, k->IsKeyDown(K::Backspace));
                io.AddKeyEvent(ImGuiKey_Delete, k->IsKeyDown(K::Delete));
                io.AddKeyEvent(ImGuiKey_Space, k->IsKeyDown(K::Space));
            }
        }

        rhi::Device* m_device;
        u32 m_framesInFlight = 2;
        shaders::ShaderSystemHost m_shaderHost{DefaultAllocator()}; // owns the ShaderSystem and the ImGui modules
        foundation::vfs::IFileSystem* m_dataFileSystem; // the application's data mount, borrowed
        UniquePtr<ImguiRenderer> m_renderer;
        ImGuiContext* m_context = nullptr;
        u32 m_width = 1280,
            m_height = 720; // the window's size at the last Render or overlay, the display size for NewFrame
        bool m_ready = false;
        bool m_frameOpen = false;
        bool m_drawReady = false;       // ImGui::Render ran since the last NewFrame
        bool m_overlayPrepared = false; // PrepareOverlay readied the view Render draws
        foundation::render::IScreenRenderer* m_overlayHost = nullptr; // the render subsystem drawing it, borrowed
    };

} // namespace imgui
