// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VFS.Zip - the `foundation.vfs.zip` module.
//
// ZipFileSystem: a read-only VFS backend over the files under one folder of a zip archive (an
// Android APK's assets/Game/, say). A stored entry is read in place, as a region of the archive
// file (FileRegionStream), so a large stored file (a game's Content.pak) is never copied whole; a
// deflated one is inflated into memory on Open. StoredRegion names where a stored entry lies, so
// another backend can read it in place too (a PakFileSystem over the pak inside the APK).

module;
#include "Core/Prelude.h"

export module foundation.vfs.zip;

import foundation.core;
import foundation.vfs;
import foundation.zip;

using namespace foundation::core;

export namespace foundation::vfs
{
    class ZipFileSystem final : public IFileSystem, public IEnumerableFileSystem
    {
    public:
        /// The files of the zip at `zipPath` under `folder` (a prefix such as "assets/Game"; empty
        /// for the whole archive), each named by its path below it.
        ZipFileSystem(StringView zipPath, StringView folder, IAllocator& allocator)
            : m_path(zipPath), m_allocator(&allocator), m_archiveFile(zipPath, FileMode::Read)
        {
            Load(folder);
        }

        ZipFileSystem(const ZipFileSystem&) = delete;
        ZipFileSystem& operator=(const ZipFileSystem&) = delete;

        /// True when the archive opened and its directory read.
        [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
        /// Why it did not, said for a person.
        [[nodiscard]] StringView Error() const noexcept { return m_error.AsView(); }
        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] StringView ArchivePath() const noexcept { return m_path.AsView(); }

        /// Where a stored (uncompressed) file lies in the archive file, to read in place. False for
        /// a file it does not hold or one that is compressed.
        [[nodiscard]] bool StoredRegion(StringView path, u64& offset, u64& size) const
        {
            const Entry* entry = Find(path);
            if (entry == nullptr || entry->method != 0)
            {
                return false;
            }
            offset = entry->dataOffset;
            size = entry->size;
            return true;
        }

        // --- IFileSystem ---
        [[nodiscard]] UniquePtr<IStream> Open(StringView path, FileMode mode) override
        {
            const Entry* entry = (m_valid && mode == FileMode::Read) ? Find(path) : nullptr;
            if (entry == nullptr)
            {
                return UniquePtr<IStream>{};
            }
            if (entry->method == 0)
            {
                // In place: its own handle on the archive, so readers on other threads never share
                // a position.
                FileRegionStream* region = m_allocator->New<FileRegionStream>(m_path.AsView(), entry->dataOffset, entry->size);
                if (!region->IsValid())
                {
                    m_allocator->Delete(region);
                    return UniquePtr<IStream>{};
                }
                return UniquePtr<IStream>{region, *m_allocator};
            }
            // Inflated whole; the archive's reader is one stream, so one inflate at a time.
            Array<byte> bytes;
            String error;
            {
                const ScopedLock lock(m_readLock);
                if (!m_archive.Read(m_zipEntries[entry->index], bytes, error))
                {
                    return UniquePtr<IStream>{};
                }
            }
            MemoryStream* stream = m_allocator->New<MemoryStream>();
            if (!bytes.IsEmpty())
            {
                (void)stream->Write(bytes.Data(), bytes.Size());
            }
            (void)stream->Seek(0, SeekOrigin::Begin);
            return UniquePtr<IStream>{stream, *m_allocator};
        }

        /// A file it holds, or a folder with a file below it.
        [[nodiscard]] bool Exists(StringView path) override
        {
            if (!m_valid)
            {
                return false;
            }
            if (Find(path) != nullptr)
            {
                return true;
            }
            for (const Entry& entry : m_entries)
            {
                StringView rest;
                if (RelativeUnder(entry.path.AsView(), path, rest))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] IEnumerableFileSystem* AsEnumerable() noexcept override { return this; }

        // --- IEnumerableFileSystem ---
        [[nodiscard]] Status Enumerate(StringView folder, Array<DirEntry>& out) override
        {
            if (!m_valid)
            {
                return Status{ErrorCode::NotFound};
            }
            for (const Entry& entry : m_entries)
            {
                StringView rest;
                if (!RelativeUnder(entry.path.AsView(), folder, rest))
                {
                    continue;
                }
                const usize slash = rest.Find(u8'/');
                if (slash == StringView::kNotFound)
                {
                    out.PushBack(DirEntry{String(rest), false});
                    continue;
                }
                const StringView dir = rest.SubStr(0, slash);
                bool listed = false;
                for (const DirEntry& known : out)
                {
                    listed = listed || (known.isDirectory && known.name.AsView() == dir);
                }
                if (!listed)
                {
                    out.PushBack(DirEntry{String(dir), true});
                }
            }
            return Status{};
        }

    private:
        struct Entry
        {
            String path;        // below the folder
            u64 dataOffset = 0; // where its data starts in the archive
            u64 size = 0;       // its bytes once extracted
            u16 method = 0;     // 0 stored, 8 deflate
            usize index = 0;    // its place among the archive's entries
        };

        void Load(StringView folder)
        {
            if (!m_archiveFile.IsValid())
            {
                m_error = Format(u8"cannot open {}", m_path);
                return;
            }
            if (!m_archive.Open(m_archiveFile, m_error))
            {
                return;
            }
            const Span<const zip::ZipEntry> entries = m_archive.Entries();
            for (usize i = 0; i < entries.Size(); ++i)
            {
                m_zipEntries.PushBack(entries[i]);
                const zip::ZipEntry& zipEntry = entries[i];
                StringView below;
                if (zipEntry.directory || !RelativeUnder(zipEntry.name.AsView(), folder, below))
                {
                    continue;
                }
                Entry entry;
                entry.path = String(below);
                entry.size = zipEntry.size;
                entry.method = zipEntry.method;
                entry.index = i;
                if (!m_archive.DataOffset(zipEntry, entry.dataOffset, m_error))
                {
                    return;
                }
                m_entries.PushBack(Move(entry));
            }
            m_valid = true;
        }

        [[nodiscard]] const Entry* Find(StringView path) const
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.path.AsView() == path)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Whether `path` lies below `folder` (empty: everything does), with `outRest` the rest.
        [[nodiscard]] static bool RelativeUnder(StringView path, StringView folder, StringView& outRest)
        {
            while (!folder.IsEmpty() && folder[folder.Size() - 1] == u8'/')
            {
                folder = folder.SubStr(0, folder.Size() - 1);
            }
            if (folder.IsEmpty())
            {
                outRest = path;
                return !path.IsEmpty();
            }
            if (path.Size() <= folder.Size() + 1 || path.SubStr(0, folder.Size()) != folder || path[folder.Size()] != u8'/')
            {
                return false;
            }
            outRest = path.SubStr(folder.Size() + 1, path.Size() - folder.Size() - 1);
            return true;
        }

        String m_path;
        IAllocator* m_allocator;
        FileStream m_archiveFile; // the reader's stream (inflating), kept open
        zip::ZipArchive m_archive;
        Array<zip::ZipEntry> m_zipEntries;
        Array<Entry> m_entries;
        Mutex m_readLock;
        String m_error;
        bool m_valid = false;
    };
}
