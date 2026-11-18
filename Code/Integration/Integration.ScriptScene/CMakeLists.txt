# Integration.ScriptScene - script behaviours on a real scene through the script subsystem: entity
# events reaching them in every language, a native behavior from its cook to a scene, one behavior in
# every language and in C++ coming out the same, a sequence played from script and its notices, and the
# script speed work's scene measures against the same work done by native components. The measures are skipped in a normal run; run them with
# --no-skip from an optimised build.

add_executable(Integration.ScriptScene TestsMain.cpp ScriptSceneBenchmarkTests.cpp EntityEventTests.cpp SequenceScriptTests.cpp
    BehaviorConformanceTests.cpp)
target_link_libraries(Integration.ScriptScene PRIVATE
    Engine::Behavior Engine::Spline Engine::Physics Engine::Animation Engine::Sequence Foundation::Sequence Foundation::Sequence.Resource
    Foundation::PropertyAnimation Foundation::Content Foundation::VFS Foundation::Resource Foundation::Run
    Foundation::Script.Resource Foundation::Script.Facades Foundation::Scene Foundation::Script
    doctest::doctest)

# A native behavior from its cook to a scene needs the cook, which a build without the tools (the
# Android one) does not have.
if(TARGET Pipeline::Script)
    target_sources(Integration.ScriptScene PRIVATE NativeBehaviorTests.cpp)
    target_link_libraries(Integration.ScriptScene PRIVATE Pipeline::Script) # the Native Behavior cook's harvest
endif()

# Each backend is independently switchable; include its leg only when the backend is built.
if(TARGET ${SCRIPTLIB_ANGELSCRIPT})
    target_link_libraries(Integration.ScriptScene PRIVATE ${SCRIPTLIB_ANGELSCRIPT})
    target_compile_definitions(Integration.ScriptScene PRIVATE INTEGRATION_HAS_ANGELSCRIPT=1)
endif()
if(TARGET ${SCRIPTLIB_LUAU})
    target_link_libraries(Integration.ScriptScene PRIVATE ${SCRIPTLIB_LUAU})
    target_compile_definitions(Integration.ScriptScene PRIVATE INTEGRATION_HAS_LUAU=1)
endif()

# C#: its classes built into an assembly over the engine's runtime, which the suite loads. Built
# twice: Debug, as the engine's cook builds a game's, and Release, to show what that costs.
if(TARGET Foundation::Script.DotNet AND NOT EMSCRIPTEN)
    find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
    set(BENCH_RUNTIME_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DotNet)
    set(BENCH_OUTPUTS)
    foreach(config Debug Release)
        set(out ${CMAKE_CURRENT_BINARY_DIR}/CSharp${config})
        add_custom_command(
            OUTPUT ${out}/Bench.dll
            COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_CURRENT_SOURCE_DIR}/CSharp/Bench.csproj -c ${config} -o ${out}
                    -p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/CSharpObj${config}/
                    -p:EngineRuntime=${BENCH_RUNTIME_DIR}/Engine.Runtime.dll -nologo -v:q --disable-build-servers
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/CSharp/Bench.cs ${CMAKE_CURRENT_SOURCE_DIR}/CSharp/Bench.csproj
                    ${BENCH_RUNTIME_DIR}/Engine.Runtime.dll DotNet.Runtime
            COMMENT "Building the C# measures' assembly (${config})")
        list(APPEND BENCH_OUTPUTS ${out}/Bench.dll)
    endforeach()
    add_custom_target(Integration.ScriptScene.CSharp DEPENDS ${BENCH_OUTPUTS})
    add_dependencies(Integration.ScriptScene Integration.ScriptScene.CSharp)
    target_link_libraries(Integration.ScriptScene PRIVATE Foundation::Script.DotNet)
    target_compile_definitions(Integration.ScriptScene PRIVATE INTEGRATION_HAS_DOTNET=1
        BENCH_CSHARP_DEBUG_DIR="${CMAKE_CURRENT_BINARY_DIR}/CSharpDebug"
        BENCH_CSHARP_RELEASE_DIR="${CMAKE_CURRENT_BINARY_DIR}/CSharpRelease")
endif()

target_compile_definitions(Integration.ScriptScene PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.ScriptScene PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.ScriptScene COMMAND Integration.ScriptScene)
