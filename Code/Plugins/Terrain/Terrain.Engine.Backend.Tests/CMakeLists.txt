# Terrain.Engine's pixel probes on the real backends (terrain and vegetation).
add_executable(Terrain.Engine.Backend.Tests TestsMain.cpp TerrainPixelProbeTests.cpp VegetationPixelProbeTests.cpp)
target_link_libraries(Terrain.Engine.Backend.Tests PRIVATE
    Plugins::Terrain.Engine Foundation::Scene Foundation::Render Foundation::RenderGraph
    Foundation::Materials.PipelineCache Foundation::Geometry Foundation::Materials Foundation::Heightfield
    Foundation::Texture.Resource Foundation::Shaders Foundation::Shaders.System
    Foundation::RHI.TestSupport Foundation::RHI.Vulkan Foundation::RHI.WebGPU
    doctest::doctest)
target_compile_definitions(Terrain.Engine.Backend.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Terrain.Engine.Backend.Tests PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Terrain.Engine.Backend.Tests PRIVATE OPTION_HAS_DX12)
endif()
target_compile_options(Terrain.Engine.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Terrain.Engine.Backend.Tests COMMAND Terrain.Engine.Backend.Tests)
