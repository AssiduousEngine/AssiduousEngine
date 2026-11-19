// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Run - a run's save file and the `Save` facade.

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

using namespace foundation::core;
using namespace foundation;

namespace engine::runtime
{
    void RunSave::Open(StringView path)
    {
        m_path = String(path, *m_allocator);
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        m_changed = false;
        if (m_path.IsEmpty() || !FileExists(m_path.AsView()))
        {
            return;
        }
        settings::RegisterSaveValuesType(); // the load instantiates the section by its name
        bool read = false;
        {
            // Closed before the file might be moved aside below.
            FileStream file(m_path.AsView(), FileMode::Read);
            read = file.IsValid() && m_store->Load(file, xml::XmlSerializerFactory()).IsOk();
        }
        if (read)
        {
            return;
        }
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        Result<String> aside = SetAsideUnreadable(m_path.AsView(), *m_allocator);
        if (aside.HasValue())
        {
            LOG_WARNING(u8"Save", u8"could not read the save '{}'; it was moved to '{}', and the game starts from an empty save",
                        m_path.AsView(), aside.Value().AsView());
            return;
        }
        LOG_WARNING(u8"Save", u8"could not read the save '{}', nor move it aside; this run writes no save, so the file stays as it is",
                    m_path.AsView());
        m_path = String(*m_allocator);
    }

    bool RunSave::Flush()
    {
        if (!IsOpen())
        {
            return false;
        }
        if (!m_changed)
        {
            return true;
        }
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
        m_changed = false;
        PersistUserData(); // a browser keeps it only once pushed to the page's storage
        return true;
    }

    bool SaveFacade::has(core::String key)
    {
        RunSave* save = Resolve();
        return save != nullptr && save->Values().Has(key.AsView());
    }

    void SaveFacade::remove(core::String key)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().Remove(key.AsView()))
        {
            save->MarkChanged();
        }
    }

    void SaveFacade::clear()
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().Clear())
        {
            save->MarkChanged();
        }
    }

    i32 SaveFacade::getInt(core::String key, i32 fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetInt(key.AsView(), fallback) : fallback;
    }

    void SaveFacade::setInt(core::String key, i32 value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetInt(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    f32 SaveFacade::getFloat(core::String key, f32 fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetFloat(key.AsView(), fallback) : fallback;
    }

    void SaveFacade::setFloat(core::String key, f32 value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetFloat(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    bool SaveFacade::getBool(core::String key, bool fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetBool(key.AsView(), fallback) : fallback;
    }

    void SaveFacade::setBool(core::String key, bool value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetBool(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    core::String SaveFacade::getString(core::String key, core::String fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetText(key.AsView(), fallback.AsView()) : fallback;
    }

    void SaveFacade::setString(core::String key, core::String value)
    {
        if (RunSave* save = Resolve();
            save != nullptr && save->Values().SetText(key.AsView(), value.AsView()))
        {
            save->MarkChanged();
        }
    }

    core::Array<f32> SaveFacade::getFloats(core::String key)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetFloats(key.AsView()) : core::Array<f32>{};
    }

    void SaveFacade::setFloats(core::String key, core::Array<f32> values)
    {
        if (RunSave* save = Resolve(); save != nullptr &&
                                       save->Values().SetFloats(key.AsView(), Span<const f32>(values.Data(), values.Size())))
        {
            save->MarkChanged();
        }
    }

    bool SaveFacade::flush()
    {
        RunSave* save = Resolve();
        return save != nullptr && save->Flush();
    }

    REFLECT_MEMBERS(SaveFacade, "rtti::engine::runtime")
    {
        builder.Attribute("scriptName", "Save");
        builder.Method<&SaveFacade::has>("has", {"key"});
        builder.Method<&SaveFacade::remove>("remove", {"key"});
        builder.Method<&SaveFacade::clear>("clear");
        builder.Method<&SaveFacade::getInt>("getInt", {"key", "fallback"});
        builder.Method<&SaveFacade::setInt>("setInt", {"key", "value"});
        builder.Method<&SaveFacade::getFloat>("getFloat", {"key", "fallback"});
        builder.Method<&SaveFacade::setFloat>("setFloat", {"key", "value"});
        builder.Method<&SaveFacade::getBool>("getBool", {"key", "fallback"});
        builder.Method<&SaveFacade::setBool>("setBool", {"key", "value"});
        builder.Method<&SaveFacade::getString>("getString", {"key", "fallback"});
        builder.Method<&SaveFacade::setString>("setString", {"key", "value"});
        builder.Method<&SaveFacade::getFloats>("getFloats", {"key"});
        builder.Method<&SaveFacade::setFloats>("setFloats", {"key", "values"});
        builder.Method<&SaveFacade::flush>("flush");
        builder.Constructor();
    }

    void RegisterSaveScriptFacade()
    {
        static const bool once = []()
        {
            settings::RegisterSaveValuesType(); // reading a save file instantiates the section by name
            RegisterArrayType<f32>();           // getFloats/setFloats: a list crosses as array<float>
            GlobalTypeRegistry().Register(SaveFacade::StaticType());
            foundation::script::RegisterExtraFacadeName(u8"Save");
            return true;
        }();
        (void)once;
    }
}
