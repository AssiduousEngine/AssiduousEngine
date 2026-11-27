# Luau.Foundation's tests: the VM, its contexts and the bound surface.
add_executable(Luau.Foundation.Tests TestsMain.cpp ScriptLuauTests.cpp)
target_link_libraries(Luau.Foundation.Tests PRIVATE Plugins::Luau.Foundation doctest::doctest)
target_compile_definitions(Luau.Foundation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Luau.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Luau.Foundation.Tests COMMAND Luau.Foundation.Tests)
