add_executable(Sequence.Tests TestsMain.cpp SequenceTests.cpp)
target_link_libraries(Sequence.Tests PRIVATE
    Foundation::Sequence Foundation::PropertyAnimation Foundation::Scene Foundation::Core doctest::doctest)
target_compile_definitions(Sequence.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Sequence.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Sequence.Tests COMMAND Sequence.Tests)
