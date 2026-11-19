# Engine::Behavior - `engine.behavior`: entity behaviors
# (BehaviorComponent, per-scene dispatch, the run's single gameplay script context).
# Backend-neutral: links Foundation::Script only for the registry; entry points
# register concrete backends.
util_add_engine_library(Engine.Behavior ALIAS Engine::Behavior)
target_link_libraries(Engine.Behavior
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Run Foundation::Scene Engine::Scene
           Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
           Foundation::Resource Foundation::Content
           Foundation::Profiler Engine::Domain Foundation::Policy)

target_sources(Engine.Behavior
    PRIVATE
        BehaviorSubsystemImpl.cpp   # implementation unit: the REFLECT_* bodies (GCC gcm rule)
    PUBLIC
        FILE_SET behavior_subsystem_modules TYPE CXX_MODULES FILES
            BehaviorSubsystem.cppm
            BehaviorComponents.cppm)

