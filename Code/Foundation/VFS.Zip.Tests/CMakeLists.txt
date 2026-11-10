# Foundation::VFS.Zip unit tests (doctest), over the zip fixtures in Data/Tests/Zip.

add_executable(VFS.Zip.Tests TestsMain.cpp ZipFileSystemTests.cpp)
target_link_libraries(VFS.Zip.Tests PRIVATE Foundation::VFS.Zip Foundation::VFS.Pak Foundation::Zip doctest::doctest)
target_compile_definitions(VFS.Zip.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(VFS.Zip.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME VFS.Zip.Tests COMMAND VFS.Zip.Tests)
