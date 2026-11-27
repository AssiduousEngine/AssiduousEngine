# CSharp.Engine's tests: the plugin loading and unloading through a host, as a player loads it.
add_executable(CSharp.Engine.Tests TestsMain.cpp CSharpPluginTests.cpp)
util_link_plugins(CSharp.Engine.Tests ROLE player PLUGINS csharp)
target_link_libraries(CSharp.Engine.Tests PRIVATE
    Foundation::Runtime Foundation::Script Foundation::Core doctest::doctest)
target_compile_definitions(CSharp.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(CSharp.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME CSharp.Engine.Tests COMMAND CSharp.Engine.Tests)
