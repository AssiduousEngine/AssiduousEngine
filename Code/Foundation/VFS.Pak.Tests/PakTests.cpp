// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <cstring>

import foundation.core;
import foundation.vfs;
import foundation.vfs.pak;

using namespace foundation::core;
using namespace foundation::vfs;

namespace
{
    Span<const byte> Bytes(const char* s)
    {
        return Span<const byte>{reinterpret_cast<const byte*>(s), std::strlen(s)};
    }
}

TEST_CASE("vfs.pak: build, open, read, enumerate")
{
    const StringView pak = u8"scratch_pak_test.pak";
    FileDelete(pak);

    // --- build ---
    {
        PakBuilder builder;
        builder.Add(u8"hello.txt", Bytes("hello world"));
        builder.Add(u8"data/blob.bin", Bytes("XYZ"));
        builder.Add(u8"data/deep/leaf.txt", Bytes("leaf"));
        REQUIRE(builder.Write(pak).IsOk());
    }

    // --- open ---
    PakFileSystem fs(pak);
    REQUIRE(fs.IsValid());
    CHECK(fs.EntryCount() == 3u);

    // Capabilities: read and enumerate, but not writable.
    CHECK(fs.AsEnumerable() != nullptr);
    CHECK(fs.AsWritable() == nullptr);

    // Existence.
    CHECK(fs.Exists(u8"hello.txt"));
    CHECK(fs.Exists(u8"data/blob.bin"));
    CHECK_FALSE(fs.Exists(u8"missing.txt"));

    // Read an entry back.
    {
        UniquePtr<IStream> stream = fs.Open(u8"hello.txt", FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        char buffer[16] = {};
        const u64 n = stream->Read(buffer, 11);
        CHECK(n == 11u);
        CHECK(std::strncmp(buffer, "hello world", 11) == 0);
    }

    // Write mode is unsupported (read-only archive).
    CHECK_FALSE(static_cast<bool>(fs.Open(u8"hello.txt", FileMode::Write)));
    // Missing entry.
    CHECK_FALSE(static_cast<bool>(fs.Open(u8"nope", FileMode::Read)));

    // Enumerate root: a file (hello.txt) and a directory (data/).
    {
        Array<DirEntry> entries;
        REQUIRE(fs.Enumerate(u8"", entries).IsOk());
        bool foundFile = false;
        bool foundDir = false;
        for (const DirEntry& e : entries)
        {
            if (e.name == u8"hello.txt")
            {
                foundFile = true;
                CHECK_FALSE(e.isDirectory);
            }
            if (e.name == u8"data")
            {
                foundDir = true;
                CHECK(e.isDirectory);
            }
        }
        CHECK(foundFile);
        CHECK(foundDir);
    }

    // Enumerate a subfolder: a file (blob.bin) and a nested directory (deep/).
    {
        Array<DirEntry> entries;
        REQUIRE(fs.Enumerate(u8"data", entries).IsOk());
        bool foundBlob = false;
        bool foundDeep = false;
        for (const DirEntry& e : entries)
        {
            if (e.name == u8"blob.bin")
            {
                foundBlob = true;
                CHECK_FALSE(e.isDirectory);
            }
            if (e.name == u8"deep")
            {
                foundDeep = true;
                CHECK(e.isDirectory);
            }
        }
        CHECK(foundBlob);
        CHECK(foundDeep);
    }

    FileDelete(pak);
}

TEST_CASE("vfs.pak: a non-pak file is rejected")
{
    const StringView path = u8"scratch_pak_bad.pak";
    const byte junk[] = {byte{1}, byte{2}, byte{3}, byte{4}};
    REQUIRE(WriteFile(path, Span<const byte>{junk, ArrayCount(junk)}).IsOk());

    PakFileSystem fs(path);
    CHECK_FALSE(fs.IsValid());
    CHECK_FALSE(fs.Exists(u8"anything"));

    FileDelete(path);
}

namespace
{
    Array<u8> ReadAll(StringView path)
    {
        Array<u8> bytes;
        FileStream file(path, FileMode::Read);
        if (!file.IsValid())
        {
            return bytes;
        }
        bytes.Resize(static_cast<usize>(file.Size()));
        (void)file.Read(bytes.Data(), bytes.Size());
        return bytes;
    }
    void WriteAll(StringView path, const Array<u8>& bytes)
    {
        FileDelete(path);
        FileStream file(path, FileMode::Write);
        REQUIRE(file.IsValid());
        (void)file.Write(bytes.Data(), bytes.Size());
    }
    void PutU64(Array<u8>& bytes, usize at, u64 value)
    {
        for (usize i = 0; i < 8; ++i)
        {
            bytes[at + i] = static_cast<u8>((value >> (8 * i)) & 0xFFu);
        }
    }
    u64 GetU64(const Array<u8>& bytes, usize at)
    {
        u64 v = 0;
        for (usize i = 0; i < 8; ++i)
        {
            v |= static_cast<u64>(bytes[at + i]) << (8 * i);
        }
        return v;
    }
}

TEST_CASE("vfs.pak: a corrupt or truncated header is refused rather than sized from")
{
    // Header layout: magic u32 @0, version u32 @4, entryCount u64 @8, tocOffset u64 @16, tocSize u64 @24.
    const StringView good = u8"scratch_pak_good.pak";
    const StringView bad = u8"scratch_pak_bad.pak";
    FileDelete(good);
    {
        PakBuilder builder;
        builder.Add(u8"a.txt", Bytes("alpha"));
        builder.Add(u8"b.txt", Bytes("beta"));
        REQUIRE(builder.Write(good).IsOk());
    }
    const Array<u8> original = ReadAll(good);
    REQUIRE(original.Size() > 32u);
    const u64 tocOffset = GetU64(original, 16);
    const u64 tocSize = GetU64(original, 24);
    REQUIRE(tocOffset >= 32u);

    SUBCASE("entry count the table cannot hold")
    {
        Array<u8> bytes = original;
        PutU64(bytes, 8, 0x7FFFFFFFFFFFFFFFull);
        WriteAll(bad, bytes);
        PakFileSystem fs(bad);
        CHECK_FALSE(fs.IsValid());
    }
    SUBCASE("table past the end of the file")
    {
        Array<u8> bytes = original;
        PutU64(bytes, 16, static_cast<u64>(bytes.Size()) + 4096u);
        WriteAll(bad, bytes);
        PakFileSystem fs(bad);
        CHECK_FALSE(fs.IsValid());
    }
    SUBCASE("table longer than what follows its offset")
    {
        Array<u8> bytes = original;
        PutU64(bytes, 24, tocSize + 1000u);
        WriteAll(bad, bytes);
        PakFileSystem fs(bad);
        CHECK_FALSE(fs.IsValid());
    }
    SUBCASE("an entry whose bytes lie outside the data heap")
    {
        Array<u8> bytes = original;
        // Entry 0 at tocOffset: u16 locatorLength, locator, u64 offset, u64 storedSize ...
        const usize len = bytes[static_cast<usize>(tocOffset)] | (bytes[static_cast<usize>(tocOffset) + 1] << 8);
        const usize storedSizeAt = static_cast<usize>(tocOffset) + 2 + len + 8;
        PutU64(bytes, storedSizeAt, 1u << 30);
        WriteAll(bad, bytes);
        PakFileSystem fs(bad);
        CHECK_FALSE(fs.IsValid());
    }
    SUBCASE("a file truncated inside the table")
    {
        Array<u8> bytes = original;
        bytes.Resize(static_cast<usize>(tocOffset) + 5);
        WriteAll(bad, bytes);
        PakFileSystem fs(bad);
        CHECK_FALSE(fs.IsValid());
    }
    SUBCASE("the untouched file still opens")
    {
        PakFileSystem fs(good);
        CHECK(fs.IsValid());
        CHECK(fs.EntryCount() == 2u);
    }
    FileDelete(bad);
    FileDelete(good);
}

TEST_CASE("vfs.pak: an entry opens as a view over its region, and every one of thousands is found")
{
    // Find searched the table linearly, and Open read the entry whole into a buffer and copied it
    // again into a memory stream: a header read paid for the whole envelope.
    const StringView pak = u8"scratch_pak_view.pak";
    FileDelete(pak);
    constexpr int kEntries = 3000;
    {
        PakBuilder builder;
        for (int i = 0; i < kEntries; ++i)
        {
            String name = Format(u8"set/entry{}.bin", i);
            String text = Format(u8"payload-{}-", i);
            for (int r = 0; r < 20; ++r)
            {
                text.Append(u8"0123456789");
            }
            Array<byte> bytes;
            for (const utf8char c : text.AsView())
            {
                bytes.PushBack(static_cast<byte>(c));
            }
            builder.Add(name.AsView(), Span<const byte>{bytes.Data(), bytes.Size()});
        }
        REQUIRE(builder.Write(pak).IsOk());
    }
    PakFileSystem fs(pak);
    REQUIRE(fs.IsValid());
    REQUIRE(fs.EntryCount() == static_cast<usize>(kEntries));
    const int probes[] = {0, 1234, kEntries - 1};
    for (const int i : probes)
    {
        const String name = Format(u8"set/entry{}.bin", i);
        UniquePtr<IStream> stream = fs.Open(name.AsView(), FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        const String head = Format(u8"payload-{}-", i);
        CHECK(stream->Size() == static_cast<i64>(head.Size() + 200));
        Array<char> buffer;
        buffer.Resize(head.Size());
        REQUIRE(stream->Read(buffer.Data(), buffer.Size()) == buffer.Size());
        CHECK(std::strncmp(buffer.Data(), reinterpret_cast<const char*>(head.Data()), head.Size()) == 0);
        // Seeking within the region reads from it, not from the archive's start.
        REQUIRE(stream->Seek(-10, SeekOrigin::End) >= 0);
        char tail[10] = {};
        REQUIRE(stream->Read(tail, 10) == 10u);
        CHECK(std::strncmp(tail, "0123456789", 10) == 0);
    }
    CHECK_FALSE(fs.Exists(u8"set/entry3000.bin"));
    FileDelete(pak);
}
