add_executable(Sequence.Pipeline.Tests TestsMain.cpp SequencePipelineTests.cpp)
target_link_libraries(Sequence.Pipeline.Tests PRIVATE
    Pipeline::Sequence Foundation::Sequence Foundation::Sequence.Resource Foundation::PropertyAnimation
    Foundation::Content Foundation::Resource Foundation::VFS Pipeline::Core doctest::doctest)
target_compile_definitions(Sequence.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Sequence.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Sequence.Pipeline.Tests COMMAND Sequence.Pipeline.Tests)
