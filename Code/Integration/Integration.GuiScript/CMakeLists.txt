# Integration.GuiScript - the GUI (foundation.gui) and its satellites driven from the script backends
# (the GUI's reflected widgets built, styled, bound and clicked, the gamekit's screen stack, the
# sandbox's game menu played from its real files, and the engine's GUI (Engine.GUI) found and
# driven from a script, from AngelScript and Luau). It crosses layers (scripting and a GUI library), so it
# lives here and the libraries' own suites stay backend-neutral. Each language's leg is guarded by
# `#ifdef HAS_PLUGIN_<LANGUAGE>`, which its plugin defines for what links it, and each is linked when
# it is built ($<TARGET_NAME_IF_EXISTS:...>).
add_executable(Integration.GuiScript TestsMain.cpp GuiScriptTests.cpp GamekitScriptTests.cpp ShowcaseScriptTests.cpp EngineGuiScriptTests.cpp)
target_link_libraries(Integration.GuiScript PRIVATE
    Foundation::GUI.Gamekit Foundation::GUI Foundation::GUI.Resources Engine::GUI Foundation::Runtime
    Foundation::Content Foundation::Resource Foundation::VFS Foundation::Core Foundation::Input Foundation::Script Foundation::Script.Facades
    $<TARGET_NAME_IF_EXISTS:Plugins::AngelScript.Foundation> $<TARGET_NAME_IF_EXISTS:Plugins::Luau.Foundation>
    doctest::doctest)
target_compile_definitions(Integration.GuiScript PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    GUI_SHOWCASE_DATA="${CMAKE_SOURCE_DIR}/Data") # the game menu's files, played from the data folder
target_compile_options(Integration.GuiScript PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.GuiScript COMMAND Integration.GuiScript)
