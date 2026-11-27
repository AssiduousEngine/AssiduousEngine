# Particles.Engine (Plugins::Particles.Engine) - `engine.particles`, the particles plugin's runtime:
# the ECS component and its manager, a dedicated ParticleRenderer and the ParticleSubsystem that
# registers it with the render subsystem, and the `scene.particles` facade. Every host loads it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
           Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Render Engine::Render Foundation::Resource Foundation::Content
           Foundation::Texture Foundation::Texture.Resource
           Foundation::Geometry Foundation::Materials Foundation::Script.Facades
           Engine::Domain Foundation::Policy Foundation::Profiler
    PRIVATE Engine::Composition) # EnginePluginServices, which takes the domain and the scene manager

target_sources(${PLUGIN_PART}
    PRIVATE
        ParticlesEngine.cpp         # the part's entry
        ParticleSubsystemImpl.cpp   # REFLECT_VALUE body (see ParticleComponents.cppm)
    PUBLIC
        FILE_SET particles_subsystem_modules TYPE CXX_MODULES FILES
            ParticleRenderData.cppm
            ParticleRenderer.cppm
            ParticleComponents.cppm
            ParticleSubsystem.cppm
            ParticlesSubsystemModule.cppm)
