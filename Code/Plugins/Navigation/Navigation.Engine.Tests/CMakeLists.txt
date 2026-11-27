# Navigation.Engine's tests: zones, agents and the scene's navigation.
add_executable(Navigation.Engine.Tests TestsMain.cpp NavigationSceneTests.cpp)
target_link_libraries(Navigation.Engine.Tests PRIVATE
    Plugins::Navigation.Engine Foundation::Scene Foundation::Resource Foundation::Content Foundation::VFS
    Foundation::Core Foundation::Xml Foundation::Xml.Serialization
    doctest::doctest)
target_compile_definitions(Navigation.Engine.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Navigation.Engine.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Navigation.Engine.Tests COMMAND Navigation.Engine.Tests)
