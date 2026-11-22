# Dialogue.Tooling (Plugins::Dialogue.Tooling) - the dialogue plugin's Tooling part, which the editor,
# the exporter and the MCP host load: its MCP tools, dialogue_validate and dialogue_read.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Editor::Mcp Editor::Project Foundation::Content Foundation::Json Foundation::Mcp
    Foundation::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE DialogueTooling.cpp) # the part's entry
