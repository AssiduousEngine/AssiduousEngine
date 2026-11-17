# Foundation::Script.DotNet tests: the host starts the installed runtime and calls into a C# probe
# assembly, built here by `dotnet build`.
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
set(PROBE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Probe)
set(PROBE_BINDINGS ${CMAKE_CURRENT_BINARY_DIR}/Engine.g.cs)
# The bindings, generated from the engine's reflection, then the probe built with them.
add_custom_command(
    OUTPUT ${PROBE_BINDINGS}
    COMMAND Tools.DotNetBindings ${PROBE_BINDINGS}
    DEPENDS Tools.DotNetBindings
    COMMENT "Generating the C# bindings")
add_custom_command(
    OUTPUT ${PROBE_OUT}/Probe.dll ${PROBE_OUT}/Probe.runtimeconfig.json
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_CURRENT_SOURCE_DIR}/Probe/Probe.csproj -c Debug -o ${PROBE_OUT}
            -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/ProbeObj/
            -p:EngineBindings=${PROBE_BINDINGS}
            -p:EngineBridge=${CMAKE_SOURCE_DIR}/Code/Foundation/Script.DotNet/Managed/Bridge.cs -nologo -v:q
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Probe/Probe.cs ${CMAKE_CURRENT_SOURCE_DIR}/Probe/Probe.csproj ${PROBE_BINDINGS}
            ${CMAKE_SOURCE_DIR}/Code/Foundation/Script.DotNet/Managed/Bridge.cs
    COMMENT "Building the C# probe assembly")
add_custom_target(Script.DotNet.Probe DEPENDS ${PROBE_OUT}/Probe.dll)

# A game's assembly over the engine's managed runtime (built by Tools.DotNetBindings into DotNet/).
set(TESTGAME_OUT ${CMAKE_CURRENT_BINARY_DIR}/TestGame)
set(RUNTIME_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DotNet)
add_custom_command(
    OUTPUT ${TESTGAME_OUT}/TestGame.dll
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/TestGame.csproj -c Debug -o ${TESTGAME_OUT}
            -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/TestGameObj/
            -p:EngineRuntime=${RUNTIME_DIR}/Engine.Runtime.dll -nologo -v:q
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/Mover.cs ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/TestGame.csproj
            ${RUNTIME_DIR}/Engine.Runtime.dll DotNet.Runtime
    COMMENT "Building the C# test game assembly")
# The same game rebuilt with a change (REBUILT doubles Mover's speed): a second image for reload.
set(TESTGAME_REBUILT_OUT ${CMAKE_CURRENT_BINARY_DIR}/TestGameRebuilt)
add_custom_command(
    OUTPUT ${TESTGAME_REBUILT_OUT}/TestGame.dll
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/TestGame.csproj -c Debug -o ${TESTGAME_REBUILT_OUT}
            -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/TestGameRebuiltObj/
            -p:EngineRuntime=${RUNTIME_DIR}/Engine.Runtime.dll -p:DefineConstants=REBUILT -nologo -v:q
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/Mover.cs ${CMAKE_CURRENT_SOURCE_DIR}/TestGame/TestGame.csproj
            ${RUNTIME_DIR}/Engine.Runtime.dll DotNet.Runtime
    COMMENT "Building the rebuilt C# test game assembly")
add_custom_target(Script.DotNet.TestGame DEPENDS ${TESTGAME_OUT}/TestGame.dll ${TESTGAME_REBUILT_OUT}/TestGame.dll)

add_executable(Script.DotNet.Tests TestsMain.cpp DotNetHostTests.cpp DotNetBridgeTests.cpp DotNetBindingsTests.cpp DotNetRuntimeTests.cpp DotNetScriptTests.cpp)
add_dependencies(Script.DotNet.Tests Script.DotNet.Probe Script.DotNet.TestGame)
target_link_libraries(Script.DotNet.Tests PRIVATE Foundation::Script.DotNet Foundation::Script.Facades doctest::doctest)
target_compile_definitions(Script.DotNet.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    PROBE_DIR="${PROBE_OUT}"
    TESTGAME_DIR="${TESTGAME_OUT}"
    TESTGAME_REBUILT_DIR="${TESTGAME_REBUILT_OUT}"
    RUNTIME_DIR="${RUNTIME_DIR}")
target_compile_options(Script.DotNet.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Script.DotNet.Tests COMMAND Script.DotNet.Tests)
