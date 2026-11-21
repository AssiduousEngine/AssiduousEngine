# Dialogue.Pipeline (Plugins::Dialogue.Pipeline) - `dialogue.pipeline`, the dialogue plugin's
# Pipeline part, which the editor and the command-line tools load: the format's parser, the declared
# commands' notation, the cook's checks, and the dialogue's assets with their builders, importer and
# creators.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Content Foundation::VFS Pipeline::Core Pipeline::Importer
           Pipeline::Input # the input maps' actions a line may name
    PRIVATE Foundation::Runtime Foundation::Policy Foundation::Input Foundation::Scene Foundation::Scene.Resource
            Engine::Composition) # a level loaded into a scratch scene for its entities
target_sources(${PLUGIN_PART}
    PRIVATE
        DialoguePipeline.cpp # the part's entry
        DialogueParse.cpp    # the parser
        DialogueCheck.cpp    # the cook's checks
        DialogueAssets.cpp   # the assets, builders, importer and creators
    PUBLIC
        FILE_SET dialogue_pipeline_modules TYPE CXX_MODULES FILES
            DialoguePipelineModule.cppm
            DialogueParse.cppm
            DialogueCheck.cppm
            DialogueAssets.cppm)
