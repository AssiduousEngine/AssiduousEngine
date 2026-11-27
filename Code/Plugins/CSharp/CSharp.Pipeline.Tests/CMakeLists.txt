# CSharp.Pipeline's tests: the C# cook's parsing, and a project built with `dotnet build`.
add_executable(CSharp.Pipeline.Tests TestsMain.cpp DotNetScriptCookTests.cpp)
target_link_libraries(CSharp.Pipeline.Tests PRIVATE Plugins::CSharp.Pipeline Foundation::Script.Facades doctest::doctest)
target_compile_definitions(CSharp.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(CSharp.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME CSharp.Pipeline.Tests COMMAND CSharp.Pipeline.Tests)
