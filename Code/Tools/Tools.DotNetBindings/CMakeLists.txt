# Tools.DotNetBindings - writes the engine's C# bindings, generated from its reflection.
add_executable(Tools.DotNetBindings Main.cpp)
target_link_libraries(Tools.DotNetBindings PRIVATE Foundation::Script.DotNet)
