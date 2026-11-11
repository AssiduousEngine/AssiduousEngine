# Foundation::Shell.SDL3 - `foundation.shell.sdl3` (SDL3-based IShell for Windows, Linux, macOS and Android:
# windowing, input, events). The desktop run loop lives in foundation.runtime.desktop, so this
# backend needs only the shell interface and SDL3, with no client or graphics dependency.

util_add_engine_library(Shell.SDL3 ALIAS Foundation::Shell.SDL3)
target_link_libraries(Shell.SDL3
    PUBLIC Foundation::Shell Foundation::Policy
    PRIVATE ${ENGINE_SDL_TARGET}) # vendored static SDL3 (or system SDL3 under ENGINE_SYSTEM_SDL)

target_sources(Shell.SDL3
    PRIVATE
        SDL3ShellImpl.cpp   # out-of-line SDL backend defs (see SDL3Shell.cppm)
    PUBLIC
        FILE_SET shell_desktop_modules TYPE CXX_MODULES FILES
            SDL3Shell.cppm)

