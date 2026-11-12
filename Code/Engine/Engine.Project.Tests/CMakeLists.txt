# Engine::Project unit tests (doctest; the manifest's own rules, no editor).

add_executable(Engine.Project.Tests
    TestsMain.cpp
    QualityLevelTests.cpp)

target_link_libraries(Engine.Project.Tests PRIVATE Engine::Project Foundation::Core doctest::doctest)

target_compile_definitions(Engine.Project.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Engine.Project.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Project.Tests COMMAND Engine.Project.Tests)
