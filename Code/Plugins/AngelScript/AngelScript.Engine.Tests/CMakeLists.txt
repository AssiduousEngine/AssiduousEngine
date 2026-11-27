# AngelScript.Engine's tests: the plugin loading and unloading through a host, as a player loads it.
add_executable(AngelScript.Engine.Tests TestsMain.cpp AngelScriptPluginTests.cpp)
util_link_plugins(AngelScript.Engine.Tests ROLE player PLUGINS angelscript)
target_link_libraries(AngelScript.Engine.Tests PRIVATE
    Foundation::Runtime Foundation::Script Foundation::Core doctest::doctest)
target_compile_definitions(AngelScript.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(AngelScript.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME AngelScript.Engine.Tests COMMAND AngelScript.Engine.Tests)
