# Editor::Sequence tests: the panel's UI-free half (the sequence document over a scratch project,
# the preview over a scene).
add_executable(Editor.Sequence.Tests TestsMain.cpp DocumentTests.cpp)
target_link_libraries(Editor.Sequence.Tests PRIVATE
    Editor::Sequence Editor::Core Foundation::Content Foundation::Scene Foundation::PropertyAnimation
    Foundation::Sequence Foundation::Sequence.Resource Pipeline::Sequence
    doctest::doctest)
target_compile_definitions(Editor.Sequence.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Sequence.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Sequence.Tests COMMAND Editor.Sequence.Tests)
