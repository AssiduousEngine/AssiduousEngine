# Terrain.Pipeline's tests: the terrain, splatmap and vegetation mask cooks and imports.
add_executable(Terrain.Pipeline.Tests TestsMain.cpp TerrainPipelineTests.cpp SplatmapPipelineTests.cpp VegetationMaskPipelineTests.cpp)
target_link_libraries(Terrain.Pipeline.Tests PRIVATE
    Plugins::Terrain.Pipeline Pipeline::Texture Foundation::Heightfield Foundation::Heightfield.Resource
    Pipeline::Core Pipeline::Importer Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Image Foundation::Image.IO
    doctest::doctest)
target_compile_definitions(Terrain.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Terrain.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Terrain.Pipeline.Tests COMMAND Terrain.Pipeline.Tests)
