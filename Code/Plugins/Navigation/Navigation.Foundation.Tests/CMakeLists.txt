# Navigation.Foundation's tests: building and querying navmeshes.
add_executable(Navigation.Foundation.Tests TestsMain.cpp NavigationTests.cpp)
target_link_libraries(Navigation.Foundation.Tests PRIVATE
    Plugins::Navigation.Foundation
    doctest::doctest)
target_compile_definitions(Navigation.Foundation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Navigation.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Navigation.Foundation.Tests COMMAND Navigation.Foundation.Tests)
