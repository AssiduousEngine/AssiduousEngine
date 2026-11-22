# Dialogue.Editor (Plugins::Dialogue.Editor) - `dialogue.editor`, the dialogue plugin's Editor part,
# which only the editor loads: the dialogue file's, the cast's and the dialogue assets' pages.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Editor::Core Editor::App
    PRIVATE Foundation::Runtime Foundation::Policy Foundation::GUI.Code Editor::Project)
target_sources(${PLUGIN_PART}
    PRIVATE
        DialogueEditor.cpp # the part's entry
        DialoguePages.cpp  # the pages and their factories
    PUBLIC
        FILE_SET dialogue_editor_modules TYPE CXX_MODULES FILES
            DialoguePages.cppm)
