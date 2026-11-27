# Terrain.Foundation (Plugins::Terrain.Foundation) - the terrain plugin's types: the terrain's
# chunks and LOD (`foundation.terrain`), the cooked terrain and splatmap (`foundation.terrain.resource`),
# vegetation scattering (`foundation.vegetation`) and the vegetation mask
# (`foundation.vegetation.resource`). Not a part: every part links it, and no host loads it.
util_add_plugin_library()
target_link_libraries(${PLUGIN_LIBRARY}
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Heightfield Foundation::Lod
           Foundation::Texture.Resource Foundation::Policy)

target_sources(${PLUGIN_LIBRARY}
    PRIVATE
        ScatterImpl.cpp
    PUBLIC
        FILE_SET terrain_foundation_modules TYPE CXX_MODULES FILES
            Terrain.cppm
            Splatmap.cppm
            TerrainResource.cppm
            VegetationMask.cppm
            ScatterLayer.cppm
            Scatter.cppm
            TerrainFoundationModule.cppm)
