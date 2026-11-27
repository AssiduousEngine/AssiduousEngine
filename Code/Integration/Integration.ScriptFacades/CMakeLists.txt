# Integration.ScriptFacades - subsystem script facades exercised end-to-end on the script backends
# (Net.*, Audio.*, Http.*, RenderQuality.*, Display.*, Text.* and Saves.* driven from AngelScript and Luau). These tests cross
# layers (scripting and a subsystem), so they live here rather than in the subsystem's own unit-test
# suite, which stays backend-neutral. Each language's leg is guarded by `#ifdef HAS_PLUGIN_<LANGUAGE>`,
# which its plugin defines for what links it, and each is linked when it is built
# ($<TARGET_NAME_IF_EXISTS:...>).
add_executable(Integration.ScriptFacades TestsMain.cpp
    NetFacadeScriptTests.cpp AudioFacadeScriptTests.cpp HttpFacadeScriptTests.cpp
    RenderQualityFacadeScriptTests.cpp DisplayFacadeScriptTests.cpp TextFacadeScriptTests.cpp SavesFacadeScriptTests.cpp)
target_link_libraries(Integration.ScriptFacades PRIVATE
    Foundation::Core Foundation::Script Foundation::Net Foundation::Net.Manager
    Foundation::Http Foundation::Http.Client Foundation::Http.Script
    Foundation::Audio Engine::Audio Foundation::Audio.Resource Engine::Render Engine::Run
    Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Scene Foundation::Scene.Resource Foundation::Net.Replication
    $<TARGET_NAME_IF_EXISTS:Plugins::AngelScript.Foundation> $<TARGET_NAME_IF_EXISTS:Plugins::Luau.Foundation>
    doctest::doctest)
target_compile_definitions(Integration.ScriptFacades PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.ScriptFacades PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.ScriptFacades COMMAND Integration.ScriptFacades)
