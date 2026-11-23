// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - :resource partition
//
// A resource managed by the graph (texture or buffer): its descriptor, the
// allocated GPU object, reference/lifetime tracking computed during compile, and
// barrier state. Fields are public data the graph orchestrator manipulates directly.

module;
#include "Core/Prelude.h"

export module foundation.rendergraph:resource;

import foundation.core;
import foundation.rhi;
import :types;
import :descriptors;
import :persistent_resource;

using namespace foundation::core;

export namespace foundation::rendergraph
{
    namespace rhi = foundation::rhi;

    class RenderGraphResource
    {
    public:
        RenderGraphResource(StringView resourceName, RGResourceType type, RGResourceLifetime life)
            : name(resourceName), resourceType(type), lifetime(life)
        {
        }

        // Allocate GPU resources for a transient texture.
        [[nodiscard]] Status AllocateTexture(rhi::Device& device)
        {
            rhi::TextureDesc rhiDesc = textureDesc.ToTextureDesc(name.AsView());
            rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::Sampled; // may be sampled
            if (rhi::IsDepthFormat(textureDesc.format))
            {
                rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::DepthStencil;
            }
            else
            {
                rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::RenderTarget;
            }

            // All-or-nothing: assign the members only once every view is created. Leaving `texture`
            // set with a null `textureView` after a view-create failure would poison the transient
            // pool and desync render-pass attachments (see ReturnTransientResources /
            // ExecuteRenderPass).
            rhi::TextureHandle tex{};
            if (!device.CreateTexture(rhiDesc, tex).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::TextureViewHandle view{};
            if (!device.CreateTextureView(tex, rhi::TextureViewDesc{}, view).IsOk())
            {
                device.DestroyTexture(tex);
                return Status{ErrorCode::Unknown};
            }

            rhi::TextureViewHandle depthOnly{};
            if (rhi::IsDepthFormat(textureDesc.format) && rhi::HasStencil(textureDesc.format))
            {
                rhi::TextureViewDesc depthDesc{};
                depthDesc.aspect = rhi::TextureAspect::DepthOnly;
                depthDesc.label = u8"RGDepthOnlyView";
                if (!device.CreateTextureView(tex, depthDesc, depthOnly).IsOk())
                {
                    device.DestroyTextureView(view);
                    device.DestroyTexture(tex);
                    return Status{ErrorCode::Unknown};
                }
            }

            texture = tex;
            textureView = view;
            depthOnlyView = depthOnly;
            TakeExtent(&rhiDesc);
            lastKnownState = rhi::ResourceState::Undefined;
            return Status{};
        }

        // Allocate GPU resources for a transient buffer.
        [[nodiscard]] Status AllocateBuffer(rhi::Device& device)
        {
            rhi::BufferDesc rhiDesc{};
            rhiDesc.size = bufferDesc.size;
            rhiDesc.usage = bufferDesc.usage;
            rhiDesc.label = name.AsView();

            rhi::BufferHandle buf{};
            if (!device.CreateBuffer(rhiDesc, buf).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            buffer = buf;
            lastKnownState = rhi::ResourceState::Undefined;
            return Status{};
        }

        // Release GPU resources for a transient resource (no-op otherwise).
        void ReleaseTransient(rhi::Device& device)
        {
            if (lifetime != RGResourceLifetime::Transient)
            {
                return;
            }
            if (depthOnlyView.IsValid())
            {
                device.DestroyTextureView(depthOnlyView);
            }
            if (textureView.IsValid())
            {
                device.DestroyTextureView(textureView);
            }
            if (texture.IsValid())
            {
                device.DestroyTexture(texture);
            }
            if (buffer.IsValid())
            {
                device.DestroyBuffer(buffer);
            }
        }

        /// The mip and layer counts of the texture it now holds (its description, read when it took
        /// the texture); 1 each without one.
        void TakeExtent(const rhi::TextureDesc* desc) noexcept
        {
            textureMipCount = desc != nullptr ? desc->mipLevelCount : 1;
            textureLayerCount = desc != nullptr ? desc->arrayLayerCount : 1;
        }

        [[nodiscard]] u32 TotalMipLevels() const
        {
            if (texture.IsValid())
            {
                return textureMipCount;
            }
            if (resourceType == RGResourceType::Texture)
            {
                return textureDesc.mipLevelCount;
            }
            return 1;
        }
        [[nodiscard]] u32 TotalArrayLayers() const
        {
            if (texture.IsValid())
            {
                return textureLayerCount;
            }
            if (resourceType == RGResourceType::Texture)
            {
                return textureDesc.arrayLayerCount;
            }
            return 1;
        }

        void ResetTracking()
        {
            refCount = 0;
            firstWriter = PassHandle::Invalid();
            lastReader = PassHandle::Invalid();
            firstUsePass = -1;
            lastUsePass = -1;
        }

        // --- identity / lifetime ---
        String name;
        RGResourceType resourceType;
        RGResourceLifetime lifetime;
        u32 generation = 1;

        // --- reference tracking (computed during compile) ---
        i32 refCount = 0;
        PassHandle firstWriter = PassHandle::Invalid();
        PassHandle lastReader = PassHandle::Invalid();
        i32 firstUsePass = -1; // the lifetime aliasing would need; nothing reads it yet
        i32 lastUsePass = -1;

        // --- texture data ---
        RGTextureDesc textureDesc;
        rhi::TextureHandle texture{};
        rhi::TextureViewHandle textureView{};
        rhi::TextureViewHandle depthOnlyView{};
        u32 textureMipCount = 1;   // of `texture` (TakeExtent)
        u32 textureLayerCount = 1;
        // Stable id of the backing physical texture; changes when a transient is (re)allocated a
        // different texture (e.g. on resize). Consumers caching a bind group over textureView key on
        // this so a reused-address view does not alias a stale, destroyed texture.
        u64 textureGeneration = 0;

        // --- buffer data ---
        RGBufferDesc bufferDesc;
        rhi::BufferHandle buffer{};

        // --- state tracking ---
        rhi::ResourceState lastKnownState = rhi::ResourceState::Undefined;
        Optional<rhi::ResourceState> finalState; // transition-to after last use (imported)
        bool readableAfterWrite = false;         // transition to ShaderRead after last writer

        // --- persistent data (null for transient/imported) ---
        UniquePtr<PersistentResource> persistentData;
    };
}
