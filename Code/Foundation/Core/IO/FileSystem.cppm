// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :filesystem partition
//
// Whole-file convenience helpers over FileStream. Directory queries live in
// :system (DirectoryExists/CreateDirectory/RemoveDirectory) and are re-exported
// here for a single filesystem surface.

module;
#include "Core/Prelude.h"

export module foundation.core:filesystem;

import :base;
import :allocator;
import :array;
import :span;
import :string;
import :system;
import :io;
import :path; // PathJoin (CopyDirectoryRecursive)
import :format; // TemporaryPathFor
import :atomic;

export namespace foundation::core
{
    // Reads an entire file into a byte buffer.
    [[nodiscard]] inline Result<Array<byte>> ReadFile(StringView path,
                                                      IAllocator& allocator = DefaultAllocator())
    {
        FileStream file(path, FileMode::Read);
        if (!file.IsValid())
        {
            return Err(ErrorCode::NotFound);
        }

        const i64 size = file.Size();
        if (size < 0)
        {
            return Err(ErrorCode::Internal);
        }

        Array<byte> data(allocator);
        data.Resize(static_cast<usize>(size));
        if (size > 0)
        {
            const u64 read = file.Read(data.Data(), static_cast<u64>(size));
            if (read != static_cast<u64>(size))
            {
                return Err(ErrorCode::Internal);
            }
        }
        return data;
    }

    // Who may read a file a write makes: whoever its folder lets (Shared), or its owner alone
    // (OwnerOnly, for a secret such as a token).
    enum class FileAccess : u8
    {
        Shared,
        OwnerOnly
    };

    // The temporary file a whole-file write goes to before it is renamed over `path`:
    // "<path>.<process>-<n>.tmp", unique per process and per write, so two writers of one file
    // never share a temporary. The ".tmp" ending is what file watchers skip.
    [[nodiscard]] inline String TemporaryPathFor(StringView path, IAllocator& allocator = DefaultAllocator())
    {
        static Atomic<u32> writes{0};
        const u32 n = writes.fetch_add(1, MemoryOrder::Relaxed);
        String temporary(path, allocator);
        temporary.Append(Format(u8".{}-{}.tmp", ProcessId(), n).AsView());
        return temporary;
    }

    // Writes a byte buffer to a file so that a reader, or the next run after a crash or a power
    // loss, sees the old contents or the new ones, never a torn mix: the bytes go to a temporary
    // file beside the target (TemporaryPathFor), are flushed to the device, and the temporary is
    // renamed over the target (a rename on one volume replaces it atomically). A failed write
    // leaves the target as it was and no temporary behind.
    [[nodiscard]] inline Status WriteFileAtomic(StringView path, Span<const byte> data,
                                                IAllocator& allocator = DefaultAllocator(),
                                                FileAccess access = FileAccess::Shared)
    {
        const String temporary = TemporaryPathFor(path, allocator);
        bool written = false;
        {
            FileStream file(temporary.AsView(), FileMode::Write);
            written = file.IsValid() && (access == FileAccess::Shared || file.RestrictToOwner()) &&
                      (data.IsEmpty() || file.Write(data.Data(), data.Size()) == data.Size()) &&
                      file.Sync();
        }
        if (!written)
        {
            (void)FileDelete(temporary.AsView());
            return Status{ErrorCode::Internal};
        }
        if (!FileMove(temporary.AsView(), path))
        {
            (void)FileDelete(temporary.AsView());
            return Status{ErrorCode::Internal};
        }
        return Status{};
    }

    // Writes a byte buffer to a file, replacing its contents whole or not at all
    // (WriteFileAtomic): never a truncated file after a crash or a full disk.
    [[nodiscard]] inline Status WriteFile(StringView path, Span<const byte> data)
    {
        return WriteFileAtomic(path, data);
    }

    // Moves a file that could not be read out of the way, so that what replaces it never writes
    // over it and it can still be recovered by hand: to "<path>.unreadable", or "<path>.unreadable-2",
    // -3 and on when that is taken. The path it went to, or an error when it could not be moved
    // (then nothing may be written over it).
    [[nodiscard]] inline Result<String> SetAsideUnreadable(StringView path,
                                                           IAllocator& allocator = DefaultAllocator())
    {
        String aside(path, allocator);
        aside.Append(u8".unreadable");
        for (u32 n = 2; FileExists(aside.AsView()); ++n)
        {
            aside = String(path, allocator);
            aside.Append(Format(u8".unreadable-{}", n).AsView());
        }
        if (!FileMove(path, aside.AsView()))
        {
            return Err(ErrorCode::Internal);
        }
        return aside;
    }

    // Copies the folder `from` and everything under it into `to` (made as needed), each file with
    // FileCopyPreserving, so what is there already is overwritten and executables keep their +x
    // bit. Composes the portable primitives, as RemoveDirectoryRecursive does. False at the first
    // failure (what was copied before it stays), or when `from` is no folder.
    inline bool CopyDirectoryRecursive(StringView from, StringView to) // NOLINT(misc-no-recursion)
    {
        if (!DirectoryExists(from) || !CreateDirectories(to))
        {
            return false;
        }
        struct Entry
        {
            String name;
            bool isDirectory;
        };
        Array<Entry> entries;
        if (!ListDirectory(
                from,
                [](void* ctx, StringView name, bool isDirectory)
                { static_cast<Array<Entry>*>(ctx)->PushBack(Entry{String(name), isDirectory}); },
                &entries))
        {
            return false;
        }
        for (const Entry& entry : entries)
        {
            const String source = PathJoin(from, entry.name.AsView());
            const String target = PathJoin(to, entry.name.AsView());
            const bool copied = entry.isDirectory ? CopyDirectoryRecursive(source.AsView(), target.AsView())
                                                  : FileCopyPreserving(source.AsView(), target.AsView());
            if (!copied)
            {
                return false;
            }
        }
        return true;
    }

    // Removes a directory and everything under it. `RemoveDirectory` is the raw backend
    // primitive (rmdir), which removes only empty directories and silently fails on a
    // populated one. This composes the portable primitives, with no per-platform code.
    // Returns true when the directory is gone afterwards (a missing dir counts as gone).
    inline bool RemoveDirectoryRecursive(StringView path) // NOLINT(misc-no-recursion)
    {
        if (!DirectoryExists(path))
        {
            return true;
        }
        // Snapshot the children first: entry names are only valid during the callback,
        // and deleting while the backend iterates the directory is undefined.
        struct Entry
        {
            String name;
            bool isDirectory;
        };
        struct Collect
        {
            Array<Entry>* entries;
        };
        Array<Entry> entries;
        Collect collect{&entries};
        if (!ListDirectory(
                path,
                [](void* ctx, StringView name, bool isDirectory)
                {
                    auto* c = static_cast<Collect*>(ctx);
                    c->entries->PushBack(Entry{String(name), isDirectory});
                },
                &collect))
        {
            return false;
        }
        for (const Entry& entry : entries)
        {
            String child(path);
            child.Append(u8'/');
            child.Append(entry.name.AsView());
            if (entry.isDirectory)
            {
                if (!RemoveDirectoryRecursive(child.AsView()))
                {
                    return false;
                }
            }
            else if (!FileDelete(child.AsView()))
            {
                return false;
            }
        }
        return RemoveDirectory(path);
    }
}
