# Terrain.Engine's tests: the terrain and vegetation components, the renderer on the null device, the splat cache.
add_executable(Terrain.Engine.Tests TestsMain.cpp TerrainComponentTests.cpp TerrainRendererTests.cpp SplatTextureCacheTests.cpp VegetationComponentTests.cpp)
target_link_libraries(Terrain.Engine.Tests PRIVATE
    Plugins::Terrain.Engine Foundation::VFS Foundation::Scene Foundation::Scene.Resource Foundation::Resource
    Foundation::Heightfield Foundation::RHI Foundation::RHI.Null Foundation::Render Foundation::Shaders
    Foundation::Shaders.System Foundation::Geometry Foundation::Materials
    doctest::doctest)
target_compile_definitions(Terrain.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Terrain.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Terrain.Engine.Tests COMMAND Terrain.Engine.Tests)
