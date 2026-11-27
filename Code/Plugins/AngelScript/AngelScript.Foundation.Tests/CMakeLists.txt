# AngelScript.Foundation's tests: the VM, its contexts and the bound surface. Links the Luau backend
# too (guarded alias): the registry test proves both backends resolve side by side.
add_executable(AngelScript.Foundation.Tests TestsMain.cpp ScriptAngelScriptTests.cpp)
target_link_libraries(AngelScript.Foundation.Tests PRIVATE
    Plugins::AngelScript.Foundation ${SCRIPTLIB_LUAU} doctest::doctest
    ThirdParty::AngelScript) # a test counts the engine's modules through AngelScriptEngineHandle
target_compile_definitions(AngelScript.Foundation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(AngelScript.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME AngelScript.Foundation.Tests COMMAND AngelScript.Foundation.Tests)
