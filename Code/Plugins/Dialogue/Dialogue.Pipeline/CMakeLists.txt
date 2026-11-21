# Dialogue.Pipeline (Plugins::Dialogue.Pipeline) - `dialogue.pipeline`, the dialogue plugin's
# Pipeline part, which the editor and the command-line tools load: the format's parser and the
# declared commands' notation.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART}
    PRIVATE
        DialoguePipeline.cpp # the part's entry
        DialogueParse.cpp    # the parser
    PUBLIC
        FILE_SET dialogue_pipeline_modules TYPE CXX_MODULES FILES
            DialoguePipelineModule.cppm
            DialogueParse.cppm)
