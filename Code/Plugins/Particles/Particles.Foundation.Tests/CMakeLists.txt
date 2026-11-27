# Particles.Foundation's tests (doctest; CPU only, no GPU or renderer): the simulation, the curves,
# the cooked effect's serializer and its factory.

add_executable(Particles.Foundation.Tests
    TestsMain.cpp
    ParticlesTests.cpp
    ParticleCurveTests.cpp
    ParticleEffectFactoryTests.cpp
    ParticleEffectSerializerTests.cpp
)

target_link_libraries(Particles.Foundation.Tests PRIVATE
    Plugins::Particles.Foundation Foundation::Xml.Serialization
    Foundation::Content Foundation::Resource Foundation::VFS doctest::doctest)

target_compile_definitions(Particles.Foundation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Particles.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Particles.Foundation.Tests COMMAND Particles.Foundation.Tests)
