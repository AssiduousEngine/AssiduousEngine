// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Run - a run's saves (the global store, slots, slot reads), the `Saves` facade and the
// `SaveStore` a script holds.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // the Save facade reflection body

module engine.run;

import foundation.core;
import foundation.settings;
import foundation.script;
import foundation.script.facades;  // RegisterExtraFacadeName
import foundation.xml.serialization; // a save file is XML, like the user settings beside it
import foundation.input;             // the bindings section's type

using namespace foundation::core;
using namespace foundation;

namespace engine::runtime
{
    // The types of a save file's sections (its values, its saved time, the bindings), registered
    // once: a load makes each by its name.
    static void RegisterSaveSections()
    {
        static const bool once = []()
        {
            settings::RegisterSaveValuesType();
            foundation::input::RegisterInputTypes();
            return true;
        }();
        (void)once;
    }

    bool SaveFile::Open(StringView path)
    {
        m_path = String(path, *m_allocator);
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        m_changed = false;
        if (m_path.IsEmpty() || !FileExists(m_path.AsView()))
        {
            return true;
        }
        RegisterSaveSections(); // the load instantiates each section by its name
        bool read = false;
        {
            // Closed before the file might be moved aside below.
            FileStream file(m_path.AsView(), FileMode::Read);
            read = file.IsValid() && m_store->Load(file, xml::XmlSerializerFactory()).IsOk();
        }
        if (read)
        {
            return true;
        }
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        Result<String> aside = SetAsideUnreadable(m_path.AsView(), *m_allocator);
        if (aside.HasValue())
        {
            LOG_WARNING(u8"Save", u8"could not read the save '{}'; it was moved to '{}', and the game starts from an empty save",
                        m_path.AsView(), aside.Value().AsView());
            return true;
        }
        LOG_WARNING(u8"Save", u8"could not read the save '{}', nor move it aside; this run writes no save, so the file stays as it is",
                    m_path.AsView());
        m_path = String(*m_allocator);
        return false;
    }

    bool SaveFile::Read(StringView path)
    {
        m_path = String(*m_allocator); // a read names no file to write
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        m_changed = false;
        if (!FileExists(path))
        {
            return false;
        }
        RegisterSaveSections();
        FileStream file(path, FileMode::Read);
        if (file.IsValid() && m_store->Load(file, xml::XmlSerializerFactory()).IsOk())
        {
            return true;
        }
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        return false;
    }

    i64 SaveFile::SavedAt() const noexcept
    {
        const settings::SaveInfo* info = m_store->Find<settings::SaveInfo>();
        return info != nullptr ? info->savedAt : 0;
    }

    bool SaveFile::Flush(i64 now)
    {
        if (!IsOpen())
        {
            return false;
        }
        if (!m_changed)
        {
            return true;
        }
        const i64 before = SavedAt();
        m_store->Section<settings::SaveInfo>().savedAt = now;
        if (!Write())
        {
            m_store->Section<settings::SaveInfo>().savedAt = before;
            return false;
        }
        m_changed = false;
        return true;
    }

    bool SaveFile::WriteAsIs()
    {
        return IsOpen() && Write();
    }

    bool SaveFile::Write()
    {
        MemoryStream buffer(*m_allocator);
        if (!m_store->Save(buffer, xml::XmlSerializerFactory()).IsOk())
        {
            LOG_WARNING(u8"Save", u8"could not encode the save '{}'", m_path.AsView());
            return false;
        }
        (void)CreateDirectories(PathParent(m_path.AsView()));
        if (!WriteFileAtomic(m_path.AsView(), buffer.Bytes(), *m_allocator).IsOk())
        {
            LOG_WARNING(u8"Save", u8"could not write the save '{}'", m_path.AsView());
            return false;
        }
        PersistUserData(); // a browser keeps it only once pushed to the page's storage
        return true;
    }

    // ---- the run's saves ----

    namespace
    {
        constexpr StringView kGlobalFile = u8"global.save.xml";
        constexpr StringView kSlotPrefix = u8"slot-";
        constexpr StringView kSlotSuffix = u8".save.xml";
        constexpr usize kSlotNameMax = 32;

        // A slot's file read for its saved time, for the order of slots.
        struct ListedSlot
        {
            String name;
            i64 savedAt = 0;
        };
    }

    RunSaves::RunSaves(IAllocator& allocator)
        : m_allocator(&allocator), m_folder(allocator), m_global(allocator), m_slotName(allocator)
    {
        m_global.SetSerial(m_nextSerial++);
    }

    void RunSaves::Open(StringView folder)
    {
        m_slot = nullptr;
        m_slotName = String(*m_allocator);
        m_reads.Clear();
        m_folder = String(folder, *m_allocator);
        (void)m_global.Open(m_folder.IsEmpty() ? StringView{} : GlobalPath().AsView());
        m_global.SetSerial(m_nextSerial++); // a handle to the store before reaches nothing
    }

    i64 RunSaves::Now() const noexcept
    {
        return m_clock != nullptr ? m_clock() : GetUnixTime();
    }

    String RunSaves::GlobalPath() const
    {
        return PathJoin(m_folder.AsView(), kGlobalFile, *m_allocator);
    }

    String RunSaves::SlotPath(StringView folded) const
    {
        String file(kSlotPrefix, *m_allocator);
        file.Append(folded);
        file.Append(kSlotSuffix);
        return PathJoin(m_folder.AsView(), file.AsView(), *m_allocator);
    }

    bool RunSaves::IsSlotName(StringView name) noexcept
    {
        if (name.IsEmpty() || name.Size() > kSlotNameMax)
        {
            return false;
        }
        for (const utf8char c : name)
        {
            const bool letter = (c >= utf8char('a') && c <= utf8char('z')) || (c >= utf8char('A') && c <= utf8char('Z'));
            const bool digit = c >= utf8char('0') && c <= utf8char('9');
            if (!letter && !digit && c != utf8char('-') && c != utf8char('_'))
            {
                return false;
            }
        }
        return true;
    }

    String RunSaves::FoldSlotName(StringView name)
    {
        String folded(name);
        for (usize i = 0; i < folded.Size(); ++i)
        {
            if (folded[i] >= utf8char('A') && folded[i] <= utf8char('Z'))
            {
                folded[i] = static_cast<utf8char>(folded[i] - utf8char('A') + utf8char('a'));
            }
        }
        return folded;
    }

    bool RunSaves::CheckSlotName(StringView name, StringView call, String& folded) const
    {
        if (!IsSlotName(name))
        {
            LOG_WARNING(u8"Save", u8"{}: '{}' is not a slot name (1 to 32 letters, digits, '-' and '_')", call, name);
            return false;
        }
        if (m_folder.IsEmpty())
        {
            LOG_WARNING(u8"Save", u8"{}: this run keeps no saves folder, so it has no slots", call);
            return false;
        }
        folded = FoldSlotName(name);
        return true;
    }

    void RunSaves::ForgetReads(StringView folded)
    {
        for (usize i = m_reads.Size(); i > 0; --i)
        {
            if (m_reads[i - 1].name.AsView() == folded)
            {
                m_reads.RemoveAt(i - 1);
            }
        }
    }

    void RunSaves::LetGoOfSlot(bool write)
    {
        if (m_slot.Get() != nullptr && write && m_slotWrites == SlotWrites::AsItPlays)
        {
            (void)m_slot->Flush(Now());
        }
        m_slot = nullptr;
        m_slotName = String(*m_allocator);
    }

    SaveStoreHandle RunSaves::OpenSlot(StringView name, SlotWrites writes)
    {
        String folded;
        if (!CheckSlotName(name, u8"openSlot", folded))
        {
            return {};
        }
        if (m_slot.Get() != nullptr && m_slotName == folded)
        {
            m_slotWrites = writes;
            return {m_slot->Serial()};
        }
        const String path = SlotPath(folded.AsView());
        const bool made = !FileExists(path.AsView());
        UniquePtr<SaveFile> file = MakeUnique<SaveFile>(*m_allocator, *m_allocator);
        if (!file->Open(path.AsView()))
        {
            return {}; // the warning named the file; the open slot stays as it was
        }
        if (made && !file->WriteAsIs())
        {
            return {}; // a slot that cannot be made is not opened
        }
        LetGoOfSlot(true);
        file->SetSerial(m_nextSerial++);
        m_slot = Move(file);
        m_slotName = Move(folded);
        m_slotWrites = writes;
        return {m_slot->Serial()};
    }

    SaveStoreHandle RunSaves::ReadSlot(StringView name)
    {
        String folded;
        if (!CheckSlotName(name, u8"readSlot", folded))
        {
            return {};
        }
        ForgetReads(folded.AsView());
        const String path = SlotPath(folded.AsView());
        if (!FileExists(path.AsView()))
        {
            return {};
        }
        UniquePtr<SaveFile> file = MakeUnique<SaveFile>(*m_allocator, *m_allocator);
        if (!file->Read(path.AsView()))
        {
            LOG_WARNING(u8"Save", u8"could not read the slot '{}' ('{}'); it is left as it is", folded.AsView(), path.AsView());
            return {};
        }
        file->SetSerial(m_nextSerial++);
        const SaveStoreHandle handle{file->Serial()};
        m_reads.PushBack(SlotRead{Move(folded), Move(file)});
        return handle;
    }

    SaveStoreHandle RunSaves::OpenedSlot() const noexcept
    {
        return m_slot.Get() != nullptr ? SaveStoreHandle{m_slot->Serial()} : SaveStoreHandle{};
    }

    void RunSaves::CloseSlot()
    {
        LetGoOfSlot(true);
    }

    Array<String> RunSaves::Slots() const
    {
        Array<String> names;
        if (m_folder.IsEmpty())
        {
            return names;
        }
        struct Gather
        {
            Array<String>* files;
        } gather{&names};
        (void)ListDirectory(
            m_folder.AsView(),
            [](void* context, StringView entry, bool isDirectory)
            {
                if (!isDirectory && entry.StartsWith(kSlotPrefix) && entry.EndsWith(kSlotSuffix) &&
                    entry.Size() > kSlotPrefix.Size() + kSlotSuffix.Size())
                {
                    static_cast<Gather*>(context)->files->PushBack(
                        String(entry.SubStr(kSlotPrefix.Size(), entry.Size() - kSlotPrefix.Size() - kSlotSuffix.Size())));
                }
            },
            &gather);
        Array<ListedSlot> listed;
        for (const String& name : names)
        {
            if (!IsSlotName(name.AsView()) || FoldSlotName(name.AsView()) != name)
            {
                continue; // not a file this engine wrote
            }
            SaveFile file(*m_allocator);
            if (!file.Read(SlotPath(name.AsView()).AsView()))
            {
                LOG_WARNING(u8"Save", u8"could not read the slot '{}'; it is left out of the slots, and left as it is", name.AsView());
                continue;
            }
            listed.PushBack(ListedSlot{name, file.SavedAt()});
        }
        // The one saved last first; never saved (0) last; equal times by name.
        for (usize i = 1; i < listed.Size(); ++i)
        {
            for (usize j = i; j > 0; --j)
            {
                const ListedSlot& a = listed[j - 1];
                const ListedSlot& b = listed[j];
                const bool before = b.savedAt > a.savedAt || (b.savedAt == a.savedAt && b.name.AsView().Compare(a.name.AsView()) < 0);
                if (!before)
                {
                    break;
                }
                Swap(listed[j - 1], listed[j]);
            }
        }
        Array<String> ordered;
        ordered.Reserve(listed.Size());
        for (ListedSlot& slot : listed)
        {
            ordered.PushBack(Move(slot.name));
        }
        return ordered;
    }

    bool RunSaves::HasSlot(StringView name) const
    {
        String folded;
        return CheckSlotName(name, u8"hasSlot", folded) && FileExists(SlotPath(folded.AsView()).AsView());
    }

    bool RunSaves::DeleteSlot(StringView name)
    {
        String folded;
        if (!CheckSlotName(name, u8"deleteSlot", folded))
        {
            return false;
        }
        if (m_slot.Get() != nullptr && m_slotName == folded)
        {
            LetGoOfSlot(false);
        }
        ForgetReads(folded.AsView());
        const bool deleted = FileDelete(SlotPath(folded.AsView()).AsView());
        if (deleted)
        {
            PersistUserData();
        }
        return deleted;
    }

    bool RunSaves::CopySlot(StringView from, StringView to)
    {
        String source;
        String target;
        if (!CheckSlotName(from, u8"copySlot", source) || !CheckSlotName(to, u8"copySlot", target))
        {
            return false;
        }
        if (m_slot.Get() != nullptr && m_slotName == target)
        {
            LOG_WARNING(u8"Save", u8"copySlot: '{}' is the open slot, and its next write would undo the copy; close it first", target.AsView());
            return false;
        }
        if (source == target)
        {
            return HasSlot(source.AsView());
        }
        if (m_slot.Get() != nullptr && m_slotName == source && m_slotWrites == SlotWrites::AsItPlays)
        {
            (void)m_slot->Flush(Now());
        }
        Result<Array<byte>> bytes = ReadFile(SlotPath(source.AsView()).AsView());
        if (!bytes.HasValue())
        {
            return false;
        }
        ForgetReads(target.AsView());
        if (!WriteFileAtomic(SlotPath(target.AsView()).AsView(), Span<const byte>(bytes.Value().Data(), bytes.Value().Size()), *m_allocator)
                 .IsOk())
        {
            LOG_WARNING(u8"Save", u8"copySlot: could not write the slot '{}'", target.AsView());
            return false;
        }
        PersistUserData();
        return true;
    }

    i64 RunSaves::SlotTime(StringView name) const
    {
        String folded;
        if (!CheckSlotName(name, u8"slotTime", folded))
        {
            return 0;
        }
        if (m_slot.Get() != nullptr && m_slotName == folded)
        {
            return m_slot->SavedAt(); // as last written
        }
        SaveFile file(*m_allocator);
        return file.Read(SlotPath(folded.AsView()).AsView()) ? file.SavedAt() : 0;
    }

    SaveFile* RunSaves::Resolve(SaveStoreHandle handle) noexcept
    {
        if (!handle.IsSet())
        {
            return nullptr;
        }
        if (handle.serial == m_global.Serial())
        {
            return &m_global;
        }
        if (m_slot.Get() != nullptr && handle.serial == m_slot->Serial())
        {
            return m_slot.Get();
        }
        for (SlotRead& read : m_reads)
        {
            if (handle.serial == read.file->Serial())
            {
                return read.file.Get();
            }
        }
        return nullptr;
    }

    bool RunSaves::IsReadOnly(SaveStoreHandle handle) const noexcept
    {
        for (const SlotRead& read : m_reads)
        {
            if (handle.serial == read.file->Serial())
            {
                return true;
            }
        }
        return false;
    }

    bool RunSaves::Flush()
    {
        const i64 now = Now();
        bool written = !m_global.IsOpen() || m_global.Flush(now);
        if (m_slot.Get() != nullptr && m_slotWrites == SlotWrites::AsItPlays)
        {
            written = m_slot->Flush(now) && written;
        }
        return written;
    }

    bool RunSaves::Flush(SaveStoreHandle handle)
    {
        if (IsReadOnly(handle))
        {
            return false;
        }
        SaveFile* file = Resolve(handle);
        return file != nullptr && file->Flush(Now());
    }

    String RunSaves::NameOf(SaveStoreHandle handle) const
    {
        if (m_slot.Get() != nullptr && handle.serial == m_slot->Serial())
        {
            return m_slotName;
        }
        for (const SlotRead& read : m_reads)
        {
            if (handle.serial == read.file->Serial())
            {
                return read.name;
            }
        }
        return String(*m_allocator);
    }

    void RunSaves::WarnStale()
    {
        if (!m_warnedStale)
        {
            m_warnedStale = true;
            LOG_WARNING(u8"Save", u8"a save store was used after it was closed, deleted or replaced (or was never opened): "
                                  u8"its reads give their fallbacks and its writes go nowhere");
        }
    }

    void RunSaves::WarnReadOnly()
    {
        if (!m_warnedReadOnly)
        {
            m_warnedReadOnly = true;
            LOG_WARNING(u8"Save", u8"a slot read (Saves.readSlot) is read only: write to the slot opened with Saves.openSlot");
        }
    }

    // ---- the script's store ----

    namespace
    {
        // The store a script's handle reaches, through the calling script's run's saves; null (and a
        // warning once) when it is gone. `write`: refused for a slot read.
        SaveFile* StoreOf(SaveStoreHandle handle, bool write)
        {
            RunSaves* saves = SavesFacade::Resolve();
            if (saves == nullptr)
            {
                return nullptr; // no run (an editor tool): every fallback
            }
            SaveFile* file = saves->Resolve(handle);
            if (file == nullptr)
            {
                saves->WarnStale();
                return nullptr;
            }
            if (write && saves->IsReadOnly(handle))
            {
                saves->WarnReadOnly();
                return nullptr;
            }
            return file;
        }

        void Changed(SaveFile* file, bool changed)
        {
            if (file != nullptr && changed)
            {
                file->MarkChanged();
            }
        }
    }

    bool SaveStoreHandle::isValid() const
    {
        RunSaves* saves = SavesFacade::Resolve();
        return saves != nullptr && saves->Resolve(*this) != nullptr;
    }

    core::String SaveStoreHandle::name() const
    {
        RunSaves* saves = SavesFacade::Resolve();
        return saves != nullptr ? saves->NameOf(*this) : core::String{};
    }

    bool SaveStoreHandle::has(core::String key) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr && file->Values().Has(key.AsView());
    }

    void SaveStoreHandle::remove(core::String key) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().Remove(key.AsView()));
    }

    void SaveStoreHandle::clear() const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().Clear());
    }

    i32 SaveStoreHandle::getInt(core::String key, i32 fallback) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr ? file->Values().GetInt(key.AsView(), fallback) : fallback;
    }

    void SaveStoreHandle::setInt(core::String key, i32 value) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().SetInt(key.AsView(), value));
    }

    f32 SaveStoreHandle::getFloat(core::String key, f32 fallback) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr ? file->Values().GetFloat(key.AsView(), fallback) : fallback;
    }

    void SaveStoreHandle::setFloat(core::String key, f32 value) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().SetFloat(key.AsView(), value));
    }

    bool SaveStoreHandle::getBool(core::String key, bool fallback) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr ? file->Values().GetBool(key.AsView(), fallback) : fallback;
    }

    void SaveStoreHandle::setBool(core::String key, bool value) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().SetBool(key.AsView(), value));
    }

    core::String SaveStoreHandle::getString(core::String key, core::String fallback) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr ? file->Values().GetText(key.AsView(), fallback.AsView()) : fallback;
    }

    void SaveStoreHandle::setString(core::String key, core::String value) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().SetText(key.AsView(), value.AsView()));
    }

    core::Array<f32> SaveStoreHandle::getFloats(core::String key) const
    {
        SaveFile* file = StoreOf(*this, false);
        return file != nullptr ? file->Values().GetFloats(key.AsView()) : core::Array<f32>{};
    }

    void SaveStoreHandle::setFloats(core::String key, core::Array<f32> values) const
    {
        SaveFile* file = StoreOf(*this, true);
        Changed(file, file != nullptr && file->Values().SetFloats(key.AsView(), Span<const f32>(values.Data(), values.Size())));
    }

    bool SaveStoreHandle::flush() const
    {
        RunSaves* saves = SavesFacade::Resolve();
        if (saves == nullptr)
        {
            return false;
        }
        if (saves->Resolve(*this) == nullptr)
        {
            saves->WarnStale();
            return false;
        }
        return saves->Flush(*this);
    }

    // ---- the Saves facade ----

    SaveStoreHandle SavesFacade::global()
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? saves->GlobalHandle() : SaveStoreHandle{};
    }

    SaveStoreHandle SavesFacade::openSlot(core::String name)
    {
        return openSlot(Move(name), SlotWrites::AsItPlays);
    }

    SaveStoreHandle SavesFacade::openSlot(core::String name, SlotWrites writes)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? saves->OpenSlot(name.AsView(), writes) : SaveStoreHandle{};
    }

    SaveStoreHandle SavesFacade::readSlot(core::String name)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? saves->ReadSlot(name.AsView()) : SaveStoreHandle{};
    }

    SaveStoreHandle SavesFacade::openedSlot()
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? saves->OpenedSlot() : SaveStoreHandle{};
    }

    void SavesFacade::closeSlot()
    {
        if (RunSaves* saves = Resolve())
        {
            saves->CloseSlot();
        }
    }

    core::Array<core::String> SavesFacade::slots()
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? saves->Slots() : core::Array<core::String>{};
    }

    bool SavesFacade::hasSlot(core::String name)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr && saves->HasSlot(name.AsView());
    }

    bool SavesFacade::deleteSlot(core::String name)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr && saves->DeleteSlot(name.AsView());
    }

    bool SavesFacade::copySlot(core::String from, core::String to)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr && saves->CopySlot(from.AsView(), to.AsView());
    }

    f64 SavesFacade::slotTime(core::String name)
    {
        RunSaves* saves = Resolve();
        return saves != nullptr ? static_cast<f64>(saves->SlotTime(name.AsView())) : 0.0;
    }

    bool SavesFacade::flush()
    {
        RunSaves* saves = Resolve();
        return saves != nullptr && saves->Flush();
    }

    REFLECT_ENUM(SlotWrites, "rtti::engine::runtime")
    {
        builder.Value("AsItPlays", SlotWrites::AsItPlays).Value("AtSavePoints", SlotWrites::AtSavePoints);
    }

    REFLECT_VALUE(SaveStoreHandle, "rtti::engine::runtime")
    {
        builder.Attribute("scriptName", "SaveStore");
        builder.Method<&SaveStoreHandle::isValid>("isValid");
        builder.Method<&SaveStoreHandle::name>("name");
        builder.Method<&SaveStoreHandle::has>("has", {"key"});
        builder.Method<&SaveStoreHandle::remove>("remove", {"key"});
        builder.Method<&SaveStoreHandle::clear>("clear");
        builder.Method<&SaveStoreHandle::getInt>("getInt", {"key", "fallback"});
        builder.Method<&SaveStoreHandle::setInt>("setInt", {"key", "value"});
        builder.Method<&SaveStoreHandle::getFloat>("getFloat", {"key", "fallback"});
        builder.Method<&SaveStoreHandle::setFloat>("setFloat", {"key", "value"});
        builder.Method<&SaveStoreHandle::getBool>("getBool", {"key", "fallback"});
        builder.Method<&SaveStoreHandle::setBool>("setBool", {"key", "value"});
        builder.Method<&SaveStoreHandle::getString>("getString", {"key", "fallback"});
        builder.Method<&SaveStoreHandle::setString>("setString", {"key", "value"});
        builder.Method<&SaveStoreHandle::getFloats>("getFloats", {"key"});
        builder.Method<&SaveStoreHandle::setFloats>("setFloats", {"key", "values"});
        builder.Method<&SaveStoreHandle::flush>("flush");
        builder.Constructor(); // some backends only materialize constructible foreign values
    }

    REFLECT_MEMBERS(SavesFacade, "rtti::engine::runtime")
    {
        builder.Attribute("scriptName", "Saves");
        builder.Method<&SavesFacade::global>("global");
        builder.Method<static_cast<SaveStoreHandle (*)(core::String)>(&SavesFacade::openSlot)>("openSlot", {"name"});
        builder.Method<static_cast<SaveStoreHandle (*)(core::String, SlotWrites)>(&SavesFacade::openSlot)>("openSlot",
                                                                                                       {"name", "writes"});
        builder.Method<&SavesFacade::readSlot>("readSlot", {"name"});
        builder.Method<&SavesFacade::openedSlot>("openedSlot");
        builder.Method<&SavesFacade::closeSlot>("closeSlot");
        builder.Method<&SavesFacade::slots>("slots");
        builder.Method<&SavesFacade::hasSlot>("hasSlot", {"name"});
        builder.Method<&SavesFacade::deleteSlot>("deleteSlot", {"name"});
        builder.Method<&SavesFacade::copySlot>("copySlot", {"from", "to"});
        builder.Method<&SavesFacade::slotTime>("slotTime", {"name"});
        builder.Method<&SavesFacade::flush>("flush");
        builder.Constructor();
    }

    void RegisterSaveScriptFacade()
    {
        static const bool once = []()
        {
            RegisterSaveSections();   // reading a save file instantiates each section by name
            RegisterArrayType<f32>();           // getFloats/setFloats: a list crosses as array<float>
            RegisterArrayType<core::String>(); // slots(): the names cross as array<string>
            RttiRegisterEnum_SlotWrites();
            GlobalTypeRegistry().Register(TypeOf<SlotWrites>());
            RttiRegisterValue_SaveStoreHandle();
            GlobalTypeRegistry().Register(TypeOf<SaveStoreHandle>());
            GlobalTypeRegistry().Register(SavesFacade::StaticType());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SaveStoreHandle>());
            foundation::script::RegisterExtraFacadeName(u8"SaveStore");
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SlotWrites>());
            foundation::script::RegisterExtraFacadeName(u8"SlotWrites");
            foundation::script::RegisterExtraFacadeName(u8"Saves");
            return true;
        }();
        (void)once;
    }
}
