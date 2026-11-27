# Engine::DefaultApp - `engine.defaultapp` (DefaultApplication: the
# opinionated IApplication that registers the standard engine subsystems). Kept in
# its own library so the base client (Foundation::Runtime.Client) never pulls in the
# engine subsystem libraries; only apps that opt into the defaults link this.
# A new engine subsystem is added as a link dependency here.

util_add_engine_library(Engine.DefaultApp ALIAS Engine::DefaultApp)
target_link_libraries(Engine.DefaultApp
    PUBLIC Foundation::Core Foundation::RHI Foundation::Runtime.Client Engine::Run Foundation::Shell Foundation::Graphics
           Foundation::VFS
           Foundation::Scene Engine::Scene Engine::Render Engine::Animation
           Foundation::Physics Engine::Physics
           Foundation::Audio Foundation::Audio.Resource Engine::Audio
           Foundation::Fonts.Resource
           Foundation::Input Engine::Input Foundation::Script
           Foundation::Script.Resource Engine::Behavior
           Foundation::Resource Foundation::Content Foundation::Scene.Resource
           Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Animation.Resource
           Foundation::PropertyAnimation.Resource
           Foundation::Input.Resource Foundation::Physics.Resource
           Foundation::Texture.Resource Foundation::Image.Resource Foundation::Model.Resource
           Foundation::Heightfield.Resource
            Foundation::Net Foundation::Net.Manager Engine::Net
            Foundation::Profiler Foundation::Profiler.GUI Foundation::Policy Engine::Composition 
            Foundation::Image Foundation::Image.IO Foundation::RHI.Capture)

# The script VM built into the default application: C#, when it is on (OPTION_ENABLE_DOTNET and the
# target-or-empty ${SCRIPTLIB_DOTNET}). AngelScript and Luau are plugins the host links and a
# project enables; a run resolves the backend by the game script's language.
target_link_libraries(Engine.DefaultApp PUBLIC ${SCRIPTLIB_DOTNET})
# C# (OPTION_ENABLE_DOTNET, desktop): the managed runtime it runs in is built beside the binaries.
if(DOTNET_SCRIPTING AND NOT EMSCRIPTEN)
    add_dependencies(Engine.DefaultApp DotNet.Runtime)
endif()

target_sources(Engine.DefaultApp
    PRIVATE
        DefaultApplicationImpl.cpp   # out-of-line defs (see DefaultApplication.cppm)
        QualityLevelsImpl.cpp        # the quality levels a run offers (see QualityLevels.cppm)
        DisplaySettingsImpl.cpp      # how a run's window presents (see DisplaySettings.cppm)
    PUBLIC
        FILE_SET runtime_defaultapp_modules TYPE CXX_MODULES FILES
            DefaultApplication.cppm
            ProfilerOverlayHost.cppm
            QualityLevels.cppm
            DisplaySettings.cppm)

target_link_libraries(Engine.DefaultApp PUBLIC Engine::GUI) # the engine GUI
# The engine version a plugin host checks plugins against, and the project's quality levels a run
# offers (the :quality partition's interface names ProjectSettings and the user settings store).
target_link_libraries(Engine.DefaultApp PUBLIC Engine::Project Foundation::Settings)
