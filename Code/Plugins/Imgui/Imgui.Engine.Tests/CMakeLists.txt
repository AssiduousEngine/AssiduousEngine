add_executable(Imgui.Engine.Tests TestsMain.cpp ImguiRendererTests.cpp)
target_link_libraries(Imgui.Engine.Tests PRIVATE Plugins::Imgui.Engine doctest::doctest)
target_compile_definitions(Imgui.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Imgui.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Imgui.Engine.Tests COMMAND Imgui.Engine.Tests)
