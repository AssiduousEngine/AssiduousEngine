# Terrain.Pipeline (Plugins::Terrain.Pipeline) - the terrain plugin's assets: the terrain and the
# splatmap (`terrain.pipeline`) and the vegetation mask (`vegetation.pipeline`), their builders,
# importers and File > New's creators. The editor and the tools load it; a player never does.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::VFS Pipeline::Core Pipeline::Importer
           Foundation::Image Foundation::Image.IO Pipeline::Texture Foundation::Content Foundation::Policy
    PRIVATE Foundation::Runtime)

target_sources(${PLUGIN_PART}
    PRIVATE
        TerrainPipeline.cpp       # the part's entry
        TerrainCreatorsImpl.cpp   # File > New, asset_create
        TerrainAssetImpl.cpp
        VegetationCreatorsImpl.cpp
        VegetationMaskAssetImpl.cpp
    PUBLIC
        FILE_SET terrain_pipeline_modules TYPE CXX_MODULES FILES
            TerrainAsset.cppm
            VegetationMaskAsset.cppm)
