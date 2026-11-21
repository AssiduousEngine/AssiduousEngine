add_executable(Engine.Sequence.Tests TestsMain.cpp SequenceSystemTests.cpp)
target_link_libraries(Engine.Sequence.Tests PRIVATE
    Engine::Sequence Foundation::Sequence Foundation::Sequence.Resource Foundation::PropertyAnimation
    Foundation::Scene Foundation::Messaging Foundation::Run Foundation::Core doctest::doctest)
target_compile_definitions(Engine.Sequence.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Sequence.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.Sequence.Tests COMMAND Engine.Sequence.Tests)
