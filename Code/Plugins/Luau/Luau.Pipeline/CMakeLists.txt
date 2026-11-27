# Luau.Pipeline (Plugins::Luau.Pipeline) - `luau.pipeline`: the Luau cook (check, cook and
# the new-file starters), which compiles with the VM. The editor and the tools load it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
           Pipeline::Script Foundation::Policy
    PRIVATE Foundation::Profiler Foundation::Runtime Pipeline::Importer)

target_sources(${PLUGIN_PART}
    PRIVATE
        LuauPipeline.cpp       # the part's entry
        LuauScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_luau_pipeline_modules TYPE CXX_MODULES FILES LuauScriptCook.cppm)
