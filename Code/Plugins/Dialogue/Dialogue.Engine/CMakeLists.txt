# Dialogue.Engine (Plugins::Dialogue.Engine) - `dialogue.engine`, the dialogue plugin's runtime: its
# settings type. Every host loads it; it is declared from the package's Plugin.xml and linked into
# the engine's players, editor and tools as one of the official plugins a project may enable.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Policy
    PRIVATE Engine::Project) # the settings' attributes
target_sources(${PLUGIN_PART}
    PRIVATE
        DialogueEngine.cpp   # the part's entry
        DialogueSettings.cpp # the settings type's reflection
    PUBLIC
        FILE_SET dialogue_engine_modules TYPE CXX_MODULES FILES
            DialogueEngineModule.cppm
            DialogueSettings.cppm)
