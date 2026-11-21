# Dialogue.Pipeline (Plugins::Dialogue.Pipeline) - the dialogue plugin's Pipeline part, which the editor and the command-line tools load.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE DialoguePipeline.cpp) # the part's entry
