# Dialogue.Tooling (Plugins::Dialogue.Tooling) - the dialogue plugin's Tooling part, which the editor, the exporter and the MCP host load.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE DialogueTooling.cpp) # the part's entry
