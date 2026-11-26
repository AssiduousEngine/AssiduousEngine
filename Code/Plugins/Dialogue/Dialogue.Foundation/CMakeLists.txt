# Dialogue.Foundation (Plugins::Dialogue.Foundation) - `dialogue.foundation`, the dialogue plugin's
# types: its settings, the compiled conversations and the cast, and the factories that load them.
# Not a part: every part links it, and no host loads it.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy
           Foundation::Settings # the settings' attributes
)
target_sources(${PLUGIN_LIBRARY}
    PRIVATE
        DialogueSettings.cpp     # the settings type's reflection
        DialogueConversation.cpp # the compiled conversations
        DialogueCast.cpp         # the cast
        DialogueResources.cpp    # the types' registration
    PUBLIC
        FILE_SET dialogue_foundation_modules TYPE CXX_MODULES FILES
            DialogueFoundationModule.cppm
            DialogueSettings.cppm
            DialogueConversation.cppm
            DialogueCast.cppm
            DialogueResources.cppm)
