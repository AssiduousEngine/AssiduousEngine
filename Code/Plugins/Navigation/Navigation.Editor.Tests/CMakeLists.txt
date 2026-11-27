# Navigation.Editor's tests: the bake over static bodies and terrain.
add_executable(Navigation.Editor.Tests TestsMain.cpp NavigationBakeTests.cpp NavigationEditorTests.cpp)
target_link_libraries(Navigation.Editor.Tests PRIVATE
    Plugins::Navigation.Editor Plugins::Navigation.Engine Plugins::Navigation.Pipeline Editor::Scene Editor::Mcp Editor::Camera Editor::ViewportTools Foundation::GUI.Toolkit Foundation::Input Foundation::Json Foundation::Mcp Engine::Render Engine::Physics Foundation::Physics Foundation::Geometry
    Plugins::Terrain.Engine Foundation::Heightfield
    Foundation::Scene Foundation::Content Foundation::Resource Foundation::VFS Foundation::Core
    doctest::doctest)
target_compile_definitions(Navigation.Editor.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Navigation.Editor.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Navigation.Editor.Tests COMMAND Navigation.Editor.Tests)
