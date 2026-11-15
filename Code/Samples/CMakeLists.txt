# Sample applications. Built when their required backends are available.

# Stage runtime deps (any linked DLLs) next to a sample exe: a thin wrapper over
# util_copy_runtime_deps that the sample CMakeLists call.
function(util_copy_sdl3_dll target)
    util_copy_runtime_deps(${target}) # SDL3 is vendored static; no DLL to stage
endfunction()

# An RHI sample's program: an executable, or on Android a shared library the app's activity loads
# (SDL's Java glue calls its SDL_main; scripts/android-app.sh packs it as libmain.so). The sample's
# own `int main(int argc, char** argv)` becomes that SDL_main there, through AndroidSampleMain.h.
function(util_add_sample_app target)
    if(ANDROID)
        add_library(${target} SHARED ${ARGN})
        target_compile_options(${target} PRIVATE
            "SHELL:-include ${CMAKE_SOURCE_DIR}/Code/Samples/Framework/AndroidSampleMain.h")
    else()
        add_executable(${target} ${ARGN})
    endif()
endfunction()

# NetEcho - console proof of foundation.net (no graphics); builds and runs anywhere.
add_subdirectory(NetEcho)

# The windowed samples present through a GraphicsDevice, so they need the desktop
# shell and the GPU factory.
if(TARGET Foundation::Shell.SDL3 AND TARGET Foundation::Runtime.Desktop AND TARGET Foundation::Graphics.Gpu)
    add_subdirectory(HelloWindow)
    add_subdirectory(ScriptPlayground)   # script consumer proof (Mover/Spinner behaviors on the ScriptSubsystem)
    add_subdirectory(MultiWindow)
    # Samples with an ImGui debug UI honor the plugin option, or an
    # -DOPTION_PLUGIN_IMGUI=OFF configure dies on util_link_plugins asking for a plugin not added.
    if(OPTION_PLUGIN_IMGUI)
        add_subdirectory(InputActions)       # foundation.input consumer proof (named actions end-to-end)
        add_subdirectory(PhysicsPlayground)  # physics consumer proof (crates/trigger/raycast on Jolt)
        add_subdirectory(AudioPlayground)    # audio consumer proof (ambient loop and positional one-shots on miniaudio)
        add_subdirectory(TerrainPlayground)  # terrain consumer proof (a heightfield built in code, the engine.terrain renderer, a HUD)
        add_subdirectory(RenderStressTest)   # worst-case benchmark to keep perf honest
    endif()
    # These dev harnesses link the authoring pipeline (ModelImporter and the cook libraries), so they
    # only build when the tooling is present (OPTION_BUILD_TOOLS; always off for web).
    if(OPTION_BUILD_TOOLS AND OPTION_PLUGIN_IMGUI)
        add_subdirectory(Sandbox)            # the dev harness we grow the engine in
        add_subdirectory(AnimStressTest)     # animation/skinning benchmark (Sandbox copy, stripped toward a grid)
        add_subdirectory(AnimatedCrowd)      # skinned-crowd instancing (InstancedMeshComponent and InstancedSkinning)
        add_subdirectory(ParticleFX)         # particle-system showcase (CPU sim and the dedicated ParticleRenderer)
        add_subdirectory(CustomShader)       # a low-level custom shader (toon) through the material-driven renderer
    endif()
else()
    message(STATUS "Samples: skipping HelloWindow/MultiWindow (need desktop shell + Vulkan GPU factory)")
endif()

# RHI sample framework and the higher-level sample groups (backend-agnostic through Samples::Framework).
add_subdirectory(Framework)
if(TARGET Samples::Framework)
    add_subdirectory(RHI)  # backend-agnostic via the framework (Smoketest gates itself on Vulkan)
    add_subdirectory(WebScene) # full-renderer exercise scene (desktop build; also the web sample)
    add_subdirectory(VG)  # VG sandbox: end-to-end VG stack demo
    add_subdirectory(GUISandbox) # GUI sandbox: foundation.gui's widgets, sheets and scripts on the VG renderer
endif()
