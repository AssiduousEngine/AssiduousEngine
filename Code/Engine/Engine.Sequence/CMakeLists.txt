# Engine::Sequence - `engine.sequence`: a scene's sequences as they play. The scene system that
# steps every play, where plays report (the run's bus, or their owner's entity events), the script
# surface (scene.sequences, SequencePlay) and the domain. It depends on no domain it drives: each
# domain adds its own track kinds.

util_add_engine_library(Engine.Sequence ALIAS Engine::Sequence)
target_link_libraries(Engine.Sequence PUBLIC
    Foundation::Core Foundation::Messaging Foundation::Resource Foundation::Scene Foundation::Script.Facades
    Foundation::Sequence Foundation::Sequence.Resource Foundation::Profiler Engine::Domain Foundation::Policy)

target_sources(Engine.Sequence
    PRIVATE
        SequenceSubsystemImpl.cpp
    PUBLIC
        FILE_SET engine_sequence_modules TYPE CXX_MODULES FILES
            SequenceSubsystem.cppm)
