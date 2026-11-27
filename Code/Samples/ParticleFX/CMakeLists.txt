# ParticleFX - the particle-system showcase (CPU-sim fountain drawn by the dedicated ParticleRenderer).
# Same host set as the other samples, plus the particle core and its scene/render integration.

add_executable(ParticleFX main.cpp)

target_link_libraries(ParticleFX PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.SDL3 Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Engine::Composition
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Foundation::Geometry
    Foundation::Materials
    Plugins::Particles.Pipeline # ParticleEffectAsset and its bake (the sample cooks its demo effect)
    Pipeline::Core Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Policy)

util_copy_sdl3_dll(ParticleFX)

# Dear ImGui and particles, which the app loads as plugins (its part linked, and the registry listing it).
util_link_plugins(ParticleFX ROLE player PLUGINS imgui particles)
