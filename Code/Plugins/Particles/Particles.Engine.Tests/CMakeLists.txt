# Particles.Engine's tests (doctest; the scene and resource integration, a component's
# resource-ref round trip).

add_executable(Particles.Engine.Tests
    TestsMain.cpp
    ResourceRefTests.cpp)

target_link_libraries(Particles.Engine.Tests PRIVATE
    Plugins::Particles.Engine Engine::Render
    Foundation::Scene Foundation::Scene.Resource Foundation::Resource Foundation::VFS Foundation::Content doctest::doctest)

target_compile_definitions(Particles.Engine.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Particles.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Particles.Engine.Tests COMMAND Particles.Engine.Tests)
