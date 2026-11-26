# Dialogue.Engine (Plugins::Dialogue.Engine) - `dialogue.engine`, the dialogue plugin's runtime: the
# runner each run holds with its staging, the `Dialogue` facade and the stock box, over the types in
# Dialogue.Foundation. Every host loads it; it is declared from the package's Plugin.xml and linked
# into the engine's players, editor and tools as one of the official plugins a project may enable.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Resource Foundation::Content Foundation::Policy
           Foundation::Run      # the runner is a run's service
           Engine::Run          # the run's save, and the contribution that installs the runner
           Foundation::Scene Foundation::Script Foundation::Script.Facades # the notices' scene and entities
           Foundation::Sequence # the line track
           Foundation::GUI Foundation::GUI.Gamekit Engine::GUI # the stock box
    PRIVATE Engine::Domain      # the dialogue's domain
            Engine::Composition # EnginePluginServices, which takes it
            Engine::Behavior    # the services the run's scripts find (its resources, its input)
            Foundation::Input Foundation::Sequence.Resource Foundation::GUI.Resources
            Engine::Animation Engine::Audio Engine::Render Engine::Sequence # the staging
)
target_sources(${PLUGIN_PART}
    PRIVATE
        DialogueEngine.cpp       # the part's entry
        DialogueRunner.cpp       # the runner
        DialogueRun.cpp          # the runner a run holds, with the run's host
        DialogueFacade.cpp       # the `Dialogue` facade
        DialogueLineTrack.cpp    # lines on a sequence's timeline
        DialogueStockBox.cpp     # the plugin's own dialogue box
    PUBLIC
        FILE_SET dialogue_engine_modules TYPE CXX_MODULES FILES
            DialogueEngineModule.cppm
            DialogueRunner.cppm
            DialogueRun.cppm
            DialogueFacade.cppm
            DialogueLineTrack.cppm
            DialogueStockBox.cppm)
