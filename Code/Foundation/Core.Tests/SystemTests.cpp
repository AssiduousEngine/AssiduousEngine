// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstdlib> // std::abort (the crash handler child)
#include <cstring>
#if !defined(_WIN32)
#include <sys/stat.h> // stat/chmod: FileCopyPreserving's +x-preservation check
#endif

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// --- System ----------------------------------------------------------------

TEST_CASE("system: high-resolution time advances")
{
    CHECK(GetTickFrequency() > 0u);

    const u64 t0 = GetTicks();
    SleepMilliseconds(2);
    const u64 t1 = GetTicks();

    CHECK(t1 > t0);

    const f64 seconds = TicksToSeconds(t1 - t0);
    CHECK(seconds > 0.0);
    CHECK(seconds < 1.0); // a 2ms sleep should be well under a second
    CHECK(TicksToMilliseconds(t1 - t0) >= 1.0);
}

TEST_CASE("system: the wall clock is in Unix seconds, on FileStat's scale")
{
    const i64 now = GetUnixTime();
    CHECK(now > 1767225600); // after 2026-01-01, when this was written
    CHECK(now < 4102444800); // before 2100: seconds, not milliseconds

    // A file written now has a modified time within a few seconds of the clock.
    const char8_t* path = u8"scratch_sys_unix_time.bin";
    const byte one[] = {static_cast<byte>(7)};
    REQUIRE(WriteFile(path, Span<const byte>(one, 1)).IsOk());
    u64 size = 0;
    i64 modified = 0;
    REQUIRE(FileStat(path, size, modified));
    CHECK(modified >= now - 5);
    CHECK(modified <= GetUnixTime() + 5);
    (void)FileDelete(path);
}

TEST_CASE("system: CivilTimeOf reads a Unix time as a calendar date and a clock")
{
    const CivilTime epoch = CivilTimeOf(0);
    CHECK(epoch.year == 1970);
    CHECK(epoch.month == 1u);
    CHECK(epoch.day == 1u);
    const CivilTime leap = CivilTimeOf(951782400 + 3723); // 2000-02-29 01:02:03 UTC
    CHECK(leap.year == 2000);
    CHECK(leap.month == 2u);
    CHECK(leap.day == 29u);
    CHECK(leap.hour == 1u);
    CHECK(leap.minute == 2u);
    CHECK(leap.second == 3u);
    const CivilTime before = CivilTimeOf(-1); // 1969-12-31 23:59:59
    CHECK(before.year == 1969);
    CHECK(before.month == 12u);
    CHECK(before.day == 31u);
    CHECK(before.hour == 23u);
    static_assert(CivilTimeOf(1793491200).day == 1u); // 2026-11-01

    // The local offset is a whole number of minutes, within the zones there are.
    const i64 offset = GetLocalTimeOffset(GetUnixTime());
    CHECK(offset % 60 == 0);
    CHECK(offset >= -12 * 3600);
    CHECK(offset <= 14 * 3600);
}

TEST_CASE("system: info queries are sane")
{
    CHECK(LogicalCoreCount() >= 1u);

    const usize pageSize = PageSize();
    CHECK(pageSize >= 4096u);
    CHECK(IsPowerOfTwo(pageSize));
}

TEST_CASE("system: the host platform tag is the one export and plugin manifests name it by")
{
#if defined(__EMSCRIPTEN__)
    CHECK(GetHostPlatformName() == StringView(u8"Web"));
#elif defined(_WIN32)
    CHECK(GetHostPlatformName() == StringView(u8"Win64"));
#elif defined(__ANDROID__)
    CHECK(GetHostPlatformName() == StringView(u8"Android64"));
#else
    CHECK(GetHostPlatformName() == StringView(u8"Linux64"));
#endif
}

TEST_CASE("system: page allocation is usable and page-aligned")
{
    const usize pageSize = PageSize();

    void* p = PageAllocate(pageSize);
    REQUIRE(p != nullptr);
    CHECK(IsAligned(p, pageSize));

    MemSet(p, 0x5A, pageSize);
    CHECK(static_cast<u8*>(p)[0] == 0x5Au);
    CHECK(static_cast<u8*>(p)[pageSize - 1] == 0x5Au);

    PageFree(p, pageSize);
}

// --- System: files ---------------------------------------------------------

TEST_CASE("system: file write / read / seek / size round-trip")
{
    const StringView path = u8"scratch_system_test.tmp";
    const char payload[] = "engine file IO";
    const u64 length = sizeof(payload) - 1; // exclude null terminator

    // Write
    {
        FileHandle f = FileOpen(path, FileMode::Write);
        REQUIRE(FileIsValid(f));
        CHECK(FileWrite(f, payload, length) == static_cast<i64>(length));
        FileClose(f);
    }

    CHECK(FileExists(path));

    // Read back
    {
        FileHandle f = FileOpen(path, FileMode::Read);
        REQUIRE(FileIsValid(f));
        CHECK(FileSize(f) == static_cast<i64>(length));

        char buffer[32] = {};
        CHECK(FileRead(f, buffer, length) == static_cast<i64>(length));
        CHECK(buffer[0] == 'e');

        // Seek back to a known offset and re-read.
        CHECK(FileSeek(f, 7, SeekOrigin::Begin) == 7);
        char c = 0;
        CHECK(FileRead(f, &c, 1) == 1);
        CHECK(c == 'f'); // "engine file IO"[7]

        FileClose(f);
    }

    CHECK(FileDelete(path));
    CHECK_FALSE(FileExists(path));
}

TEST_CASE("system: a file lock is held by one handle at a time, and the file stays readable")
{
    const StringView path = u8"scratch_system_lock.tmp";
    {
        FileHandle f = FileOpen(path, FileMode::Write);
        REQUIRE(FileIsValid(f));
        CHECK(FileWrite(f, "1234\n", 5) == 5);
        FileClose(f);
    }
    FileHandle first = FileOpen(path, FileMode::ReadWrite);
    FileHandle second = FileOpen(path, FileMode::ReadWrite);
    REQUIRE(FileIsValid(first));
    REQUIRE(FileIsValid(second));
    CHECK(FileTryLock(first));
    CHECK_FALSE(FileTryLock(second)); // another handle, even in this process

    // The locked file's own bytes still read through another handle (what a second tool reads
    // to say who holds it).
    FileHandle reader = FileOpen(path, FileMode::Read);
    REQUIRE(FileIsValid(reader));
    char buffer[8] = {};
    CHECK(FileRead(reader, buffer, 5) == 5);
    CHECK(buffer[0] == '1');
    FileClose(reader);

    FileUnlock(first);
    CHECK(FileTryLock(second)); // released: the other takes it
    CHECK_FALSE(FileTryLock(first));
    FileClose(second); // closing releases it too
    CHECK(FileTryLock(first));
    FileClose(first);
    CHECK_FALSE(FileTryLock(kInvalidFile));
    CHECK(FileDelete(path));
}

TEST_CASE("system: opening a missing file fails cleanly")
{
    FileHandle f = FileOpen(u8"scratch_definitely_missing.xyz", FileMode::Read);
    CHECK_FALSE(FileIsValid(f));
}

TEST_CASE("system: console write does not crash")
{
    ConsoleWrite(u8"[console-test] console output check\n");
    CHECK(true);
}

TEST_CASE("system: OpenPathInFileManager rejects an empty path without launching")
{
    // Guard only: an empty or whitespace path must return false and not spawn a process. A real
    // path is not exercised here because it would pop a file-manager window during the test run.
    CHECK_FALSE(OpenPathInFileManager(u8""));
}

TEST_CASE("system: GetCurrentDirectory returns a non-empty absolute path")
{
    const String cwd = GetCurrentDirectory();
    CHECK_FALSE(cwd.IsEmpty());
    CHECK(PathIsAbsolute(cwd.AsView()));
}

TEST_CASE("system: PathAbsolute keeps an absolute path and puts a relative one under the working directory")
{
    const String cwd = GetCurrentDirectory();
    CHECK(PathAbsolute(cwd.AsView()) == cwd);
    const String made = PathAbsolute(u8"scratch_sys_abs/leaf.txt");
    CHECK(PathIsAbsolute(made.AsView()));
    CHECK(made == PathJoin(cwd.AsView(), u8"scratch_sys_abs/leaf.txt"));
}

TEST_CASE("system: GetHomeDirectory names the user's home, which exists")
{
    const String home = GetHomeDirectory();
#if !PLATFORM_WEB
    REQUIRE_FALSE(home.IsEmpty());
    CHECK(PathIsAbsolute(home.AsView()));
    CHECK(DirectoryExists(home.AsView()));
#endif
}

TEST_CASE("system: GetTempDirectory names a folder that exists")
{
    const String temp = GetTempDirectory();
    REQUIRE_FALSE(temp.IsEmpty());
    CHECK(PathIsAbsolute(temp.AsView()));
    CHECK(DirectoryExists(temp.AsView()));
}

TEST_CASE("system: CopyDirectoryRecursive copies a tree and overwrites what is there")
{
    const StringView from = u8"scratch_sys_copytree_from";
    const StringView to = u8"scratch_sys_copytree_to";
    (void)RemoveDirectoryRecursive(from);
    (void)RemoveDirectoryRecursive(to);
    const auto write = [](StringView path, StringView text)
    { return WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())); };
    REQUIRE(CreateDirectories(u8"scratch_sys_copytree_from/a/b"));
    REQUIRE(CreateDirectories(u8"scratch_sys_copytree_from/empty"));
    REQUIRE(write(u8"scratch_sys_copytree_from/top.txt", u8"top").IsOk());
    REQUIRE(write(u8"scratch_sys_copytree_from/a/b/deep.txt", u8"deep").IsOk());
    REQUIRE(CreateDirectories(to));
    REQUIRE(write(u8"scratch_sys_copytree_to/top.txt", u8"stale and longer").IsOk());

    CHECK(CopyDirectoryRecursive(from, to));
    Result<Array<byte>> top = ReadFile(u8"scratch_sys_copytree_to/top.txt");
    REQUIRE(top.HasValue());
    CHECK(StringView(reinterpret_cast<const utf8char*>(top.Value().Data()), top.Value().Size()) == StringView(u8"top"));
    Result<Array<byte>> deep = ReadFile(u8"scratch_sys_copytree_to/a/b/deep.txt");
    REQUIRE(deep.HasValue());
    CHECK(StringView(reinterpret_cast<const utf8char*>(deep.Value().Data()), deep.Value().Size()) == StringView(u8"deep"));
    CHECK(DirectoryExists(u8"scratch_sys_copytree_to/empty")); // an empty folder is copied too

    CHECK_FALSE(CopyDirectoryRecursive(u8"scratch_sys_copytree_missing", to)); // no source folder
    (void)RemoveDirectoryRecursive(from);
    (void)RemoveDirectoryRecursive(to);
}

TEST_CASE("system: CreateDirectories makes every missing segment")
{
    const StringView root = u8"scratch_sys_mkdirs";
    CHECK(CreateDirectories(u8"scratch_sys_mkdirs/a/b/c"));
    CHECK(DirectoryExists(u8"scratch_sys_mkdirs/a/b/c"));
    CHECK(CreateDirectories(u8"scratch_sys_mkdirs/a/b/c")); // idempotent
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a/b/c");
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a/b");
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a");
    (void)RemoveDirectory(root);
}

TEST_CASE("system: FileCopyPreserving copies bytes and keeps the mode")
{
    const StringView src = u8"scratch_sys_copy_src.bin";
    const StringView dst = u8"scratch_sys_copy_dst.bin";
    {
        FileHandle f = FileOpen(src, FileMode::Write);
        REQUIRE(FileIsValid(f));
        const char payload[] = "exec-me";
        (void)FileWrite(f, payload, sizeof(payload));
        FileClose(f);
    }
#if !defined(_WIN32)
    (void)::chmod("scratch_sys_copy_src.bin", 0755); // the +x bit the copy must keep
#endif
    CHECK(FileCopyPreserving(src, dst));
    u64 srcSize = 0, dstSize = 0;
    i64 t = 0;
    CHECK(FileStat(src, srcSize, t));
    CHECK(FileStat(dst, dstSize, t));
    CHECK(srcSize == dstSize);
#if !defined(_WIN32)
    struct stat st{};
    REQUIRE(::stat("scratch_sys_copy_dst.bin", &st) == 0);
    CHECK((st.st_mode & 0111) != 0); // execute bits preserved
#endif
    (void)FileDelete(src);
    (void)FileDelete(dst);
}

// --- RunProcess (blocking spawn + capture) ---------------------------------
// Subprocess spawning is a desktop-only capability (RunProcess drives the cook-time naga/tint
// shell-outs). Web has no fork/exec, so these do not apply there.
#if !PLATFORM_WEB

TEST_CASE("system: RunProcess captures stdout and reports exit 0")
{
#if defined(_WIN32)
    const StringView exe = u8"C:\\Windows\\System32\\cmd.exe";
    const StringView args[] = {u8"/c", u8"echo hello"};
#else
    const StringView exe = u8"/bin/echo";
    const StringView args[] = {u8"hello"};
#endif
    const ProcessResult r =
        RunProcess(exe, Span<const StringView>(args, sizeof(args) / sizeof(args[0])));
    CHECK(r.Ran());
    CHECK(r.Ok());
    CHECK(r.exitCode == 0);
    // echo emits "hello" then a newline, so the capture begins with the token.
    CHECK(r.output.AsView().StartsWith(u8"hello"));
}

TEST_CASE("system: RunProcess propagates a non-zero exit code")
{
#if defined(_WIN32)
    const StringView exe = u8"C:\\Windows\\System32\\cmd.exe";
    const StringView args[] = {u8"/c", u8"exit 3"};
    const int expected = 3;
#else
    const StringView exe = u8"/bin/sh";
    const StringView args[] = {u8"-c", u8"exit 3"};
    const int expected = 3;
#endif
    const ProcessResult r = RunProcess(exe, Span<const StringView>(args, 2));
    CHECK(r.Ran());
    CHECK_FALSE(r.Ok());
    CHECK(r.exitCode == expected);
}

// LaunchDetached starts a script that writes its arguments to a file, one per line, and returns
// before reading them back: the arguments arrive whole, spaces and all.
TEST_CASE("system: LaunchDetached starts a program with its arguments and does not wait")
{
    const String folder = GetCurrentDirectory();
    const String record = PathJoin(folder.AsView(), u8"scratch_sys_launch_record.txt");
#if defined(_WIN32)
    const String script = PathJoin(folder.AsView(), u8"scratch_sys_launch.cmd");
    const char body[] = "@echo off\r\n(echo %~2& echo %~3)> \"%~1\"\r\n";
#else
    const String script = PathJoin(folder.AsView(), u8"scratch_sys_launch.sh");
    const char body[] = "#!/bin/sh\nout=\"$1\"; shift\nprintf '%s\\n' \"$@\" > \"$out.part\"\nmv \"$out.part\" \"$out\"\n";
#endif
    (void)FileDelete(record.AsView());
    {
        FileHandle f = FileOpen(script.AsView(), FileMode::Write);
        REQUIRE(FileIsValid(f));
        (void)FileWrite(f, body, sizeof(body) - 1);
        FileClose(f);
    }
#if !defined(_WIN32)
    REQUIRE(::chmod(reinterpret_cast<const char*>(script.CStr()), 0755) == 0);
#endif
    const StringView args[] = {record.AsView(), u8"two words", u8"plain"};
    REQUIRE(LaunchDetached(script.AsView(), Span<const StringView>(args, 3)));

    String recorded;
    for (int attempt = 0; attempt < 500 && recorded.IsEmpty(); ++attempt)
    {
        FileHandle f = FileOpen(record.AsView(), FileMode::Read);
        if (FileIsValid(f))
        {
            char buffer[256];
            const i64 got = FileRead(f, buffer, sizeof(buffer));
            FileClose(f);
            if (got > 0)
            {
                recorded = String(StringView(reinterpret_cast<const char8_t*>(buffer), static_cast<usize>(got)));
            }
        }
        if (recorded.IsEmpty())
        {
            SleepMilliseconds(10);
        }
    }
    CHECK(recorded.AsView().StartsWith(u8"two words"));
    CHECK(recorded.AsView().Contains(u8"plain"));
    (void)FileDelete(record.AsView());
    (void)FileDelete(script.AsView());
}

TEST_CASE("system: LaunchDetached reports a program it cannot find")
{
    CHECK_FALSE(LaunchDetached(u8"scratch-no-such-program-anywhere", Span<const StringView>()));
    CHECK_FALSE(LaunchDetached(u8"", Span<const StringView>()));
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall GetHandleInformation(void* handle, unsigned long* flags);
#endif

// A process this one starts (a file manager, a build) must not inherit its sockets: a child that
// outlives it would hold the port (an editor's MCP server, a game's listener) after it has gone.
TEST_CASE("system: sockets are not inherited by a process this one starts")
{
    REQUIRE(InitializeNetworking());
    u16 port = 0;
    const SocketHandle listener = TcpListen(0, /*loopbackOnly*/ true, &port);
    REQUIRE(listener != kInvalidSocket);
    const SocketHandle udp = UdpOpen(0);
    REQUIRE(udp != kInvalidSocket);
    const SocketHandle client = TcpConnect(0x7F000001u, port);
    REQUIRE(client != kInvalidSocket);
    SocketHandle accepted = kInvalidSocket;
    for (int i = 0; i < 200 && accepted == kInvalidSocket; ++i)
    {
        accepted = TcpAccept(listener);
        if (accepted == kInvalidSocket)
            SleepMilliseconds(5);
    }
    REQUIRE(accepted != kInvalidSocket);

    const SocketHandle sockets[] = {listener, udp, client, accepted};
#if defined(_WIN32)
    for (const SocketHandle socket : sockets)
    {
        unsigned long flags = 0;
        REQUIRE(GetHandleInformation(reinterpret_cast<void*>(socket), &flags) != 0);
        CHECK((flags & 1ul) == 0ul); // HANDLE_FLAG_INHERIT
    }
#else
    // The child shell looks for each descriptor among its own.
    String script;
    for (const SocketHandle socket : sockets)
        script += Format(u8"test -e /proc/self/fd/{} && echo inherited {}; ", static_cast<u64>(socket), static_cast<u64>(socket));
    script += u8"echo done";
    const StringView args[] = {u8"-c", script.AsView()};
    const ProcessResult r = RunProcess(u8"/bin/sh", Span<const StringView>(args, 2));
    REQUIRE(r.Ok());
    CHECK_FALSE(r.output.AsView().Contains(u8"inherited"));
    CHECK(r.output.AsView().Contains(u8"done"));
#endif
    for (const SocketHandle socket : sockets)
        SocketClose(socket);
}

#if !defined(_WIN32)
TEST_CASE("system: an open file is not inherited by a process this one starts")
{
    // FileOpen opened without O_CLOEXEC, so a build or a file manager the editor started held every
    // file the editor had open, past the editor's own life.
    const FileHandle file = FileOpen(u8"scratch_cloexec.txt", FileMode::Write);
    REQUIRE(FileIsValid(file));
    const String script = Format(u8"test -e /proc/self/fd/{} && echo inherited; echo done", static_cast<u64>(file));
    const StringView args[] = {u8"-c", script.AsView()};
    const ProcessResult r = RunProcess(u8"/bin/sh", Span<const StringView>(args, 2));
    FileClose(file);
    FileDelete(u8"scratch_cloexec.txt");
    REQUIRE(r.Ok());
    CHECK_FALSE(r.output.AsView().Contains(u8"inherited"));
}
#endif

TEST_CASE("system: an environment value longer than 1023 bytes is read whole")
{
    // It was read into a 1024-byte buffer and cut there (a long PATH, a compiler's INCLUDE list).
    String longValue;
    for (int i = 0; i < 300; ++i)
        longValue.Append(u8"abcdefghij");
    REQUIRE(SetEnvironmentVariable(u8"ENGINE_TEST_LONG_VALUE", longValue.AsView()));
    const Optional<String> read = GetEnvironmentVariable(u8"ENGINE_TEST_LONG_VALUE");
    REQUIRE(read.HasValue());
    CHECK(read->Size() == longValue.Size());
    CHECK(*read == longValue);
    String edge; // and one exactly at the buffer's edge
    for (int i = 0; i < 1023; ++i)
        edge.PushBack(u8'x');
    REQUIRE(SetEnvironmentVariable(u8"ENGINE_TEST_LONG_VALUE", edge.AsView()));
    CHECK(GetEnvironmentVariable(u8"ENGINE_TEST_LONG_VALUE")->Size() == 1023u);
}

TEST_CASE("system: RunProcess reports failure to spawn a missing binary")
{
    // Explicit path, no PATH search: a bogus name cannot resolve on any platform.
    const ProcessResult r = RunProcess(u8"scratch_no_such_binary_zzz", Span<const StringView>());
    CHECK_FALSE(r.Ran());
    CHECK(r.exitCode < 0);
}

#if !defined(_WIN32)
TEST_CASE("system: RunProcess passes every argument, keeps all its output, and stops a run past its timeout")
{
    // Arguments past 62 were dropped, output past 64 KiB was cut (keeping the start, losing a
    // build's errors at the end), and a hung tool held the caller for good.
    Array<String> words;
    for (int i = 0; i < 100; ++i)
        words.PushBack(Format(u8"a{}", i));
    Array<StringView> args;
    args.PushBack(StringView(u8"-c"));
    args.PushBack(StringView(u8"echo $# \"${100}\""));
    args.PushBack(StringView(u8"sh")); // $0
    for (const String& w : words)
        args.PushBack(w.AsView());
    const ProcessResult many = RunProcess(u8"/bin/sh", Span<const StringView>(args.Data(), args.Size()));
    REQUIRE(many.Ok());
    CHECK(many.output.AsView().StartsWith(u8"100 a99"));

    // About 200 KiB of output, its last line kept.
    const StringView big[] = {u8"-c", u8"i=0; while [ $i -lt 4000 ]; do echo 'line of fifty characters to fill the output ......'; i=$((i+1)); done; echo END"};
    const ProcessResult loud = RunProcess(u8"/bin/sh", Span<const StringView>(big, 2));
    REQUIRE(loud.Ok());
    CHECK(loud.output.Size() > 128u * 1024u);
    CHECK(loud.output.AsView().EndsWith(u8"END\n"));

    // A run past its timeout is killed and says so.
    const StringView slow[] = {u8"-c", u8"sleep 30"};
    const Stopwatch clock = Stopwatch::StartNew();
    const ProcessResult late = RunProcess(u8"/bin/sh", Span<const StringView>(slow, 2), 300);
    CHECK(late.timedOut);
    CHECK_FALSE(late.Ran());
    CHECK(clock.Elapsed().AsMilliseconds() < 10000.0);
}
#endif

#endif // !PLATFORM_WEB (subprocess spawning is desktop-only)

// NormalizePathSeparators is fanned out to the platform backends; the Win32 OpenPathInFileManager
// relies on it (Explorer refuses '/'). The direction is OS-native and therefore host-dependent
// ('\' on Windows, '/' on POSIX), so the separator expectations are compiled per platform. The
// empty, exact-fit and overlong contract is shared and asserted once.
TEST_CASE("system: NormalizePathSeparators yields OS-native separators")
{
    char out[64];

#if PLATFORM_WINDOWS
    SUBCASE("forward slashes become backslashes on Windows")
    {
        CHECK(NormalizePathSeparators(u8"a/b/My Dir", out, sizeof(out)));
        CHECK(std::strcmp(out, "a\\b\\My Dir") == 0);
    }
    SUBCASE("already-native path is unchanged")
    {
        CHECK(NormalizePathSeparators(u8"C:\\home\\foo", out, sizeof(out)));
        CHECK(std::strcmp(out, "C:\\home\\foo") == 0);
    }
#else
    SUBCASE("backslashes become forward slashes on POSIX")
    {
        CHECK(NormalizePathSeparators(u8"a\\b\\My Dir", out, sizeof(out)));
        CHECK(std::strcmp(out, "a/b/My Dir") == 0);
    }
    SUBCASE("already-native path is unchanged")
    {
        CHECK(NormalizePathSeparators(u8"/home/foo/bar", out, sizeof(out)));
        CHECK(std::strcmp(out, "/home/foo/bar") == 0);
    }
#endif
    SUBCASE("empty path is rejected")
    {
        CHECK_FALSE(NormalizePathSeparators(u8"", out, sizeof(out)));
    }
    SUBCASE("overlong path is rejected, not truncated mid-path")
    {
        // 8 chars + NUL needs 9; a capacity of 8 must refuse rather than write a partial path.
        char small[8];
        CHECK_FALSE(NormalizePathSeparators(u8"a\\bcdefg", small, sizeof(small)));
    }
    SUBCASE("exact fit (length + NUL == cap) is accepted")
    {
        char exact[8];
        CHECK(NormalizePathSeparators(u8"a\\bcdef", exact, sizeof(exact))); // 7 + NUL == 8
#if PLATFORM_WINDOWS
        CHECK(std::strcmp(exact, "a\\bcdef") == 0);
#else
        CHECK(std::strcmp(exact, "a/bcdef") == 0);
#endif
    }
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
#include <unistd.h>

// The fatal-signal backtrace: a child installs the handler and segfaults; the parent asserts
// that the crash banner and backtrace reached stderr and that the default signal disposition was
// preserved (the child still dies by the signal, so cores and exit status behave as usual).
TEST_CASE("system: InstallCrashBacktrace prints a native stack on SIGSEGV")
{
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    // Under AddressSanitizer InstallCrashBacktrace is a no-op: the sanitizer's own handler
    // reports the faulting frame and the freeing stack, and ours would replace it.
    MESSAGE("AddressSanitizer build: the engine's crash handler yields to the sanitizer's");
    return;
#endif
    int pipeFds[2];
    REQUIRE(pipe(pipeFds) == 0);

    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // Child: route stderr into the pipe, install, crash.
        dup2(pipeFds[1], 2);
        close(pipeFds[0]);
        close(pipeFds[1]);
        InstallCrashBacktrace();
        volatile int* nullPointer = nullptr;
        *nullPointer = 42; // SIGSEGV: the handler prints, then re-raises with SIG_DFL
        _exit(0);          // unreachable
    }

    close(pipeFds[1]);
    char buffer[8192];
    usize total = 0;
    ssize_t n = 0;
    while ((n = read(pipeFds[0], buffer + total, sizeof(buffer) - 1 - total)) > 0)
    {
        total += static_cast<usize>(n);
        if (total >= sizeof(buffer) - 1)
        {
            break;
        }
    }
    buffer[total] = '\0';
    close(pipeFds[0]);

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));                       // default disposition preserved
    CHECK(WTERMSIG(status) == SIGSEGV);               // died by the segfault, after printing
    CHECK(std::strstr(buffer, "SIGSEGV") != nullptr); // the banner
    CHECK(std::strstr(buffer, "backtrace") != nullptr);
    CHECK(std::strstr(buffer, "0x") != nullptr); // at least one frame line
}
#endif

#if defined(_WIN32)
// Declared alone: windows.h's macros would rename this file's calls into Core.
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);

namespace
{
    // Recurses until the stack runs out; the volatile frame keeps the optimizer from making it a loop.
    int OverflowStack(int depth)
    {
        volatile char frame[4096];
        frame[0] = static_cast<char>(depth);
        if (depth < 0) // never: it keeps clang from calling the recursion a mistake
            return 0;
        return OverflowStack(depth + 1) + frame[0];
    }

    // What a crashing child left behind: its output, and its minidump and report, read back.
    struct CrashRun
    {
        ProcessResult run;
        String dumpName;
        Array<byte> dump;
        String report;
    };

    // Runs Core.Tests again with only the child case below, asked to crash `how`, its user data
    // folder pointed at scratch through LOCALAPPDATA (which the child inherits).
    CrashRun RunCrashingChild(StringView how)
    {
        CrashRun result;
        const String scratch = PathAbsolute(u8"crash_handler_scratch");
        (void)RemoveDirectoryRecursive(scratch.AsView());
        REQUIRE(CreateDirectory(scratch.AsView()));
        const Optional<String> savedLocalAppData = GetEnvironmentVariable(u8"LOCALAPPDATA");
        REQUIRE(SetEnvironmentVariable(u8"LOCALAPPDATA", scratch.AsView()));
        REQUIRE(SetEnvironmentVariable(u8"ENGINE_CRASH_CHILD", how));
        const StringView args[] = {u8"--test-case=system: crash handler child crashes when asked"};
        result.run = RunProcess(GetExecutablePath().AsView(), Span<const StringView>(args, 1), 120000);
        (void)SetEnvironmentVariable(u8"ENGINE_CRASH_CHILD", u8"");
        (void)SetEnvironmentVariable(u8"LOCALAPPDATA",
                                     savedLocalAppData.HasValue() ? savedLocalAppData.Value().AsView() : StringView());

        struct Found
        {
            String dump;
            String report;
        } found;
        const String crashes = PathJoin(PathJoin(scratch.AsView(), USER_DATA_DIR_NAME).AsView(), u8"crashes");
        (void)ListDirectory(
            crashes.AsView(),
            [](void* context, StringView name, bool isDirectory)
            {
                Found& into = *static_cast<Found*>(context);
                if (!isDirectory && name.StartsWith(u8"Core.Tests-") && name.EndsWith(u8".dmp"))
                    into.dump = String(name);
                if (!isDirectory && name.StartsWith(u8"Core.Tests-") && name.EndsWith(u8".txt"))
                    into.report = String(name);
            },
            &found);
        result.dumpName = found.dump;
        if (!found.dump.IsEmpty())
        {
            Result<Array<byte>> dump = ReadFile(PathJoin(crashes.AsView(), found.dump.AsView()).AsView());
            if (dump.HasValue())
                result.dump = Move(dump.Value());
        }
        if (!found.report.IsEmpty())
        {
            const Result<Array<byte>> report = ReadFile(PathJoin(crashes.AsView(), found.report.AsView()).AsView());
            if (report.HasValue())
                result.report =
                    String(StringView(reinterpret_cast<const utf8char*>(report.Value().Data()), report.Value().Size()));
        }
        (void)RemoveDirectoryRecursive(scratch.AsView());
        return result;
    }

    bool IsMinidump(const Array<byte>& bytes)
    {
        return bytes.Size() > 4 && std::memcmp(bytes.Data(), "MDMP", 4) == 0; // a minidump's signature
    }
}

// The child half of the tests below, run as a process of its own. Without the variable it does nothing.
TEST_CASE("system: crash handler child crashes when asked")
{
    const Optional<String> asked = GetEnvironmentVariable(u8"ENGINE_CRASH_CHILD");
    if (!asked.HasValue() || asked.Value().IsEmpty())
    {
        return;
    }
    // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX: no Windows Error Reporting dialog to wait on.
    (void)SetErrorMode(0x0001 | 0x0002);
    InstallCrashBacktrace();
    const StringView how = asked.Value().AsView();
    if (how == StringView(u8"abort"))
    {
        std::abort(); // the SIGABRT handler reports, then abort() ends the process
    }
    if (how == StringView(u8"overflow"))
    {
        (void)OverflowStack(0);
    }
    volatile int* nullPointer = nullptr;
    *nullPointer = 42; // the filter reports, then Windows ends the process with the exception code
}

#if defined(__has_feature) // nested so MSVC only lexes __has_feature (see TestMain.h)
#if __has_feature(address_sanitizer)
#define SYSTEM_TESTS_ASAN 1
#endif
#endif

// The unhandled exception filter: a crash prints its exception, the address and a backtrace of
// module offsets to stderr, and leaves a minidump with the same lines beside it in the user data
// folder's crashes.
TEST_CASE("system: InstallCrashBacktrace writes a report and a minidump on an access violation")
{
#if defined(SYSTEM_TESTS_ASAN)
    MESSAGE("AddressSanitizer build: the engine's crash handler yields to the sanitizer's");
    return;
#endif
    const CrashRun crash = RunCrashingChild(u8"access");
    CHECK_FALSE(crash.run.timedOut);
    CHECK(crash.run.exitCode != 0); // ended by the exception (0xC0000005), not a clean exit
    const StringView output = crash.run.output.AsView();
    CHECK(output.Contains(u8"EXCEPTION_ACCESS_VIOLATION"));
    CHECK(output.Contains(u8"writing address 0x0000000000000000"));
    CHECK(output.Contains(u8"Core.Tests.exe+0x")); // the faulting frame, as a module offset
    CHECK(output.Contains(u8"minidump: "));

    REQUIRE_FALSE(crash.dumpName.IsEmpty());
    CHECK(IsMinidump(crash.dump));
    CHECK(crash.report.AsView().Contains(u8"EXCEPTION_ACCESS_VIOLATION"));
    CHECK(crash.report.AsView().Contains(u8"Core.Tests.exe+0x"));
    CHECK(crash.report.AsView().Contains(crash.dumpName.AsView())); // names the minidump it sits beside
}

// abort() (where a fatal error and std::terminate end) never reaches the filter; the SIGABRT
// handler reports it the same way.
TEST_CASE("system: InstallCrashBacktrace writes a report and a minidump on abort")
{
#if defined(SYSTEM_TESTS_ASAN)
    MESSAGE("AddressSanitizer build: the engine's crash handler yields to the sanitizer's");
    return;
#endif
    const CrashRun crash = RunCrashingChild(u8"abort");
    CHECK_FALSE(crash.run.timedOut);
    CHECK(crash.run.exitCode != 0);
    CHECK(crash.run.output.AsView().Contains(u8"=== abort()"));
    CHECK(crash.run.output.AsView().Contains(u8"Core.Tests.exe+0x"));
    REQUIRE_FALSE(crash.dumpName.IsEmpty());
    CHECK(IsMinidump(crash.dump));
    CHECK(crash.report.AsView().Contains(u8"abort()"));
}

// A blown stack leaves the crashing thread too little to write a minidump on; the reporter thread
// writes it, and walks the crashed thread's stack from the outside.
TEST_CASE("system: InstallCrashBacktrace writes a report and a minidump on a stack overflow")
{
#if defined(SYSTEM_TESTS_ASAN)
    MESSAGE("AddressSanitizer build: the engine's crash handler yields to the sanitizer's");
    return;
#endif
    const CrashRun crash = RunCrashingChild(u8"overflow");
    CHECK_FALSE(crash.run.timedOut);
    CHECK(crash.run.exitCode != 0);
    CHECK(crash.run.output.AsView().Contains(u8"EXCEPTION_STACK_OVERFLOW"));
    CHECK(crash.run.output.AsView().Contains(u8"Core.Tests.exe+0x"));
    REQUIRE_FALSE(crash.dumpName.IsEmpty());
    CHECK(IsMinidump(crash.dump));
    CHECK(crash.report.AsView().Contains(u8"EXCEPTION_STACK_OVERFLOW"));
}
#endif

TEST_CASE("system: FillEntropy fills the whole span with bytes that never repeat a draw")
{
    byte first[32] = {};
    byte second[32] = {};
    REQUIRE(FillEntropy(Span<byte>{first, sizeof(first)}));
    REQUIRE(FillEntropy(Span<byte>{second, sizeof(second)}));
    CHECK(std::memcmp(first, second, sizeof(first)) != 0);
    bool anyNonZero = false;
    for (byte b : first)
    {
        anyNonZero = anyNonZero || b != byte{0};
    }
    CHECK(anyNonZero);
    byte none[1] = {byte{7}};
    CHECK(FillEntropy(Span<byte>{none, 0})); // an empty span is trivially filled
    // Past the OS call's per-request cap (getentropy takes 256 at a time) the fill chunks:
    // every part of a long buffer, the tail included, is written.
    byte many[300];
    std::memset(many, 0, sizeof(many));
    REQUIRE(FillEntropy(Span<byte>{many, sizeof(many)}));
    bool headWritten = false;
    bool tailWritten = false;
    for (usize i = 0; i < 256; ++i)
    {
        headWritten = headWritten || many[i] != byte{0};
    }
    for (usize i = 256; i < sizeof(many); ++i)
    {
        tailWritten = tailWritten || many[i] != byte{0};
    }
    CHECK(headWritten);
    CHECK(tailWritten); // 44 zero bytes of entropy: odds of 1 in 2^352
    CHECK(none[0] == byte{7});
}


TEST_CASE("system: a crash note names what the thread is doing, nested, cut to fit, and gone after")
{
    const auto current = []() { return StringView(reinterpret_cast<const utf8char*>(CrashNoteScope::Current())); };
    CHECK(CrashNoteScope::Current() == nullptr);
    {
        CrashNoteScope outer(u8"Orb", u8".", u8"onUpdate", u8" on 'orb'");
        CHECK(current() == StringView(u8"Orb.onUpdate on 'orb'"));
        {
            CrashNoteScope inner(u8"Gate.onOpen");
            CHECK(current() == StringView(u8"Gate.onOpen"));
        }
        CHECK(current() == StringView(u8"Orb.onUpdate on 'orb'"));
    }
    CHECK(CrashNoteScope::Current() == nullptr);
    String longer;
    for (int i = 0; i < 40; ++i)
    {
        longer += StringView(u8"abcdefgh");
    }
    CrashNoteScope cut(longer.AsView());
    CHECK(current().Size() < longer.Size()); // cut to its buffer, still a terminated string
    CHECK(longer.AsView().StartsWith(current()));
}
