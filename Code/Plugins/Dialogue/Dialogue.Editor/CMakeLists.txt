# Dialogue.Editor (Plugins::Dialogue.Editor) - the dialogue plugin's Editor part, which only the editor loads.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE DialogueEditor.cpp) # the part's entry
