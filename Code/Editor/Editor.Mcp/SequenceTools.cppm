// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :sequence_tools partition
//
// sequence_read / sequence_edit: a sequence asset for an agent, whatever its track kinds. The read
// gives its parts, its markers and each track: its kind (by type name and by the name the editor
// shows), its part, its key channels with their keys, its sections, and its reflected fields. The
// edit takes a list of edits, applies them in order to the sequence as stored and writes it once;
// a refused edit refuses the call and nothing is written. Kinds are found in the type registry
// (SequenceTrackKinds), and keys and sections go through each kind's own editing calls, so a kind
// a domain or a plugin adds is read and edited with no list here. A field is read and set by its
// reflected path ("sections.0.clip", "keys.1.name", "color"), the same path the inspector edits.

module;
#include "Core/Prelude.h"

export module editor.mcp:sequence_tools;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import foundation.propertyanimation;
import foundation.sequence;
import sequence.pipeline;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;

export namespace editor::mcp
{
    /// The sequence tools, as static members: GCC cannot read back a partition whose
    /// namespace-scope inline function instantiates a template.
    struct SequenceTools
    {
        // === Reading reflected values ===

        [[nodiscard]] static StringView NameOf(const char* name)
        {
            return StringView(reinterpret_cast<const utf8char*>(name));
        }

        /// The name a kind shows in the editor (its displayName), else its type name.
        [[nodiscard]] static String KindLabel(const TypeInfo& kind)
        {
            if (const Variant* shown = FindAttribute(kind, "displayName"); shown != nullptr && shown->Is<String>())
                return shown->Get<String>();
            return String(NameOf(kind.name));
        }

        [[nodiscard]] static JsonValue VariantJson(const Variant& value)
        {
            if (value.Is<f32>())
                return JsonValue::MakeNumber(value.Get<f32>());
            if (value.Is<f64>())
                return JsonValue::MakeNumber(value.Get<f64>());
            if (value.Is<i32>())
                return JsonValue::MakeNumber(value.Get<i32>());
            if (value.Is<u32>())
                return JsonValue::MakeNumber(value.Get<u32>());
            if (value.Is<u8>())
                return JsonValue::MakeNumber(value.Get<u8>());
            if (value.Is<bool>())
                return JsonValue::MakeBool(value.Get<bool>());
            if (value.Is<String>())
                return JsonValue::MakeString(value.Get<String>());
            if (value.Is<Float3>())
            {
                const Float3 v = value.Get<Float3>();
                JsonValue out = JsonValue::MakeArray();
                out.Add(JsonValue::MakeNumber(v.x));
                out.Add(JsonValue::MakeNumber(v.y));
                out.Add(JsonValue::MakeNumber(v.z));
                return out;
            }
            if (value.Is<Color>())
            {
                const Color c = value.Get<Color>();
                JsonValue out = JsonValue::MakeArray();
                out.Add(JsonValue::MakeNumber(c.r));
                out.Add(JsonValue::MakeNumber(c.g));
                out.Add(JsonValue::MakeNumber(c.b));
                out.Add(JsonValue::MakeNumber(c.a));
                return out;
            }
            return JsonValue::MakeNull();
        }

        /// A value of `type` at `address`: a reference as its guid, an enum by its name, a list as
        /// an array, a structure as an object of its fields, a leaf as JSON.
        [[nodiscard]] static JsonValue ValueJson(const TypeInfo& type, void* address, const Variant& fallback)
        {
            if (IsReferenceType(type) && address != nullptr)
            {
                const Guid* id = type.reference->Id(address);
                return id != nullptr && !id->IsNil() ? detail::GuidToJson(*id) : JsonValue::MakeNull();
            }
            if (type.enumeratorCount > 0 && address != nullptr)
            {
                i64 raw = 0;
                MemCopy(&raw, address, Min(static_cast<usize>(type.size), sizeof(raw)));
                for (u32 i = 0; i < type.enumeratorCount; ++i)
                    if (type.enumerators[i].value == raw)
                        return JsonValue::MakeString(String(NameOf(type.enumerators[i].name)));
                return JsonValue::MakeNumber(static_cast<f64>(raw));
            }
            if (type.container != nullptr && address != nullptr)
            {
                const Instance list(address, &type);
                JsonValue out = JsonValue::MakeArray();
                const usize count = ContainerSize(*type.container, list);
                for (usize i = 0; i < count; ++i)
                {
                    if (type.container->addressAt != nullptr)
                    {
                        const Instance element = type.container->addressAt(list, i);
                        out.Add(element.IsEmpty() ? JsonValue::MakeNull() : ValueJson(*element.Type(), element.Pointer(), {}));
                    }
                    else
                        out.Add(VariantJson(type.container->getAt(list, i)));
                }
                return out;
            }
            const JsonValue leaf = VariantJson(fallback);
            if (!leaf.IsNull() || type.propertyCount == 0 || address == nullptr)
                return leaf;
            return FieldsJson(Instance(address, &type));
        }

        /// Every reflected field of `object`, its bases' first.
        [[nodiscard]] static JsonValue FieldsJson(const Instance& object)
        {
            JsonValue out = JsonValue::MakeObject();
            Array<const TypeInfo*> chain;
            for (const TypeInfo* t = object.Type(); t != nullptr; t = t->base)
                chain.PushBack(t);
            for (usize c = chain.Size(); c-- > 0;)
                for (u32 i = 0; i < chain[c]->propertyCount; ++i)
                {
                    const PropertyInfo& property = chain[c]->properties[i];
                    if (property.type == nullptr)
                        continue;
                    void* address = property.address != nullptr ? property.address(object) : nullptr;
                    const Variant value = property.get != nullptr && (property.type->container == nullptr) ? property.get(object) : Variant{};
                    out.Set(String(NameOf(property.name)), ValueJson(*property.type, address, value));
                }
            return out;
        }

        // === Writing reflected values ===

        [[nodiscard]] static Result<bool, String> WriteLeaf(const Instance& owner, const PropertyInfo& property, const JsonValue& value, StringView path)
        {
            const TypeInfo& type = *property.type;
            void* address = property.address != nullptr ? property.address(owner) : nullptr;
            if (IsReferenceType(type))
            {
                Guid id;
                if (!value.IsNull() && (!value.IsString() || !Guid::TryParse(value.AsString().AsView(), id)))
                    return Err(Format(u8"'{}' is an asset reference: give an asset guid, or null", path));
                if (address == nullptr)
                    return Err(Format(u8"'{}' cannot be written in place", path));
                type.reference->ClearBinding(address);
                type.reference->SetId(address, id);
                return true;
            }
            if (type.enumeratorCount > 0)
            {
                if (address == nullptr)
                    return Err(Format(u8"'{}' cannot be written in place", path));
                i64 raw = 0;
                bool found = false;
                if (value.IsString())
                {
                    for (u32 i = 0; i < type.enumeratorCount; ++i)
                        if (NameOf(type.enumerators[i].name) == value.AsString().AsView())
                        {
                            raw = type.enumerators[i].value;
                            found = true;
                        }
                }
                if (!found)
                {
                    String names;
                    for (u32 i = 0; i < type.enumeratorCount; ++i)
                        names.Append(Format(u8"{}{}", i == 0 ? StringView{} : StringView(u8", "), NameOf(type.enumerators[i].name)).AsView());
                    return Err(Format(u8"'{}' takes one of: {}", path, names.AsView()));
                }
                MemCopy(address, &raw, Min(static_cast<usize>(type.size), sizeof(raw)));
                return true;
            }
            Variant written;
            if (&type == &TypeOf<f32>() && value.IsNumber())
                written = Variant::From(static_cast<f32>(value.AsNumber()));
            else if (&type == &TypeOf<i32>() && value.IsNumber())
                written = Variant::From(static_cast<i32>(value.AsInt()));
            else if (&type == &TypeOf<u32>() && value.IsNumber())
                written = Variant::From(static_cast<u32>(value.AsInt()));
            else if (&type == &TypeOf<u8>() && value.IsNumber())
                written = Variant::From(static_cast<u8>(value.AsInt()));
            else if (&type == &TypeOf<bool>() && value.IsBool())
                written = Variant::From(value.AsBool());
            else if (&type == &TypeOf<String>() && value.IsString())
                written = Variant::From(value.AsString());
            else if (&type == &TypeOf<Float3>() && value.IsArray() && value.Count() == 3)
                written = Variant::From(Float3{static_cast<f32>(value.At(0).AsNumber()), static_cast<f32>(value.At(1).AsNumber()),
                                         static_cast<f32>(value.At(2).AsNumber())});
            else if (&type == &TypeOf<Color>() && value.IsArray() && (value.Count() == 3 || value.Count() == 4))
                written = Variant::From(Color{static_cast<f32>(value.At(0).AsNumber()), static_cast<f32>(value.At(1).AsNumber()),
                                        static_cast<f32>(value.At(2).AsNumber()), value.Count() == 4 ? static_cast<f32>(value.At(3).AsNumber()) : 1.0f});
            else
                return Err(Format(u8"'{}' is a {}: give it in the shape sequence_read shows", path, NameOf(type.name)));
            if (property.set == nullptr || !property.set(owner, written).IsOk())
                return Err(Format(u8"'{}' could not be set (read-only?)", path));
            return true;
        }

        /// Sets the field at `path` under `root` ("sections.0.clip"): fields by name, list
        /// elements by index.
        [[nodiscard]] static Result<bool, String> SetPath(const Instance& root, StringView path, const JsonValue& value)
        {
            Array<String> segments;
            usize start = 0;
            for (usize i = 0; i <= path.Size(); ++i)
                if (i == path.Size() || path[i] == u8'.')
                {
                    segments.PushBack(String(path.SubStr(start, i - start)));
                    start = i + 1;
                }
            Instance at = root;
            for (usize s = 0; s < segments.Size(); ++s)
            {
                const StringView segment = segments[s].AsView();
                if (at.IsEmpty() || at.Type() == nullptr)
                    return Err(Format(u8"'{}': nothing at '{}'", path, segment));
                if (const ContainerInfo* list = at.Type()->container)
                {
                    usize index = 0;
                    bool digits = !segment.IsEmpty();
                    for (const utf8char c : segment)
                    {
                        digits = digits && c >= u8'0' && c <= u8'9';
                        index = index * 10 + static_cast<usize>(c - u8'0');
                    }
                    if (!digits || index >= ContainerSize(*list, at) || list->addressAt == nullptr)
                        return Err(Format(u8"'{}': '{}' is not an element of the list (it has {})", path, segment, ContainerSize(*list, at)));
                    if (s + 1 == segments.Size())
                        return Err(Format(u8"'{}': name a field of the element", path));
                    at = list->addressAt(at, index);
                    continue;
                }
                const PropertyInfo* property = FindProperty(*at.Type(), reinterpret_cast<const char*>(segments[s].CStr()));
                if (property == nullptr || property->type == nullptr)
                    return Err(Format(u8"'{}': {} has no field '{}' (sequence_read lists them)", path, NameOf(at.Type()->name), segment));
                if (s + 1 == segments.Size())
                    return WriteLeaf(at, *property, value, path);
                void* address = property->address != nullptr ? property->address(at) : nullptr;
                if (address == nullptr)
                    return Err(Format(u8"'{}': '{}' cannot be descended into", path, segment));
                at = Instance(address, property->type);
            }
            return Err(Format(u8"'{}' names no field", path));
        }

        // === The sequence ===

        /// The kind a name gives: a registered kind's type name or the name the editor shows.
        [[nodiscard]] static const TypeInfo* FindKind(StringView name)
        {
            for (const TypeInfo* kind : sequence::SequenceTrackKinds())
                if (NameOf(kind->name) == name || KindLabel(*kind).AsView() == name)
                    return kind;
            return nullptr;
        }

        [[nodiscard]] static String KindNames()
        {
            String names;
            for (const TypeInfo* kind : sequence::SequenceTrackKinds())
                names.Append(Format(u8"{}{} ({})", names.IsEmpty() ? StringView{} : StringView(u8", "), KindLabel(*kind), NameOf(kind->name)).AsView());
            return names;
        }

        [[nodiscard]] static JsonValue TrackJson(const sequence::Sequence& sequence, usize index)
        {
            sequence::SequenceTrack& track = *sequence.tracks[index];
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"index", JsonValue::MakeNumber(static_cast<f64>(index)));
            out.Set(u8"kind", JsonValue::MakeString(String(NameOf(track.GetType()->name))));
            out.Set(u8"kindName", JsonValue::MakeString(KindLabel(*track.GetType())));
            const bool bound = track.binding >= 0 && track.binding < static_cast<i32>(sequence.bindings.Size());
            out.Set(u8"part", bound ? JsonValue::MakeString(sequence.bindings[static_cast<usize>(track.binding)].name) : JsonValue::MakeNull());
            if (const auto* property = Cast<sequence::PropertySequenceTrack>(&track))
            {
                out.Set(u8"source", JsonValue::MakeString(property->property.source));
                out.Set(u8"target", JsonValue::MakeString(property->property.target));
                out.Set(u8"property", JsonValue::MakeString(property->property.propertyPath));
            }
            JsonValue channels = JsonValue::MakeArray();
            for (usize c = 0; c < track.KeyChannelCount(); ++c)
            {
                JsonValue channel = JsonValue::MakeObject();
                channel.Set(u8"name", JsonValue::MakeString(track.KeyChannelName(c)));
                JsonValue keys = JsonValue::MakeArray();
                if (const Curve* curve = track.KeyCurve(c))
                    for (const CurveKey& key : curve->Keys())
                    {
                        JsonValue k = JsonValue::MakeObject();
                        k.Set(u8"time", JsonValue::MakeNumber(key.time));
                        k.Set(u8"value", JsonValue::MakeNumber(key.value));
                        keys.Add(Move(k));
                    }
                else
                {
                    Array<f32> times;
                    track.GetKeyTimes(c, times);
                    for (const f32 t : times)
                    {
                        JsonValue k = JsonValue::MakeObject();
                        k.Set(u8"time", JsonValue::MakeNumber(t));
                        keys.Add(Move(k));
                    }
                }
                channel.Set(u8"keys", Move(keys));
                channels.Add(Move(channel));
            }
            out.Set(u8"channels", Move(channels));
            JsonValue sections = JsonValue::MakeArray();
            for (usize i = 0; i < track.SectionCount(); ++i)
            {
                f32 start = 0.0f;
                f32 length = 0.0f;
                String label;
                track.GetSection(i, start, length, label);
                JsonValue section = JsonValue::MakeObject();
                section.Set(u8"start", JsonValue::MakeNumber(start));
                section.Set(u8"length", JsonValue::MakeNumber(length));
                section.Set(u8"label", JsonValue::MakeString(Move(label)));
                sections.Add(Move(section));
            }
            out.Set(u8"sections", Move(sections));
            out.Set(u8"fields", FieldsJson(Instance(&track, track.GetType())));
            return out;
        }

        [[nodiscard]] static JsonValue SequenceJson(const content::Instance& instance, const sequence::Sequence& sequence)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"guid", detail::GuidToJson(instance.Id()));
            out.Set(u8"name", JsonValue::MakeString(String(instance.Name())));
            out.Set(u8"length", JsonValue::MakeNumber(Max(sequence.duration, sequence.ComputeDuration())));
            out.Set(u8"frameRate", JsonValue::MakeNumber(sequence.frameRate));
            JsonValue parts = JsonValue::MakeArray();
            for (const sequence::SequenceBinding& binding : sequence.bindings)
                parts.Add(JsonValue::MakeString(binding.name));
            out.Set(u8"parts", Move(parts));
            JsonValue markers = JsonValue::MakeArray();
            for (const sequence::SequenceMarker& marker : sequence.markers)
            {
                JsonValue m = JsonValue::MakeObject();
                m.Set(u8"time", JsonValue::MakeNumber(marker.time));
                m.Set(u8"name", JsonValue::MakeString(marker.name));
                m.Set(u8"hold", JsonValue::MakeBool(marker.hold));
                markers.Add(Move(m));
            }
            out.Set(u8"markers", Move(markers));
            JsonValue tracks = JsonValue::MakeArray();
            for (usize t = 0; t < sequence.tracks.Size(); ++t)
                tracks.Add(TrackJson(sequence, t));
            out.Set(u8"tracks", Move(tracks));
            JsonValue kinds = JsonValue::MakeArray();
            for (const TypeInfo* kind : sequence::SequenceTrackKinds())
            {
                JsonValue k = JsonValue::MakeObject();
                k.Set(u8"kind", JsonValue::MakeString(String(NameOf(kind->name))));
                k.Set(u8"kindName", JsonValue::MakeString(KindLabel(*kind)));
                kinds.Add(Move(k));
            }
            out.Set(u8"kinds", Move(kinds));
            return out;
        }

        [[nodiscard]] static Result<content::Instance*, String> Resolve(const ProjectSession& session, const JsonValue& args)
        {
            if (session.project == nullptr)
                return Err(String(u8"no project is open (call project_open first)"));
            Guid id;
            if (!Guid::TryParse(args.Get(u8"guid").AsString().AsView(), id))
                return Err(String(u8"`guid` is not a valid guid"));
            content::Instance* instance = session.project->SourceDb().GetInstance(id);
            if (instance == nullptr)
                return Err(String(u8"no asset with that guid in the open project"));
            return instance;
        }

        [[nodiscard]] static Result<RefPtr<pipeline::SequenceAsset>, String> Read(content::Instance& instance)
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::SequenceAsset>(object.Get());
            if (asset == nullptr)
                return Err(Format(u8"'{}' is not a sequence (asset_list's type SequenceAsset)", instance.Name()));
            return RefPtr<pipeline::SequenceAsset>(asset);
        }

        // === One edit ===

        [[nodiscard]] static Result<sequence::SequenceTrack*, String> TrackArg(sequence::Sequence& sequence, const JsonValue& edit)
        {
            const i64 index = edit.Get(u8"track").AsInt(-1);
            if (!edit.Get(u8"track").IsNumber() || index < 0 || index >= static_cast<i64>(sequence.tracks.Size()))
                return Err(Format(u8"`track` is a track's index (0 to {})", static_cast<i64>(sequence.tracks.Size()) - 1));
            return sequence.tracks[static_cast<usize>(index)].Get();
        }

        [[nodiscard]] static Result<usize, String> ChannelArg(const sequence::SequenceTrack& track, const JsonValue& edit)
        {
            const JsonValue channel = edit.Get(u8"channel");
            if (channel.IsNull() && track.KeyChannelCount() == 1)
                return usize{0};
            if (channel.IsNumber() && channel.AsInt() >= 0 && static_cast<usize>(channel.AsInt()) < track.KeyChannelCount())
                return static_cast<usize>(channel.AsInt());
            for (usize c = 0; c < track.KeyChannelCount(); ++c)
                if (channel.IsString() && track.KeyChannelName(c).AsView() == channel.AsString().AsView())
                    return c;
            String names;
            for (usize c = 0; c < track.KeyChannelCount(); ++c)
                names.Append(Format(u8"{}{}", c == 0 ? StringView{} : StringView(u8", "), track.KeyChannelName(c)).AsView());
            return Err(track.KeyChannelCount() == 0 ? String(u8"this track holds sections, not keys")
                                                    : Format(u8"`channel` is one of: {} (or its index)", names.AsView()));
        }

        /// Applies one edit; fills `result` with what it made (an index). The reason when refused.
        [[nodiscard]] static Result<bool, String> Apply(sequence::Sequence& sequence, const JsonValue& edit, JsonValue& result)
        {
            const String op = edit.Get(u8"op").AsString();
            if (op == StringView(u8"length"))
            {
                sequence.duration = Max(0.0f, static_cast<f32>(edit.Get(u8"seconds").AsNumber()));
                return true;
            }
            if (op == StringView(u8"add_part"))
            {
                const String name = edit.Get(u8"name").AsString();
                if (name.IsEmpty() || sequence.FindBinding(name.AsView()) >= 0)
                    return Err(Format(u8"add_part needs a `name` not already a part ('{}')", name.AsView()));
                sequence.bindings.PushBack(sequence::SequenceBinding{name});
                result.Set(u8"part", JsonValue::MakeNumber(static_cast<f64>(sequence.bindings.Size() - 1)));
                return true;
            }
            if (op == StringView(u8"remove_part"))
            {
                const i32 part = sequence.FindBinding(edit.Get(u8"name").AsString().AsView());
                if (part < 0)
                    return Err(String(u8"remove_part: no part of that `name`"));
                sequence.bindings.RemoveAt(static_cast<usize>(part));
                for (RefPtr<sequence::SequenceTrack>& track : sequence.tracks)
                    track->binding = track->binding == part ? -1 : (track->binding > part ? track->binding - 1 : track->binding);
                return true;
            }
            if (op == StringView(u8"add_track"))
            {
                const TypeInfo* kind = FindKind(edit.Get(u8"kind").AsString().AsView());
                if (kind == nullptr)
                    return Err(Format(u8"add_track: `kind` is one of: {}", KindNames().AsView()));
                RefPtr<ISerializable> made = GlobalSerializableRegistry().Create(kind->id);
                RefPtr<sequence::SequenceTrack> track(Cast<sequence::SequenceTrack>(made.Get()));
                if (!track)
                    return Err(String(u8"add_track: the kind could not be made"));
                if (auto* property = Cast<sequence::PropertySequenceTrack>(track.Get()))
                {
                    const String source = edit.Get(u8"source").AsString();
                    property->property.source = source.IsEmpty() ? String(propertyanimation::kComponentSource) : source;
                    property->property.target = edit.Get(u8"target").AsString();
                    property->property.propertyPath = edit.Get(u8"property").AsString();
                    if (propertyanimation::FindPropertyTargetSource(property->property.source.AsView()) == nullptr)
                        return Err(Format(u8"add_track: no target source '{}' (component, settings, or one a plugin registers)",
                                          property->property.source));
                    if (property->property.target.IsEmpty() || property->property.propertyPath.IsEmpty())
                        return Err(String(u8"add_track: a property track needs `target` (a component's name or Transform; a "
                                          u8"settings id with source settings) and `property`"));
                    const String valueKind = edit.Get(u8"valueKind").AsString();
                    const bool transform = property->property.source.AsView() == propertyanimation::kComponentSource &&
                                           property->property.target.AsView() == propertyanimation::kTransformTarget;
                    if (valueKind == StringView(u8"Float"))
                        property->property.kind = propertyanimation::TrackValueKind::Float;
                    else if (valueKind == StringView(u8"Float3"))
                        property->property.kind = propertyanimation::TrackValueKind::Float3;
                    else if (valueKind == StringView(u8"Color"))
                        property->property.kind = propertyanimation::TrackValueKind::Color;
                    else if (valueKind == StringView(u8"Quat"))
                        property->property.kind = propertyanimation::TrackValueKind::Quat;
                    else if (transform)
                        property->property.kind = property->property.propertyPath == StringView(u8"rotation") ? propertyanimation::TrackValueKind::Quat
                                                                                                              : propertyanimation::TrackValueKind::Float3;
                    else
                        return Err(String(u8"add_track: give `valueKind` (Float, Float3, Color or Quat) for the property"));
                }
                // Whether it plays on a part is the kind's, and a property track's source's, so it is
                // asked once the source is known.
                const JsonValue partArg = edit.Get(u8"part");
                if (!partArg.IsNull())
                {
                    track->binding = sequence.FindBinding(partArg.AsString().AsView());
                    if (track->binding < 0)
                        return Err(String(u8"add_track: no part of that `part` name (add_part first)"));
                }
                else if (track->NeedsBinding())
                    return Err(Format(u8"add_track: a {} track plays on a part: give `part`", KindLabel(*kind)));
                if (!edit.Get(u8"name").IsNull())
                    track->name = edit.Get(u8"name").AsString();
                sequence.tracks.PushBack(track);
                result.Set(u8"track", JsonValue::MakeNumber(static_cast<f64>(sequence.tracks.Size() - 1)));
                return true;
            }
            if (op == StringView(u8"remove_track"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                sequence.tracks.RemoveAt(static_cast<usize>(edit.Get(u8"track").AsInt()));
                return true;
            }
            if (op == StringView(u8"key"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                Result<usize, String> channel = ChannelArg(*track.Value(), edit);
                if (!channel.HasValue())
                    return Err(Move(channel.Error()));
                const i32 index = track.Value()->InsertKey(channel.Value(), static_cast<f32>(edit.Get(u8"time").AsNumber()));
                if (index < 0)
                    return Err(String(u8"key: the channel takes no key"));
                if (edit.Get(u8"value").IsNumber())
                {
                    Curve* curve = track.Value()->KeyCurve(channel.Value());
                    if (curve == nullptr)
                        return Err(String(u8"key: this channel's keys hold no number (set its fields instead)"));
                    curve->Keys()[static_cast<usize>(index)].value = static_cast<f32>(edit.Get(u8"value").AsNumber());
                    curve->ApplyTangentModes();
                }
                result.Set(u8"key", JsonValue::MakeNumber(index));
                return true;
            }
            if (op == StringView(u8"remove_key"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                Result<usize, String> channel = ChannelArg(*track.Value(), edit);
                if (!channel.HasValue())
                    return Err(Move(channel.Error()));
                Array<f32> times;
                track.Value()->GetKeyTimes(channel.Value(), times);
                const f32 at = static_cast<f32>(edit.Get(u8"time").AsNumber());
                for (usize i = 0; i < times.Size(); ++i)
                    if (Abs(times[i] - at) < 1e-4f)
                    {
                        const usize gone[] = {i};
                        track.Value()->DeleteKeys(channel.Value(), Span<const usize>(gone, 1));
                        return true;
                    }
                return Err(Format(u8"remove_key: no key at {} on that channel", at));
            }
            if (op == StringView(u8"section"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                if (track.Value()->KeyChannelCount() != 0)
                    return Err(String(u8"section: this track holds keys, not sections"));
                const f32 start = static_cast<f32>(edit.Get(u8"start").AsNumber());
                const i32 index = track.Value()->AddSection(start);
                if (index < 0)
                    return Err(String(u8"section: this kind takes no more sections (set its fields instead)"));
                if (edit.Get(u8"length").IsNumber())
                    track.Value()->SetSection(static_cast<usize>(index), start, static_cast<f32>(edit.Get(u8"length").AsNumber()));
                const JsonValue fields = edit.Get(u8"fields");
                for (const String& key : fields.Keys())
                {
                    Result<bool, String> set = SetPath(Instance(track.Value(), track.Value()->GetType()),
                                                       Format(u8"sections.{}.{}", index, key.AsView()).AsView(), fields.Get(key));
                    if (!set.HasValue())
                        return Err(Move(set.Error()));
                }
                result.Set(u8"section", JsonValue::MakeNumber(index));
                return true;
            }
            if (op == StringView(u8"remove_section"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                const i64 index = edit.Get(u8"section").AsInt(-1);
                const usize before = track.Value()->SectionCount();
                if (index < 0 || static_cast<usize>(index) >= before)
                    return Err(Format(u8"remove_section: `section` is an index (0 to {})", static_cast<i64>(before) - 1));
                track.Value()->RemoveSection(static_cast<usize>(index));
                if (track.Value()->SectionCount() == before)
                    return Err(String(u8"remove_section: this kind keeps its section"));
                return true;
            }
            if (op == StringView(u8"marker"))
            {
                const f32 time = Max(0.0f, static_cast<f32>(edit.Get(u8"time").AsNumber()));
                usize at = 0;
                while (at < sequence.markers.Size() && sequence.markers[at].time <= time)
                    ++at;
                const String name = edit.Get(u8"name").IsString() ? edit.Get(u8"name").AsString() : String(u8"marker");
                sequence.markers.Insert(at, sequence::SequenceMarker{time, name, edit.Get(u8"hold").AsBool()});
                result.Set(u8"marker", JsonValue::MakeNumber(static_cast<f64>(at)));
                return true;
            }
            if (op == StringView(u8"remove_marker"))
            {
                const i64 index = edit.Get(u8"marker").AsInt(-1);
                if (index < 0 || index >= static_cast<i64>(sequence.markers.Size()))
                    return Err(Format(u8"remove_marker: `marker` is an index (0 to {})", static_cast<i64>(sequence.markers.Size()) - 1));
                sequence.markers.RemoveAt(static_cast<usize>(index));
                return true;
            }
            if (op == StringView(u8"set"))
            {
                Result<sequence::SequenceTrack*, String> track = TrackArg(sequence, edit);
                if (!track.HasValue())
                    return Err(Move(track.Error()));
                if (edit.Get(u8"path").AsString() == StringView(u8"part"))
                {
                    const JsonValue part = edit.Get(u8"value");
                    track.Value()->binding = part.IsNull() ? -1 : sequence.FindBinding(part.AsString().AsView());
                    if (!part.IsNull() && track.Value()->binding < 0)
                        return Err(String(u8"set part: no part of that name"));
                    return true;
                }
                return SetPath(Instance(track.Value(), track.Value()->GetType()), edit.Get(u8"path").AsString().AsView(), edit.Get(u8"value"));
            }
            return Err(Format(u8"`op` '{}' is not one of: length, add_part, remove_part, add_track, remove_track, key, remove_key, "
                              u8"section, remove_section, marker, remove_marker, set",
                              op.AsView()));
        }

        static void Register(foundation::mcp::McpServer& server, ProjectSession& session)
        {
            using foundation::mcp::SchemaBuilder;
            using foundation::mcp::ToolResult;
            ProjectSession* s = &session;

            server.RegisterTool(
                u8"sequence_read",
                u8"A sequence asset (a cutscene timeline): its length and frame rate, its parts (the cast's names, "
                u8"filled when it plays), its markers (a hold stops the playhead until the game resumes it), and each "
                u8"track by index: its kind (type name, and the name the editor shows), its part (null: the whole "
                u8"sequence), a property track's source, target and property, its key channels with their keys (a curve's "
                u8"with values), its sections, and its reflected `fields` (the paths sequence_edit's `set` takes, such "
                u8"as sections.0.clip). `kinds` lists every registered kind.",
                SchemaBuilder().Str(u8"guid", u8"the sequence's guid (asset_list: type SequenceAsset)", true).Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
                [s](const JsonValue& args) -> ToolResult
                {
                    Result<content::Instance*, String> instance = Resolve(*s, args);
                    if (!instance.HasValue())
                        return Err(Move(instance.Error()));
                    Result<RefPtr<pipeline::SequenceAsset>, String> asset = Read(*instance.Value());
                    if (!asset.HasValue())
                        return Err(Move(asset.Error()));
                    return SequenceJson(*instance.Value(), asset.Value()->sequence);
                });

            server.RegisterTool(
                u8"sequence_edit",
                u8"Edit a sequence asset: `edits` is a list applied in order, then the sequence is written once; a "
                u8"refused edit refuses the call and nothing is written. Each edit is an object with `op`: "
                u8"length {seconds}; add_part {name}; remove_part {name}; add_track {kind (a kind's type or editor "
                u8"name), part (when the kind plays on one), name?, and for a property track source? (component, the "
                u8"default: a part's component; settings: the scene's settings, no part; or a plugin's), target (a "
                u8"component's name or Transform; a settings id such as environment), property (a path), valueKind "
                u8"(Float, Float3, Color, Quat; Transform's are known)}; "
                u8"remove_track {track}; key {track, channel (name or index; omit for one), time, value? (a curve's)}; "
                u8"remove_key {track, channel, time}; section {track, start, length?, fields? (the section's fields by "
                u8"name: clip, offset, speed...)}; remove_section {track, section}; marker {time, name?, hold?}; "
                u8"remove_marker {marker}; set {track, path (a field's path from sequence_read, or part), value}. "
                u8"Returns each edit's result (the index it made) and the sequence as sequence_read gives it. An open "
                u8"sequence panel shows the write when it holds no unsaved edits; asset_cook before a game plays it.",
                SchemaBuilder()
                    .Str(u8"guid", u8"the sequence's guid", true)
                    .Arr(u8"edits", u8"object", u8"the edits, in order", true)
                    .Build(),
                foundation::mcp::ToolAnnotations::Overwrites(),
                [s](const JsonValue& args) -> ToolResult
                {
                    Result<content::Instance*, String> instance = Resolve(*s, args);
                    if (!instance.HasValue())
                        return Err(Move(instance.Error()));
                    Result<RefPtr<pipeline::SequenceAsset>, String> asset = Read(*instance.Value());
                    if (!asset.HasValue())
                        return Err(Move(asset.Error()));
                    sequence::Sequence& sequence = asset.Value()->sequence;
                    const JsonValue edits = args.Get(u8"edits");
                    JsonValue results = JsonValue::MakeArray();
                    for (i64 i = 0; i < edits.Count(); ++i)
                    {
                        JsonValue result = JsonValue::MakeObject();
                        Result<bool, String> applied = Apply(sequence, edits.At(i), result);
                        if (!applied.HasValue())
                            return Err(Format(u8"edit {} refused - {}. Nothing was written.", i, applied.Error().AsView()));
                        results.Add(Move(result));
                    }
                    sequence.duration = Max(sequence.duration, sequence.ComputeDuration()); // never cuts off what it holds
                    if (!instance.Value()->WriteObject(*asset.Value()).IsOk())
                        return Err(Format(u8"could not write '{}'", instance.Value()->Name()));
                    if (s->onAssetWritten)
                        s->onAssetWritten(instance.Value()->Id());
                    JsonValue out = SequenceJson(*instance.Value(), sequence);
                    out.Set(u8"results", Move(results));
                    return out;
                });
        }
    };
}
