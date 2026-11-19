# Headless behavior tests: a real script VM (registered by the test, so the library stays
# backend-neutral), a real Scene, no device or window dependencies.
# BehaviorSceneTests.cpp interleaves AngelScript and Luau cases (each drives its own real VM), so it
# builds only when both backends are on. Splitting it into per-backend files would let it run in a
# single-backend configuration.
if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
add_executable(Engine.Behavior.Tests TestsMain.cpp BehaviorSceneTests.cpp)
target_link_libraries(Engine.Behavior.Tests PRIVATE
    Engine::Behavior Foundation::Script.Resource Foundation::Scene Foundation::Scene.Resource
    Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Physics Engine::Physics
    Engine::Render Foundation::RHI.Null Foundation::Audio Engine::Audio
    Foundation::Animation Engine::Animation
    Foundation::Particles Engine::Particles
    Foundation::Net Foundation::Net.Replication
    Foundation::Spline Engine::Spline
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU} doctest::doctest)
target_compile_definitions(Engine.Behavior.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Behavior.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.Behavior.Tests COMMAND Engine.Behavior.Tests)
endif()
