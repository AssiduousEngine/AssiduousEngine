# CSharp.Tooling (Plugins::CSharp.Tooling) - the csharp plugin's Tooling part, which the editor, the
# exporter and the MCP host load: the export's player hook. A project's C# is compiled into its own
# player, so the export builds one, configured with the project's C# folder (and, for the web, the
# bindings and the emsdk NativeAOT-LLVM builds with).
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Editor::Project Foundation::Core Foundation::Policy
    PRIVATE Pipeline::Script Foundation::VFS Foundation::Runtime)
target_sources(${PLUGIN_PART}
    PRIVATE
        CSharpTooling.cpp # the part's entry
        CSharpExportImpl.cpp
    PUBLIC
        FILE_SET csharp_tooling_modules TYPE CXX_MODULES FILES CSharpExport.cppm)
