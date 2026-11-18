// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.DotNet - the C# script backend (see DotNetScript.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module foundation.script.dotnet;

import foundation.core;
import foundation.script;

using namespace foundation::core;

namespace foundation::script::dotnet
{
    namespace
    {
        class DotNetScriptContext;

        class DotNetScriptObject final : public ScriptObject
        {
        public:
            DotNetScriptObject(RefPtr<DotNetScriptContext> context, DotNetRuntime& runtime, u64 instance)
                : m_context(Move(context)), m_runtime(&runtime), m_instance(instance)
            {
            }
            ~DotNetScriptObject() override { m_runtime->Release(m_instance); }

            [[nodiscard]] Result<Variant> Invoke(StringView method, Span<Variant> args) override;
            // A bound method is called by the name it was bound by: the handle's id is its index in
            // m_bound plus one.
            [[nodiscard]] ScriptMethod Bind(StringView method, usize argCount) override
            {
                (void)argCount;
                for (usize i = 0; i < m_bound.Size(); ++i)
                    if (m_bound[i].AsView() == method)
                        return ScriptMethod{static_cast<u64>(i + 1)};
                m_bound.PushBack(String(method));
                return ScriptMethod{static_cast<u64>(m_bound.Size())};
            }
            [[nodiscard]] Result<Variant> Invoke(ScriptMethod method, Span<Variant> args) override
            {
                if (method.id == 0 || method.id > m_bound.Size())
                    return Err(ErrorCode::NotFound);
                const String name = m_bound[static_cast<usize>(method.id - 1)]; // a bind during the call may move the list
                return Invoke(name.AsView(), args);
            }
            [[nodiscard]] Result<Variant> GetProperty(StringView name) override;

        private:
            RefPtr<DotNetScriptContext> m_context;
            DotNetRuntime* m_runtime;
            u64 m_instance;
            Array<String> m_bound;
        };

        class DotNetScriptContext final : public IScriptContext
        {
        public:
            explicit DotNetScriptContext(DotNetRuntime& runtime) : m_runtime(&runtime) {}
            // Its assemblies go with it: another run's context has its own (Owner).
            ~DotNetScriptContext() override { m_runtime->ReleaseOwner(Owner()); }

            // This context as the runtime's owner of what it loads: its own copy of each assembly, so
            // two runs never share a class's statics.
            [[nodiscard]] u64 Owner() const noexcept { return reinterpret_cast<u64>(this); }

            void SetErrorHandler(IScriptErrorHandler* handler) override { m_errors = handler; }

            Status Load(StringView source, StringView chunkName, Span<const ScriptSection> includes) override
            {
                (void)source;
                (void)includes;
                Report(ScriptErrorKind::Compile, chunkName, -1,
                       u8"C# does not compile at run time: its project builds into an assembly at cook");
                return Status{ErrorCode::NotSupported};
            }

            Status LoadBehaviorModule(Span<const BehaviorModuleClass> classes, StringView moduleName) override
            {
                for (const BehaviorModuleClass& entry : classes)
                {
                    if (entry.bytecode.IsEmpty())
                    {
                        Report(ScriptErrorKind::Compile, entry.name, -1,
                               u8"this C# class has no built assembly (cook its project)");
                        return Status{ErrorCode::NotFound};
                    }
                    // Every class of a project carries its assembly; the runtime loads an image
                    // once for this context while it is current, and anew after another replaced it.
                    if (!m_runtime->LoadAssembly(Owner(), entry.bytecode, entry.symbols).IsOk())
                    {
                        Report(ScriptErrorKind::Compile, moduleName, -1, m_runtime->Failure());
                        return Status{ErrorCode::Unknown};
                    }
                }
                return Status{};
            }

            // Globals live on this side: C# has no global scope the engine reaches into.
            void SetGlobal(StringView name, const Variant& value) override
            {
                for (Global& global : m_globals)
                    if (global.name.AsView() == name)
                    {
                        global.value = value;
                        return;
                    }
                m_globals.PushBack(Global{String(name), value});
            }
            [[nodiscard]] Variant GetGlobal(StringView name) override
            {
                for (const Global& global : m_globals)
                    if (global.name.AsView() == name)
                        return global.value;
                return Variant{};
            }
            [[nodiscard]] bool HasFunction(StringView name) const override
            {
                (void)name;
                return false;
            }
            [[nodiscard]] Result<Variant> Call(StringView function, Span<Variant> args) override
            {
                (void)function;
                (void)args;
                return Err(ErrorCode::NotSupported);
            }

            [[nodiscard]] RefPtr<ScriptObject> CreateInstance(StringView className, Span<Variant> args) override
            {
                // C# runs inside this context: what it calls in the engine resolves its run's services.
                ScriptCallScope scope(this);
                const Result<u64> made = m_runtime->CreateInstance(Owner(), className, Span<const Variant>{args.Data(), args.Size()});
                if (!made.HasValue())
                {
                    ReportFailure(className);
                    return {};
                }
                return RefPtr<ScriptObject>(
                    MakeRef<DotNetScriptObject>(MemoryAllocator(), RefPtr<DotNetScriptContext>(this), *m_runtime, made.Value()).Get());
            }

            /// A failed call's reason, as a runtime error at the C# line that threw, with the stack
            /// below it, when its stack names one.
            void ReportFailure(StringView where)
            {
                const StringView failure = m_runtime->Failure();
                const Array<ScriptStackFrame> stack = ParseCSharpStack(failure);
                StringView file = where;
                i32 line = -1;
                for (const ScriptStackFrame& frame : stack)
                    if (!frame.file.IsEmpty())
                    {
                        file = frame.file.AsView();
                        line = frame.line;
                        break;
                    }
                // The message is the exception's first line; its stack goes as the frames.
                const usize newline = failure.Find(u8'\n');
                const StringView message = stack.IsEmpty() || newline == StringView::kNotFound ? failure : failure.SubStr(0, newline);
                Report(ScriptErrorKind::Runtime, file, line, message, stack.AsSpan());
            }

        private:
            struct Global
            {
                String name;
                Variant value;
            };

            void Report(ScriptErrorKind kind, StringView module, i32 line, StringView message, Span<const ScriptStackFrame> stack = {})
            {
                if (m_errors != nullptr)
                    m_errors->OnError(ScriptError{kind, module, line, -1, message, stack});
                else
                    LogScriptFault(module, line, message, stack);
            }

            DotNetRuntime* m_runtime;
            IScriptErrorHandler* m_errors = nullptr;
            Array<Global> m_globals;
        };

        // Every call into C# runs inside the object's context, so what the C# calls in the engine
        // (Input, gui, entity.send and the other facades) resolves that run's services, as a call
        // into the other languages does.
        Result<Variant> DotNetScriptObject::Invoke(StringView method, Span<Variant> args)
        {
            ScriptCallScope scope(m_context.Get());
            Result<Variant> result = m_runtime->Invoke(m_instance, method, Span<const Variant>{args.Data(), args.Size()});
            // A member that is not there is the caller's to handle (a handler a class does not
            // declare); a call that threw is a script fault.
            if (!result.HasValue() && result.Error() != ErrorCode::NotFound)
                m_context->ReportFailure(method);
            return result;
        }

        Result<Variant> DotNetScriptObject::GetProperty(StringView name)
        {
            ScriptCallScope scope(m_context.Get());
            return m_runtime->GetField(m_instance, name);
        }

        // A C# handler the engine holds (a GUI event's, a message's): called through the runtime as an
        // instance, inside the context that gave it, and let go when the engine drops it.
        class DotNetScriptDelegate final : public IScriptDelegate
        {
        public:
            DotNetScriptDelegate(DotNetRuntime& runtime, u64 handler, RefPtr<IScriptContext> context)
                : m_runtime(&runtime), m_handler(handler), m_context(Move(context))
            {
            }
            ~DotNetScriptDelegate() override { m_runtime->Release(m_handler); }

            [[nodiscard]] Result<Variant> Invoke(Span<Variant> args) override
            {
                ScriptCallScope scope(m_context.Get());
                Result<Variant> result = m_runtime->Invoke(m_handler, u8"Invoke", Span<const Variant>{args.Data(), args.Size()});
                if (!result.HasValue())
                {
                    if (auto* context = static_cast<DotNetScriptContext*>(m_context.Get()))
                        context->ReportFailure(u8"handler");
                    else
                        LOG_ERROR(u8"Script", u8"C# handler: {}", m_runtime->Failure());
                }
                return result;
            }

        private:
            DotNetRuntime* m_runtime;
            u64 m_handler;
            RefPtr<IScriptContext> m_context; // the C# context the handler came from
        };

        DotNetRuntime* s_handlerRuntime = nullptr; // the process runtime the handlers belong to
        IAllocator* s_handlerAllocator = nullptr;  // the C# script manager's, which makes them

        Variant MakeHandler(void* handle)
        {
            if (s_handlerRuntime == nullptr || s_handlerAllocator == nullptr)
                return Variant{};
            RefPtr<IScriptContext> context(CurrentScriptContext());
            return Variant::From(RefPtr<IScriptDelegate>(
                MakeRef<DotNetScriptDelegate>(*s_handlerAllocator, *s_handlerRuntime, reinterpret_cast<u64>(handle), Move(context)).Get()));
        }

        class DotNetScriptManager final : public IScriptManager
        {
        public:
            explicit DotNetScriptManager(DotNetRuntime* runtime) : m_runtime(runtime) {}

            // The surface C# sees is generated from reflection when the engine builds.
            void RegisterType(const TypeInfo& type) override { (void)type; }

            [[nodiscard]] RefPtr<IScriptContext> CreateContext() override
            {
                if (m_runtime == nullptr)
                    return {};
                return RefPtr<IScriptContext>(MakeRef<DotNetScriptContext>(MemoryAllocator(), *m_runtime).Get());
            }

            // A C# file compiles with its project when the project builds (at cook); alone it is
            // not checked here.
            [[nodiscard]] bool Check(StringView source, StringView chunkName, IScriptErrorHandler& errors) override
            {
                (void)source;
                (void)chunkName;
                (void)errors;
                return true;
            }

            void SetBudget(const ScriptBudget& budget) override { m_budget = budget; }
            [[nodiscard]] ScriptBudget Budget() const override { return m_budget; }

            [[nodiscard]] ScriptCapabilities Capabilities() const override { return ScriptCapabilities::Bytecode; }

        private:
            DotNetRuntime* m_runtime;
            ScriptBudget m_budget;
        };
    }

    DotNetRuntime* ProcessRuntime(IAllocator& allocator)
    {
        static DotNetRuntime runtime;
        static bool tried = false;
        if (!runtime.IsStarted() && !tried)
        {
            tried = true;
            if (!runtime.Start(allocator).IsOk())
                LOG_ERROR(u8"Script", u8"C# is unavailable: {}", runtime.Failure());
        }
        return runtime.IsStarted() ? &runtime : nullptr;
    }

    RefPtr<IScriptManager> CreateDotNetScriptManager(IAllocator& allocator)
    {
        DotNetRuntime* runtime = ProcessRuntime(allocator);
        if (runtime != nullptr)
        {
            // C# handlers crossing in become the engine's script delegates, on this runtime.
            s_handlerRuntime = runtime;
            s_handlerAllocator = &allocator;
            SetCallbackMaker(&MakeHandler);
        }
        return RefPtr<IScriptManager>(MakeRef<DotNetScriptManager>(allocator, runtime).Get());
    }

    Array<ScriptStackFrame> ParseCSharpStack(StringView trace)
    {
        Array<ScriptStackFrame> frames;
        usize lineStart = 0;
        while (lineStart < trace.Size())
        {
            usize lineEnd = trace.Find(u8'\n', lineStart);
            if (lineEnd == StringView::kNotFound)
                lineEnd = trace.Size();
            StringView line = trace.SubStr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            while (!line.IsEmpty() && (line[0] == u8' ' || line[0] == u8'\t'))
                line = line.SubStr(1, line.Size() - 1);
            while (!line.IsEmpty() && line[line.Size() - 1] == u8'\r')
                line = line.SubStr(0, line.Size() - 1);
            if (!line.StartsWith(u8"at "))
                continue;
            ScriptStackFrame frame;
            const StringView in = u8" in ";
            const StringView lineMarker = u8":line ";
            const usize inAt = line.Find(in);
            const usize lineAt = inAt == StringView::kNotFound ? StringView::kNotFound : line.Find(lineMarker, inAt);
            frame.function = String(line.SubStr(3, (inAt == StringView::kNotFound ? line.Size() : inAt) - 3));
            if (lineAt != StringView::kNotFound)
            {
                String file(line.SubStr(inAt + in.Size(), lineAt - inAt - in.Size()));
                for (usize i = 0; i < file.Size(); ++i)
                    if (file[i] == u8'\\')
                        file[i] = u8'/';
                // The build's copy: <temp>/csharp-build-<hash>/src/<source path>.
                const usize build = file.AsView().Find(kCSharpBuildFolderPrefix);
                const usize src = build == StringView::kNotFound ? StringView::kNotFound : file.AsView().Find(StringView(u8"/src/"), build);
                frame.file = src == StringView::kNotFound ? Move(file) : String(file.AsView().SubStr(src + 5, file.Size() - src - 5));
                i32 value = 0;
                for (usize i = lineAt + lineMarker.Size(); i < line.Size() && line[i] >= u8'0' && line[i] <= u8'9'; ++i)
                    value = value * 10 + (line[i] - u8'0');
                frame.line = value > 0 ? value : -1;
            }
            frames.PushBack(Move(frame));
        }
        return frames;
    }

    void RegisterDotNetScriptBackend()
    {
        ScriptBackendDesc desc;
        desc.languageId = String(kCSharpLanguage);
        desc.displayName = String(u8"C#");
        desc.fileExtensions.PushBack(String(u8"cs"));
        // The C# extension reads the project file beside the scripts (Code/<name>.csproj).
        desc.vscodeExtension = String(u8"ms-dotnettools.csharp");
        desc.create = [](IAllocator& allocator) { return CreateDotNetScriptManager(allocator); };
        ScriptBackendRegistry::Get().Register(Move(desc));
    }
}
