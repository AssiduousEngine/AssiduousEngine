// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :behaviors_source partition, its body. See BehaviorsSource.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.script.resource;
import engine.behavior;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :edit;
import :entity_ref_editor;
import :behaviors_source;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace script = foundation::script;

namespace editor
{
    namespace
    {
        constexpr StringView kList = u8"behaviors";
        constexpr const char8_t* kParts[] = {u8"x", u8"y", u8"z"};

        String GuidText(const Guid& id)
        {
            if (id.IsNil())
                return String{};
            utf8char chars[37];
            id.ToChars(chars);
            return String(StringView(chars, 36));
        }
        String RowOf(const script::BehaviorProperty& property) { return Format(u8"p{}", property.hash); }
    }

    void BehaviorPropertyRows::Describe(const script::BehaviorClass& type, Array<gui::PropertyInfo>& out)
    {
        for (const script::BehaviorProperty& property : type.properties)
        {
            gui::PropertyInfo info;
            info.Name = RowOf(property);
            info.DisplayName = property.name;
            info.Description = property.description;
            switch (property.type)
            {
            case script::BehaviorPropertyType::Float: info.Kind = gui::PropertyKind::Float; break;
            case script::BehaviorPropertyType::Int: info.Kind = gui::PropertyKind::Integer; break;
            case script::BehaviorPropertyType::Bool: info.Kind = gui::PropertyKind::Bool; break;
            case script::BehaviorPropertyType::String: info.Kind = gui::PropertyKind::String; break;
            case script::BehaviorPropertyType::Color: info.Kind = gui::PropertyKind::Color; break;
            case script::BehaviorPropertyType::Vec3:
                info.Kind = gui::PropertyKind::Group;
                info.Hint = String(u8"Float3");
                break;
            case script::BehaviorPropertyType::Entity:
                info.Kind = gui::PropertyKind::Reference;
                info.Hint = String(EntityRefEditor::kHint);
                info.ReferenceType = &TypeOf<foundation::scene::EntityRef>();
                break;
            case script::BehaviorPropertyType::Asset:
                info.Kind = gui::PropertyKind::Reference;
                info.ReferenceType = AssetType(property.assetType.AsView());
                break;
            default: continue;
            }
            out.PushBack(Move(info));
        }
    }

    const TypeInfo* BehaviorPropertyRows::AssetType(StringView typeName)
    {
        // By its own name (`PrefabResource`) or its full one (`rtti::scene::PrefabResource`): a
        // script author writes the short name, and the full one tells apart two that share it.
        static constexpr StringView kSeparator = u8"::";
        for (const TypeInfo* type : GlobalTypeRegistry().All())
        {
            if (type == nullptr || type->name == nullptr)
                continue;
            const StringView name(reinterpret_cast<const utf8char*>(type->name));
            if (typeName == name)
                return type;
            if (type->namespaceName == nullptr)
                continue;
            const StringView space(reinterpret_cast<const utf8char*>(type->namespaceName));
            if (typeName.Size() == space.Size() + kSeparator.Size() + name.Size() && typeName.StartsWith(space)
                && typeName.SubStr(space.Size(), kSeparator.Size()) == kSeparator && typeName.EndsWith(name))
            {
                return type;
            }
        }
        return nullptr;
    }

    void BehaviorPropertyRows::Parts(const script::BehaviorProperty& property, Array<gui::PropertyInfo>& out)
    {
        if (property.type != script::BehaviorPropertyType::Vec3)
            return;
        for (const char8_t* part : kParts)
        {
            gui::PropertyInfo info;
            info.Name = String(part);
            info.Kind = gui::PropertyKind::Float;
            out.PushBack(Move(info));
        }
    }

    const script::BehaviorProperty* BehaviorPropertyRows::Find(const script::BehaviorClass& type, StringView row)
    {
        for (const script::BehaviorProperty& property : type.properties)
            if (RowOf(property).AsView() == row)
                return &property;
        return nullptr;
    }

    void BehaviorPropertyRows::ParseRow(StringView rest, String& row, i32& part)
    {
        usize dot = 0;
        while (dot < rest.Size() && rest[dot] != u8'.')
            ++dot;
        row = String(rest.SubStr(0, dot));
        part = -1;
        if (dot < rest.Size())
        {
            const StringView name = rest.SubStr(dot + 1, rest.Size() - dot - 1);
            for (i32 i = 0; i < 3; ++i)
                if (name == StringView(kParts[i]))
                    part = i;
        }
    }

    gui::Variant BehaviorPropertyRows::ToGrid(const script::BehaviorPropertyValue& value, script::BehaviorPropertyType type, i32 part)
    {
        switch (type)
        {
        case script::BehaviorPropertyType::Float: return gui::Variant(value.number);
        case script::BehaviorPropertyType::Int: return gui::Variant(static_cast<i64>(value.number));
        case script::BehaviorPropertyType::Bool: return gui::Variant(value.boolean);
        case script::BehaviorPropertyType::String: return gui::Variant(value.text.AsView());
        case script::BehaviorPropertyType::Color: return gui::Variant(value.color);
        case script::BehaviorPropertyType::Vec3:
            return part == 0 ? gui::Variant(static_cast<f64>(value.vector.x)) : part == 1 ? gui::Variant(static_cast<f64>(value.vector.y))
                             : part == 2 ? gui::Variant(static_cast<f64>(value.vector.z)) : gui::Variant{};
        case script::BehaviorPropertyType::Entity:
        case script::BehaviorPropertyType::Asset: return gui::Variant(GuidText(value.guid).AsView());
        default: return gui::Variant{};
        }
    }

    script::BehaviorPropertyValue BehaviorPropertyRows::FromGrid(script::BehaviorPropertyValue old, script::BehaviorPropertyType type, i32 part, const gui::Variant& write)
    {
        old.kind = type;
        switch (type)
        {
        case script::BehaviorPropertyType::Float: old.number = write.AsFloat(); break;
        case script::BehaviorPropertyType::Int: old.number = static_cast<f64>(write.AsInt()); break;
        case script::BehaviorPropertyType::Bool: old.boolean = write.AsBool(); break;
        case script::BehaviorPropertyType::String: old.text = String(write.AsString()); break;
        case script::BehaviorPropertyType::Color: old.color = write.AsColor(); break;
        case script::BehaviorPropertyType::Vec3:
            if (part == 0)
                old.vector.x = static_cast<f32>(write.AsFloat());
            else if (part == 1)
                old.vector.y = static_cast<f32>(write.AsFloat());
            else if (part == 2)
                old.vector.z = static_cast<f32>(write.AsFloat());
            break;
        case script::BehaviorPropertyType::Entity:
        case script::BehaviorPropertyType::Asset:
        {
            Guid id;
            (void)Guid::TryParse(Trim(write.AsString()), id);
            old.guid = id;
            break;
        }
        default: break;
        }
        return old;
    }

    BehaviorsSource::BehaviorsSource(SceneEditContext& edit, EditorContext* context, Span<const Guid> entities)
        : m_edit(&edit), m_context(context)
    {
        for (const Guid& id : entities)
            m_entities.PushBack(id);
        m_signature = Signature();
    }

    void BehaviorsSource::Refresh()
    {
        // Called every frame while the entities are selected: the grid hears of it only when the
        // components' values moved.
        const bool moved = m_shown.Take(
            [this](ISerializer& ar)
            {
                for (usize o = 0; o < m_entities.Size(); ++o)
                    if (engine::behavior::BehaviorComponent* component = Component(o))
                        engine::behavior::Serialize(ar, *component);
            });
        const u64 signature = Signature();
        if (signature != m_signature)
        {
            m_signature = signature;
            NotifyPropertiesChanged();
        }
        else if (moved)
            NotifyValuesChanged();
    }

    engine::behavior::BehaviorComponent* BehaviorsSource::Component(usize object) const
    {
        if (object >= m_entities.Size())
            return nullptr;
        auto* manager = m_edit->Scene().GetSystem<engine::behavior::BehaviorComponentManager>();
        const foundation::scene::EntityHandle e = m_edit->Resolve(m_entities[object]);
        return manager != nullptr && e.IsAssigned() ? manager->Edit(e) : nullptr;
    }

    // The fewest any entity has, since a list shows what every object holds.
    usize BehaviorsSource::BehaviorCount() const
    {
        usize count = ~usize{0};
        for (usize o = 0; o < m_entities.Size(); ++o)
        {
            const engine::behavior::BehaviorComponent* component = Component(o);
            count = Min(count, component != nullptr ? component->behaviors.Size() : usize{0});
        }
        return count == ~usize{0} ? 0u : count;
    }

    const script::BehaviorClass* BehaviorsSource::ClassOf(const engine::behavior::AttachedBehavior& behavior) const
    {
        if (behavior.behaviorClass.Get() != nullptr)
            return behavior.behaviorClass.Get();
        if (behavior.behaviorClass.id.IsNil() || m_context == nullptr || m_context->Resources() == nullptr)
            return nullptr;
        return m_context->Resources()->Bind<script::BehaviorClass>(behavior.behaviorClass.id).Get();
    }

    const script::BehaviorProperty* BehaviorsSource::PropertyOf(usize behavior, StringView row) const
    {
        const engine::behavior::BehaviorComponent* component = Component(0);
        if (component == nullptr || behavior >= component->behaviors.Size())
            return nullptr;
        const script::BehaviorClass* type = ClassOf(component->behaviors[behavior]);
        return type != nullptr ? BehaviorPropertyRows::Find(*type, row) : nullptr;
    }

    u64 BehaviorsSource::Signature() const
    {
        u64 hash = 1469598103934665603ull;
        for (usize o = 0; o < m_entities.Size(); ++o)
            if (const engine::behavior::BehaviorComponent* component = Component(o))
            {
                hash = (hash ^ component->behaviors.Size()) * 1099511628211ull;
                for (const engine::behavior::AttachedBehavior& behavior : component->behaviors)
                {
                    hash = (hash ^ Hash<Guid>{}(behavior.behaviorClass.id)) * 1099511628211ull;
                    hash = (hash ^ (ClassOf(behavior) != nullptr ? 1u : 0u)) * 1099511628211ull;
                }
            }
        return hash;
    }

    BehaviorsSource::Where BehaviorsSource::Parse(StringView path)
    {
        Where where;
        if (!path.StartsWith(kList))
            return where;
        StringView rest = path.SubStr(kList.Size(), path.Size() - kList.Size());
        if (rest.IsEmpty())
        {
            where.list = true;
            return where;
        }
        if (rest[0] != u8'.')
            return where;
        rest = rest.SubStr(1, rest.Size() - 1);
        usize at = 0;
        i64 index = 0;
        while (at < rest.Size() && rest[at] >= u8'0' && rest[at] <= u8'9')
            index = index * 10 + (rest[at++] - u8'0');
        if (at == 0)
            return where;
        where.behavior = index;
        if (at == rest.Size())
            return where;
        rest = rest.SubStr(at + 1, rest.Size() - at - 1);
        BehaviorPropertyRows::ParseRow(rest, where.row, where.part);
        return where;
    }

    void BehaviorsSource::GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const
    {
        if (parent.IsEmpty())
        {
            gui::PropertyInfo list;
            list.Name = String(kList);
            list.DisplayName = String(u8"Behaviors");
            list.Kind = gui::PropertyKind::List;
            list.ListOps = gui::PropertyListAdd | gui::PropertyListRemove | gui::PropertyListMove;
            out.PushBack(Move(list));
            return;
        }
        const Where where = Parse(parent);
        const engine::behavior::BehaviorComponent* first = Component(0);
        if (where.list)
        {
            const usize count = BehaviorCount();
            for (usize i = 0; i < count; ++i)
            {
                gui::PropertyInfo behavior;
                behavior.Name = Format(u8"{}", i);
                behavior.Kind = gui::PropertyKind::Group;
                const script::BehaviorClass* type = first != nullptr ? ClassOf(first->behaviors[i]) : nullptr;
                const bool none = first == nullptr || first->behaviors[i].behaviorClass.id.IsNil();
                behavior.DisplayName = none ? Format(u8"Behavior {}", i + 1)
                                            : type != nullptr ? Format(u8"Behavior {}: {}", i + 1, type->className)
                                                              : Format(u8"Behavior {}: (not cooked)", i + 1);
                out.PushBack(Move(behavior));
            }
            return;
        }
        if (where.behavior < 0 || first == nullptr || static_cast<usize>(where.behavior) >= first->behaviors.Size())
            return;
        if (!where.row.IsEmpty())
        {
            // A vector property's parts.
            const script::BehaviorProperty* property = PropertyOf(static_cast<usize>(where.behavior), where.row.AsView());
            if (property != nullptr && where.part < 0)
                BehaviorPropertyRows::Parts(*property, out);
            return;
        }
        gui::PropertyInfo script;
        script.Name = String(u8"class");
        script.DisplayName = String(u8"Class");
        script.Kind = gui::PropertyKind::Reference;
        script.ReferenceType = &script::BehaviorClass::StaticType(); // its registered type: TypeOf names an unregistered one "<value>"
        out.PushBack(Move(script));
        gui::PropertyInfo enabled;
        enabled.Name = String(u8"enabled");
        enabled.DisplayName = String(u8"Enabled");
        enabled.Kind = gui::PropertyKind::Bool;
        out.PushBack(Move(enabled));
        gui::PropertyInfo interval;
        interval.Name = String(u8"interval");
        interval.DisplayName = String(u8"Update Interval");
        interval.Description = String(u8"Seconds between onUpdate calls (0: every frame)");
        interval.Kind = gui::PropertyKind::Float;
        interval.HasRange = true;
        interval.Min = 0.0;
        interval.Max = 3600.0;
        interval.Step = 0.05;
        out.PushBack(Move(interval));
        if (const script::BehaviorClass* type = ClassOf(first->behaviors[static_cast<usize>(where.behavior)]))
            BehaviorPropertyRows::Describe(*type, out);
    }

    gui::Variant BehaviorsSource::GetValue(StringView path, usize object) const
    {
        const Where where = Parse(path);
        if (where.list)
            return gui::Variant(static_cast<i64>(BehaviorCount()));
        const engine::behavior::BehaviorComponent* component = Component(object);
        if (component == nullptr || where.behavior < 0 || static_cast<usize>(where.behavior) >= component->behaviors.Size() || where.row.IsEmpty())
            return gui::Variant{};
        const engine::behavior::AttachedBehavior& behavior = component->behaviors[static_cast<usize>(where.behavior)];
        if (where.row == StringView(u8"class"))
            return gui::Variant(GuidText(behavior.behaviorClass.id).AsView());
        if (where.row == StringView(u8"enabled"))
            return gui::Variant(behavior.enabled);
        if (where.row == StringView(u8"interval"))
            return gui::Variant(static_cast<f64>(behavior.updateInterval));
        const script::BehaviorProperty* property = PropertyOf(static_cast<usize>(where.behavior), where.row.AsView());
        if (property == nullptr)
            return gui::Variant{};
        const engine::behavior::BehaviorPropertyOverride* over = behavior.FindOverride(property->hash);
        return BehaviorPropertyRows::ToGrid(over != nullptr ? over->value : property->defaultValue, property->type, where.part);
    }

    gui::Variant BehaviorsSource::GetDefault(StringView path, usize object) const
    {
        (void)object;
        const Where where = Parse(path);
        if (where.behavior < 0 || where.row.IsEmpty())
            return gui::Variant{};
        if (where.row == StringView(u8"enabled"))
            return gui::Variant(true);
        if (where.row == StringView(u8"interval"))
            return gui::Variant(0.0);
        if (where.row == StringView(u8"class"))
            return gui::Variant{};
        const script::BehaviorProperty* property = PropertyOf(static_cast<usize>(where.behavior), where.row.AsView());
        return property != nullptr ? BehaviorPropertyRows::ToGrid(property->defaultValue, property->type, where.part) : gui::Variant{};
    }

    void BehaviorsSource::Mutate(StringView words, const Function<void(engine::behavior::BehaviorComponent&)>& mutate)
    {
        const String group = Format(u8"inspector.behaviors.{}", ++m_groups); // never coalesced with the last
        m_edit->Commands().BeginGroup(group.AsView());
        for (const Guid& entity : m_entities)
            (void)m_edit->MutateComponent<engine::behavior::BehaviorComponent>(entity, mutate);
        m_edit->Commands().EndGroup();
        (void)words;
        Refresh();
    }

    void BehaviorsSource::CommitEdit(StringView path, const gui::PropertyWrite& write)
    {
        const Where where = Parse(path);
        if (where.behavior < 0 || where.row.IsEmpty())
            return;
        const usize index = static_cast<usize>(where.behavior);
        if (where.row == StringView(u8"class"))
        {
            Guid id;
            (void)Guid::TryParse(Trim(write.Value.AsString()), id);
            Mutate(u8"class", [index, id](engine::behavior::BehaviorComponent& c)
                   {
                       if (index >= c.behaviors.Size())
                           return;
                       c.behaviors[index].behaviorClass = foundation::resource::Ref<script::BehaviorClass>{};
                       c.behaviors[index].behaviorClass.SetId(id);
                       c.behaviors[index].overrides.Clear(); // another script's properties
                   });
            return;
        }
        if (where.row == StringView(u8"enabled"))
        {
            const bool enabled = write.Value.AsBool();
            Mutate(u8"enabled", [index, enabled](engine::behavior::BehaviorComponent& c)
                   {
                       if (index < c.behaviors.Size())
                           c.behaviors[index].enabled = enabled;
                   });
            return;
        }
        if (where.row == StringView(u8"interval"))
        {
            const f32 interval = static_cast<f32>(Max(0.0, write.Value.AsFloat()));
            Mutate(u8"interval", [index, interval](engine::behavior::BehaviorComponent& c)
                   {
                       if (index < c.behaviors.Size())
                           c.behaviors[index].updateInterval = interval;
                   });
            return;
        }
        const script::BehaviorProperty* property = PropertyOf(index, where.row.AsView());
        if (property == nullptr)
            return;
        const script::BehaviorProperty desc = *property;
        const i32 part = where.part;
        const gui::Variant value = write.Value;
        Mutate(desc.name.AsView(), [index, desc, part, value](engine::behavior::BehaviorComponent& c)
               {
                   if (index >= c.behaviors.Size())
                       return;
                   engine::behavior::AttachedBehavior& behavior = c.behaviors[index];
                   const engine::behavior::BehaviorPropertyOverride* over = behavior.FindOverride(desc.hash);
                   behavior.SetOverride(desc.hash, BehaviorPropertyRows::FromGrid(over != nullptr ? over->value : desc.defaultValue, desc.type, part, value));
               });
    }

    bool BehaviorsSource::EditList(StringView path, const gui::ListEdit& edit)
    {
        if (!Parse(path).list)
            return false;
        const gui::ListEdit change = edit;
        Mutate(u8"behaviors", [change](engine::behavior::BehaviorComponent& c)
               {
                   switch (change.Kind)
                   {
                   case gui::ListEdit::Op::Insert:
                       c.behaviors.Insert(Min(change.Index, c.behaviors.Size()), engine::behavior::AttachedBehavior{});
                       break;
                   case gui::ListEdit::Op::Remove:
                       if (change.Index < c.behaviors.Size())
                           c.behaviors.RemoveAt(change.Index);
                       break;
                   case gui::ListEdit::Op::Move:
                       if (change.Index < c.behaviors.Size() && change.To < c.behaviors.Size() && change.Index != change.To)
                       {
                           engine::behavior::AttachedBehavior moved = Move(c.behaviors[change.Index]);
                           c.behaviors.RemoveAt(change.Index);
                           c.behaviors.Insert(change.To, Move(moved));
                       }
                       break;
                   }
               });
        return true;
    }

    bool BehaviorsSource::Revert(StringView path)
    {
        const Where where = Parse(path);
        if (where.behavior < 0 || where.row.IsEmpty())
            return false;
        const usize index = static_cast<usize>(where.behavior);
        const script::BehaviorProperty* property = PropertyOf(index, where.row.AsView());
        if (property == nullptr)
        {
            // Its own rows revert to their defaults.
            if (where.row == StringView(u8"enabled") || where.row == StringView(u8"interval"))
            {
                const bool enabled = where.row == StringView(u8"enabled");
                Mutate(u8"revert", [index, enabled](engine::behavior::BehaviorComponent& c)
                       {
                           if (index >= c.behaviors.Size())
                               return;
                           if (enabled)
                               c.behaviors[index].enabled = true;
                           else
                               c.behaviors[index].updateInterval = 0.0f;
                       });
                return true;
            }
            return false;
        }
        const u64 hash = property->hash;
        Mutate(u8"revert", [index, hash](engine::behavior::BehaviorComponent& c)
               {
                   if (index < c.behaviors.Size())
                       c.behaviors[index].RemoveOverride(hash);
               });
        return true;
    }
}
