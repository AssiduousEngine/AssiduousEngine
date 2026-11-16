# Tools.DotNetBindings - writes the engine's C# bindings, generated from its reflection.
add_executable(Tools.DotNetBindings Main.cpp)
# The whole script surface: every facade and every component's reflection, as a game's scripts see it.
target_link_libraries(Tools.DotNetBindings PRIVATE Foundation::Script.DotNet Engine::Composition)

# The engine's managed runtime (Engine.Runtime.dll and its runtimeconfig), built with the bindings
# into DotNet/ beside the binaries, where the engine looks for it.
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
set(DOTNET_RUNTIME_SOURCE ${CMAKE_SOURCE_DIR}/Code/Foundation/Script.DotNet/Managed)
set(DOTNET_RUNTIME_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DotNet)
set(DOTNET_RUNTIME_BINDINGS ${CMAKE_CURRENT_BINARY_DIR}/Engine.g.cs)
add_custom_command(
    OUTPUT ${DOTNET_RUNTIME_BINDINGS}
    COMMAND Tools.DotNetBindings ${DOTNET_RUNTIME_BINDINGS}
    DEPENDS Tools.DotNetBindings
    COMMENT "Generating the C# bindings")
add_custom_command(
    OUTPUT ${DOTNET_RUNTIME_OUT}/Engine.Runtime.dll ${DOTNET_RUNTIME_OUT}/Engine.Runtime.runtimeconfig.json
    COMMAND ${DOTNET_EXECUTABLE} build ${DOTNET_RUNTIME_SOURCE}/Engine.Runtime.csproj -c Debug -o ${DOTNET_RUNTIME_OUT}
            -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/RuntimeObj/
            -p:EngineBindings=${DOTNET_RUNTIME_BINDINGS} -nologo -v:q
    DEPENDS ${DOTNET_RUNTIME_SOURCE}/Engine.Runtime.csproj ${DOTNET_RUNTIME_SOURCE}/Bridge.cs ${DOTNET_RUNTIME_SOURCE}/Runtime.cs
            ${DOTNET_RUNTIME_BINDINGS}
    COMMENT "Building the engine's managed runtime")
add_custom_target(DotNet.Runtime ALL DEPENDS ${DOTNET_RUNTIME_OUT}/Engine.Runtime.dll)
