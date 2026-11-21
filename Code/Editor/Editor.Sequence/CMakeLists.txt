# Editor::Sequence - `editor.sequence`: sequences authored in the scene (the document with its own
# history, the preview that poses the scene through the game's own play).
util_add_engine_library(Editor.Sequence ALIAS Editor::Sequence)
target_link_libraries(Editor.Sequence
    PUBLIC
        Foundation::Core Foundation::Content Foundation::Resource Foundation::Scene
        Foundation::Sequence Foundation::Sequence.Resource Pipeline::Sequence
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Editor::PropertyAnimation Foundation::PropertyAnimation Foundation::Policy)

target_sources(Editor.Sequence
    PRIVATE
        SequenceDocumentImpl.cpp
        SequencePreviewImpl.cpp
        SequenceSourceImpl.cpp
        SequencePanelImpl.cpp
    PUBLIC
        FILE_SET editorsequence_modules TYPE CXX_MODULES FILES
            SequenceDocument.cppm
            SequencePreview.cppm
            SequenceSource.cppm
            SequencePanel.cppm
            SequenceEditorModule.cppm)
