add_executable(Editor.Scene.Tests TestsMain.cpp SceneThumbnailTests.cpp SceneActionsTests.cpp ZoomReadoutTests.cpp ViewGizmoTests.cpp SceneGridTests.cpp EditContextTests.cpp GizmoTests.cpp SelectToolTests.cpp CameraPreviewTests.cpp SceneMcpToolsTests.cpp SceneMcpEditToolsTests.cpp SceneServicesTests.cpp ScenePageTests.cpp ParityTests.cpp)
target_link_libraries(Editor.Scene.Tests PRIVATE Editor::Scene Editor::Core Foundation::Content Engine::Render Engine::Behavior Engine::Spline Pipeline::Scene Engine::Animation Engine::Navigation Engine::Physics Foundation::Physics Plugins::Terrain.Engine Pipeline::ModelImporter Foundation::VFS Foundation::Geometry Foundation::Materials Pipeline::Materials Pipeline::Render Plugins::Particles.Foundation Foundation::Mcp Foundation::Json Foundation::GUI Foundation::GUI.Toolkit Editor::App Editor::Sequence Engine::Sequence Pipeline::Sequence Foundation::Sequence doctest::doctest)
target_compile_definitions(Editor.Scene.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_SOURCE_DIR="${BUILDSYSTEM_TEST_SOURCE_DIR}") # the page's markup the page tests load
target_compile_options(Editor.Scene.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Scene.Tests COMMAND Editor.Scene.Tests)
