# CSharp.Foundation (Plugins::CSharp.Foundation) - the C# language: hosts .NET (CoreCLR) through
# hostfxr (finds the installed runtime, starts it from a runtimeconfig.json and hands out pointers to
# a C# assembly's [UnmanagedCallersOnly] methods), or calls the runtime compiled into the program
# (an exported player; the web runs C# as NativeAOT-LLVM bitcode linked in). Not a part: the Engine
# part registers the backend from it and the Pipeline part's cook builds and describes with it.

util_add_plugin_library()
# The facades and the scene: a scene component crosses to C# as its entity (a value, no handle).
target_link_libraries(${PLUGIN_LIBRARY} PUBLIC Foundation::Core Foundation::Script PRIVATE Foundation::Script.Facades Foundation::Scene)

target_sources(${PLUGIN_LIBRARY}
    PUBLIC
        FILE_SET dotnet_modules TYPE CXX_MODULES FILES
            DotNetModule.cppm
            DotNetHost.cppm
            DotNetBridge.cppm
            DotNetBindings.cppm
            DotNetRuntime.cppm
            DotNetScript.cppm
    PRIVATE
        DotNetBridgeImpl.cpp
        DotNetBindingsImpl.cpp
        DotNetRuntimeImpl.cpp
        DotNetScriptImpl.cpp
        DotNetCompiledIn.cpp)

# What links the language knows it is there: `#ifdef HAS_PLUGIN_CSHARP`.
target_compile_definitions(${PLUGIN_LIBRARY} INTERFACE HAS_PLUGIN_CSHARP)
