# CSharp.Engine (Plugins::CSharp.Engine) - the part every host loads: it registers the C# backend, so
# a run can start a C# context, and takes it back as the plugin unloads.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC Foundation::Core Foundation::Runtime Foundation::Script Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE CSharpEngine.cpp) # the part's entry
# Where .NET is hosted, the managed runtime it runs in is built beside the binaries.
if(NOT EMSCRIPTEN)
    add_dependencies(${PLUGIN_PART} DotNet.Runtime)
endif()
