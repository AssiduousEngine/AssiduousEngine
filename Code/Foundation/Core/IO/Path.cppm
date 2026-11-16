// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :path partition
//
// UTF-8 path string manipulation (POSIX '/' separator). Non-owning queries
// return StringView into the input; PathJoin builds a new String.

module;
#include "Core/Prelude.h"

export module foundation.core:path;

import :base;
import :allocator;
import :string;
import :array; // PathNormalizeLexically's components

export namespace foundation::core
{
    inline constexpr utf8char kPathSeparator = utf8char('/');

    [[nodiscard]] inline bool PathIsSeparator(utf8char c) noexcept
    {
        return c == utf8char('/') || c == utf8char('\\');
    }

    // Absolute means rooted on this host's filesystem. On Windows that also covers a
    // drive-qualified root ("D:\x", "D:/x") and UNC ("\\server\share"); a leading
    // separator alone ("\x") is drive-relative, but treating it as absolute keeps
    // the POSIX contract and matches how the engine builds paths. PathJoin depends
    // on this: an absolute `b` must win rather than be appended to `a`.
    [[nodiscard]] inline bool PathIsAbsolute(StringView path) noexcept
    {
        if (path.IsEmpty())
        {
            return false;
        }
        if (PathIsSeparator(path[0]))
        {
            return true; // POSIX root, and UNC "\\..." on Windows
        }
#if PLATFORM_WINDOWS
        // "X:\..." or "X:/...": a drive-qualified root.
        if (path.Size() >= 3 && path[1] == utf8char(':') && PathIsSeparator(path[2]))
        {
            const utf8char c = path[0];
            return (c >= utf8char('A') && c <= utf8char('Z')) ||
                   (c >= utf8char('a') && c <= utf8char('z'));
        }
#endif
        return false;
    }

    // Whether `name` is one plain path component: not empty, not "." or "..", with no separator
    // and no colon (a drive, a scheme). What a name read from a file must be before it is joined
    // onto a folder, so the join stays a child of that folder.
    [[nodiscard]] inline bool PathIsPlainComponent(StringView name) noexcept
    {
        if (name.IsEmpty() || name == StringView(u8".") || name == StringView(u8".."))
        {
            return false;
        }
        for (const utf8char c : name)
        {
            if (PathIsSeparator(c) || c == utf8char(':') || c == utf8char('\0'))
            {
                return false;
            }
        }
        return true;
    }

    // Whether `path` is relative and stays under the folder it is joined to: not absolute, no
    // colon (a drive, a scheme), and no ".." or empty component. What a path read from a file (a
    // model's texture uri) must be before it is joined onto a folder.
    [[nodiscard]] inline bool PathStaysInside(StringView path) noexcept
    {
        if (path.IsEmpty() || PathIsAbsolute(path))
        {
            return false;
        }
        usize start = 0;
        for (usize i = 0; i <= path.Size(); ++i)
        {
            if (i == path.Size() || PathIsSeparator(path[i]))
            {
                const StringView part = path.SubStr(start, i - start);
                if (part != StringView(u8".") && !PathIsPlainComponent(part))
                {
                    return false;
                }
                start = i + 1;
            }
        }
        return true;
    }

    // The final component (after the last separator).
    [[nodiscard]] inline StringView PathFilename(StringView path) noexcept
    {
        usize start = 0;
        for (usize i = 0; i < path.Size(); ++i)
        {
            if (PathIsSeparator(path[i]))
            {
                start = i + 1;
            }
        }
        return path.SubStr(start, path.Size() - start);
    }

    // The extension including the dot (e.g. ".png"), or empty. A leading-dot
    // filename (".gitignore") has no extension.
    [[nodiscard]] inline StringView PathExtension(StringView path) noexcept
    {
        const StringView name = PathFilename(path);
        usize dot = name.Size();
        for (usize i = 0; i < name.Size(); ++i)
        {
            if (name[i] == utf8char('.'))
            {
                dot = i;
            }
        }
        if (dot == name.Size() || dot == 0)
        {
            return StringView{};
        }
        return name.SubStr(dot, name.Size() - dot);
    }

    // The filename without its extension.
    [[nodiscard]] inline StringView PathStem(StringView path) noexcept
    {
        const StringView name = PathFilename(path);
        const StringView ext = PathExtension(path);
        return name.SubStr(0, name.Size() - ext.Size());
    }

    // Everything before the last separator (empty if there is none). A root keeps its separator:
    // the parent of "/file" is "/" and of "C:\\file" is "C:\\" (cut before it, they read as a
    // relative path and a drive's current folder).
    [[nodiscard]] inline StringView PathParent(StringView path) noexcept
    {
        usize lastSep = path.Size();
        for (usize i = 0; i < path.Size(); ++i)
        {
            if (PathIsSeparator(path[i]))
            {
                lastSep = i;
            }
        }
        if (lastSep == path.Size())
        {
            return StringView{};
        }
        const bool driveRoot = lastSep == 2 && path[1] == u8':';
        if ((lastSep == 0 || driveRoot) && path.Size() > lastSep + 1)
        {
            return path.SubStr(0, lastSep + 1);
        }
        return path.SubStr(0, lastSep);
    }

    // Joins two paths with a single separator. If `b` is absolute it wins.
    [[nodiscard]] inline String PathJoin(StringView a, StringView b,
                                         IAllocator& allocator = DefaultAllocator())
    {
        if (a.IsEmpty() || PathIsAbsolute(b))
        {
            return String{b, allocator};
        }

        String result{a, allocator};
        if (!PathIsSeparator(a[a.Size() - 1]))
        {
            result.PushBack(kPathSeparator);
        }
        result.Append(b);
        return result;
    }

    // `path` with its separators made '/', and empty, "." and ".." components resolved by the text
    // alone (a ".." above the root is dropped; a relative path keeps its leading ".."). Links are
    // not followed: a link inside a folder can still lead out of it.
    [[nodiscard]] inline String PathNormalizeLexically(StringView path, IAllocator& allocator = DefaultAllocator())
    {
        String root{allocator};
        usize i = 0;
#if PLATFORM_WINDOWS
        if (path.Size() >= 2 && path[1] == utf8char(':'))
        {
            root.PushBack(path[0]);
            root.PushBack(utf8char(':'));
            i = 2;
        }
#endif
        const bool absolute = i < path.Size() && PathIsSeparator(path[i]);
        if (absolute)
        {
            root.PushBack(kPathSeparator);
            ++i;
#if PLATFORM_WINDOWS
            if (root.Size() == 1 && i < path.Size() && PathIsSeparator(path[i])) // UNC "\\server\share"
            {
                root.PushBack(kPathSeparator);
                ++i;
            }
#endif
        }
        Array<StringView> parts(allocator);
        usize start = i;
        for (; i <= path.Size(); ++i)
        {
            if (i < path.Size() && !PathIsSeparator(path[i]))
            {
                continue;
            }
            const StringView part = path.SubStr(start, i - start);
            start = i + 1;
            if (part.IsEmpty() || part == StringView(u8"."))
            {
                continue;
            }
            if (part == StringView(u8".."))
            {
                if (!parts.IsEmpty() && parts[parts.Size() - 1] != StringView(u8".."))
                {
                    parts.PopBack();
                }
                else if (!absolute)
                {
                    parts.PushBack(part);
                }
                continue;
            }
            parts.PushBack(part);
        }
        String out = Move(root);
        for (usize n = 0; n < parts.Size(); ++n)
        {
            if (n > 0)
            {
                out.PushBack(kPathSeparator);
            }
            out.Append(parts[n]);
        }
        if (out.IsEmpty())
        {
            out.PushBack(utf8char('.'));
        }
        return out;
    }

    // Whether `path` is `folder` or inside it, both taken as PathNormalizeLexically gives them
    // (on Windows ignoring ASCII case, as its file system does).
    [[nodiscard]] inline bool PathIsInside(StringView path, StringView folder)
    {
        const String p = PathNormalizeLexically(path);
        const String f = PathNormalizeLexically(folder);
        if (p.Size() < f.Size())
        {
            return false;
        }
        for (usize i = 0; i < f.Size(); ++i)
        {
            utf8char a = p[i];
            utf8char b = f[i];
#if PLATFORM_WINDOWS
            if (a >= utf8char('A') && a <= utf8char('Z'))
                a = static_cast<utf8char>(a + 32);
            if (b >= utf8char('A') && b <= utf8char('Z'))
                b = static_cast<utf8char>(b + 32);
#endif
            if (a != b)
            {
                return false;
            }
        }
        return p.Size() == f.Size() || f[f.Size() - 1] == kPathSeparator || p[f.Size()] == kPathSeparator;
    }
}
