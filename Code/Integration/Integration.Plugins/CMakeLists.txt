# Integration.Plugins - plugin packages in every host: a project's own, the reference game's
# (Data/SampleProjects/NativeSample, all four parts), built in this tree so every lane compiles it,
# and the engine's official dialogue plugin; both linked into this suite the way a host links its
# plugins, and loaded the way the editor, the player, the cook and the MCP host each load them. A
# build that adds a project's packages (ENGINE_PROJECT_PLUGINS) adds its own copy of them, so this
# suite stays out of it.
util_add_plugin(${CMAKE_SOURCE_DIR}/Data/SampleProjects/NativeSample/Plugins/NativeSample)

add_executable(Integration.Plugins TestsMain.cpp NativeSampleTests.cpp DialoguePluginTests.cpp DialogueCookTests.cpp DialogueEditorTests.cpp
    DialogueRunTests.cpp MillbrookSampleTests.cpp)
util_link_plugins(Integration.Plugins ROLE editor PLUGINS nativesample dialogue) # every part an editor loads
target_link_libraries(Integration.Plugins PRIVATE
    Editor::App Editor::Mcp Editor::Project Editor::Core Engine::DefaultApp Engine::Composition Engine::Project Engine::Run Engine::Behavior Engine::Render Engine::Sequence Engine::GUI Foundation::GUI Foundation::GUI.Gamekit Foundation::GUI.Resources
    Foundation::Sequence Foundation::Sequence.Resource Foundation::PropertyAnimation Foundation::Run Foundation::Script Foundation::Script.Facades Foundation::Script.Resource
    Pipeline::Core Pipeline::Importer Pipeline::Registration Foundation::Mcp Foundation::Json Foundation::Content
    Foundation::Scene Foundation::Scene.Resource Foundation::VFS Foundation::Runtime Foundation::Runtime.Client
    Foundation::Core doctest::doctest)
# The dialogue facade's tests run behaviors in each backend that is built.
if(TARGET ${SCRIPTLIB_ANGELSCRIPT})
    target_link_libraries(Integration.Plugins PRIVATE ${SCRIPTLIB_ANGELSCRIPT})
    target_compile_definitions(Integration.Plugins PRIVATE INTEGRATION_HAS_ANGELSCRIPT=1)
endif()
if(TARGET ${SCRIPTLIB_LUAU})
    target_link_libraries(Integration.Plugins PRIVATE ${SCRIPTLIB_LUAU})
    target_compile_definitions(Integration.Plugins PRIVATE INTEGRATION_HAS_LUAU=1)
endif()
target_compile_definitions(Integration.Plugins PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Plugins PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Plugins COMMAND Integration.Plugins)
