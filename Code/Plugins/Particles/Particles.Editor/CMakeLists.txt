# Particles.Editor (Plugins::Particles.Editor) - `editor.particles`: the particle effect page (the
# systems as a tree, the live preview, the rows of the chosen system) and the effects' thumbnails.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource
    Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::GUI Foundation::GUI.Toolkit
    Editor::Core Editor::App Editor::Scene Editor::Preview Foundation::Policy)
target_sources(${PLUGIN_PART}
    PRIVATE
        ParticlesEditor.cpp       # the part's entry
        ParticleEffectPageImpl.cpp
    PUBLIC
        FILE_SET editor_particles_modules TYPE CXX_MODULES FILES
            ParticleEffectPage.cppm
            EditorParticlesModule.cppm)
