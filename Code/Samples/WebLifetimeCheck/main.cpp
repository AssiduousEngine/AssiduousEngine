// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebLifetimeCheck: in a browser, GPU resources destroyed while work still uses them stay alive for
// that work.
//
// The WebGPU backend frees a destroyed buffer, texture, view or sampler at once, releasing its
// reference, on the ground that what recorded and submitted work uses holds references of its
// own. This app tests that every frame. It makes a texture, its view, a sampler, a uniform buffer
// and a bind group, records a full-screen draw that samples them, and then destroys them: on even
// frames right after recording, before the frame's commands are submitted, and on odd frames at the
// start of the next frame, after the submit and before the browser has run it. If the browser did
// not keep them, it reports "Destroyed ... used in a submit" (or a lost device) to the console and
// the frame is dropped. The app logs its frame count; a clean console beside it is the pass.
//
// The wasm preset build emits WebLifetimeCheck.html, .js and .wasm; open the .html in a WebGPU
// browser over https.

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.web;  // RunApplication (the browser runner), required by APP_MAIN
import foundation.shell.web;    // WebShell, required by APP_MAIN
import foundation.graphics;     // GraphicsDevice and FrameContext
import foundation.graphics.gpu; // CreateGraphicsDevice
import foundation.rhi;

#include "Runtime.Client/AppMain.h"

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace rhi = foundation::rhi;

namespace
{
    // A full-screen triangle sampling the frame's texture, tinted by its uniform. Bindings are the
    // WebGPU backend's shifted ones: uniforms from 0, textures from 100, samplers from 300.
    constexpr const char8_t kWgsl[] = u8R"(
        struct Tint { color : vec4f };
        @group(0) @binding(0) var<uniform> tint : Tint;
        @group(0) @binding(100) var image : texture_2d<f32>;
        @group(0) @binding(300) var imageSampler : sampler;

        struct VSOut {
            @builtin(position) position : vec4f,
            @location(0) uv : vec2f,
        };
        @vertex fn vs(@builtin(vertex_index) index : u32) -> VSOut {
            var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
            var out : VSOut;
            out.position = vec4f(corners[index], 0.0, 1.0);
            out.uv = corners[index] * vec2f(0.5, -0.5) + vec2f(0.5, 0.5);
            return out;
        }
        @fragment fn fs(in : VSOut) -> @location(0) vec4f {
            return textureSample(image, imageSampler, in.uv * 4.0) * tint.color;
        }
    )";

    constexpr core::u32 kImageSize = 8;
    constexpr core::u32 kReportEvery = 300;

    // What one frame makes and destroys.
    struct FrameSet
    {
        rhi::TextureHandle texture{};
        rhi::TextureViewHandle view{};
        rhi::SamplerHandle sampler{};
        rhi::BufferHandle tint{};
        rhi::BindGroupHandle group{};
    };

    class LifetimeCheckApp final : public runtime::IApplication
    {
    public:
        void OnStartup(runtime::IApplicationHost& host) override
        {
            graphics::GraphicsDevice* gpu = host.Graphics();
            if (gpu == nullptr)
            {
                core::ConsoleWrite(u8"WebLifetimeCheck: no graphics device - nothing to check.\n");
                return;
            }
            m_device = gpu->Raw();

            rhi::ShaderModuleDesc moduleDesc;
            moduleDesc.code = core::Span<const core::u8>(reinterpret_cast<const core::u8*>(kWgsl),
                                                         core::StringView(kWgsl).Size());
            moduleDesc.label = u8"LifetimeCheckWGSL";
            if (!m_device->CreateShaderModule(moduleDesc, m_shader).IsOk())
            {
                return;
            }

            const rhi::BindGroupLayoutEntry entries[] = {
                rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            };
            rhi::BindGroupLayoutDesc layoutDesc;
            layoutDesc.entries = core::Span<const rhi::BindGroupLayoutEntry>(entries, 3);
            layoutDesc.label = u8"LifetimeCheckBGL";
            if (!m_device->CreateBindGroupLayout(layoutDesc, m_layout).IsOk())
            {
                return;
            }
            rhi::PipelineLayoutDesc plDesc;
            rhi::BindGroupLayoutHandle sets[1] = {m_layout};
            plDesc.bindGroupLayouts = core::Span<rhi::BindGroupLayoutHandle const>(sets, 1);
            plDesc.label = u8"LifetimeCheckPL";
            if (!m_device->CreatePipelineLayout(plDesc, m_pipelineLayout).IsOk())
            {
                return;
            }
        }

        void OnRenderWindow(runtime::IApplicationHost&, graphics::FrameContext& frame) override
        {
            if (!m_pipeline.IsValid() && m_pipelineLayout.IsValid())
            {
                MakePipeline(frame);
            }
            // Last frame's set was submitted and the browser may not have run it yet.
            Destroy(m_submitted);

            FrameSet set;
            const bool made = m_pipeline.IsValid() && Make(set);

            rhi::ClearColor clear;
            clear.r = 0.05f;
            clear.g = 0.06f;
            clear.b = 0.10f;
            clear.a = 1.0f;
            rhi::RenderPassEncoder* pass = frame.BeginBackbufferPass(clear);
            if (pass != nullptr && made)
            {
                pass->SetPipeline(m_pipeline);
                pass->SetBindGroup(0, set.group);
                pass->SetViewport(0.0f, 0.0f, static_cast<core::f32>(frame.width),
                                  static_cast<core::f32>(frame.height), 0.0f, 1.0f);
                pass->SetScissor(0, 0, frame.width, frame.height);
                pass->Draw(3);
            }
            frame.EndBackbufferPass();

            // Even frames: gone before the commands that use them are submitted.
            if ((m_frames & 1u) == 0u)
            {
                Destroy(set);
            }
            else
            {
                m_submitted = set;
            }

            ++m_frames;
            if (made && m_frames % kReportEvery == 0)
            {
                core::ConsoleWrite(core::Format(u8"[lifetime-check] {} frames drawn with resources destroyed "
                                                u8"before their commands ran\n",
                                                m_frames)
                                       .AsView());
            }
        }

        void OnShutdown(runtime::IApplicationHost&) override
        {
            if (m_device == nullptr)
            {
                return;
            }
            Destroy(m_submitted);
            if (m_pipeline.IsValid())
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            if (m_pipelineLayout.IsValid())
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
            }
            if (m_layout.IsValid())
            {
                m_device->DestroyBindGroupLayout(m_layout);
            }
            if (m_shader.IsValid())
            {
                m_device->DestroyShaderModule(m_shader);
            }
        }

    private:
        // In the back buffer's format, which the browser picks (RGBA or BGRA).
        void MakePipeline(const graphics::FrameContext& frame)
        {
            const rhi::TextureDesc* backbuffer = m_device->Describe(frame.backbuffer);
            if (backbuffer == nullptr)
            {
                return;
            }
            rhi::ColorTargetState colorTarget;
            colorTarget.format = backbuffer->format;
            colorTarget.writeMask = rhi::ColorWriteMask::All;
            rhi::RenderPipelineDesc pipelineDesc;
            pipelineDesc.layout = m_pipelineLayout;
            pipelineDesc.vertex.shader = {m_shader, u8"vs", rhi::ShaderStage::Vertex};
            pipelineDesc.fragment = rhi::FragmentState{};
            pipelineDesc.fragment->shader = {m_shader, u8"fs", rhi::ShaderStage::Fragment};
            pipelineDesc.fragment->targets = core::Span<const rhi::ColorTargetState>(&colorTarget, 1);
            pipelineDesc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pipelineDesc.label = u8"LifetimeCheckPipeline";
            if (!m_device->CreateRenderPipeline(pipelineDesc, m_pipeline).IsOk())
            {
                m_pipeline = {};
            }
        }

        // A checker whose colours turn with the frame, its view and a sampler, a tint, and the
        // group that binds them.
        [[nodiscard]] bool Make(FrameSet& set)
        {
            rhi::TextureDesc td;
            td.width = kImageSize;
            td.height = kImageSize;
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"lifetime.image";
            if (!m_device->CreateTexture(td, set.texture).IsOk())
            {
                return false;
            }
            core::u8 pixels[kImageSize * kImageSize * 4];
            const core::u8 turn = static_cast<core::u8>(m_frames * 3u);
            for (core::u32 y = 0; y < kImageSize; ++y)
            {
                for (core::u32 x = 0; x < kImageSize; ++x)
                {
                    const bool light = ((x + y) & 1u) == 0u;
                    core::u8* p = pixels + (y * kImageSize + x) * 4;
                    p[0] = light ? turn : 40;
                    p[1] = light ? 200 : 40;
                    p[2] = light ? static_cast<core::u8>(255 - turn) : 60;
                    p[3] = 255;
                }
            }
            rhi::TransferBatch* batch = nullptr;
            rhi::Queue* queue = m_device->GetQueue(rhi::QueueType::Graphics);
            if (queue == nullptr || !queue->CreateTransferBatch(batch).IsOk())
            {
                Destroy(set);
                return false;
            }
            rhi::TextureDataLayout layout;
            layout.bytesPerRow = kImageSize * 4;
            layout.rowsPerImage = kImageSize;
            batch->WriteTexture(set.texture, core::Span<const core::u8>(pixels, sizeof(pixels)), layout,
                                rhi::Extent3D{kImageSize, kImageSize, 1});
            (void)batch->Submit();
            queue->DestroyTransferBatch(batch);

            rhi::SamplerDesc sd;
            sd.minFilter = rhi::FilterMode::Nearest;
            sd.magFilter = rhi::FilterMode::Nearest;
            rhi::BufferDesc bd;
            bd.size = 16;
            bd.usage = rhi::BufferUsage::Uniform;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"lifetime.tint";
            if (!m_device->CreateTextureView(set.texture, rhi::TextureViewDesc{}, set.view).IsOk() ||
                !m_device->CreateSampler(sd, set.sampler).IsOk() || !m_device->CreateBuffer(bd, set.tint).IsOk())
            {
                Destroy(set);
                return false;
            }
            if (void* mapped = m_device->Map(set.tint))
            {
                const core::f32 tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                core::MemCopy(mapped, tint, sizeof(tint));
                m_device->FlushRange(set.tint, 0, sizeof(tint));
                m_device->Unmap(set.tint);
            }

            const rhi::BindGroupEntry groupEntries[] = {
                rhi::BindGroupEntry::BufferEntry(set.tint, 0, 16),
                rhi::BindGroupEntry::TextureEntry(set.view),
                rhi::BindGroupEntry::SamplerEntry(set.sampler),
            };
            rhi::BindGroupDesc gd;
            gd.layout = m_layout;
            gd.entries = core::Span<const rhi::BindGroupEntry>(groupEntries, 3);
            gd.label = u8"lifetime.group";
            if (!m_device->CreateBindGroup(gd, set.group).IsOk())
            {
                set.group = {};
                Destroy(set);
                return false;
            }
            return true;
        }

        void Destroy(FrameSet& set)
        {
            if (set.group.IsValid())
            {
                m_device->DestroyBindGroup(set.group);
            }
            m_device->DestroyBuffer(set.tint);
            m_device->DestroySampler(set.sampler);
            m_device->DestroyTextureView(set.view);
            m_device->DestroyTexture(set.texture);
            set = FrameSet{};
        }

        rhi::Device* m_device = nullptr;
        rhi::ShaderModuleHandle m_shader{};
        rhi::BindGroupLayoutHandle m_layout{};
        rhi::PipelineLayoutHandle m_pipelineLayout{};
        rhi::RenderPipelineHandle m_pipeline{};
        FrameSet m_submitted;
        core::u32 m_frames = 0;
    };
}

APP_MAIN(LifetimeCheckApp)
