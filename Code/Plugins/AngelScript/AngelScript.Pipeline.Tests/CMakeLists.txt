# AngelScript.Pipeline's tests: the AngelScript cook. They register it, as the part's load does; the
# library under test stays backend-neutral and resolves through the registry.
add_executable(AngelScript.Pipeline.Tests TestsMain.cpp AngelScriptCookTests.cpp)
target_link_libraries(AngelScript.Pipeline.Tests PRIVATE
    Plugins::AngelScript.Pipeline Plugins::AngelScript.Foundation Pipeline::Script
    Foundation::Script.Resource Foundation::Script Foundation::Script.Facades
    Engine::Composition # RegisterAllScriptFacades: the complete engine facade surface the
                        # run:: and ui:: regression cook compiles against
    doctest::doctest)
target_compile_definitions(AngelScript.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(AngelScript.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME AngelScript.Pipeline.Tests COMMAND AngelScript.Pipeline.Tests)
