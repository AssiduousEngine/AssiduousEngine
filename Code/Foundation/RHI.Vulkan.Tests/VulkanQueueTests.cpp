// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vulkan backend: queue-family rules. Barriers must not name graphics stages on a compute-only
// family, a requested compute queue is granted when the adapter has a compute family, and the
// device-latched swap-chain semaphores belong to the graphics queue's submit whichever queue
// submits first. The stage rule is pure and pinned here; the device cases run on a real adapter and
// skip without one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "RHI.Vulkan/VkIncludes.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;

using namespace foundation::core;
using namespace foundation::rhi;

TEST_CASE("vk.barrier: stage masks are cut to what the recording queue family can execute")
{
    using vk::maskStagesForQueue;
    const u64 shaderAccess = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // Graphics executes everything: untouched.
    CHECK(maskStagesForQueue(shaderAccess, QueueType::Graphics) == shaderAccess);
    // Compute-only: the graphics half goes, the compute half stays.
    CHECK(maskStagesForQueue(shaderAccess, QueueType::Compute) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    // Transfer-only: no shader stage at all; a transfer stage survives.
    CHECK(maskStagesForQueue(shaderAccess | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, QueueType::Transfer) ==
          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    // A mask the cut would empty (a render-target state on a compute queue) becomes ALL_COMMANDS,
    // valid on every family and merely stronger, never zero, which is invalid.
    CHECK(maskStagesForQueue(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, QueueType::Compute) ==
          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    // Zero in, zero out (the caller decides what an empty mask means).
    CHECK(maskStagesForQueue(0, QueueType::Compute) == 0u);
    // Ray-tracing and indirect stages are legal on compute families and survive.
    CHECK(maskStagesForQueue(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, QueueType::Compute) ==
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
}

TEST_CASE("vk.barrier: the access half is cut with the stage half, as one pair")
{
    // Cutting stages alone leaves VERTEX_ATTRIBUTE_READ on a compute queue under ALL_COMMANDS,
    // which is invalid because ALL_COMMANDS expands per family and the compute expansion has no
    // vertex-input stage. Stage and access must move together.
    using vk::maskForQueue;
    using vk::getStageAccess;
    const vk::StageAccess vertex = getStageAccess(ResourceState::VertexBuffer);
    REQUIRE((vertex.accessMask & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) != 0);
    const vk::StageAccess onCompute = maskForQueue(vertex, QueueType::Compute);
    CHECK(onCompute.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(onCompute.accessMask == 0u); // execution-only: valid; a dangling graphics access is not
    // Shader access on compute keeps its shader bits; on transfer it keeps none.
    const vk::StageAccess write = getStageAccess(ResourceState::ShaderWrite);
    CHECK((maskForQueue(write, QueueType::Compute).accessMask & VK_ACCESS_2_SHADER_WRITE_BIT) != 0);
    CHECK((maskForQueue(write, QueueType::Transfer).accessMask & VK_ACCESS_2_SHADER_WRITE_BIT) == 0);
    // Graphics is untouched on both halves.
    const vk::StageAccess g = maskForQueue(vertex, QueueType::Graphics);
    CHECK(g.stageMask == vertex.stageMask);
    CHECK(g.accessMask == vertex.accessMask);
    // Transfer access survives everywhere.
    const vk::StageAccess copy = getStageAccess(ResourceState::CopyDst);
    CHECK(maskForQueue(copy, QueueType::Transfer).accessMask == copy.accessMask);
}

namespace
{
    // A device on the first adapter, with a compute queue requested; null when there is no
    // Vulkan adapter (CI) so the device cases skip rather than fail.
    Device* MakeDeviceWithComputeQueue(Backend*& outBackend)
    {
        outBackend = nullptr;
        (void)vk::CreateBackend(vk::VkBackendDesc{}, outBackend);
        if (outBackend == nullptr || outBackend->EnumerateAdapters().IsEmpty())
        {
            return nullptr;
        }
        DeviceDesc dd{};
        dd.computeQueueCount = 1;
        Device* device = nullptr;
        if (!outBackend->EnumerateAdapters()[0]->CreateDevice(dd, device).IsOk())
        {
            return nullptr;
        }
        return device;
    }
}

TEST_CASE("vk.queues: a requested compute queue is granted exactly when the adapter has a compute-only family")
{
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    auto* adapter = static_cast<vk::VkAdapterImpl*>(backend->EnumerateAdapters()[0]);
    const u32 expected = adapter->findQueueFamily(QueueType::Compute) >= 0 ? 1u : 0u;
    CHECK(device->GetQueueCount(QueueType::Compute) == expected);
    CHECK(device->GetQueueCount(QueueType::Graphics) >= 1u);
    MESSAGE("compute queues: ", device->GetQueueCount(QueueType::Compute));

    // A barrier with shader access recorded on the compute family: the masked stages are what
    // reach the driver. Records and finishes cleanly (validation layers, when present, would
    // flag a graphics stage here).
    if (expected == 1u)
    {
        CommandPool* pool = nullptr;
        REQUIRE(device->CreateCommandPool(QueueType::Compute, pool).IsOk());
        CommandEncoder* enc = nullptr;
        REQUIRE(pool->CreateEncoder(enc).IsOk());
        REQUIRE(enc != nullptr);
        MemoryBarrier mb{};
        mb.oldState = ResourceState::ShaderWrite;
        mb.newState = ResourceState::ShaderRead;
        // The MultiQueue sample's hand-over: a vertex buffer handed to compute as storage, whose
        // barrier would carry VERTEX_ATTRIBUTE_READ onto the compute family unmasked.
        BufferDesc bd{};
        bd.size = 256;
        bd.usage = BufferUsage::Vertex | BufferUsage::Storage;
        bd.label = u8"vk.queues.vertex";
        BufferHandle buffer{};
        REQUIRE(device->CreateBuffer(bd, buffer).IsOk());
        BufferBarrier bb{};
        bb.buffer = buffer;
        bb.oldState = ResourceState::VertexBuffer;
        bb.newState = ResourceState::ShaderWrite;
        BarrierGroup group{};
        group.memoryBarriers = Span<const MemoryBarrier>(&mb, 1);
        group.bufferBarriers = Span<const BufferBarrier>(&bb, 1);
        enc->Barrier(group);
        CommandBuffer* cb = enc->Finish();
        CHECK(cb != nullptr);
        pool->DestroyEncoder(enc);
        device->DestroyCommandPool(pool);
        device->DestroyBuffer(buffer);
    }
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("vk.upload: a texture written through a transfer batch is tracked as shader-read")
{
    // The batch moved the image to SHADER_READ_ONLY on the GPU but left its tracked layout
    // UNDEFINED, so the encoder's next barrier on it started from UNDEFINED and discarded the upload.
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    TextureDesc td{};
    td.format = TextureFormat::RGBA8Unorm;
    td.width = 2;
    td.height = 2;
    td.arrayLayerCount = 2;
    td.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
    td.label = u8"vk.upload";
    Texture* texture = nullptr;
    REQUIRE(device->CreateTexture(td, texture).IsOk());
    auto* vkTexture = static_cast<vk::VkTextureImpl*>(texture);
    CHECK(vkTexture->getSubresourceLayout(0, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

    Queue* queue = device->GetQueue(QueueType::Graphics);
    REQUIRE(queue != nullptr);
    TransferBatch* batch = nullptr;
    REQUIRE(queue->CreateTransferBatch(batch).IsOk());
    const u8 pixels[16] = {};
    TextureDataLayout layout{};
    layout.bytesPerRow = 8;
    layout.rowsPerImage = 2;
    batch->WriteTexture(texture, Span<const u8>{pixels, 16}, layout, Extent3D{2, 2, 1}, 0, 1);
    REQUIRE(batch->Submit().IsOk());
    CHECK(vkTexture->getSubresourceLayout(0, 1) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vkTexture->getSubresourceLayout(0, 0) == VK_IMAGE_LAYOUT_UNDEFINED); // not written

    // Writing it again starts from the tracked layout and leaves it shader-read.
    batch->Reset();
    batch->WriteTexture(texture, Span<const u8>{pixels, 16}, layout, Extent3D{2, 2, 1}, 0, 0);
    REQUIRE(batch->Submit().IsOk());
    CHECK(vkTexture->getSubresourceLayout(0, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vkTexture->currentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // collapsed

    queue->DestroyTransferBatch(batch);
    device->DestroyTexture(texture);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("vk.submit: a fenced submit given no fence still runs its command buffers")
{
    // The fenced overload returned early on a null fence: the commands never ran, and in a frame
    // submit the acquire semaphore was never waited on.
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    BufferDesc bd{};
    bd.size = 16;
    bd.memory = MemoryLocation::CpuToGpu;
    bd.usage = BufferUsage::CopySrc;
    bd.label = u8"vk.submit.src";
    BufferHandle src{};
    REQUIRE(device->CreateBuffer(bd, src).IsOk());
    bd.memory = MemoryLocation::GpuToCpu;
    bd.usage = BufferUsage::CopyDst;
    bd.label = u8"vk.submit.dst";
    BufferHandle dst{};
    REQUIRE(device->CreateBuffer(bd, dst).IsOk());
    void* srcBytes = device->Map(src);
    REQUIRE(srcBytes != nullptr);
    for (u32 i = 0; i < 16; ++i)
    {
        static_cast<u8*>(srcBytes)[i] = static_cast<u8>(i + 1);
    }
    device->Unmap(src);
    void* dstBytes = device->Map(dst);
    REQUIRE(dstBytes != nullptr);
    MemSet(dstBytes, 0, 16);
    device->Unmap(dst);

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());
    enc->CopyBufferToBuffer(src, 0, dst, 0, 16);
    CommandBuffer* cb = enc->Finish();
    REQUIRE(cb != nullptr);
    CommandBuffer* const cbs[] = {cb};
    device->GetQueue(QueueType::Graphics)->Submit(Span<CommandBuffer* const>{cbs, 1}, nullptr, 0);
    device->WaitIdle();

    dstBytes = device->Map(dst);
    REQUIRE(dstBytes != nullptr);
    CHECK(static_cast<const u8*>(dstBytes)[0] == 1u);
    CHECK(static_cast<const u8*>(dstBytes)[15] == 16u);
    device->Unmap(dst);

    pool->DestroyEncoder(enc);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("vk.lifetime: a buffer destroyed after commands naming it were recorded lives until those commands ran")
{
    // The commands copy into the buffer; it is destroyed before they are submitted. Its memory must
    // stay until the queue's timeline passes their submission, and go after.
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    BufferDesc bd{};
    bd.size = 4096;
    bd.memory = MemoryLocation::CpuToGpu;
    bd.usage = BufferUsage::CopySrc;
    bd.label = u8"vk.lifetime.src";
    BufferHandle src{};
    REQUIRE(device->CreateBuffer(bd, src).IsOk());
    bd.memory = MemoryLocation::GpuOnly;
    bd.usage = BufferUsage::CopyDst;
    bd.label = u8"vk.lifetime.dst";
    BufferHandle dst{};
    REQUIRE(device->CreateBuffer(bd, dst).IsOk());
    const u64 held = device->Counters().bufferBytes;

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());
    enc->CopyBufferToBuffer(src, 0, dst, 0, 4096);
    const BufferHandle kept = dst;
    device->DestroyBuffer(dst);
    CHECK(device->Describe(kept) == nullptr);          // stale at once
    CHECK(device->Counters().bufferBytes == held);       // its memory stays: the recording names it
    CommandBuffer* cb = enc->Finish();
    pool->DestroyEncoder(enc);
    CHECK(device->Counters().bufferBytes == held);       // finished, not submitted
    CommandBuffer* const cbs[] = {cb};
    device->GetQueue(QueueType::Graphics)->Submit(Span<CommandBuffer* const>{cbs, 1});
    device->WaitIdle();                                  // the timeline passes the submission
    CHECK(device->Counters().bufferBytes < held);        // freed

    device->DestroyCommandPool(pool);
    device->DestroyBuffer(src);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("vk.descriptors: plain and update-after-bind sets come from pools of their own kind")
{
    // One list held both kinds: an update-after-bind set could land in a plain pool (invalid),
    // and plain sets filled the update-after-bind pools.
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    const VkDevice vkDevice = static_cast<vk::VkDeviceImpl*>(device)->handle();
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(vkDevice, &lci, nullptr, &empty) == VK_SUCCESS);
    {
        vk::VkDescriptorPoolManager pools(vkDevice, 4);
        VkDescriptorPool plain = VK_NULL_HANDLE;
        REQUIRE(pools.allocate(empty, plain).IsOk());
        CHECK(pools.poolCount(false) == 1u);
        CHECK(pools.poolCount(true) == 0u);
        VkDescriptorPool afterBind = VK_NULL_HANDLE;
        REQUIRE(pools.allocate(empty, afterBind, /*updateAfterBind*/ true).IsOk());
        CHECK(pools.poolCount(true) == 1u);
        CHECK(afterBind != plain);
        pools.Destroy();
    }
    vkDestroyDescriptorSetLayout(vkDevice, empty, nullptr);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("vk.barrier: a storage image read and written takes the GENERAL layout")
{
    // ShaderRead | ShaderWrite (the render graph's ReadWriteStorage) was tested for its read first
    // and took SHADER_READ_ONLY_OPTIMAL, a layout a storage write may not use.
    CHECK(vk::getImageLayout(ResourceState::ShaderRead | ResourceState::ShaderWrite, TextureFormat::RGBA16Float) ==
          VK_IMAGE_LAYOUT_GENERAL);
    CHECK(vk::getImageLayout(ResourceState::ShaderRead, TextureFormat::RGBA16Float) ==
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
