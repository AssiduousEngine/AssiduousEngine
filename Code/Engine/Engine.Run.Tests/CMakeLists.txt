# ApplicationRunTests.cpp interleaves AngelScript and Luau cases (each drives its own real VM), so it
# builds only when both backends are on (splitting it per backend would lift this).
if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
add_executable(Engine.Run.Tests TestsMain.cpp ApplicationRunTests.cpp)
target_link_libraries(Engine.Run.Tests PRIVATE
    Engine::Run Foundation::Net Foundation::Net.Manager Foundation::Http Foundation::Http.Script
    Foundation::Input Foundation::Shell ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    Foundation::Scene.Resource Foundation::Content Foundation::Resource Foundation::VFS
    doctest::doctest)
target_compile_definitions(Engine.Run.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Run.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
# C#: the test game assembly Script.DotNet.Tests builds (its Game class).
if(DOTNET_SCRIPTING) # the target may be added after this folder: dependencies resolve at generate
    target_link_libraries(Engine.Run.Tests PRIVATE Foundation::Script.DotNet)
    add_dependencies(Engine.Run.Tests Script.DotNet.TestGame)
    target_compile_definitions(Engine.Run.Tests PRIVATE
        CSHARP_TESTGAME_DIR="${CMAKE_BINARY_DIR}/Code/Foundation/Script.DotNet.Tests/TestGame")
endif()
add_test(NAME Engine.Run.Tests COMMAND Engine.Run.Tests)
endif()
