// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI.Null.Tests: textures, views and samplers by handle, and when a destroyed one is really freed.
// A destroyed one's handle is stale at once; the object waits, as a buffer does, for every
// recording begun before the destroy (the Null device's submissions complete as they are made).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;

using namespace foundation::core;
using namespace foundation::rhi;

namespace
{
    struct NullDeviceFixture
    {
        Backend* backend = nullptr;
        Device* device = nullptr;
        NullDeviceFixture()
        {
            REQUIRE(null::CreateNullBackend(backend).IsOk());
            REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
        }
        ~NullDeviceFixture()
        {
            device->Destroy();
            backend->Destroy();
        }
        [[nodiscard]] TextureHandle MakeTexture(u32 size = 16)
        {
            TextureDesc desc{};
            desc.width = size;
            desc.height = size;
            desc.format = TextureFormat::RGBA8Unorm;
            desc.usage = TextureUsage::Sampled;
            TextureHandle texture{};
            REQUIRE(device->CreateTexture(desc, texture).IsOk());
            return texture;
        }
        [[nodiscard]] u64 Textures() const { return device->Counters().textures; }
    };
}

TEST_CASE("texture lifetime: destroyed textures, views and samplers are stale at once, every copy, and calls on them do nothing")
{
    NullDeviceFixture f;
    TextureHandle texture = f.MakeTexture(32);
    TextureViewHandle view{};
    REQUIRE(f.device->CreateTextureView(texture, TextureViewDesc{}, view).IsOk());
    SamplerDesc sd{};
    sd.maxAnisotropy = 4;
    SamplerHandle sampler{};
    REQUIRE(f.device->CreateSampler(sd, sampler).IsOk());
    CHECK(f.device->Describe(texture)->width == 32u);
    CHECK(f.device->TextureOf(view) == texture);
    CHECK(f.device->Describe(sampler)->maxAnisotropy == 4u);

    const TextureHandle textureCopy = texture;
    const TextureViewHandle viewCopy = view;
    const SamplerHandle samplerCopy = sampler;
    f.device->DestroySampler(sampler);
    f.device->DestroyTextureView(view);
    f.device->DestroyTexture(texture);
    CHECK_FALSE(texture.IsValid()); // the caller's own copies are cleared
    CHECK_FALSE(view.IsValid());
    CHECK_FALSE(sampler.IsValid());
    CHECK(f.device->Describe(textureCopy) == nullptr);
    CHECK(f.device->Describe(viewCopy) == nullptr);
    CHECK_FALSE(f.device->TextureOf(viewCopy).IsValid());
    CHECK(f.device->Describe(samplerCopy) == nullptr);

    TextureViewHandle fromStale{};
    CHECK_FALSE(f.device->CreateTextureView(textureCopy, TextureViewDesc{}, fromStale).IsOk());
    CHECK_FALSE(fromStale.IsValid());
    TextureHandle again = textureCopy;
    f.device->DestroyTexture(again); // destroying twice does nothing
    CHECK_FALSE(again.IsValid());

    // A new texture in the freed slot is another handle; the old one still names nothing.
    const TextureHandle next = f.MakeTexture(8);
    CHECK(next != textureCopy);
    CHECK(f.device->Describe(textureCopy) == nullptr);
    CHECK(f.device->Describe(next)->width == 8u);
}

TEST_CASE("texture lifetime: a view outlives its texture's destroy, and then names a stale texture")
{
    NullDeviceFixture f;
    TextureHandle texture = f.MakeTexture();
    TextureViewHandle view{};
    REQUIRE(f.device->CreateTextureView(texture, TextureViewDesc{}, view).IsOk());
    const TextureHandle kept = texture;
    f.device->DestroyTexture(texture);
    REQUIRE(f.device->Describe(view) != nullptr); // the view is still its own
    CHECK(f.device->TextureOf(view) == kept);
    CHECK(f.device->Describe(f.device->TextureOf(view)) == nullptr);
    f.device->DestroyTextureView(view);
}

TEST_CASE("texture lifetime: on the Null device, a texture destroyed while a recording made before it is open is freed at that recording's submit")
{
    NullDeviceFixture f;
    const u64 before = f.Textures();
    TextureHandle texture = f.MakeTexture();
    CHECK(f.Textures() == before + 1u);

    CommandPool* pool = nullptr;
    REQUIRE(f.device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    f.device->DestroyTexture(texture);
    CHECK(f.Textures() == before + 1u); // the open recording may name it
    CommandBuffer* cb = encoder->Finish();
    pool->DestroyEncoder(encoder);
    CHECK(f.Textures() == before + 1u); // finished, not submitted
    CommandBuffer* cbs[1] = {cb};
    f.device->GetQueue(QueueType::Graphics)->Submit(Span<CommandBuffer* const>(cbs, 1));
    CHECK(f.Textures() == before); // submitted and (on Null) complete: freed
    f.device->DestroyCommandPool(pool);
}

TEST_CASE("texture lifetime: a swap chain's image handles name its images while it lives, and nothing after")
{
    NullDeviceFixture f;
    SwapChainDesc desc{};
    desc.width = 64;
    desc.height = 32;
    desc.format = TextureFormat::BGRA8UnormSrgb;
    SwapChain* swapChain = nullptr;
    REQUIRE(f.device->CreateSwapChain(nullptr, desc, swapChain).IsOk());
    REQUIRE(swapChain->AcquireNextImage().IsOk());
    const TextureHandle image = swapChain->CurrentTexture();
    const TextureViewHandle imageView = swapChain->CurrentTextureView();
    REQUIRE(f.device->Describe(image) != nullptr);
    CHECK(f.device->TextureOf(imageView) == image);
    CHECK_FALSE(swapChain->CurrentAlternateTextureView().IsValid()); // none asked for
    f.device->DestroySwapChain(swapChain);
    CHECK(f.device->Describe(image) == nullptr);
    CHECK(f.device->Describe(imageView) == nullptr);
}

TEST_CASE("texture lifetime: a texture handle from one device is refused by another")
{
    NullDeviceFixture a;
    NullDeviceFixture b;
    const TextureHandle fromA = a.MakeTexture(4);
    const TextureHandle fromB = b.MakeTexture(8);
    CHECK(fromA.index == fromB.index); // the same slot in each device's table
    CHECK(b.device->Describe(fromA) == nullptr);
    CHECK(a.device->Describe(fromB) == nullptr);
    TextureViewHandle view{};
    CHECK_FALSE(b.device->CreateTextureView(fromA, TextureViewDesc{}, view).IsOk());
    CHECK(a.device->Describe(fromA)->width == 4u);
}
