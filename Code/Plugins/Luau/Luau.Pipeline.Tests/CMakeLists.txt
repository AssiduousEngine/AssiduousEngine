# Luau.Pipeline's tests: the Luau cook.
add_executable(Luau.Pipeline.Tests TestsMain.cpp LuauCookTests.cpp)
target_link_libraries(Luau.Pipeline.Tests PRIVATE
    Plugins::Luau.Pipeline Plugins::Luau.Foundation Pipeline::Script
    Foundation::Script.Resource Foundation::Script Foundation::Script.Facades
    doctest::doctest)
target_compile_definitions(Luau.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Luau.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Luau.Pipeline.Tests COMMAND Luau.Pipeline.Tests)
