// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:compute_pipeline - ComputePipeline over WGPUComputePipeline.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:compute_pipeline;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;
import :pipeline_layout;
import :shader_module;
import :tables;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuComputePipeline final : public ComputePipeline
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device, const WebGpuTables* tables,
                          const ComputePipelineDesc& pipelineDesc)
        {
            m_api = &api;
            const WebGpuPipelineLayout* pipelineLayout = LookUp(tables, pipelineDesc.layout);
            const WebGpuShaderModule* module = LookUp(tables, pipelineDesc.compute.module);
            if (pipelineLayout == nullptr || module == nullptr)
            {
                return ErrorCode::InvalidArgument;
            }

            String entry(EntryPointOf(pipelineDesc.compute, module));
            if (entry.IsEmpty())
            {
                return ErrorCode::InvalidArgument; // no entry named by the stage or its module
            }
            WGPUComputePipelineDescriptor wgpuDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(pipelineDesc.label);
            wgpuDesc.layout = pipelineLayout->Handle();
            m_pushConstants = pipelineLayout->EmulationInfo();
            wgpuDesc.compute.module = module->Handle();
            wgpuDesc.compute.entryPoint = ToWgpuStringView(entry.AsView());

            m_pipeline = api.wgpuDeviceCreateComputePipeline(device, &wgpuDesc);
            return m_pipeline != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_pipeline != nullptr)
            {
                m_api->wgpuComputePipelineRelease(m_pipeline);
                m_pipeline = nullptr;
            }
        }

        [[nodiscard]] WGPUComputePipeline Handle() const { return m_pipeline; }
        [[nodiscard]] const PushConstantEmulation& PushConstants() const { return m_pushConstants; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUComputePipeline m_pipeline = nullptr;
        PushConstantEmulation m_pushConstants;
    };
}
