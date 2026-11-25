add_executable(Dialogue.Pipeline.Tests TestsMain.cpp DialogueParseTests.cpp DialogueFormTests.cpp DialogueCheckTests.cpp
    DialogueRunnerTests.cpp DialogueLevelsTests.cpp)
target_link_libraries(Dialogue.Pipeline.Tests PRIVATE Plugins::Dialogue.Pipeline
    Foundation::Scene Foundation::Scene.Resource # a real saved level for the levels the cook reads
    doctest::doctest)
target_compile_definitions(Dialogue.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Dialogue.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Dialogue.Pipeline.Tests COMMAND Dialogue.Pipeline.Tests)
