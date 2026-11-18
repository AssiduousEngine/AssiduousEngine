// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :logger partition
//
// Logging frontend: log levels, the references a line may carry, the ILogSink interface, the
// Logger (sink list and level filter) and the Logf frontend behind the LOG_* macros.
// Concrete sinks (Console/File/Ring) live in their own partitions.
//
// Thread-safe: an atomic level filters cheaply on the hot path; a mutex guards
// sink registration and dispatch.

module;
#include "Core/Prelude.h"

export module foundation.core:logger;

import :base;
import :array;
import :string;
import :format;
import :atomic;
import :mutex;
import :scoped_lock;
import :span;
import :guid;

export namespace foundation::core
{
    enum class LogLevel : u8
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal,
        Off, // sentinel: filters out everything
    };

    [[nodiscard]] inline const utf8char* LogLevelName(LogLevel level) noexcept
    {
        switch (level)
        {
        case LogLevel::Trace:
            return u8"Trace";
        case LogLevel::Debug:
            return u8"Debug";
        case LogLevel::Info:
            return u8"Info";
        case LogLevel::Warning:
            return u8"Warning";
        case LogLevel::Error:
            return u8"Error";
        case LogLevel::Fatal:
            return u8"Fatal";
        case LogLevel::Off:
            return u8"Off";
        }
        return u8"?";
    }

    // -----------------------------------------------------------------------
    // References
    // -----------------------------------------------------------------------

    /// The kinds of reference the engine makes; a plugin or a language may add its own.
    inline constexpr StringView kLogSourceReference = u8"source"; // fields: file (on disk, or a project's source path), line, column
    inline constexpr StringView kLogAssetReference = u8"asset";   // fields: guid
    inline constexpr StringView kLogEntityReference = u8"entity"; // fields: scene, path
    inline constexpr StringView kLogNodeReference = u8"node";     // fields: document, id

    /// A place a log line points at, which a tool can follow (open the file at the line, select
    /// the asset): its kind and its fields, in the order the kind gives them. `start` and
    /// `length` are the part of the message that stands for it, in bytes; a reference a log call
    /// attached has no part (length 0) and stands for the whole line.
    struct LogReference
    {
        String kind;
        Array<String> fields;
        usize start = 0;
        usize length = 0;

        /// Field `index`, empty when it has fewer.
        [[nodiscard]] StringView Field(usize index) const noexcept
        {
            return index < fields.Size() ? fields[index].AsView() : StringView{};
        }
    };

    /// A file on disk, at a 1-based line and column (0: not given).
    [[nodiscard]] inline LogReference SourceReference(StringView file, u32 line = 0, u32 column = 0)
    {
        LogReference reference;
        reference.kind = String(kLogSourceReference);
        reference.fields.PushBack(String(file));
        reference.fields.PushBack(line > 0 ? Format(u8"{}", line) : String{});
        reference.fields.PushBack(column > 0 ? Format(u8"{}", column) : String{});
        return reference;
    }

    /// An asset, by its id.
    [[nodiscard]] inline LogReference AssetReference(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        LogReference reference;
        reference.kind = String(kLogAssetReference);
        reference.fields.PushBack(String(StringView(text, 36)));
        return reference;
    }

    // -----------------------------------------------------------------------
    // Sinks
    // -----------------------------------------------------------------------
    class ILogSink
    {
    public:
        virtual ~ILogSink() = default;
        virtual void Write(LogLevel level, StringView category, StringView message) noexcept = 0;
        /// A line with the references it carries. A sink that keeps none (a console, a file, the
        /// fixed-size ring) writes the text alone, which is the default.
        virtual void WriteReferenced(LogLevel level, StringView category, StringView message,
                                     Span<const LogReference> references) noexcept
        {
            (void)references;
            Write(level, category, message);
        }
    };

    namespace detail
    {
        inline void FormatLine(String& line, LogLevel level, StringView category,
                               StringView message)
        {
            // "[Level] category: message\n"
            line.Clear();
            line.PushBack(utf8char('['));
            line.Append(StringView(LogLevelName(level)));
            line.PushBack(utf8char(']'));
            line.PushBack(utf8char(' '));
            line.Append(category);
            line.PushBack(utf8char(':'));
            line.PushBack(utf8char(' '));
            line.Append(message);
            line.PushBack(utf8char('\n'));
        }
    }

    // -----------------------------------------------------------------------
    // Logger - owns the sink list and the active level filter.
    // -----------------------------------------------------------------------
    // Thread-safe: an atomic level filters cheaply on the hot path; a mutex
    // guards sink registration and dispatch.
    class Logger
    {
    public:
        // Sinks are non-owning; the caller manages their lifetime.
        void AddSink(ILogSink* sink)
        {
            if (sink == nullptr)
            {
                return;
            }
            ScopedLock lock(m_mutex);
            m_sinks.PushBack(sink);
        }

        void RemoveSink(ILogSink* sink) noexcept
        {
            ScopedLock lock(m_mutex);
            for (usize i = 0; i < m_sinks.Size(); ++i)
            {
                if (m_sinks[i] == sink)
                {
                    m_sinks.RemoveAtSwap(i);
                    return;
                }
            }
        }

        void SetMinLevel(LogLevel level) noexcept { m_minLevel.store(level); }
        [[nodiscard]] LogLevel MinLevel() const noexcept { return m_minLevel.load(); }

        [[nodiscard]] bool IsEnabled(LogLevel level) const noexcept
        {
            return static_cast<u8>(level) >= static_cast<u8>(m_minLevel.load());
        }

        void Dispatch(LogLevel level, StringView category, StringView message,
                      Span<const LogReference> references = {}) noexcept
        {
            ScopedLock lock(m_mutex);
            for (ILogSink* sink : m_sinks)
            {
                if (references.IsEmpty())
                {
                    sink->Write(level, category, message);
                }
                else
                {
                    sink->WriteReferenced(level, category, message, references);
                }
            }
        }

    private:
        Array<ILogSink*> m_sinks;
        Atomic<LogLevel> m_minLevel{LogLevel::Info};
        Mutex m_mutex;
    };

    [[nodiscard]] Logger& GlobalLogger() noexcept
    {
        static Logger instance;
        return instance;
    }

    // -----------------------------------------------------------------------
    // Frontend - formats and dispatches (used by the LOG_* macros).
    // The format buffer is UTF-8, matching the Logger/sink UTF-8 API.
    // -----------------------------------------------------------------------
    template <typename... Args>
    void Logf(LogLevel level, StringView category, FormatString<Args...> fmt, const Args&... args)
    {
        Logger& logger = GlobalLogger();
        if (!logger.IsEnabled(level))
        {
            return;
        }

        FormatBuffer buffer;
        FormatToV(buffer, fmt.data, args...);
        const String message = String(buffer.View());
        logger.Dispatch(level, category, message);
    }

    /// Logf with the references the line carries (the LOG_*_AT macros).
    template <typename... Args>
    void LogfReferenced(LogLevel level, StringView category, Span<const LogReference> references,
                        FormatString<Args...> fmt, const Args&... args)
    {
        Logger& logger = GlobalLogger();
        if (!logger.IsEnabled(level))
        {
            return;
        }

        FormatBuffer buffer;
        FormatToV(buffer, fmt.data, args...);
        const String message = String(buffer.View());
        logger.Dispatch(level, category, message, references);
    }

    /// LogfReferenced with one reference.
    template <typename... Args>
    void LogfAt(LogLevel level, StringView category, const LogReference& reference,
                FormatString<Args...> fmt, const Args&... args)
    {
        LogfReferenced(level, category, Span<const LogReference>(&reference, 1), fmt, args...);
    }
}
