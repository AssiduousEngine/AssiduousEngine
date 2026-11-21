add_executable(Sequence.Resource.Tests TestsMain.cpp SequenceResourceTests.cpp)
target_link_libraries(Sequence.Resource.Tests PRIVATE
    Foundation::Sequence.Resource Foundation::Sequence Foundation::PropertyAnimation Foundation::Scene
    Foundation::Resource Foundation::Content Foundation::VFS Foundation::Core doctest::doctest)
target_compile_definitions(Sequence.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Sequence.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Sequence.Resource.Tests COMMAND Sequence.Resource.Tests)
