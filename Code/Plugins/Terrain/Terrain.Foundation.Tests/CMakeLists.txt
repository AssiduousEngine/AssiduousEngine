# Terrain.Foundation's tests: the chunks and LOD, the cooked terrain and splatmap, scattering and the mask.
add_executable(Terrain.Foundation.Tests TestsMain.cpp TerrainTests.cpp TerrainResourceTests.cpp SplatmapTests.cpp ScatterTests.cpp VegetationMaskTests.cpp)
target_link_libraries(Terrain.Foundation.Tests PRIVATE
    Plugins::Terrain.Foundation Foundation::Heightfield Foundation::Heightfield.Resource
    Foundation::Resource Foundation::Content Foundation::VFS
    doctest::doctest)
target_compile_definitions(Terrain.Foundation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Terrain.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Terrain.Foundation.Tests COMMAND Terrain.Foundation.Tests)
