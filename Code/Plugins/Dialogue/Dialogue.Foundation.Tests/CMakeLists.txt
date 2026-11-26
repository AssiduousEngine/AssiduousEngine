add_executable(Dialogue.Foundation.Tests TestsMain.cpp DialogueSettingsTests.cpp DialogueModelTests.cpp)
target_link_libraries(Dialogue.Foundation.Tests PRIVATE Plugins::Dialogue.Foundation Engine::Project doctest::doctest)
target_compile_definitions(Dialogue.Foundation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Dialogue.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Dialogue.Foundation.Tests COMMAND Dialogue.Foundation.Tests)
