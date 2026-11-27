# CSharp.Tooling's tests: the export's player hook, alone and as an exporting host loads the plugin.
add_executable(CSharp.Tooling.Tests TestsMain.cpp CSharpToolingTests.cpp)
util_link_plugins(CSharp.Tooling.Tests ROLE export PLUGINS csharp)
target_link_libraries(CSharp.Tooling.Tests PRIVATE
    Plugins::CSharp.Tooling Editor::Project Pipeline::Script Foundation::Content Foundation::VFS Foundation::Runtime Foundation::Core doctest::doctest)
target_compile_definitions(CSharp.Tooling.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(CSharp.Tooling.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME CSharp.Tooling.Tests COMMAND CSharp.Tooling.Tests)
