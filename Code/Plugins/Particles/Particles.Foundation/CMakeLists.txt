# Particles.Foundation (Plugins::Particles.Foundation) - the particles plugin's types: the CPU
# particle runtime (`foundation.particles`: the SoA streams, the initializer and behavior modules,
# the effect, its systems and emitters, the simulator) and the cooked effect with its serializer and
# factory (`foundation.particles.resource`). Not a part: every part links it, and no host loads it.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Core Foundation::Content Foundation::Resource
           Foundation::Texture Foundation::Texture.Resource
           Foundation::Geometry Foundation::Geometry.Resource
           Foundation::Materials Foundation::Materials.Resource Foundation::Policy)

target_sources(${PLUGIN_LIBRARY}
    PRIVATE
        ParticleModulesImpl.cpp   # module and range-type reflection bodies (kept out of interfaces for GCC)
        ParticleEffectReflectionImpl.cpp # effect/system/emitter reflection bodies (kept out of interfaces for GCC)
    PUBLIC
        FILE_SET particles_modules TYPE CXX_MODULES FILES
            ParticleTypes.cppm
            ParticleStreams.cppm
            ParticleModules.cppm
            ParticleEffect.cppm
            ParticlesModule.cppm
            ParticleEffectResource.cppm)
