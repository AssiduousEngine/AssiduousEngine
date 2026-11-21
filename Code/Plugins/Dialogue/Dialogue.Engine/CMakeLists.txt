# Dialogue.Engine (Plugins::Dialogue.Engine) - `dialogue.engine`, the dialogue plugin's runtime: its
# settings type, the compiled conversations and the cast, and the factories that load them. Every
# host loads it; it is declared from the package's Plugin.xml and linked into the engine's players,
# editor and tools as one of the official plugins a project may enable.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Resource Foundation::Content Foundation::Policy
    PRIVATE Engine::Project     # the settings' attributes
            Engine::Domain      # the dialogue's domain
            Engine::Composition # EnginePluginServices, which takes it
)
target_sources(${PLUGIN_PART}
    PRIVATE
        DialogueEngine.cpp       # the part's entry
        DialogueSettings.cpp     # the settings type's reflection
        DialogueConversation.cpp # the compiled conversations
        DialogueCast.cpp         # the cast
        DialogueResources.cpp    # the types' registration
    PUBLIC
        FILE_SET dialogue_engine_modules TYPE CXX_MODULES FILES
            DialogueEngineModule.cppm
            DialogueSettings.cppm
            DialogueConversation.cppm
            DialogueCast.cppm
            DialogueResources.cppm)
