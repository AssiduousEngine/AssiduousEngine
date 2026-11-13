// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials.PipelineCache - the `foundation.materials.pipelinecache` module.
///
/// The render-side PSO cache: the one piece of the shader and material stack that lives
/// outside the resource system. It maps a PipelineConfig and render-target signature to a
/// compiled RHI RenderPipeline, pulling shader variants from the ShaderSystem and mapping
/// the material's render state to RHI pipeline state.
///
/// Hot reload is handled by version polling at the point of use: each cached entry records
/// the ShaderSystem version of its shader at build time; GetPipeline compares against
/// the current version and lazily rebuilds when a shader was invalidated (reloaded).
/// This costs one HashMap lookup, a key compare and an integer compare per draw, with no
/// listener bookkeeping. Superseded pipelines are retired and freed by TickRetired() once the
/// frames in flight have passed.

module;
#include "Core/Prelude.h"

export module foundation.materials.pipelinecache;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials;

using namespace foundation::core;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::materials
{

    class PipelineStateCache
    {
    public:
        /// `framesInFlight`: how many frames a pipeline superseded by a reload is kept for after
        /// it (TickRetired), since frames still in flight may draw with it.
        PipelineStateCache(shaders::ShaderSystem& shaderSystem, rhi::Device& device, u32 framesInFlight = 3) noexcept
            : m_shaders(&shaderSystem), m_device(&device), m_framesInFlight(framesInFlight == 0 ? 1u : framesInFlight)
        {
        }

        ~PipelineStateCache()
        {
            Clear();
            ReleaseRetired();
        }

        PipelineStateCache(const PipelineStateCache&) = delete;
        PipelineStateCache& operator=(const PipelineStateCache&) = delete;

        // Returns a RenderPipeline for (config, layout, colorOverride), building it on the
        // first request and rebuilding it when the shader has since been reloaded. `layout`
        // is the assembled pipeline layout (frame/object/material bind-group layouts); the
        // caller owns it. Returns null on build failure; a failure is kept, not retried every
        // draw, until the shader's version changes (an edit that may fix it).
        [[nodiscard]] rhi::RenderPipeline*
        GetPipeline(const PipelineConfig& config, rhi::PipelineLayout* layout,
                    rhi::TextureFormat colorOverride = rhi::TextureFormat::Undefined)
        {
            const u64 hash = KeyOf(config, layout, colorOverride);
            const u64 version = m_shaders->Version(config.shaderName);
            Array<Entry>* bucket = m_entries.Find(hash);
            if (bucket != nullptr)
            {
                // Matched on the whole key, not the hash: two configs whose hashes collide are
                // two pipelines.
                for (Entry& e : *bucket)
                {
                    if (!e.Matches(config, layout, colorOverride))
                    {
                        continue;
                    }
                    if (e.builtVersion == version)
                    {
                        return e.pipeline; // null: it failed for this version of the shader
                    }
                    // The shader was reloaded since: the stale pipeline retires, one rebuilds.
                    if (e.pipeline != nullptr)
                    {
                        m_retired.PushBack(Retired{e.pipeline, m_framesInFlight});
                    }
                    e.pipeline = Build(config, layout, colorOverride);
                    e.builtVersion = version;
                    return e.pipeline;
                }
            }
            else
            {
                bucket = &m_entries.InsertOrAssign(hash, Array<Entry>{});
            }
            Entry entry;
            entry.shaderName = String(config.shaderName);
            entry.config = config;
            entry.config.shaderName = StringView{}; // the owned name is matched instead
            entry.layout = layout;
            entry.colorOverride = colorOverride;
            entry.pipeline = Build(config, layout, colorOverride);
            entry.builtVersion = version;
            rhi::RenderPipeline* result = entry.pipeline;
            bucket->PushBack(Move(entry));
            ++m_count;
            return result;
        }

        [[nodiscard]] usize Size() const noexcept { return m_count; }
        /// How many pipelines it has tried to build: a failure kept is not tried again.
        [[nodiscard]] u64 BuildAttempts() const noexcept { return m_builds; }
        [[nodiscard]] usize RetiredCount() const noexcept { return m_retired.Size(); }

        // Once a frame: frees the pipelines superseded by a reload whose frames in flight have
        // all completed (framesInFlight of these ticks after).
        void TickRetired()
        {
            for (usize i = 0; i < m_retired.Size();)
            {
                if (m_retired[i].framesLeft <= 1)
                {
                    if (m_retired[i].pipeline != nullptr)
                    {
                        m_device->DestroyRenderPipeline(m_retired[i].pipeline);
                    }
                    m_retired.RemoveAtSwap(i);
                }
                else
                {
                    --m_retired[i].framesLeft;
                    ++i;
                }
            }
        }

        // Frees every pipeline superseded by a reload now: the GPU is idle (shutdown, a test).
        void ReleaseRetired()
        {
            for (Retired& r : m_retired)
            {
                if (r.pipeline != nullptr)
                {
                    m_device->DestroyRenderPipeline(r.pipeline);
                }
            }
            m_retired.Clear();
        }

        // Destroys all live pipelines without retiring them; call at shutdown.
        void Clear()
        {
            for (auto& bucket : m_entries)
            {
                for (Entry& e : bucket.value)
                {
                    if (e.pipeline != nullptr)
                    {
                        m_device->DestroyRenderPipeline(e.pipeline);
                    }
                }
            }
            m_entries.Clear();
            m_count = 0;
        }

    private:
        // One pipeline and the whole key it was built for (the name owned: a config's is a view).
        struct Entry
        {
            String shaderName;
            PipelineConfig config; // its shaderName empty; shaderName above stands for it
            rhi::PipelineLayout* layout = nullptr;
            rhi::TextureFormat colorOverride = rhi::TextureFormat::Undefined;
            rhi::RenderPipeline* pipeline = nullptr;
            u64 builtVersion = 0;

            [[nodiscard]] bool Matches(const PipelineConfig& other, rhi::PipelineLayout* otherLayout,
                                       rhi::TextureFormat otherOverride) const noexcept
            {
                if (layout != otherLayout || colorOverride != otherOverride || shaderName.AsView() != other.shaderName)
                {
                    return false;
                }
                PipelineConfig named = config;
                named.shaderName = other.shaderName;
                return named == other;
            }
        };
        struct Retired
        {
            rhi::RenderPipeline* pipeline = nullptr;
            u32 framesLeft = 0;
        };

        static u64 KeyOf(const PipelineConfig& config, rhi::PipelineLayout* layout,
                         rhi::TextureFormat colorOverride)
        {
            u64 h = config.HashCode();
            h = h * 31u + reinterpret_cast<u64>(layout);
            h = h * 31u + static_cast<u64>(colorOverride);
            return h;
        }

        rhi::RenderPipeline* Build(const PipelineConfig& config, rhi::PipelineLayout* layout,
                                   rhi::TextureFormat colorOverride)
        {
            ++m_builds;
            rhi::ShaderModule* vs = m_shaders->GetVariant(
                config.shaderName, shaders::ShaderStage::Vertex, config.shaderFlags);
            if (vs == nullptr)
            {
                return nullptr;
            }

            rhi::RenderPipelineDesc desc{};
            desc.layout = layout;
            desc.label = config.shaderName;

            // --- vertex ---
            // Up to three buffers, in slot order matching the renderer's draw bindings: the mesh
            // stream (slot 0); the skinning stream (slot 1, joints at location 6 and weights at 7)
            // when skinned; the instance-stepped offsets stream (location 5) when instanced. That
            // gives [mesh], [mesh, skin], [mesh, offsets] or [mesh, skin, offsets]. The shaders pin
            // their input locations.
            rhi::VertexBufferLayout buffers[3] = {
                VertexLayoutHelper::BufferLayout(config.vertexLayout), {}, {}};
            u32 bufferCount = (config.vertexLayout != VertexLayoutType::None) ? 1u : 0u;
            if (config.vertexLayout == VertexLayoutType::SkinnedMesh)
            {
                buffers[bufferCount++] = VertexLayoutHelper::SkinningStreamBufferLayout();
            }
            if (config.instanced)
            {
                buffers[bufferCount++] = VertexLayoutHelper::InstanceOffsetsBufferLayout();
            }
            desc.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
            if (bufferCount > 0)
            {
                desc.vertex.buffers = Span<const rhi::VertexBufferLayout>{buffers, bufferCount};
            }

            // --- fragment (omitted for depth-only passes) ---
            // Up to colorTargetCount targets (MRT). Target 0 is the shaded color, blended per blendMode,
            // its format from colorOverride when set (the per-view HDR or LDR format). Targets 1+ are
            // the G-buffer aux outputs (view normal, motion vector), unblended, formats from
            // config.colorFormats.
            rhi::ColorTargetState colorTargets[rhi::MaxColorAttachments] = {};
            if (!config.depthOnly)
            {
                rhi::ShaderModule* fs = m_shaders->GetVariant(
                    config.shaderName, shaders::ShaderStage::Fragment, config.shaderFlags);
                if (fs == nullptr)
                {
                    return nullptr;
                }
                // Honor colorTargetCount as given, with no fallback to 1: 0 means a fragment that writes
                // no color (the masked shadow pass: alpha-test discard and depth only).
                const u32 count = Min<u32>(config.colorTargetCount, rhi::MaxColorAttachments);
                for (u32 i = 0; i < count; ++i)
                {
                    colorTargets[i].format =
                        (i == 0 && colorOverride != rhi::TextureFormat::Undefined)
                            ? colorOverride
                            : config.colorFormats[i];
                    colorTargets[i].blend =
                        (i == 0) ? BlendFor(config.blendMode) : Optional<rhi::BlendState>{};
                    // Aux G-buffer targets (1+) write only when writeAuxTargets is set. Transparent draws
                    // bind them to match the pass but leave the opaque normal and velocity underneath intact.
                    colorTargets[i].writeMask = (i == 0 || config.writeAuxTargets)
                                                    ? config.colorWriteMask
                                                    : rhi::ColorWriteMask::None;
                }
                rhi::FragmentState frag{};
                frag.shader = rhi::StageOf(fs, rhi::ShaderStage::Fragment);
                frag.targets = Span<const rhi::ColorTargetState>{colorTargets, count};
                desc.fragment = frag;
            }

            // --- primitive ---
            desc.primitive.topology = config.topology;
            desc.primitive.frontFace = config.frontFace;
            desc.primitive.cullMode = CullFor(config.cullMode);
            desc.primitive.fillMode = config.fillMode;

            // --- depth/stencil ---
            if (config.depthMode != DepthMode::Disabled)
            {
                rhi::DepthStencilState ds{};
                ds.format = config.depthFormat;
                ds.depthTestEnabled = config.depthMode == DepthMode::ReadWrite ||
                                      config.depthMode == DepthMode::ReadOnly;
                ds.depthWriteEnabled = config.depthMode == DepthMode::ReadWrite ||
                                       config.depthMode == DepthMode::WriteOnly;
                ds.depthCompare = config.depthCompare;
                ds.depthBias = config.depthBias;
                ds.depthBiasSlopeScale = config.depthBiasSlopeScale;
                desc.depthStencil = ds;
            }

            // --- multisample ---
            desc.multisample.count = config.sampleCount;

            rhi::RenderPipeline* pipeline = nullptr;
            if (!m_device->CreateRenderPipeline(desc, pipeline).IsOk())
            {
                return nullptr;
            }
            return pipeline;
        }

        static Optional<rhi::BlendState> BlendFor(BlendMode mode)
        {
            switch (mode)
            {
            case BlendMode::Opaque:
            case BlendMode::Masked:
                return {}; // no blending
            case BlendMode::AlphaBlend:
                return rhi::BlendState::AlphaBlend();
            case BlendMode::Additive:
                return rhi::BlendState{
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add}};
            case BlendMode::Multiply:
                return rhi::BlendState{
                    {rhi::BlendFactor::Dst, rhi::BlendFactor::Zero, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::Dst, rhi::BlendFactor::Zero, rhi::BlendOperation::Add}};
            case BlendMode::PremultipliedAlpha:
                return rhi::BlendState{{rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                        rhi::BlendOperation::Add},
                                       {rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                        rhi::BlendOperation::Add}};
            }
            return {};
        }

        static rhi::CullMode CullFor(CullModeConfig c)
        {
            switch (c)
            {
            case CullModeConfig::None:
                return rhi::CullMode::None;
            case CullModeConfig::Back:
                return rhi::CullMode::Back;
            case CullModeConfig::Front:
                return rhi::CullMode::Front;
            }
            return rhi::CullMode::None;
        }

        shaders::ShaderSystem* m_shaders; // borrowed
        rhi::Device* m_device;            // borrowed
        u32 m_framesInFlight = 3;
        HashMap<u64, Array<Entry>> m_entries; // by the key's hash; each entry holds the whole key
        usize m_count = 0;
        u64 m_builds = 0;
        Array<Retired> m_retired; // superseded by a reload, freed by TickRetired once idle
    };

} // namespace foundation::materials
