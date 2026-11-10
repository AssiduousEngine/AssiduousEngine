// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VFS.Zip tests: the files under one folder of a zip, read through the VFS (Data/Tests/Zip's
// project.zip: Game/ holding a stored noise.bin and a deflated Level1.scene), and a pak read in
// place from inside another file, as a game's Content.pak is from an Android APK.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.vfs.pak;
import foundation.vfs.zip;
import foundation.zip;

using namespace foundation::core;
using namespace foundation::vfs;

namespace
{
    String Fixture(StringView name)
    {
        return PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(TEST_SOURCE_DIR)), u8"Data/Tests/Zip").AsView(), name);
    }

    Array<byte> ReadAll(IStream& stream)
    {
        Array<byte> out(static_cast<usize>(stream.Size()));
        if (!out.IsEmpty())
        {
            (void)stream.Read(out.Data(), out.Size());
        }
        return out;
    }

    // The entry's bytes as the zip reader extracts them, to compare with what the VFS reads.
    Array<byte> Extracted(StringView zipPath, StringView name)
    {
        FileStream file(zipPath, FileMode::Read);
        foundation::zip::ZipArchive zip;
        String error;
        Array<byte> out;
        if (zip.Open(file, error))
        {
            for (const foundation::zip::ZipEntry& entry : zip.Entries())
            {
                if (entry.name.AsView() == name)
                {
                    (void)zip.Read(entry, out, error);
                }
            }
        }
        return out;
    }

    bool Same(const Array<byte>& a, const Array<byte>& b)
    {
        if (a.Size() != b.Size())
        {
            return false;
        }
        for (usize i = 0; i < a.Size(); ++i)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    bool Lists(const Array<DirEntry>& entries, StringView name, bool isDirectory)
    {
        for (const DirEntry& entry : entries)
        {
            if (entry.name.AsView() == name && entry.isDirectory == isDirectory)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("vfs.zip: a folder of a zip reads as a file system, stored files in place and deflated ones inflated")
{
    const String zipPath = Fixture(u8"project.zip");
    ZipFileSystem fs(zipPath.AsView(), u8"Game", DefaultAllocator());
    REQUIRE(fs.IsValid());
    CHECK(fs.AsEnumerable() != nullptr);
    CHECK(fs.AsWritable() == nullptr);

    CHECK(fs.Exists(u8"Project.xml"));
    CHECK(fs.Exists(u8"Scenes")); // a folder with a file below it
    CHECK(fs.Exists(u8"Scenes/Level1.scene"));
    CHECK_FALSE(fs.Exists(u8"Game/Project.xml")); // named below the folder, not with it
    CHECK_FALSE(fs.Exists(u8"Missing.txt"));

    Array<DirEntry> top;
    REQUIRE(fs.AsEnumerable()->Enumerate(u8"", top).IsOk());
    CHECK(Lists(top, u8"Project.xml", false));
    CHECK(Lists(top, u8"Empty.txt", false));
    CHECK(Lists(top, u8"Audio", true));
    CHECK(Lists(top, u8"Scenes", true));
    Array<DirEntry> scenes;
    REQUIRE(fs.AsEnumerable()->Enumerate(u8"Scenes", scenes).IsOk());
    REQUIRE(scenes.Size() == 1u);
    CHECK(Lists(scenes, u8"Level1.scene", false));

    // Stored: read in place, the same bytes the zip reader extracts, and a region to read it by.
    u64 offset = 0, size = 0;
    REQUIRE(fs.StoredRegion(u8"Audio/noise.bin", offset, size));
    CHECK(size == 3000u);
    UniquePtr<IStream> noise = fs.Open(u8"Audio/noise.bin", FileMode::Read);
    REQUIRE(static_cast<bool>(noise));
    CHECK(noise->Size() == 3000);
    const Array<byte> noiseBytes = ReadAll(*noise);
    CHECK(Same(noiseBytes, Extracted(zipPath.AsView(), u8"Game/Audio/noise.bin")));
    CHECK(noise->Seek(2990, SeekOrigin::Begin) == 2990);
    byte tail[32] = {};
    CHECK(noise->Read(tail, sizeof(tail)) == 10u); // reads stop at the file's end, not the archive's

    // Deflated: inflated whole, and no region to read it in place by.
    CHECK_FALSE(fs.StoredRegion(u8"Scenes/Level1.scene", offset, size));
    UniquePtr<IStream> scene = fs.Open(u8"Scenes/Level1.scene", FileMode::Read);
    REQUIRE(static_cast<bool>(scene));
    CHECK(scene->Size() == 8092);
    CHECK(Same(ReadAll(*scene), Extracted(zipPath.AsView(), u8"Game/Scenes/Level1.scene")));

    CHECK_FALSE(static_cast<bool>(fs.Open(u8"Project.xml", FileMode::Write))); // read-only

    // The whole archive, with no folder: names keep their own.
    ZipFileSystem whole(zipPath.AsView(), u8"", DefaultAllocator());
    REQUIRE(whole.IsValid());
    CHECK(whole.Exists(u8"Game/Project.xml"));

    ZipFileSystem missing(u8"scratch_no_such.zip", u8"Game", DefaultAllocator());
    CHECK_FALSE(missing.IsValid());
    CHECK_FALSE(missing.Exists(u8"Project.xml"));
}

// A game's Content.pak stored in its APK is read where it lies: a pak over a region of a larger
// file, its offsets its own.
TEST_CASE("vfs.pak: a pak inside a larger file reads in place from its region")
{
    const StringView pakPath = u8"scratch_inner.pak";
    const StringView outerPath = u8"scratch_outer.bin";
    {
        PakBuilder builder;
        const char hello[] = "hello world";
        builder.Add(u8"hello.txt", Span<const byte>(reinterpret_cast<const byte*>(hello), sizeof(hello) - 1));
        const char leaf[] = "leaf";
        builder.Add(u8"data/leaf.txt", Span<const byte>(reinterpret_cast<const byte*>(leaf), sizeof(leaf) - 1));
        REQUIRE(builder.Write(pakPath).IsOk());
    }
    const Result<Array<byte>> pak = ReadFile(pakPath);
    REQUIRE(pak.HasValue());
    Array<byte> outer;
    for (int i = 0; i < 37; ++i)
    {
        outer.PushBack(static_cast<byte>(0xAB)); // what lies before it in the file
    }
    for (const byte b : pak.Value())
    {
        outer.PushBack(b);
    }
    for (int i = 0; i < 11; ++i)
    {
        outer.PushBack(static_cast<byte>(0xCD)); // and after
    }
    REQUIRE(WriteFile(outerPath, Span<const byte>(outer.Data(), outer.Size())).IsOk());

    PakFileSystem fs(outerPath, 37, pak.Value().Size(), DefaultAllocator());
    REQUIRE(fs.IsValid());
    CHECK(fs.EntryCount() == 2u);
    UniquePtr<IStream> hello = fs.Open(u8"hello.txt", FileMode::Read);
    REQUIRE(static_cast<bool>(hello));
    char text[16] = {};
    CHECK(hello->Read(text, 11) == 11u);
    CHECK(StringView(reinterpret_cast<const utf8char*>(text), 11) == u8"hello world");
    CHECK(fs.Exists(u8"data/leaf.txt"));

    PakFileSystem misplaced(outerPath, 36, pak.Value().Size(), DefaultAllocator()); // one byte off
    CHECK_FALSE(misplaced.IsValid());
    PakFileSystem pastEnd(outerPath, 37, outer.Size(), DefaultAllocator()); // runs past the file
    CHECK_FALSE(pastEnd.IsValid());

    (void)FileDelete(pakPath);
    (void)FileDelete(outerPath);
}
