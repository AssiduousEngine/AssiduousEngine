# Tools.DotNetBindings - writes the engine's C# bindings, generated from its reflection.
add_executable(Tools.DotNetBindings Main.cpp)
# The whole script surface: every facade and every component's reflection, as an application's scripts see it.
target_link_libraries(Tools.DotNetBindings PRIVATE Foundation::Script.DotNet Engine::Composition)

# The engine's managed runtime (Engine.Runtime.dll and its runtimeconfig), built with the bindings
# into DotNet/ beside the binaries, where the engine looks for it.
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
# Global (cached), since dotnet_link_compiled_runtime below is called from other folders.
set(DOTNET_RUNTIME_SOURCE ${CMAKE_SOURCE_DIR}/Code/Foundation/Script.DotNet/Managed CACHE INTERNAL "")
set(DOTNET_RUNTIME_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DotNet CACHE INTERNAL "")
set(DOTNET_RUNTIME_BINDINGS ${CMAKE_CURRENT_BINARY_DIR}/Engine.g.cs CACHE INTERNAL "")
add_custom_command(
    OUTPUT ${DOTNET_RUNTIME_BINDINGS}
    COMMAND Tools.DotNetBindings ${DOTNET_RUNTIME_BINDINGS}
    DEPENDS Tools.DotNetBindings
    COMMENT "Generating the C# bindings")
# Release whatever the engine's configuration: every call a game makes into the engine runs this
# assembly's code, and a game debugging its own scripts steps over it, not into it.
add_custom_command(
    OUTPUT ${DOTNET_RUNTIME_OUT}/Engine.Runtime.dll ${DOTNET_RUNTIME_OUT}/Engine.Runtime.runtimeconfig.json
           ${DOTNET_RUNTIME_OUT}/Engine.g.cs
    COMMAND ${DOTNET_EXECUTABLE} build ${DOTNET_RUNTIME_SOURCE}/Engine.Runtime.csproj -c Release -o ${DOTNET_RUNTIME_OUT}
            -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/RuntimeObj/
            -p:EngineBindings=${DOTNET_RUNTIME_BINDINGS} -nologo -v:q --disable-build-servers
    # The bindings beside the runtime: a web build (which cannot run this tool) is given them.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DOTNET_RUNTIME_BINDINGS} ${DOTNET_RUNTIME_OUT}/Engine.g.cs
    DEPENDS ${DOTNET_RUNTIME_SOURCE}/Engine.Runtime.csproj ${DOTNET_RUNTIME_SOURCE}/Bridge.cs ${DOTNET_RUNTIME_SOURCE}/Runtime.cs
            ${DOTNET_RUNTIME_SOURCE}/ValueMath.cs ${DOTNET_RUNTIME_BINDINGS}
    COMMENT "Building the engine's managed runtime")
add_custom_target(DotNet.Runtime ALL DEPENDS ${DOTNET_RUNTIME_OUT}/Engine.Runtime.dll)
