// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Shader helpers: an RHI sample's shaders are files under Data/Samples/RHI (each naming its entry
/// points in its own comments, `// entry: vertex VSMain`), compiled with DXC as the sample starts,
/// SPIR-V for Vulkan and WebGPU and DXIL for DX12, with the shared direct-compile options. Where
/// there is no shader compiler (Android) they come from the pack Tools.ShaderPack --entries cooks
/// from the same files (Data/Samples/RHI/shaders.dpak).

export module samples.framework:shader_helpers;

import foundation.core;
import foundation.rhi;
import foundation.shaders;

using namespace foundation::core;
using namespace foundation;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

export namespace samples::framework
{

    /// Compile HLSL source to a ShaderModule with a specific shader model.
    inline Status CompileToModule(shaders::Compiler* compiler, rhi::Device* device,
                                  StringView hlslSource, shaders::ShaderStage stage,
                                  StringView entryPoint, StringView label, StringView shaderModel,
                                  rhi::ShaderModule*& out)
    {
        out = nullptr;

        bool isDX12 = (device->type == rhi::DeviceType::DX12);
        auto target = isDX12 ? shaders::ShaderTarget::DXIL : shaders::ShaderTarget::SPIRV;

        // The options a cooked pack of the same file uses too (Tools.ShaderPack --entries).
        shaders::CompileOptions opts =
            shaders::DirectCompileOptions(target, shaderModel, device->type == rhi::DeviceType::WebGPU);
        opts.sourceName = label;

        shaders::CompileResult cr{};
        Status r = compiler->compile(reinterpret_cast<const u8*>(hlslSource.Data()),
                                     hlslSource.Size(), stage, entryPoint, target, opts, cr);

        if (r != ErrorCode::Ok)
        {
            if (cr.messages)
            {
                const String lbl = String(label);
                rhi::LogErrorf("Shader compile failed (%s): %s",
                               reinterpret_cast<const char*>(lbl.CStr()), cr.messages);
            }
            compiler->freeResult(cr);
            return ErrorCode::Unknown;
        }

        rhi::ShaderModuleDesc desc{};
        desc.code = Span<const u8>(cr.bytecode, cr.bytecodeSize);
        desc.label = label;
        desc.entryPoint = entryPoint;
        r = device->CreateShaderModule(desc, out);

        compiler->freeResult(cr);
        return r;
    }

    /// Compile HLSL source to a ShaderModule. Default shader model 6.0.
    inline Status CompileToModule(shaders::Compiler* compiler, rhi::Device* device,
                                  StringView hlslSource, shaders::ShaderStage stage,
                                  StringView entryPoint, StringView label, rhi::ShaderModule*& out)
    {
        return CompileToModule(compiler, device, hlslSource, stage, entryPoint, label, u8"6_0",
                               out);
    }

    /// Where an RHI sample's shaders come from: their files, compiled with DXC, or where there is no
    /// compiler, the pack cooked from them. Opened by SampleApp before the sample's OnInit.
    class SampleShaders
    {
    public:
        SampleShaders() = default;
        SampleShaders(const SampleShaders&) = delete;
        SampleShaders& operator=(const SampleShaders&) = delete;
        ~SampleShaders() { Close(); }

        /// `folder` holds the samples' shader files (Data/Samples/RHI); its shaders.dpak, if there
        /// is one, is read too, for when no compiler is (a cooked pack never wins over the files).
        void Open(IAllocator& allocator, StringView folder)
        {
            Close();
            m_folder = String(folder);
            if (!shaders::createCompiler(shaders::CompilerDesc{}, m_compiler).IsOk())
            {
                m_compiler = nullptr;
            }
            const String packPath = Format(u8"{}/shaders.dpak", folder);
            if (FileExists(packPath.AsView()))
            {
                FileStream in(packPath.AsView(), FileMode::Read);
                m_pack = MakeUnique<shaders::CookedShaderPack>(allocator);
                if (!in.IsValid() || !m_pack->Read(in).IsOk())
                {
                    rhi::LogErrorf("SampleShaders: '%s' could not be read", reinterpret_cast<const char*>(packPath.CStr()));
                    m_pack.Reset();
                }
            }
        }

        void Close()
        {
            if (m_compiler != nullptr)
            {
                m_compiler->Destroy();
                m_compiler = nullptr;
            }
            m_pack.Reset();
        }

        [[nodiscard]] bool HasCompiler() const noexcept { return m_compiler != nullptr; }
        [[nodiscard]] bool HasPack() const noexcept { return m_pack.Get() != nullptr; }

        /// The module for one entry of `file` (relative to the folder, "Sample001_Triangle/Triangle.hlsl").
        Status Load(rhi::Device* device, StringView file, shaders::ShaderStage stage, StringView entryPoint, StringView label,
                    StringView shaderModel, rhi::ShaderModule*& out)
        {
            out = nullptr;
            if (m_compiler != nullptr)
            {
                const String path = Format(u8"{}/{}", m_folder, file);
                const Result<Array<byte>> bytes = ReadFile(path.AsView());
                if (!bytes.HasValue())
                {
                    rhi::LogErrorf("SampleShaders: no shader file '%s'", reinterpret_cast<const char*>(path.CStr()));
                    return ErrorCode::NotFound;
                }
                const StringView source(reinterpret_cast<const char8_t*>(bytes.Value().Data()), bytes.Value().Size());
                return CompileToModule(m_compiler, device, source, stage, entryPoint, label, shaderModel, out);
            }
            const String name = shaders::EntryShaderName(file, entryPoint);
            if (m_pack.Get() == nullptr)
            {
                rhi::LogErrorf("SampleShaders: no shader compiler and no cooked pack for '%s'", reinterpret_cast<const char*>(name.CStr()));
                return ErrorCode::NotFound;
            }
            const shaders::CookedShaderFormat format =
                device->type == rhi::DeviceType::DX12 ? shaders::CookedShaderFormat::Dxil : shaders::CookedShaderFormat::SpirV;
            const Array<byte>* blob = m_pack->Find(name.AsView(), stage, shaders::ShaderFlags::None, format);
            if (blob == nullptr)
            {
                rhi::LogErrorf("SampleShaders: '%s' is not in the cooked pack (Tools.ShaderPack --entries)",
                               reinterpret_cast<const char*>(name.CStr()));
                return ErrorCode::NotFound;
            }
            rhi::ShaderModuleDesc desc{};
            desc.code = Span<const u8>(reinterpret_cast<const u8*>(blob->Data()), blob->Size());
            desc.label = label;
            desc.entryPoint = entryPoint;
            return device->CreateShaderModule(desc, out);
        }

    private:
        String m_folder;
        shaders::Compiler* m_compiler = nullptr;
        UniquePtr<shaders::CookedShaderPack> m_pack;
    };

} // namespace samples::framework
