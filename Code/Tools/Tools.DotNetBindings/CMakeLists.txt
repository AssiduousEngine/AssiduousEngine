# Tools.DotNetBindings - writes the engine's C# bindings, generated from its reflection.
add_executable(Tools.DotNetBindings Main.cpp)
# The whole script surface: every facade and every component's reflection, as a game's scripts see it.
target_link_libraries(Tools.DotNetBindings PRIVATE Foundation::Script.DotNet Engine::Composition)
