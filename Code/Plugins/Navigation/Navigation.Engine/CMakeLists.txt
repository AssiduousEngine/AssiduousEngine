# Navigation.Engine (Plugins::Navigation.Engine) - the navigation plugin's runtime: the zone and agent
# components, the per-scene navigation system and the NavigationSubsystem (`engine.navigation`).
# Every host loads it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene Foundation::Resource
           Foundation::Render Engine::Render
           Foundation::Script.Facades Foundation::Profiler Engine::Domain Foundation::Policy
    PRIVATE Engine::Composition) # EnginePluginServices, which takes the domain and the scene managers

target_sources(${PLUGIN_PART}
    PRIVATE
        NavigationEngine.cpp          # the part's entry
        NavigationSubsystemImpl.cpp   # REFLECT_VALUE bodies (kept out of interface units)
    PUBLIC
        FILE_SET navigation_engine_modules TYPE CXX_MODULES FILES
            NavigationModule.cppm
            NavigationComponents.cppm
            NavigationSubsystem.cppm)
