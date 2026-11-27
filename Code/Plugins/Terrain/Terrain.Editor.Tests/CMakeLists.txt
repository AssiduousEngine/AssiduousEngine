# Terrain.Editor's tests: the tools, their panels, the pages and the splatmap thumbnails.
add_executable(Terrain.Editor.Tests TestsMain.cpp SculptToolTests.cpp HoleToolTests.cpp SplatToolTests.cpp PanelProviderTests.cpp SplatmapThumbnailTests.cpp TerrainEditorPageTests.cpp TerrainPageTests.cpp VegetationPaintToolTests.cpp VegetationScatterToolTests.cpp VegetationMaskPageTests.cpp)
target_link_libraries(Terrain.Editor.Tests PRIVATE
    Plugins::Terrain.Editor Editor::Core Editor::ViewportTools Editor::App
    Pipeline::Heightfield Pipeline::Core Foundation::Heightfield Foundation::Heightfield.Resource
    Foundation::GUI Foundation::GUI.Toolkit Foundation::Texture.Resource Foundation::Geometry
    Foundation::Physics Engine::Physics Foundation::Image Foundation::Image.IO Foundation::Render
    Foundation::Scene Foundation::Content Foundation::Resource Foundation::VFS
    doctest::doctest)
target_compile_definitions(Terrain.Editor.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_SOURCE_DIR="${BUILDSYSTEM_TEST_SOURCE_DIR}")
target_compile_options(Terrain.Editor.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Terrain.Editor.Tests COMMAND Terrain.Editor.Tests)
