# Foundation::Script.DotNet - hosts .NET (CoreCLR) in the engine for C# scripting, through hostfxr:
# finds the installed runtime, starts it from a runtimeconfig.json and hands out pointers to a C#
# assembly's [UnmanagedCallersOnly] methods. Desktop only (the web runs C# as NativeAOT-LLVM bitcode
# linked into the player). Experimental: OPTION_ENABLE_DOTNET, off by default.

util_add_engine_library(Script.DotNet ALIAS Foundation::Script.DotNet)
target_link_libraries(Script.DotNet PUBLIC Foundation::Core)

target_sources(Script.DotNet
    PUBLIC
        FILE_SET dotnet_modules TYPE CXX_MODULES FILES
            DotNetModule.cppm
            DotNetHost.cppm
            DotNetBridge.cppm
            DotNetBindings.cppm
    PRIVATE
        DotNetBridgeImpl.cpp
        DotNetBindingsImpl.cpp)
