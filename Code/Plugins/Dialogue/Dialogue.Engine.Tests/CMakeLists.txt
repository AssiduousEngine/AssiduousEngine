add_executable(Dialogue.Engine.Tests TestsMain.cpp DialogueSettingsTests.cpp DialogueModelTests.cpp)
target_link_libraries(Dialogue.Engine.Tests PRIVATE Plugins::Dialogue.Engine Engine::Project doctest::doctest)
target_compile_definitions(Dialogue.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Dialogue.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Dialogue.Engine.Tests COMMAND Dialogue.Engine.Tests)
