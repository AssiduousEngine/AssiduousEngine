# Terrain.Engine (Plugins::Terrain.Engine) - the terrain plugin's runtime: the terrain component, its
# chunked geo-mipmap renderer and TerrainSubsystem (`engine.terrain`), and vegetation over it
# (`engine.vegetation`), drawn by the shared mesh renderer. Every host loads it.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC
        Foundation::Core Foundation::Scene Foundation::Resource
        Foundation::Texture.Resource Foundation::Heightfield Foundation::Heightfield.Resource
        Foundation::Geometry Foundation::Materials
        Foundation::RHI Engine::Domain Foundation::Policy
        Foundation::Render Engine::Render Engine::Scene
        Foundation::Shaders Foundation::Shaders.System Foundation::Runtime
        Foundation::Profiler
    PRIVATE Engine::Composition) # EnginePluginServices, which takes the domains and the scene managers

target_sources(${PLUGIN_PART}
    PRIVATE
        TerrainEngine.cpp # the part's entry
        TerrainComponentsImpl.cpp
        VegetationComponentsImpl.cpp
    PUBLIC
        FILE_SET terrain_engine_modules TYPE CXX_MODULES FILES
            HeightTexture.cppm
            HoledChunkMesh.cppm
            SplatTexture.cppm
            TerrainRenderData.cppm
            TerrainComponents.cppm
            TerrainRenderer.cppm
            TerrainSubsystem.cppm
            TerrainModule.cppm
            VegetationComponents.cppm
            VegetationSubsystem.cppm
            VegetationModule.cppm)
