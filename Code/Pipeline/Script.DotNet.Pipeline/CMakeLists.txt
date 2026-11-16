# Pipeline::Script.DotNet - `script.dotnet.pipeline` (the C# cook service).
# A C# project (.csproj) builds every .cs under its folder into one assembly with `dotnet build`,
# and the runtime describes its classes; each C# class cooks from that description. Cook-only
# (UI-free). Desktop, with OPTION_ENABLE_DOTNET.
util_add_engine_library(Script.DotNet.Pipeline ALIAS Pipeline::Script.DotNet)
target_link_libraries(Script.DotNet.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Foundation::Content
    Foundation::Script Foundation::Script.Resource Pipeline::Script Pipeline::Importer Foundation::Script.DotNet)
# The cook builds against the engine's managed runtime, and describes with it.
add_dependencies(Script.DotNet.Pipeline DotNet.Runtime)
target_sources(Script.DotNet.Pipeline
    PRIVATE
        DotNetScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_dotnet_pipeline_modules TYPE CXX_MODULES FILES DotNetScriptCook.cppm)
