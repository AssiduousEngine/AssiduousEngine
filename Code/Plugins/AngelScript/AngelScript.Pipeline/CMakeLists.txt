# AngelScript.Pipeline (Plugins::AngelScript.Pipeline) - the AngelScript cook: the compile check, the
# shared handler scan, the [metadata] property harvest and the new-file starters, which compile with
# the VM. The editor and the tools load it. The SDK is private (the implementation unit includes
# angelscript.h and scriptbuilder for the harvest, which keeps them out of GCC's module interface).
# The editor colours AngelScript from its definition in Data/Editor/Code.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
           Pipeline::Script Foundation::Policy
    PRIVATE Foundation::Profiler Foundation::Runtime ThirdParty::AngelScript)

target_sources(${PLUGIN_PART}
    PRIVATE
        AngelScriptPipeline.cpp # the part's entry
        AngelScriptScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_as_pipeline_modules TYPE CXX_MODULES FILES AngelScriptScriptCook.cppm)
