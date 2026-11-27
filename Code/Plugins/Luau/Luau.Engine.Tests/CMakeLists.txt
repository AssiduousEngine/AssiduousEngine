# Luau.Engine's tests: the plugin loading and unloading through a host, as a player loads it.
add_executable(Luau.Engine.Tests TestsMain.cpp LuauPluginTests.cpp)
util_link_plugins(Luau.Engine.Tests ROLE player PLUGINS luau)
target_link_libraries(Luau.Engine.Tests PRIVATE
    Foundation::Runtime Foundation::Script Foundation::Core doctest::doctest)
target_compile_definitions(Luau.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Luau.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Luau.Engine.Tests COMMAND Luau.Engine.Tests)
