// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - System backend, Win32 implementation.
//
// Written against SystemBackend.h for Windows/MSVC. The Linux dev environment does
// not compile it; the Windows CI job does.

#include "Core/System/SystemBackend.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h> // must precede windows.h (winsock2 vs the legacy winsock.h windows.h pulls in)
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET // not every SDK's mstcpip.h defines it for the targeted Windows version
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <windows.h>
#include <bcrypt.h> // BCryptGenRandom (FillEntropy)

#include <bit> // std::bit_cast (LibrarySymbol)
#include <cstdio>
#include <cstdlib> // std::realloc (CommandLine)
#include <cstring>

// windows.h #defines these to the ...A/...W variants, which would also rewrite
// our identically-named backend functions. Undo them; we call the A variants.
#undef CreateDirectory
#undef RemoveDirectory
#undef GetEnvironmentVariable // windows.h maps it to ...A; we define our own and call ...A directly
#undef SetEnvironmentVariable // ditto (SetEnvironmentVariableA)
#undef GetCurrentDirectory    // ditto (GetCurrentDirectoryA)
#undef CreateSymbolicLink     // ditto (CreateSymbolicLinkA): defined, ours called itself

namespace foundation::core::sys
{
    namespace
    {
        // A CreateProcess command line that grows as it is built (a fixed one cut a long command
        // short). CreateProcess re-parses it, so each argument is quoted only when it needs it
        // (empty, or holding whitespace or quotes). Quoting unconditionally breaks callees that
        // parse their own command line rather than using CommandLineToArgvW, notably cmd.exe,
        // which does not recognise a quoted "/c" switch.
        class CommandLine
        {
        public:
            CommandLine() = default;
            CommandLine(const CommandLine&) = delete;
            CommandLine& operator=(const CommandLine&) = delete;
            ~CommandLine() { std::free(m_text); }

            void Put(char c) noexcept
            {
                if (m_length + 2 > m_capacity)
                {
                    const std::size_t grown = m_capacity == 0 ? 1024 : m_capacity * 2;
                    char* next = static_cast<char*>(std::realloc(m_text, grown));
                    if (next == nullptr)
                    {
                        m_full = true;
                        return;
                    }
                    m_text = next;
                    m_capacity = grown;
                }
                m_text[m_length++] = c;
            }

            void PutText(const char* text) noexcept
            {
                for (const char* p = text; *p != '\0'; ++p)
                    Put(*p);
            }

            void AppendArgument(const char* s) noexcept
            {
                bool needsQuotes = (s[0] == '\0');
                for (const char* p = s; *p != '\0' && !needsQuotes; ++p)
                {
                    needsQuotes = (*p == ' ' || *p == '\t' || *p == '"');
                }
                if (!needsQuotes)
                {
                    PutText(s);
                    return;
                }
                Put('"');
                for (const char* p = s; *p != '\0'; ++p)
                {
                    if (*p == '\\')
                    {
                        // A backslash run is literal unless it precedes the closing quote, where
                        // each one must be doubled so it isn't read as escaping that quote.
                        std::size_t slashes = 0;
                        while (p[slashes] == '\\')
                            ++slashes;
                        const bool beforeClose = (p[slashes] == '\0');
                        for (std::size_t i = 0; i < slashes * (beforeClose ? 2u : 1u); ++i)
                            Put('\\');
                        p += slashes - 1;
                        continue;
                    }
                    if (*p == '"')
                        Put('\\');
                    Put(*p);
                }
                Put('"');
            }

            // The finished, NUL-terminated line for the caller to free, or null when it ran out of memory.
            [[nodiscard]] char* Take() noexcept
            {
                if (m_full || m_text == nullptr)
                {
                    return nullptr;
                }
                m_text[m_length] = '\0';
                char* text = m_text;
                m_text = nullptr;
                m_length = 0;
                m_capacity = 0;
                return text;
            }

        private:
            char* m_text = nullptr;
            std::size_t m_length = 0;
            std::size_t m_capacity = 0;
            bool m_full = false;
        };
    }

    std::uint64_t GetTicks() noexcept
    {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<std::uint64_t>(counter.QuadPart);
    }

    std::uint64_t GetTickFrequency() noexcept
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return static_cast<std::uint64_t>(frequency.QuadPart);
    }

    std::int64_t GetUnixTime() noexcept
    {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        // FILETIME (100ns since 1601) -> seconds since the unix epoch.
        const unsigned long long ft = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        return static_cast<std::int64_t>(ft / 10000000ull) - 11644473600ll;
    }

    std::int64_t GetLocalTimeOffset(std::int64_t unixTime) noexcept
    {
        // Through SYSTEMTIME so the zone's rule for that date (summer time or not) applies.
        const unsigned long long ft = static_cast<unsigned long long>(unixTime + 11644473600ll) * 10000000ull;
        FILETIME utcFile{static_cast<DWORD>(ft & 0xffffffffull), static_cast<DWORD>(ft >> 32)};
        SYSTEMTIME utc{}, local{};
        FILETIME localFile{};
        if (!FileTimeToSystemTime(&utcFile, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
            !SystemTimeToFileTime(&local, &localFile))
            return 0;
        const unsigned long long lt = (static_cast<unsigned long long>(localFile.dwHighDateTime) << 32) | localFile.dwLowDateTime;
        return (static_cast<std::int64_t>(lt) - static_cast<std::int64_t>(ft)) / 10000000ll;
    }

    void SleepMilliseconds(std::uint32_t milliseconds) noexcept { Sleep(milliseconds); }

    std::uint32_t LogicalCoreCount() noexcept
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1u;
    }

    std::size_t PageSize() noexcept
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwPageSize;
    }

    std::size_t GetEnvironmentVariable(const char* name, char* out, std::size_t outSize) noexcept
    {
        // ::GetEnvironmentVariableA returns chars written (excl null) on success, the required size
        // (incl null) if `out` is too small (nothing written), or 0 if the variable is absent.
        const DWORD n = ::GetEnvironmentVariableA(name, out, static_cast<DWORD>(outSize));
        if (n == 0)
        {
            return 0;
        }
        return (n >= outSize) ? static_cast<std::size_t>(n - 1) : static_cast<std::size_t>(n);
    }

    bool SetEnvironmentVariable(const char* name, const char* value) noexcept
    {
        return ::SetEnvironmentVariableA(name, value) != 0;
    }

    void PersistUserData() noexcept {} // files are durable once written

    std::size_t GetTempDirectory(char* out, std::size_t outSize) noexcept
    {
        // The user's temp folder, as GetTempPath resolves it: %TEMP%, else %TMP%.
        const std::size_t n = GetEnvironmentVariable("TEMP", out, outSize);
        return n != 0 ? n : GetEnvironmentVariable("TMP", out, outSize);
    }

    std::size_t GetUserDataDirectory(char* out, std::size_t outSize) noexcept
    {
        // Per-user, machine-local app data (roaming-free): %LOCALAPPDATA%.
        return GetEnvironmentVariable("LOCALAPPDATA", out, outSize);
    }

    const char* GetHostPlatformName() noexcept { return "Win64"; }
    const char* ExecutableExtension() noexcept { return ".exe"; }

    std::size_t ResolvePathLinks(const char* path, char* out, std::size_t outSize) noexcept
    {
        // Opened for no access (FILE_FLAG_BACKUP_SEMANTICS so a directory opens too): enough to
        // ask the final path of what the links lead to.
        const HANDLE file = ::CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        char buffer[4096];
        const DWORD n = ::GetFinalPathNameByHandleA(file, buffer, static_cast<DWORD>(sizeof(buffer)), FILE_NAME_NORMALIZED);
        ::CloseHandle(file);
        if (n == 0 || n >= sizeof(buffer))
        {
            return 0;
        }
        // "\\?\C:\..." for a drive path: the prefix goes ("\\?\UNC\..." stays as it is).
        const char* start = buffer;
        if (n > 4 && std::memcmp(buffer, "\\\\?\\", 4) == 0 && buffer[5] == ':')
        {
            start += 4;
        }
        const std::size_t length = std::strlen(start);
        if (out != nullptr && outSize > 0)
        {
            const std::size_t k = (length < outSize - 1) ? length : outSize - 1;
            std::memcpy(out, start, k);
            out[k] = '\0';
        }
        return length;
    }

    std::size_t GetExecutablePath(char* out, std::size_t outSize) noexcept
    {
        // GetModuleFileNameA writes the (possibly truncated) null-terminated path and returns the
        // chars written excl null; == outSize when truncated; 0 on error.
        const DWORD n = ::GetModuleFileNameA(nullptr, out, static_cast<DWORD>(outSize));
        return static_cast<std::size_t>(n);
    }

    std::size_t GetCurrentDirectory(char* out, std::size_t outSize) noexcept
    {
        // Fetch into a local buffer (a cwd is well under this), then copy with truncation so the
        // full-length return contract holds even when `out` is too small.
        char buffer[4096];
        const DWORD n = ::GetCurrentDirectoryA(static_cast<DWORD>(sizeof(buffer)), buffer);
        if (n == 0)
        {
            return 0;
        }
        const std::size_t length = static_cast<std::size_t>(n);
        if (out != nullptr && outSize > 0)
        {
            const std::size_t k = (length < outSize - 1) ? length : outSize - 1;
            std::memcpy(out, buffer, k);
            out[k] = '\0';
        }
        return length;
    }

    bool OpenPathInFileManager(const char* path) noexcept
    {
        if (path == nullptr || path[0] == '\0')
        {
            return false;
        }
        // Launch Explorer on the folder via CreateProcess (kernel32), not ShellExecute or a file://
        // URL, both of which can synchronously block or pop a protocol chooser and hang the UI thread.
        // CreateProcess returns as soon as the child starts (we don't wait), so this can never block.
        // Quotes let the path contain spaces; explorer parses its own command line. Core needs no
        // shell32 link this way. Explorer refuses forward-slash separators, so normalize to the native
        // '\' first (rejecting an overlong path rather than truncating it, which could open a wrong parent).
        char native[4096];
        if (!NormalizePathSeparators(path, native, sizeof(native)))
        {
            return false;
        }
        char command[sizeof(native) + 32];
        std::snprintf(command, sizeof(command), "explorer.exe \"%s\"", native);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                              &startup, &process))
        {
            return false;
        }
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return true;
    }

    bool LaunchDetached(const char* program, const char* const* argv, int argc) noexcept
    {
        if (program == nullptr || program[0] == '\0' || argc < 0)
        {
            return false;
        }
        // CreateProcess's own search only adds .exe, and most editors put a .cmd on PATH (VS Code's
        // `code`, beside a `code` shell script for WSL that Windows cannot run), so find the program
        // first, as an .exe, a .cmd or a .bat. A name with an extension is searched as it is.
        char found[MAX_PATH];
        DWORD foundLength = 0;
        const char* const extensions[] = {".exe", ".cmd", ".bat"};
        for (const char* extension : extensions)
        {
            foundLength = ::SearchPathA(nullptr, program, extension, sizeof(found), found, nullptr);
            if (foundLength != 0 && foundLength < sizeof(found))
                break;
        }
        if (foundLength == 0 || foundLength >= sizeof(found))
        {
            return false;
        }
        const char* extension = std::strrchr(found, '.');
        const bool script = extension != nullptr && (_stricmp(extension, ".cmd") == 0 || _stricmp(extension, ".bat") == 0);

        CommandLine line;
        if (script)
        {
            // cmd /s /c "<line>" takes everything between the outer quotes as the command, as is.
            static constexpr char kCmd[] = "\\cmd.exe";
            char shell[MAX_PATH];
            const UINT shellLength = ::GetSystemDirectoryA(shell, sizeof(shell));
            if (shellLength == 0 || shellLength + sizeof(kCmd) > sizeof(shell))
            {
                return false;
            }
            std::memcpy(shell + shellLength, kCmd, sizeof(kCmd)); // the name and its terminator
            line.AppendArgument(shell);
            line.PutText(" /d /s /c \"");
        }
        line.AppendArgument(found);
        for (int i = 0; i < argc; ++i)
        {
            line.Put(' ');
            line.AppendArgument(argv[i]);
        }
        if (script)
        {
            line.Put('"');
        }
        char* command = line.Take();
        if (command == nullptr)
        {
            return false;
        }
        // No inherited handles, a process group of its own, and no console window for the cmd.exe
        // that runs a script. CreateProcess returns once the child starts; nothing waits on it.
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const DWORD flags = CREATE_NEW_PROCESS_GROUP | (script ? CREATE_NO_WINDOW : 0);
        const BOOL ok = ::CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, flags, nullptr, nullptr,
                                         &startup, &process);
        std::free(command);
        if (!ok)
        {
            return false;
        }
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return true;
    }

    bool NormalizePathSeparators(const char* path, char* out, std::size_t cap) noexcept
    {
        // Windows-native separator is '\'; flip any forward slash. (POSIX flips the other way.)
        if (path == nullptr || out == nullptr || cap == 0)
        {
            return false;
        }
        std::size_t length = 0;
        while (path[length] != '\0')
        {
            ++length;
        }
        if (length == 0 || length + 1 > cap)
        {
            return false; // reject overlong rather than truncate mid-path
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            out[i] = (path[i] == '/') ? '\\' : path[i];
        }
        out[length] = '\0';
        return true;
    }

    int RunProcess(const char* exe, const char* const* argv, int argc, ProcessOutputSink sink, void* sinkContext,
                   std::uint32_t timeoutMilliseconds, bool* outTimedOut) noexcept
    {
        if (outTimedOut != nullptr)
        {
            *outTimedOut = false;
        }
        if (exe == nullptr || exe[0] == '\0' || argc < 0)
        {
            return -1;
        }
        const bool capture = sink != nullptr;

        // Build the command line: exe arg0 arg1 ... (see CommandLine for the quoting).
        CommandLine line;
        line.AppendArgument(exe);
        for (int i = 0; i < argc; ++i)
        {
            line.Put(' ');
            line.AppendArgument(argv[i]);
        }
        char* command = line.Take();
        if (command == nullptr)
        {
            return -1;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (capture)
        {
            if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0))
            {
                std::free(command);
                return -1;
            }
            ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); // parent-only read end
        }
        // Empty stdin (NUL) so a tool that reads stdin gets EOF instead of hanging.
        HANDLE nulIn = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, nullptr);

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nulIn;
        startup.hStdOutput = capture ? writeEnd : ::GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = capture ? writeEnd : ::GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION process{};
        const BOOL ok = ::CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, 0, nullptr,
                                         nullptr, &startup, &process);
        std::free(command);
        if (capture)
        {
            ::CloseHandle(writeEnd); // parent holds only the read end now
        }
        if (!ok)
        {
            if (capture)
                ::CloseHandle(readEnd);
            if (nulIn != INVALID_HANDLE_VALUE)
                ::CloseHandle(nulIn);
            return -1;
        }

        // Read what is there, watch the process and the clock: a blocking read would wait past a
        // timeout for output that never comes.
        const ULONGLONG started = ::GetTickCount64();
        bool killed = false;
        char buffer[4096];
        const auto drainReady = [&]() noexcept
        {
            for (;;)
            {
                DWORD available = 0;
                if (!::PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                    return;
                DWORD got = 0;
                const DWORD want = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
                if (!::ReadFile(readEnd, buffer, want, &got, nullptr) || got == 0)
                    return;
                sink(sinkContext, buffer, got);
            }
        };
        for (;;)
        {
            if (capture)
                drainReady();
            if (::WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0)
                break;
            if (timeoutMilliseconds != 0 && ::GetTickCount64() - started >= timeoutMilliseconds)
            {
                ::TerminateProcess(process.hProcess, 1);
                ::WaitForSingleObject(process.hProcess, INFINITE);
                killed = true;
                break;
            }
        }
        if (capture)
        {
            // The rest: what is left in the pipe once the process is done (or killed).
            for (;;)
            {
                DWORD got = 0;
                if (killed)
                {
                    drainReady(); // what is there, without waiting on anything it started
                    break;
                }
                if (!::ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                    break;
                sink(sinkContext, buffer, got);
            }
            ::CloseHandle(readEnd);
        }
        DWORD code = static_cast<DWORD>(-1);
        ::GetExitCodeProcess(process.hProcess, &code);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        if (nulIn != INVALID_HANDLE_VALUE)
            ::CloseHandle(nulIn);
        if (killed)
        {
            if (outTimedOut != nullptr)
                *outTimedOut = true;
            return -1;
        }
        return static_cast<int>(code);
    }

    void* PageAllocate(std::size_t size) noexcept
    {
        if (size == 0)
        {
            return nullptr;
        }
        return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    void PageFree(void* pointer, std::size_t /*size*/) noexcept
    {
        if (pointer != nullptr)
        {
            VirtualFree(pointer, 0, MEM_RELEASE);
        }
    }

    // --- Files -------------------------------------------------------------
    namespace
    {
        HANDLE ToHandle(FileHandle handle) noexcept { return reinterpret_cast<HANDLE>(handle); }
    }

    FileHandle FileOpen(const char* path, FileMode mode) noexcept
    {
        DWORD access = 0;
        DWORD creation = OPEN_EXISTING;
        switch (mode)
        {
        case FileMode::Read:
            access = GENERIC_READ;
            creation = OPEN_EXISTING;
            break;
        case FileMode::Write:
            access = GENERIC_WRITE;
            creation = CREATE_ALWAYS;
            break;
        case FileMode::ReadWrite:
            access = GENERIC_READ | GENERIC_WRITE;
            creation = OPEN_ALWAYS;
            break;
        case FileMode::Append:
            access = FILE_APPEND_DATA;
            creation = OPEN_ALWAYS;
            break;
        }

        // Share everything, to match the POSIX semantics the rest of the engine is written
        // against: there, an open reader never prevents rewriting, replacing or unlinking a
        // file. With FILE_SHARE_READ alone, any outstanding read stream would make a later
        // write open fail (e.g. regenerating a model prefab while its own payload stream is
        // still open). Callers that need exclusive access must take a real lock; a share mode
        // does not provide one (it only blocks writers, and only while a handle is open).
        constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE handle = CreateFileA(path, access, kShareAll, nullptr, creation,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return kInvalidFile;
        }
        if (mode == FileMode::Append)
        {
            SetFilePointer(handle, 0, nullptr, FILE_END);
        }
        return reinterpret_cast<FileHandle>(handle);
    }

    void FileClose(FileHandle handle) noexcept
    {
        if (handle != kInvalidFile)
        {
            CloseHandle(ToHandle(handle));
        }
    }

    // Single calls are capped at DWORD (4 GB); sufficient for buffered IO.
    // ReadFile and WriteFile take a DWORD count: a request of 4 GiB or more was cut to its low 32
    // bits. Both go in chunks of at most 1 GiB until all of it is done, the file ends, or it fails.
    std::int64_t FileRead(FileHandle handle, void* buffer, std::uint64_t bytes) noexcept
    {
        constexpr std::uint64_t kChunk = 1ull << 30;
        std::uint64_t done = 0;
        while (done < bytes)
        {
            const DWORD want = static_cast<DWORD>(bytes - done < kChunk ? bytes - done : kChunk);
            DWORD read = 0;
            if (!ReadFile(ToHandle(handle), static_cast<char*>(buffer) + done, want, &read, nullptr))
                return done > 0 ? static_cast<std::int64_t>(done) : -1;
            if (read == 0)
                break; // the end of the file
            done += read;
        }
        return static_cast<std::int64_t>(done);
    }

    std::int64_t FileWrite(FileHandle handle, const void* buffer, std::uint64_t bytes) noexcept
    {
        constexpr std::uint64_t kChunk = 1ull << 30;
        std::uint64_t done = 0;
        while (done < bytes)
        {
            const DWORD want = static_cast<DWORD>(bytes - done < kChunk ? bytes - done : kChunk);
            DWORD written = 0;
            if (!WriteFile(ToHandle(handle), static_cast<const char*>(buffer) + done, want, &written, nullptr))
                return done > 0 ? static_cast<std::int64_t>(done) : -1;
            if (written == 0)
                break;
            done += written;
        }
        return static_cast<std::int64_t>(done);
    }

    std::int64_t FileSeek(FileHandle handle, std::int64_t offset, SeekOrigin origin) noexcept
    {
        DWORD method = FILE_BEGIN;
        switch (origin)
        {
        case SeekOrigin::Begin:
            method = FILE_BEGIN;
            break;
        case SeekOrigin::Current:
            method = FILE_CURRENT;
            break;
        case SeekOrigin::End:
            method = FILE_END;
            break;
        }
        LARGE_INTEGER distance;
        distance.QuadPart = offset;
        LARGE_INTEGER result;
        if (!SetFilePointerEx(ToHandle(handle), distance, &result, method))
        {
            return -1;
        }
        return static_cast<std::int64_t>(result.QuadPart);
    }

    namespace
    {
        // The byte FileTryLock locks: far past any content, since a Win32 range lock is
        // mandatory (it would stop other handles reading the bytes it covers), and locking past
        // the end of a file is allowed.
        OVERLAPPED LockRange() noexcept
        {
            OVERLAPPED range{};
            range.Offset = 0xFFFFFFFEu;
            range.OffsetHigh = 0x7FFFFFFFu;
            return range;
        }
    }

    bool FileTryLock(FileHandle handle) noexcept
    {
        if (handle == kInvalidFile)
        {
            return false;
        }
        OVERLAPPED range = LockRange();
        return LockFileEx(ToHandle(handle), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range) != 0;
    }

    void FileUnlock(FileHandle handle) noexcept
    {
        if (handle == kInvalidFile)
        {
            return;
        }
        OVERLAPPED range = LockRange();
        (void)UnlockFileEx(ToHandle(handle), 0, 1, 0, &range);
    }

    std::int64_t FileSize(FileHandle handle) noexcept
    {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(ToHandle(handle), &size))
        {
            return -1;
        }
        return static_cast<std::int64_t>(size.QuadPart);
    }

    bool FileExists(const char* path) noexcept
    {
        const DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool FileDelete(const char* path) noexcept { return DeleteFileA(path) != 0; }

    unsigned long ProcessId() noexcept { return GetCurrentProcessId(); }

    bool FillEntropy(void* buffer, std::size_t bytes) noexcept
    {
        return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer),
                                              static_cast<ULONG>(bytes),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    std::size_t ExecutablePath(char* buffer, std::size_t capacity) noexcept
    {
        if (capacity == 0)
        {
            return 0;
        }
        const DWORD n =
            GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(capacity));
        if (n == 0 || n >= capacity) // 0 = failure; n == capacity = truncated
        {
            return 0;
        }
        return static_cast<std::size_t>(n);
    }

    bool FileMove(const char* from, const char* to) noexcept
    {
        // MoveFileEx refuses to replace a file another handle has open, even one opened with
        // FILE_SHARE_DELETE, so a save over a file a reader held failed. A rename with POSIX
        // semantics (Windows 10 1809 and later, NTFS) replaces it, and the reader keeps the file
        // it opened. Where that is not supported (an older system, a FAT volume) or the name does
        // not fit the buffer, MoveFileEx.
        constexpr int kMaxName = 4096; // wide characters, the terminator included
        alignas(FILE_RENAME_INFO) unsigned char buffer[sizeof(FILE_RENAME_INFO) + kMaxName * sizeof(WCHAR)];
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer);
        // The other calls here take paths in the ANSI code page (the ...A functions), so this does.
        const int toLength = MultiByteToWideChar(CP_ACP, 0, to, -1, info->FileName, kMaxName);
        if (toLength > 0)
        {
            HANDLE source = CreateFileA(from, DELETE | SYNCHRONIZE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (source != INVALID_HANDLE_VALUE)
            {
                info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
                info->RootDirectory = nullptr;
                info->FileNameLength = static_cast<DWORD>(toLength - 1) * sizeof(WCHAR); // no terminator
                const DWORD size = static_cast<DWORD>(sizeof(FILE_RENAME_INFO)) + info->FileNameLength;
                const BOOL renamed = SetFileInformationByHandle(source, FileRenameInfoEx, info, size);
                CloseHandle(source);
                if (renamed != 0)
                    return true;
            }
        }
        return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
    }

    bool FileSync(FileHandle handle) noexcept
    {
        return handle != kInvalidFile && FlushFileBuffers(ToHandle(handle)) != 0;
    }

    bool FileRestrictToOwner(FileHandle handle) noexcept { return handle != kInvalidFile; }

    bool FileIsOwnerOnly(const char* path) noexcept { return FileExists(path); }

    bool FileCopyPreserving(const char* from, const char* to) noexcept
    {
        // CopyFileA preserves attributes natively (there is no +x bit on Windows).
        return CopyFileA(from, to, FALSE /*overwrite*/) != 0;
    }

    bool FileStat(const char* path, unsigned long long& outSize,
                  long long& outModifiedTime) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &data) == 0 ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }
        outSize = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        // FILETIME (100ns since 1601) -> seconds since the unix epoch.
        const unsigned long long ft =
            (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        outModifiedTime = static_cast<long long>(ft / 10000000ull) - 11644473600ll;
        return true;
    }

    bool DirectoryExists(const char* path) noexcept
    {
        const DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool IsSymbolicLink(const char* path) noexcept
    {
        const DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    bool CreateSymbolicLink(const char* target, const char* link, bool directory) noexcept
    {
        DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (directory)
            flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        return ::CreateSymbolicLinkA(link, target, flags) != 0;
    }

    bool CreateDirectory(const char* path) noexcept
    {
        if (CreateDirectoryA(path, nullptr))
        {
            return true;
        }
        return DirectoryExists(path);
    }

    bool RemoveDirectory(const char* path) noexcept { return RemoveDirectoryA(path) != 0; }

    bool ListDirectory(const char* path, DirEntryCallback cb, void* ctx) noexcept
    {
        char pattern[MAX_PATH];
        const int n = std::snprintf(pattern, sizeof(pattern), "%s\\*", path);
        if (n < 0 || n >= static_cast<int>(sizeof(pattern)))
        {
            return false;
        }

        WIN32_FIND_DATAA data{};
        HANDLE handle = FindFirstFileA(pattern, &data);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        do
        {
            const char* name = data.cFileName;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            {
                continue; // skip "." and ".."
            }
            const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            cb(ctx, name, isDir);
        } while (FindNextFileA(handle, &data));

        FindClose(handle);
        return true;
    }

    void ConsoleWrite(const char* text, std::uint64_t length) noexcept
    {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(length), &written,
                  nullptr);
    }

    void ConsoleWriteError(const char* text, std::uint64_t length) noexcept
    {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(length), &written,
                  nullptr);
    }

    LibraryHandle LibraryOpen(const char* path) noexcept
    {
        // LOAD_WITH_ALTERED_SEARCH_PATH resolves what this module imports from the directory it
        // sits in, rather than from the one the .exe sits in. A plugin's parts import each other
        // by filename, and the editor stages them away from the executable for hot reload, so the
        // default order finds a stale copy beside the .exe or nothing at all.
        //
        // Only with an absolute path: the flag's behaviour on a relative one is undefined, so a
        // caller passing a bare name keeps the plain load and the ordinary search.
        const bool absolute = path != nullptr && ((path[0] != '\0' && path[1] == ':') ||
                                                  (path[0] == '\\' && path[1] == '\\'));
        if (!absolute)
        {
            return reinterpret_cast<LibraryHandle>(LoadLibraryA(path));
        }
        return reinterpret_cast<LibraryHandle>(LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    }

    void* LibrarySymbol(LibraryHandle handle, const char* name) noexcept
    {
        if (handle == nullptr)
        {
            return nullptr;
        }
        // A bit cast, not a cast, between the function pointer and void* (MSVC C4054 under /W4 /WX).
        // This backend sits below Core's module, so it uses std::bit_cast, as Core::BitCast does.
        const FARPROC proc = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
        return std::bit_cast<void*>(proc);
    }

    void LibraryClose(LibraryHandle handle) noexcept
    {
        if (handle != nullptr)
        {
            FreeLibrary(reinterpret_cast<HMODULE>(handle));
        }
    }

    // --- UDP sockets (Winsock2) ---------------------------------------------

    namespace
    {
        int g_wsaRefs = 0;
    }

    bool InitializeNetworking() noexcept
    {
        if (g_wsaRefs > 0)
        {
            ++g_wsaRefs;
            return true;
        }
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            return false;
        }
        g_wsaRefs = 1;
        return true;
    }
    void ShutdownNetworking() noexcept
    {
        if (g_wsaRefs > 0 && --g_wsaRefs == 0)
        {
            WSACleanup();
        }
    }

    // A socket a process this one starts (Explorer on a folder, a build) must not inherit: it would
    // hold the port after this process has gone. Winsock sockets are inheritable handles by default.
    static void NotInherited(SOCKET s) noexcept
    {
        ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    }

    SocketHandle UdpOpen(std::uint16_t port, std::uint16_t* outBoundPort) noexcept
    {
        const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            return kInvalidSocket;
        }
        NotInherited(s);
        u_long nonBlocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);
        // No SO_REUSEADDR: UDP has no TIME_WAIT to wait out, and with it a second socket binds a
        // port a live one holds and takes its datagrams.
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            ::closesocket(s);
            return kInvalidSocket;
        }
        // A datagram to a port nobody listens on comes back as an ICMP port-unreachable, which
        // Windows reports on the next receive as WSAECONNRESET: one client that left would stop
        // the frame's receive loop with the others' datagrams still queued. Off.
        BOOL report = FALSE;
        DWORD returned = 0;
        (void)::WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
        if (outBoundPort != nullptr)
        {
            sockaddr_in bound{};
            int len = sizeof(bound);
            *outBoundPort = (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
                                ? ntohs(bound.sin_port)
                                : port;
        }
        return static_cast<SocketHandle>(s);
    }

    void SocketClose(SocketHandle socket) noexcept
    {
        if (socket != kInvalidSocket)
        {
            ::closesocket(static_cast<SOCKET>(socket));
        }
    }

    bool ParseIPv4(const char* dottedQuad, std::uint32_t* outIp) noexcept
    {
        in_addr a{};
        if (::inet_pton(AF_INET, dottedQuad, &a) != 1)
        {
            return false;
        }
        if (outIp != nullptr)
        {
            *outIp = ntohl(a.S_un.S_addr);
        }
        return true;
    }

    bool ResolveHostIPv4(const char* host, std::uint32_t* outIp) noexcept
    {
        if (host == nullptr || host[0] == '\0')
        {
            return false;
        }
        if (ParseIPv4(host, outIp))
        {
            return true; // a literal never touches the resolver
        }
        // getaddrinfo needs Winsock up; a caller resolving before it opened a socket is fine.
        if (!InitializeNetworking())
        {
            return false;
        }
        ADDRINFOA hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        ADDRINFOA* results = nullptr;
        bool found = false;
        if (::getaddrinfo(host, nullptr, &hints, &results) == 0 && results != nullptr)
        {
            for (const ADDRINFOA* it = results; it != nullptr; it = it->ai_next)
            {
                if (it->ai_family == AF_INET && it->ai_addr != nullptr)
                {
                    const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
                    if (outIp != nullptr)
                    {
                        *outIp = ntohl(addr->sin_addr.S_un.S_addr);
                    }
                    found = true;
                    break;
                }
            }
            ::freeaddrinfo(results);
        }
        ShutdownNetworking();
        return found;
    }

    std::int64_t UdpSendTo(SocketHandle socket, std::uint32_t ip, std::uint16_t port,
                           const void* data, std::size_t size) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);
        const int n =
            ::sendto(static_cast<SOCKET>(socket), static_cast<const char*>(data),
                     static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n == SOCKET_ERROR)
        {
            return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t UdpRecvFrom(SocketHandle socket, void* out, std::size_t outCap,
                             std::uint32_t* fromIp, std::uint16_t* fromPort) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        sockaddr_in addr{};
        int n = SOCKET_ERROR;
        for (;;)
        {
            int len = sizeof(addr);
            n = ::recvfrom(static_cast<SOCKET>(socket), static_cast<char*>(out),
                           static_cast<int>(outCap), 0, reinterpret_cast<sockaddr*>(&addr), &len);
            if (n != SOCKET_ERROR)
            {
                break;
            }
            const int error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAENETRESET)
            {
                continue; // an earlier send's ICMP error, consumed: the next datagram is still there
            }
            return (error == WSAEWOULDBLOCK) ? kSocketWouldBlock : -1;
        }
        if (fromIp != nullptr)
        {
            *fromIp = ntohl(addr.sin_addr.s_addr);
        }
        if (fromPort != nullptr)
        {
            *fromPort = ntohs(addr.sin_port);
        }
        return static_cast<std::int64_t>(n);
    }

    // --- TCP sockets (Winsock2) ---------------------------------------------

    static void SetNonBlocking(SOCKET s) noexcept
    {
        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
    }

    SocketHandle TcpListen(std::uint16_t port, bool loopbackOnly, std::uint16_t* outBoundPort,
                           std::uint32_t* outBoundIp) noexcept
    {
        const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            return kInvalidSocket;
        }
        NotInherited(s);
        SetNonBlocking(s);
        // No SO_REUSEADDR here: on Windows it lets a second socket take a port that is live,
        // and a port held in TIME_WAIT never blocks a bind anyway (see SystemBackend.h).
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            ::closesocket(s);
            return kInvalidSocket;
        }
        if (::listen(s, SOMAXCONN) == SOCKET_ERROR)
        {
            ::closesocket(s);
            return kInvalidSocket;
        }
        sockaddr_in bound{};
        int len = sizeof(bound);
        const bool named = ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0;
        if (outBoundPort != nullptr)
        {
            *outBoundPort = named ? ntohs(bound.sin_port) : port;
        }
        if (outBoundIp != nullptr)
        {
            *outBoundIp = named ? ntohl(bound.sin_addr.s_addr) : 0u;
        }
        return static_cast<SocketHandle>(s);
    }

    SocketHandle TcpAccept(SocketHandle listener, std::uint32_t* fromIp,
                           std::uint16_t* fromPort) noexcept
    {
        if (listener == kInvalidSocket)
        {
            return kInvalidSocket;
        }
        sockaddr_in addr{};
        int len = sizeof(addr);
        const SOCKET fd =
            ::accept(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd == INVALID_SOCKET)
        {
            return kInvalidSocket;
        } // WSAEWOULDBLOCK: none pending
        NotInherited(fd);
        SetNonBlocking(fd);
        if (fromIp != nullptr)
        {
            *fromIp = ntohl(addr.sin_addr.s_addr);
        }
        if (fromPort != nullptr)
        {
            *fromPort = ntohs(addr.sin_port);
        }
        return static_cast<SocketHandle>(fd);
    }

    SocketHandle TcpConnect(std::uint32_t ip, std::uint16_t port) noexcept
    {
        const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            return kInvalidSocket;
        }
        NotInherited(s);
        SetNonBlocking(s);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);
        const int r = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            ::closesocket(s);
            return kInvalidSocket;
        }
        return static_cast<SocketHandle>(s);
    }

    int TcpConnectStatus(SocketHandle socket) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        const SOCKET s = static_cast<SOCKET>(socket);
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        fd_set efds;
        FD_ZERO(&efds);
        FD_SET(s, &efds);
        timeval tv{0, 0};
        const int r = ::select(0, nullptr, &wfds, &efds, &tv);
        if (r == 0)
        {
            return 0;
        }
        if (r == SOCKET_ERROR)
        {
            return -1;
        }
        if (FD_ISSET(s, &efds))
        {
            return -1;
        }
        if (FD_ISSET(s, &wfds))
        {
            int err = 0;
            int len = sizeof(err);
            if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) ==
                SOCKET_ERROR)
            {
                return -1;
            }
            return (err == 0) ? 1 : -1;
        }
        return 0;
    }

    std::int64_t TcpSend(SocketHandle socket, const void* data, std::size_t size) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        const int n = ::send(static_cast<SOCKET>(socket), static_cast<const char*>(data),
                             static_cast<int>(size), 0);
        if (n == SOCKET_ERROR)
        {
            return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t TcpRecv(SocketHandle socket, void* out, std::size_t outCap) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        const int n = ::recv(static_cast<SOCKET>(socket), static_cast<char*>(out),
                             static_cast<int>(outCap), 0);
        if (n > 0)
        {
            return static_cast<std::int64_t>(n);
        }
        if (n == 0)
        {
            return -1;
        } // peer closed
        return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
    }
}

#include <io.h>
#include <dbghelp.h> // MiniDumpWriteDump
#include <csignal>   // SIGABRT: abort(), which a fatal error and std::terminate end in

// ASAN detection, nested so MSVC only lexes __has_feature (see TestMain.h).
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WIN32_SYSTEM_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(WIN32_SYSTEM_ASAN)
#define WIN32_SYSTEM_ASAN 1
#endif

namespace foundation::core::sys
{
    namespace
    {
        // Where a crash's lines go: stderr, and the report file beside the minidump once it is
        // open. Fixed buffers only, since the process is dying, maybe with the heap's lock held.
        struct CrashOutput
        {
            int fd = -1;
            HANDLE file = INVALID_HANDLE_VALUE;

            // `length` is what snprintf returned for a buffer of `capacity`: clamped to what it holds.
            void Write(const char* text, int length, std::size_t capacity) const noexcept
            {
                if (length <= 0)
                    return;
                const std::size_t size =
                    static_cast<std::size_t>(length) < capacity ? static_cast<std::size_t>(length) : capacity - 1;
                if (fd >= 0)
                    (void)_write(fd, text, static_cast<unsigned>(size));
                if (file != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    (void)::WriteFile(file, text, static_cast<DWORD>(size), &written, nullptr);
                }
            }
        };

        // One frame as `  #NN Module.exe+0xOFFSET`: the module's file name and the address's offset
        // into its image, which stays the same from run to run (the load address does not). The
        // module's .pdb names it; the minidump, opened beside the .pdb files, names them all.
        void WriteFrame(const CrashOutput& out, int index, std::uintptr_t address) noexcept
        {
            char line[MAX_PATH + 64];
            HMODULE module = nullptr;
            if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCSTR>(address), &module) ||
                module == nullptr)
            {
                // In no module: a call through a bad pointer, or code made at run time.
                out.Write(line,
                          std::snprintf(line, sizeof(line), "  #%02d 0x%016llx\n", index,
                                        static_cast<unsigned long long>(address)),
                          sizeof(line));
                return;
            }
            char path[MAX_PATH];
            if (::GetModuleFileNameA(module, path, sizeof(path)) == 0)
            {
                path[0] = '?';
                path[1] = '\0';
            }
            const char* name = path;
            for (const char* p = path; *p != '\0'; ++p)
            {
                if (*p == '\\' || *p == '/')
                    name = p + 1;
            }
            const std::uintptr_t offset = address - reinterpret_cast<std::uintptr_t>(module);
            out.Write(line,
                      std::snprintf(line, sizeof(line), "  #%02d %s+0x%llx\n", index, name,
                                    static_cast<unsigned long long>(offset)),
                      sizeof(line));
        }

        // Walks the stack a CONTEXT describes (the crashing thread's, at the crash) through the
        // unwind tables every x64 and ARM64 image carries, writing a frame per return address. It
        // runs on another thread than the one that crashed, so a blown stack still walks; a stack
        // too wrecked to read faults the walk, and __try ends it there with what it had.
        // __try is the MSVC extension clang supports on Windows but warns of under -Wpedantic.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif
        int WriteContextBacktrace(const CrashOutput& out, const CONTEXT& crashed) noexcept
        {
            CONTEXT context = crashed;
            volatile int count = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
            __try
            {
                while (count < 64)
                {
#if defined(_M_X64) || defined(__x86_64__)
                    DWORD64& pc = context.Rip;
#else
                    DWORD64& pc = context.Pc;
#endif
                    WriteFrame(out, count, static_cast<std::uintptr_t>(pc));
                    count = count + 1;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(pc, &imageBase, nullptr);
                    if (function == nullptr)
                    {
                        // No unwind data: a leaf function, or a call through a null pointer. Either
                        // way the caller's return address is where the call left it.
#if defined(_M_X64) || defined(__x86_64__)
                        pc = *reinterpret_cast<const DWORD64*>(context.Rsp);
                        context.Rsp += sizeof(DWORD64);
#else
                        pc = context.Lr;
#endif
                    }
                    else
                    {
                        PVOID handlerData = nullptr;
                        DWORD64 establisherFrame = 0;
                        (void)::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData,
                                                 &establisherFrame, nullptr);
                    }
                    if (pc == 0)
                        break; // past the thread's first frame
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
#else
            (void)out;
            (void)context;
#endif
            return count;
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

        // What abort() reports itself as, the way Windows names it: STATUS_FATAL_APP_EXIT.
        constexpr DWORD kAbortCode = 0x40000015;
        // An MSVC-ABI C++ throw that nothing caught ("msc" with the top bits set).
        constexpr DWORD kCppExceptionCode = 0xE06D7363;

        const char* ExceptionName(DWORD code) noexcept
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
            case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
            case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
            // A trap: a failed assert or a panic outside a debugger lands here, after its own message.
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
            case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
            case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
            case kCppExceptionCode: return "uncaught C++ exception";
            case kAbortCode: return "abort()";
            default: return "unhandled exception";
            }
        }

        // How long a crashing thread waits for its report and minidump: long enough for a big
        // process's dump, short enough that a reporter stuck on a lock the crash left held does
        // not keep a dead process around for good.
        constexpr DWORD kCrashReportWaitMilliseconds = 60000;

        // Resolved when the handler is installed: at the crash, the heap may be what broke.
        struct CrashState
        {
            char directory[1024] = {};   // <user data>\<engine>\crashes; empty: stderr only
            char program[MAX_PATH] = {}; // the executable's name without .exe, which names the files
            HANDLE requested = nullptr;  // the reporter thread waits on this,
            HANDLE finished = nullptr;   // and sets this once the report and the minidump are written
            DWORD reporterId = 0;        // 0: no reporter thread, so the crashing thread writes them
            EXCEPTION_POINTERS* pointers = nullptr;
            DWORD threadId = 0;        // the thread that crashed
            const char* note = nullptr; // what it was doing (its CrashNoteSlot), read as it crashed
            volatile LONG claimed = 0; // the first crash reports; a later one waits for it
        };
        CrashState g_crash;

        // Makes each folder of `path` that is missing. `path` is cut and put back as it goes.
        void CreateDirectories(char* path) noexcept
        {
            for (char* p = path; *p != '\0'; ++p)
            {
                if ((*p == '\\' || *p == '/') && p != path && p[-1] != ':')
                {
                    const char separator = *p;
                    *p = '\0';
                    (void)::CreateDirectoryA(path, nullptr);
                    *p = separator;
                }
            }
            (void)::CreateDirectoryA(path, nullptr);
        }

        // Writes the crash's lines to stderr and to <crashes>\<program>-<date>-<time>-<pid>.txt, and
        // its minidump beside them as .dmp: the threads' stacks and the memory they point at, which
        // Visual Studio or WinDbg open with the .pdb files to show the crash as it was.
        void WriteCrashReport() noexcept
        {
            const EXCEPTION_RECORD& record = *g_crash.pointers->ExceptionRecord;
            char stem[1024 + MAX_PATH + 64] = {};
            if (g_crash.directory[0] != '\0')
            {
                CreateDirectories(g_crash.directory);
                SYSTEMTIME now{};
                ::GetLocalTime(&now);
                const int n = std::snprintf(stem, sizeof(stem), "%s\\%s-%04u%02u%02u-%02u%02u%02u-%lu",
                                            g_crash.directory, g_crash.program, now.wYear, now.wMonth, now.wDay,
                                            now.wHour, now.wMinute, now.wSecond, ::GetCurrentProcessId());
                if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(stem))
                    stem[0] = '\0';
            }
            char path[sizeof(stem) + 8];
            HANDLE reportFile = INVALID_HANDLE_VALUE;
            if (stem[0] != '\0')
            {
                (void)std::snprintf(path, sizeof(path), "%s.txt", stem);
                reportFile = ::CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            const CrashOutput out{2 /* stderr */, reportFile};

            char line[sizeof(path) + 64];
            out.Write(line,
                      std::snprintf(line, sizeof(line), "\n=== %s (0x%08lX) on thread %lu ===\n",
                                    ExceptionName(record.ExceptionCode), record.ExceptionCode, g_crash.threadId),
                      sizeof(line));
            if (g_crash.note != nullptr && g_crash.note[0] != '\0')
            {
                out.Write(line, std::snprintf(line, sizeof(line), "  in: %s\n", g_crash.note), sizeof(line));
            }
            if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                record.NumberParameters >= 2)
            {
                const ULONG_PTR operation = record.ExceptionInformation[0];
                const char* verb = operation == 0 ? "reading" : operation == 1 ? "writing" : "executing";
                out.Write(line,
                          std::snprintf(line, sizeof(line), "  %s address 0x%016llx\n", verb,
                                        static_cast<unsigned long long>(record.ExceptionInformation[1])),
                          sizeof(line));
            }
            static const char kBacktrace[] = "  backtrace (module+offset; the module's .pdb names them):\n";
            out.Write(kBacktrace, static_cast<int>(sizeof(kBacktrace) - 1), sizeof(kBacktrace));
            (void)WriteContextBacktrace(out, *g_crash.pointers->ContextRecord);

            if (stem[0] == '\0')
            {
                static const char kNoFolder[] = "  minidump: not written, no crash folder (LOCALAPPDATA is unset)\n";
                out.Write(kNoFolder, static_cast<int>(sizeof(kNoFolder) - 1), sizeof(kNoFolder));
            }
            else
            {
                (void)std::snprintf(path, sizeof(path), "%s.dmp", stem);
                const HANDLE dump =
                    ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                MINIDUMP_EXCEPTION_INFORMATION exception{};
                exception.ThreadId = g_crash.threadId;
                exception.ExceptionPointers = g_crash.pointers;
                exception.ClientPointers = FALSE; // the pointers are this process's own
                // The stacks, the memory they point at, and the threads' and modules' details: a few
                // MB that show the crash's locals, where a full dump would carry the whole heap.
                const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                             MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                             MiniDumpWithProcessThreadData);
                const bool written = dump != INVALID_HANDLE_VALUE &&
                                     ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump, type,
                                                         &exception, nullptr, nullptr) != FALSE;
                const DWORD error = written ? 0 : ::GetLastError();
                if (dump != INVALID_HANDLE_VALUE)
                    ::CloseHandle(dump);
                if (written)
                {
                    out.Write(line, std::snprintf(line, sizeof(line), "  minidump: %s\n", path), sizeof(line));
                }
                else
                {
                    (void)::DeleteFileA(path);
                    out.Write(line, std::snprintf(line, sizeof(line), "  minidump: not written (error %lu)\n", error),
                              sizeof(line));
                }
                if (reportFile != INVALID_HANDLE_VALUE)
                    out.Write(line, std::snprintf(line, sizeof(line), "  report: %s.txt\n", stem), sizeof(line));
            }
            if (reportFile != INVALID_HANDLE_VALUE)
                ::CloseHandle(reportFile);
        }

        // Parked from install until a crash: a thread with a whole stack of its own, so a crash
        // that blew the crashing thread's stack still gets its report and minidump written.
        DWORD WINAPI CrashReporterMain(LPVOID) noexcept
        {
            (void)::WaitForSingleObject(g_crash.requested, INFINITE);
            WriteCrashReport();
            (void)::SetEvent(g_crash.finished);
            return 0;
        }

        void ReportCrash(EXCEPTION_POINTERS* pointers) noexcept
        {
            if (::InterlockedCompareExchange(&g_crash.claimed, 1, 0) != 0)
            {
                // Already reporting. A second thread crashing meanwhile waits for that report, so the
                // process does not end under it; the reporter crashing in it cannot wait on itself.
                if (::GetCurrentThreadId() != g_crash.reporterId && g_crash.finished != nullptr)
                    (void)::WaitForSingleObject(g_crash.finished, kCrashReportWaitMilliseconds);
                return;
            }
            g_crash.pointers = pointers;
            g_crash.threadId = ::GetCurrentThreadId();
            g_crash.note = CrashNoteSlot();
            if (g_crash.reporterId != 0)
            {
                (void)::SetEvent(g_crash.requested);
                (void)::WaitForSingleObject(g_crash.finished, kCrashReportWaitMilliseconds);
            }
            else
            {
                WriteCrashReport();
                if (g_crash.finished != nullptr)
                    (void)::SetEvent(g_crash.finished);
            }
        }

        // Only reached with no debugger attached: a debugger is told first and stops there.
        LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pointers) noexcept
        {
            ReportCrash(pointers);
            // Then Windows' own handling, as before: Windows Error Reporting, and the exception
            // code as the exit code.
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // abort() raises SIGABRT and never reaches the filter, so it reports from here, at this
        // frame's own context. Returning lets abort() finish as it would have.
        void AbortHandler(int) noexcept
        {
            CONTEXT context{};
            ::RtlCaptureContext(&context);
            EXCEPTION_RECORD record{};
            record.ExceptionCode = kAbortCode;
#if defined(_M_X64) || defined(__x86_64__)
            record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
            record.ExceptionAddress = reinterpret_cast<PVOID>(context.Pc);
#endif
            EXCEPTION_POINTERS pointers{&record, &context};
            ReportCrash(&pointers);
        }
    }

    int WriteBacktrace(int fd) noexcept
    {
        void* frames[64];
        const USHORT count = ::CaptureStackBackTrace(1 /* not this function */, 64, frames, nullptr);
        const CrashOutput out{fd, INVALID_HANDLE_VALUE};
        for (USHORT i = 0; i < count; ++i)
        {
            WriteFrame(out, static_cast<int>(i), reinterpret_cast<std::uintptr_t>(frames[i]));
        }
        return static_cast<int>(count);
    }

    const char*& CrashNoteSlot() noexcept
    {
        static thread_local const char* note = nullptr;
        return note;
    }

    void InstallCrashBacktrace() noexcept
    {
        static bool installed = false;
        if (installed)
        {
            return;
        }
        installed = true;
#if defined(WIN32_SYSTEM_ASAN)
        // Under AddressSanitizer its own handler is the useful one (it names the faulting frame and
        // the freeing stack); ours would take its place. Leave the sanitizer in charge.
        return;
#endif

        char base[MAX_PATH];
        const std::size_t length = GetUserDataDirectory(base, sizeof(base));
        if (length != 0 && length < sizeof(base))
        {
            const int n = std::snprintf(g_crash.directory, sizeof(g_crash.directory), "%s\\%s\\crashes", base,
                                        reinterpret_cast<const char*>(USER_DATA_DIR_NAME));
            if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(g_crash.directory))
                g_crash.directory[0] = '\0';
        }

        char exe[MAX_PATH];
        const DWORD exeLength = ::GetModuleFileNameA(nullptr, exe, sizeof(exe));
        const char* name = "crash";
        if (exeLength != 0 && exeLength < sizeof(exe))
        {
            name = exe;
            for (const char* p = exe; *p != '\0'; ++p)
            {
                if (*p == '\\' || *p == '/')
                    name = p + 1;
            }
            char* dot = std::strrchr(exe, '.');
            if (dot != nullptr && dot >= name && _stricmp(dot, ".exe") == 0)
                *dot = '\0';
        }
        (void)std::snprintf(g_crash.program, sizeof(g_crash.program), "%s", name);

        g_crash.requested = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_crash.finished = ::CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual reset: every waiter wakes
        if (g_crash.requested != nullptr && g_crash.finished != nullptr)
        {
            const HANDLE reporter = ::CreateThread(nullptr, 256 * 1024, &CrashReporterMain, nullptr,
                                                   STACK_SIZE_PARAM_IS_A_RESERVATION, &g_crash.reporterId);
            if (reporter != nullptr)
                ::CloseHandle(reporter);
            else
                g_crash.reporterId = 0;
        }

        // Stack kept back for the filter when this thread (the main one) overflows its own.
        ULONG guarantee = 64 * 1024;
        (void)::SetThreadStackGuarantee(&guarantee);
        (void)::SetUnhandledExceptionFilter(&CrashFilter);
        (void)std::signal(SIGABRT, &AbortHandler);
    }
}
