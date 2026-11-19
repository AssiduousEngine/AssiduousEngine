# Engine::Run - `engine.run`: a running game as a first-class
# object (scene pairing, script run context, `Game`, instance time scale). Over scene and script.

util_add_engine_library(Engine.Run ALIAS Engine::Run)
target_link_libraries(Engine.Run PUBLIC
    Foundation::Core Foundation::Scene Foundation::Scene.Resource Foundation::Content Foundation::Resource
    Foundation::Script Engine::Script Foundation::Script.Facades
    Foundation::Net.Manager Foundation::Run Foundation::Net.Tls Foundation::Http.Client Foundation::Http.Script Engine::Net Foundation::Input Foundation::Settings Foundation::Xml.Serialization Engine::Domain Foundation::Policy)

# The run's TLS backend: Mbed TLS natively; a browser does TLS itself (Http.Client's fetch, the
# browser's WebSocket).
if(NOT EMSCRIPTEN)
    target_link_libraries(Engine.Run PRIVATE Foundation::Net.Tls.MbedTLS)
endif()

target_sources(Engine.Run
    PUBLIC
        FILE_SET engine_run_modules TYPE CXX_MODULES FILES
            NetworkController.cppm
            ApplicationRun.cppm
            RunSave.cppm
    PRIVATE
        ApplicationRunImpl.cpp
        NetworkControllerImpl.cpp
        RunSaveImpl.cpp)

