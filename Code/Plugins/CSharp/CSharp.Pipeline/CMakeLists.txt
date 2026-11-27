# CSharp.Pipeline (Plugins::CSharp.Pipeline) - the C# cook. A C# project (.csproj) builds every .cs
# under its folder into one assembly with `dotnet build`, and the runtime describes its classes; each
# C# class cooks from that description. The editor and the tools load it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Script Foundation::Script.Resource Pipeline::Script Pipeline::Importer Foundation::Policy
    PRIVATE Foundation::Runtime)
# The cook builds against the engine's managed runtime, and describes with it.
add_dependencies(${PLUGIN_PART} DotNet.Runtime)
target_sources(${PLUGIN_PART}
    PRIVATE
        CSharpPipeline.cpp # the part's entry
        DotNetScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_dotnet_pipeline_modules TYPE CXX_MODULES FILES DotNetScriptCook.cppm)
