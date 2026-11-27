# Particles.Pipeline (Plugins::Particles.Pipeline) - `particles.pipeline`: the edit-time
# ParticleEffectAsset, its builder (the bake) and File > New's creators. The editor and the tools
# load it; a player never does.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Pipeline::Core Pipeline::Importer Foundation::Content Foundation::Policy
    PRIVATE Foundation::Runtime)

target_sources(${PLUGIN_PART}
    PRIVATE
        ParticlesPipeline.cpp     # the part's entry
        ParticlesCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET particles_editor_modules TYPE CXX_MODULES FILES
            ParticleEffectAsset.cppm)
