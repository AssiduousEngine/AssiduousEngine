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

add_executable(Script.DotNet.Tests TestsMain.cpp DotNetHostTests.cpp DotNetBridgeTests.cpp DotNetBindingsTests.cpp)
add_dependencies(Script.DotNet.Tests Script.DotNet.Probe)
target_link_libraries(Script.DotNet.Tests PRIVATE Foundation::Script.DotNet Foundation::Script.Facades doctest::doctest)
target_compile_definitions(Script.DotNet.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    PROBE_DIR="${PROBE_OUT}")
target_compile_options(Script.DotNet.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Script.DotNet.Tests COMMAND Script.DotNet.Tests)
