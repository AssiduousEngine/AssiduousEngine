# Foundation::Shell.SDL3 unit tests (doctest). Force SDL's dummy video
# driver at runtime so these are headless/CI-safe.

add_executable(Shell.SDL3.Tests
    TestsMain.cpp
    SDL3ShellTests.cpp)

# The test TU includes <SDL3/SDL.h> directly to push synthetic events.
target_link_libraries(Shell.SDL3.Tests PRIVATE
    Foundation::Shell.SDL3 Foundation::Runtime.Client Foundation::Runtime.Desktop ${ENGINE_SDL_TARGET} doctest::doctest)

target_compile_definitions(Shell.SDL3.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Shell.SDL3.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Shell.SDL3.Tests COMMAND Shell.SDL3.Tests)

# Copy SDL3.dll next to the test executable on Windows. Nothing to stage in a static
# engine build (SDL is a vendored archive there). Under ENGINE_SHARED_LIBS SDL is shared, one
# runtime for this exe and Shell.SDL3.dll, so the events the tests push reach the shell, and
# TARGET_RUNTIME_DLLS picks it up.
util_copy_runtime_deps(Shell.SDL3.Tests)
