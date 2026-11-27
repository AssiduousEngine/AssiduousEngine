# Particles.Editor tests: the particle effect page over its markup.
add_executable(Particles.Editor.Tests TestsMain.cpp ParticleEffectPageTests.cpp)
target_link_libraries(Particles.Editor.Tests PRIVATE
    Plugins::Particles.Editor Editor::App Editor::Core Foundation::Content
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Particles.Editor.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_SOURCE_DIR="${BUILDSYSTEM_TEST_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Particles.Editor.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Particles.Editor.Tests COMMAND Particles.Editor.Tests)
