# Foundation::VFS.Zip - `foundation.vfs.zip` (a read-only VFS backend over a zip archive).
# Separate from the core VFS so the zip reader (and its zlib) stays out of it.

util_add_engine_library(VFS.Zip ALIAS Foundation::VFS.Zip)
target_link_libraries(VFS.Zip PUBLIC Foundation::Core Foundation::VFS Foundation::Zip Foundation::Policy)

target_sources(VFS.Zip
    PUBLIC
        FILE_SET vfs_zip_modules TYPE CXX_MODULES FILES
            ZipFileSystem.cppm)
