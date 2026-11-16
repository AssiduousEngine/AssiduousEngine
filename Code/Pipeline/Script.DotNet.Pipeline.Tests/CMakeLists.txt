# Pipeline::Script.DotNet tests: the C# cook's parsing, and a project built with `dotnet build`.
add_executable(Script.DotNet.Pipeline.Tests TestsMain.cpp DotNetScriptCookTests.cpp)
target_link_libraries(Script.DotNet.Pipeline.Tests PRIVATE Pipeline::Script.DotNet Foundation::Script.Facades doctest::doctest)
target_compile_definitions(Script.DotNet.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Script.DotNet.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Script.DotNet.Pipeline.Tests COMMAND Script.DotNet.Pipeline.Tests)
