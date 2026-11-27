# ScriptPlayground - the foundation.script entity-behaviors consumer proof: cubes driven
# by AngelScript Mover/Spinner behaviors ticked by the BehaviorSubsystem under simulation.
add_executable(ScriptPlayground main.cpp)

target_link_libraries(ScriptPlayground PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.SDL3 Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Foundation::Geometry Foundation::Geometry.Resource
    Foundation::Materials Foundation::Materials.Resource
    # Both backends (guarded aliases, empty when a backend is off) so --script= can pick either;
    # the runtime registration itself comes from Engine::DefaultApp.
    Foundation::Script $<TARGET_NAME_IF_EXISTS:Plugins::AngelScript.Foundation> $<TARGET_NAME_IF_EXISTS:Plugins::Luau.Foundation> Foundation::Script.Resource
    Foundation::Script.Facades Engine::Behavior
    Foundation::Policy)
util_copy_sdl3_dll(ScriptPlayground)
