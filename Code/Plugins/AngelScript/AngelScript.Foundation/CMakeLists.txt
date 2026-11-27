# AngelScript.Foundation (Plugins::AngelScript.Foundation) - the AngelScript language: its VM, the
# script manager and contexts over it, and the backend's registration. Not a part: the Engine part
# registers the backend from it and the Pipeline part's cook compiles with it. The SDK is private:
# consumers see only the module, never the AngelScript C++ API.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Script Foundation::Policy
    PRIVATE ThirdParty::AngelScript)

target_sources(${PLUGIN_LIBRARY}
    PRIVATE
        AngelScriptScriptImpl.cpp # module implementation unit: all SDK contact
    PUBLIC
        FILE_SET angelscript_modules TYPE CXX_MODULES FILES
            AngelScriptScript.cppm)

# What links the language knows it is there: `#ifdef HAS_PLUGIN_ANGELSCRIPT`.
target_compile_definitions(${PLUGIN_LIBRARY} INTERFACE HAS_PLUGIN_ANGELSCRIPT)
