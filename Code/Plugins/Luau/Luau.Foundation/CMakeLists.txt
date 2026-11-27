# Luau.Foundation (Plugins::Luau.Foundation) - the Luau language: its VM, the script manager and
# contexts over it, and the backend's registration (`foundation.script.luau`). Not a part: the Engine
# part registers the backend from it and the Pipeline part's cook compiles with it.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Script Foundation::Script.Facades Foundation::Policy
    PRIVATE ThirdParty::Luau)

target_sources(${PLUGIN_LIBRARY}
    PUBLIC
        FILE_SET luau_modules TYPE CXX_MODULES FILES
            LuauScript.cppm)
