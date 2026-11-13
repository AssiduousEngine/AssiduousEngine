// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Content - foundation.content implementation unit.
//
// Out-of-line definitions for Instance, Group and ContentDatabase. ContentModule.cppm keeps
// the class declarations and trivial inline accessors.

module;
#include "Core/Prelude.h"
#include <initializer_list>

module foundation.content;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;
using namespace foundation::vfs;

namespace foundation::content
{
    namespace
    {
        constexpr StringView kTextSuffix = u8".data";
        constexpr StringView kBinarySuffix = u8".bin";

        // A sidecar's "<owner>.<stream>" without its suffix, and the suffix's encoding; false when
        // the name ends in neither suffix.
        [[nodiscard]] bool StripSidecarSuffix(StringView fileName, StringView& stem,
                                              StreamEncoding& encoding) noexcept
        {
            if (fileName.EndsWith(kTextSuffix))
            {
                stem = fileName.SubStr(0, fileName.Size() - kTextSuffix.Size());
                encoding = StreamEncoding::Text;
                return true;
            }
            if (fileName.EndsWith(kBinarySuffix))
            {
                stem = fileName.SubStr(0, fileName.Size() - kBinarySuffix.Size());
                encoding = StreamEncoding::Binary;
                return true;
            }
            return false;
        }

        // Each sidecar of the instance `name` in `folder`: `visit(fileName, stream, encoding)`.
        // Streams keep no directory, so the group folder is enumerated and matched exactly.
        template <typename Visit>
        void ForEachSidecar(IFileSystem& mount, StringView folder, StringView name, Visit&& visit)
        {
            IEnumerableFileSystem* enumerable = mount.AsEnumerable();
            if (enumerable == nullptr)
            {
                return;
            }
            Array<DirEntry> entries;
            if (!enumerable->Enumerate(folder, entries).IsOk())
            {
                return;
            }
            for (const DirEntry& entry : entries)
            {
                StringView stream;
                StreamEncoding encoding = StreamEncoding::Binary;
                if (!entry.isDirectory && MatchSidecar(name, entry.name.AsView(), stream, encoding))
                {
                    visit(entry.name.AsView(), stream, encoding);
                }
            }
        }
    } // namespace

    bool IsStreamName(StringView name) noexcept
    {
        if (name.IsEmpty())
        {
            return false;
        }
        for (const utf8char c : name)
        {
            if (c == utf8char('.') || c == utf8char('/') || c == utf8char('\\'))
            {
                return false;
            }
        }
        return true;
    }

    bool MatchSidecar(StringView instanceName, StringView fileName, StringView& stream,
                      StreamEncoding& encoding) noexcept
    {
        StringView stem;
        if (instanceName.IsEmpty() || !StripSidecarSuffix(fileName, stem, encoding))
        {
            return false;
        }
        // "<name>.<stream>": the name, a dot, then a stream name (which has no dot of its own).
        if (stem.Size() <= instanceName.Size() + 1 || !stem.StartsWith(instanceName) ||
            stem[instanceName.Size()] != utf8char('.'))
        {
            return false;
        }
        const StringView candidate =
            stem.SubStr(instanceName.Size() + 1, stem.Size() - instanceName.Size() - 1);
        if (!IsStreamName(candidate))
        {
            return false;
        }
        stream = candidate;
        return true;
    }

    StringView OwnerPathOf(StringView file, StringView envelopeExtension) noexcept
    {
        const usize slash = file.FindLast(utf8char('/'));
        const usize nameStart = slash == StringView::kNotFound ? 0 : slash + 1;
        if (!envelopeExtension.IsEmpty() && file.EndsWith(envelopeExtension) &&
            file.Size() - envelopeExtension.Size() > nameStart)
        {
            return file.SubStr(0, file.Size() - envelopeExtension.Size());
        }
        StringView stem;
        StreamEncoding encoding = StreamEncoding::Binary;
        if (!StripSidecarSuffix(file, stem, encoding))
        {
            return {};
        }
        // The stream is what follows the stem's last dot; the owner is everything before it.
        const usize dot = stem.FindLast(utf8char('.'));
        if (dot == StringView::kNotFound || dot <= nameStart || !IsStreamName(stem.SubStr(dot + 1, stem.Size() - dot - 1)))
        {
            return {};
        }
        return stem.SubStr(0, dot);
    }

    ContentDatabase::ContentDatabase(IAllocator& allocator, IFileSystem& mount,
                                     SerializerFactory factory, StringView fileExtension,
                                     SerializableRegistry& serializables, TypeRegistry& types)
        : m_allocator(&allocator), m_mount(&mount),
          m_factory(static_cast<SerializerFactory&&>(factory)), m_extension(fileExtension),
          m_serializables(&serializables), m_types(&types), m_allGroups(allocator),
          m_allInstances(allocator)
    {
        m_rng = Random::FromSystemEntropy(); // asset ids made in two places never meet
        m_root = NewGroup(nullptr, u8"");
        Scan(*m_root, u8"");
    }

    // -----------------------------------------------------------------------
    // Group method definitions.
    // -----------------------------------------------------------------------
    String Group::Path() const
    {
        if (m_parent == nullptr)
        {
            return String();
        } // root
        return JoinPath(m_parent->Path().AsView(), m_name.AsView());
    }

    Group* Group::GetGroup(StringView name) const
    {
        for (Group* group : m_groups)
        {
            if (group->Name() == name)
            {
                return group;
            }
        }
        return nullptr;
    }

    Instance* Group::GetInstance(StringView name) const
    {
        for (Instance* instance : m_instances)
        {
            if (instance->Name() == name)
            {
                return instance;
            }
        }
        return nullptr;
    }

    bool Group::NameLess(StringView a, StringView b)
    {
        const usize n = Min(a.Size(), b.Size());
        for (usize i = 0; i < n; ++i)
        {
            utf8char ca = a[i];
            utf8char cb = b[i];
            if (ca >= utf8char('A') && ca <= utf8char('Z'))
            {
                ca = static_cast<utf8char>(ca + 32);
            }
            if (cb >= utf8char('A') && cb <= utf8char('Z'))
            {
                cb = static_cast<utf8char>(cb + 32);
            }
            if (ca != cb)
            {
                return ca < cb;
            }
        }
        return a.Size() < b.Size();
    }

    Group* Group::AddChildGroup(StringView name)
    {
        Group* child = m_db->NewGroup(this, name);
        usize at = m_groups.Size();
        for (usize i = 0; i < m_groups.Size(); ++i)
        {
            if (NameLess(child->Name(), m_groups[i]->Name()))
            {
                at = i;
                break;
            }
        }
        m_groups.Insert(at, child);
        return child;
    }

    Instance* Group::AddInstance(const Guid& id, StringView name, StringView typeNs,
                                 StringView typeName)
    {
        Instance* instance = m_db->NewInstance(*this, id, name, typeNs, typeName);
        usize at = m_instances.Size();
        for (usize i = 0; i < m_instances.Size(); ++i)
        {
            if (NameLess(instance->Name(), m_instances[i]->Name()))
            {
                at = i;
                break;
            }
        }
        m_instances.Insert(at, instance);
        return instance;
    }

    void Group::ResortChildren()
    {
        m_db->Touch(); // a child renamed
        m_groups.Sort([](Group* a, Group* b) { return NameLess(a->Name(), b->Name()); });
        m_instances.Sort([](Instance* a, Instance* b)
                         { return NameLess(a->Name(), b->Name()); });
    }

    void Group::RemoveInstance(Instance* instance)
    {
        m_db->Touch();
        for (usize i = 0; i < m_instances.Size(); ++i)
        {
            if (m_instances[i] == instance)
            {
                m_instances.RemoveAt(i);
                return;
            }
        }
    }

    Group* Group::CreateGroup(StringView name)
    {
        Group* existing = GetGroup(name);
        return (existing != nullptr) ? existing : AddChildGroup(name);
    }

    // The one name-dedup loop: `base`, then `base.2`, `base.3`, ... against whatever
    // `taken` probes (instances or child groups).
    template <typename TakenPredicate>
    static String FirstFreeName(StringView base, TakenPredicate&& taken)
    {
        if (!taken(base))
        {
            return String(base);
        }
        for (i32 counter = 2;; ++counter)
        {
            String candidate(base);
            candidate.PushBack(u8'.');
            utf8char digits[12];
            i32 digitCount = 0;
            for (i32 value = counter; value > 0 && digitCount < 12; value /= 10)
            {
                digits[digitCount++] = static_cast<utf8char>('0' + value % 10);
            }
            while (digitCount > 0)
            {
                candidate.PushBack(digits[--digitCount]);
            }
            if (!taken(candidate.AsView()))
            {
                return candidate;
            }
        }
    }

    String Group::UniqueInstanceName(StringView base) const
    {
        return FirstFreeName(base,
                             [this](StringView name) { return GetInstance(name) != nullptr; });
    }

    String Group::UniqueGroupName(StringView base) const
    {
        return FirstFreeName(base,
                             [this](StringView name) { return GetGroup(name) != nullptr; });
    }

    Instance* Group::CreateInstance(StringView name, const TypeInfo& primaryType)
    {
        if (Instance* existing = GetInstance(name))
        {
            return existing;
        }
        // Mint a GUID that isn't already in use. The RNG is deterministic and Scan() (load from
        // disk) does not advance it past the instances it loads, so a fresh instance added to a
        // scanned DB could reproduce the first-cooked instance's GUID and alias it (e.g. a
        // runtime-cooked texture colliding with a model's first texture). Re-roll until the id
        // is free.
        Guid id = Guid::Generate(m_db->Rng());
        while (m_db->GetInstance(id) != nullptr)
        {
            id = Guid::Generate(m_db->Rng());
        }
        // TypeInfo names are narrow ASCII; wrap in StringView.
        const StringView ns(reinterpret_cast<const utf8char*>(primaryType.namespaceName));
        const StringView nm(reinterpret_cast<const utf8char*>(primaryType.name));
        return AddInstance(id, name, ns, nm);
    }

    Instance* Group::CreateInstanceWithId(const Guid& id, StringView name,
                                          const TypeInfo& primaryType)
    {
        if (Instance* existing = GetInstance(name))
        {
            return existing;
        }
        const StringView ns(reinterpret_cast<const utf8char*>(primaryType.namespaceName));
        const StringView nm(reinterpret_cast<const utf8char*>(primaryType.name));
        return AddInstance(id, name, ns, nm);
    }

    // -----------------------------------------------------------------------
    // Instance method definitions.
    // -----------------------------------------------------------------------
    String Instance::Path() const { return JoinPath(m_group->Path().AsView(), m_name.AsView()); }

    String Instance::EnvelopePath() const
    {
        String path = Path();
        path.Append(m_db->Extension());
        return path;
    }

    String Instance::DataPath(StringView streamName, StreamEncoding encoding) const
    {
        String path = Path();
        path.PushBack(utf8char('.'));
        path.Append(streamName);
        path.Append(encoding == StreamEncoding::Text ? u8".data" : u8".bin");
        return path;
    }

    RefPtr<ISerializable> Instance::ReadObject() const
    {
        UniquePtr<IStream> stream = m_db->Mount().Open(EnvelopePath().AsView(), FileMode::Read);
        if (!stream)
        {
            return RefPtr<ISerializable>{};
        }
        // Buffered: a binary envelope reads field by field, and straight off a file each
        // field is a system call (a cooked script's were thousands, dearer still on Windows).
        BufferedStream buffered(*stream, 64 * 1024);
        Result<RefPtr<ISerializable>, String> object = ReadEnvelope(buffered, false);
        if (!object.HasValue())
        {
            return RefPtr<ISerializable>{};
        }
        // The object names its source as it stands on disk: keep it, whatever the header said.
        m_source = String(object.Value()->SourceName());
        m_sourceKnown = true;
        return Move(object.Value());
    }

    StringView Instance::SourceName() const
    {
        if (!m_sourceKnown)
        {
            // An envelope written before its header named the source: read once, then kept
            // (ReadObject records it, or an unreadable one names none).
            (void)ReadObject();
            m_sourceKnown = true;
        }
        return m_source.AsView();
    }

    Result<RefPtr<ISerializable>, String> Instance::ReadObjectFrom(IStream& stream) const
    {
        return ReadEnvelope(stream, true);
    }

    Result<RefPtr<ISerializable>, String> Instance::ReadEnvelope(IStream& stream,
                                                                 bool checkIdentity) const
    {
        UniquePtr<SerializerContext> ctx = m_db->CreateSerializer(stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Err(String(u8"the text did not open as an envelope"));
        }
        Serializer& ar = *ctx->serializer;

        // Read header.
        Guid id;
        String ns;
        String name;
        ar.Key("guid");
        ar.GuidValue(id);
        ar.Key("typeNamespace");
        ar.Text(ns);
        ar.Key("typeName");
        ar.Text(name);
        if (!ar.IsOk())
        {
            return Err(String(u8"no envelope header (guid, typeNamespace, typeName)"));
        }
        if (checkIdentity && id != m_id)
        {
            utf8char asked[37];
            utf8char mine[37];
            id.ToChars(asked);
            m_id.ToChars(mine);
            return Err(Format(u8"the envelope's guid {} is not this asset's ({})",
                              StringView(asked, 36), StringView(mine, 36)));
        }
        if (checkIdentity && (ns.AsView() != TypeNamespace() || name.AsView() != TypeName()))
        {
            return Err(Format(u8"the envelope's type '{}.{}' is not this asset's ('{}.{}')",
                              ns.AsView(), name.AsView(), TypeNamespace(), TypeName()));
        }
        // The header's source file (a self-describing envelope written since it carried one): the
        // object names it again once read, so it is passed over here.
        if (ar.IsSelfDescribing() && ar.HasKey("source"))
        {
            String source;
            ar.Key("source");
            ar.Text(source);
        }

        // Resolve the type and construct the object.
        const TypeInfo* type = m_db->Types().FindByName(reinterpret_cast<const char*>(ns.CStr()),
                                                        reinterpret_cast<const char*>(name.CStr()));
        if (type == nullptr)
        {
            return Err(Format(u8"the type '{}' is not registered in this build", name.AsView()));
        }

        RefPtr<ISerializable> object = m_db->Serializables().Create(type->id);
        if (object.Get() == nullptr)
        {
            return Err(Format(u8"the type '{}' is not serializable in this build", name.AsView()));
        }

        // Deserialize the payload under its data-version scope (a version other than the
        // type's current one fails the read here; there is no migration). A refused payload is not
        // read: it is in another layout, and reading it as this one only walks garbage.
        BeginVersionedPayload(ar, *type);
        if (ar.IsOk())
        {
            ar.Key("payload");
            ar.BeginObject();
            object->Serialize(ar);
            ar.EndObject();
        }
        EndVersionedPayload(ar);
        if (!ar.IsOk())
        {
            String error(u8"the payload did not read: a missing or misspelled key, a value "
                         u8"of the wrong kind, or dataVersions other than this build's");
            String place;
            ar.DescribeFailure(place);
            if (!place.IsEmpty())
            {
                error += StringView(u8" - ");
                error += place.AsView();
            }
            return Err(Move(error));
        }
        return object;
    }

    UniquePtr<IStream> Instance::ReadData(StringView streamName) const
    {
        if (!IsStreamName(streamName))
        {
            return nullptr;
        }
        if (UniquePtr<IStream> text =
                m_db->Mount().Open(DataPath(streamName, StreamEncoding::Text).AsView(),
                                   FileMode::Read))
        {
            return text;
        }
        return m_db->Mount().Open(DataPath(streamName, StreamEncoding::Binary).AsView(),
                                  FileMode::Read);
    }

    Status Instance::WriteObject(ISerializable& object)
    {
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        MemoryStream buffer;
        UniquePtr<SerializerContext> ctx = m_db->CreateSerializer(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        Serializer& ar = *ctx->serializer;

        // Write the header and payload (the payload wrapped in the object's data-version scope).
        String ns(m_typeNamespace);
        String nm(m_typeName);
        ar.Key("guid");
        ar.GuidValue(const_cast<Guid&>(m_id));
        ar.Key("typeNamespace");
        ar.Text(ns);
        ar.Key("typeName");
        ar.Text(nm);
        // The source file the object names, in a self-describing header only (a positional one
        // keeps its layout; cooked data names no sources): the scan reads it with the type.
        String source(object.SourceName());
        if (ar.IsSelfDescribing())
        {
            ar.Key("source");
            ar.Text(source);
        }
        BeginVersionedPayload(ar, *object.GetType());
        ar.Key("payload");
        ar.BeginObject();
        object.Serialize(ar);
        ar.EndObject();
        EndVersionedPayload(ar);

        // Let the context flush (e.g., XML writes its text output here).
        ctx->Flush(buffer);

        const Status saved = writable->Save(EnvelopePath().AsView(), buffer.Bytes());
        if (saved.IsOk())
        {
            SetSourceName(source.AsView());
        }
        return saved;
    }

    Status Instance::WriteData(StringView streamName, Span<const byte> data,
                               StreamEncoding encoding)
    {
        if (!IsStreamName(streamName))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        const Status saved = writable->Save(DataPath(streamName, encoding).AsView(), data);
        if (saved.IsOk())
        {
            // One suffix per stream: drop the other-encoding sibling so a stream never
            // exists under both suffixes at once.
            const String other = DataPath(streamName, encoding == StreamEncoding::Text
                                                          ? StreamEncoding::Binary
                                                          : StreamEncoding::Text);
            if (m_db->Mount().Exists(other.AsView()))
            {
                (void)writable->Delete(other.AsView());
            }
        }
        return saved;
    }

    Status Instance::DeleteData(StringView streamName)
    {
        if (!IsStreamName(streamName))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        Status result{};
        for (StreamEncoding encoding : {StreamEncoding::Text, StreamEncoding::Binary})
        {
            const String path = DataPath(streamName, encoding);
            if (!m_db->Mount().Exists(path.AsView()))
            {
                continue; // idempotent: nothing to remove
            }
            const Status deleted = writable->Delete(path.AsView());
            if (!deleted.IsOk())
            {
                result = deleted;
            }
        }
        return result;
    }

    UniquePtr<IStream> Instance::OpenEnvelope() const
    {
        return m_db->Mount().Open(EnvelopePath().AsView(), FileMode::Read);
    }

    Instance* ContentDatabase::CloneInstance(const Guid& id, StringView newName)
    {
        Instance* src = GetInstance(id);
        if (src == nullptr)
        {
            return nullptr;
        }
        Group& group = src->OwningGroup();
        if (group.GetInstance(newName) != nullptr)
        {
            return nullptr;
        }

        // The primary object must round-trip (re-serialized under the clone's identity, since a
        // raw envelope byte-copy would carry the source guid).
        RefPtr<ISerializable> object = src->ReadObject();
        if (object.Get() == nullptr)
        {
            return nullptr;
        }

        Guid cloneId = Guid::Generate(Rng());
        while (GetInstance(cloneId) != nullptr)
        {
            cloneId = Guid::Generate(Rng());
        }
        Instance* copy = group.AddInstance(cloneId, newName, src->TypeNamespace(), src->TypeName());
        if (copy == nullptr)
        {
            return nullptr;
        }
        if (!copy->WriteObject(*object).IsOk())
        {
            return nullptr;
        }

        // Every sidecar byte-copied under the clone's name, keeping its stream and encoding.
        ForEachSidecar(*m_mount, group.Path().AsView(), src->Name(),
                       [&](StringView, StringView stream, StreamEncoding encoding)
                       {
                           if (UniquePtr<IStream> data = src->ReadData(stream))
                           {
                               Array<byte> bytes;
                               bytes.Resize(static_cast<usize>(data->Size()));
                               if (data->Read(bytes.Data(), bytes.Size()) == bytes.Size())
                               {
                                   (void)copy->WriteData(
                                       stream, Span<const byte>{bytes.Data(), bytes.Size()}, encoding);
                               }
                           }
                       });
        return copy;
    }

    Status ContentDatabase::CopyContentForward(Instance& dest, ContentDatabase& src,
                                               const Guid& srcId)
    {
        Instance* source = src.GetInstance(srcId);
        if (source == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }

        // Primary object: round-trips under dest's identity. dest.guid == src.guid (the variant cook
        // keeps product-guid == source-guid in every DB), so the re-serialized bytes are identical.
        RefPtr<ISerializable> object = source->ReadObject();
        if (object.Get() == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }
        const Status wrote = dest.WriteObject(*object);
        if (!wrote.IsOk())
        {
            return wrote;
        }

        // Every sidecar byte-copied into dest under the same stream and encoding: the clone's
        // copy, but across databases and mounts.
        ForEachSidecar(*src.m_mount, source->OwningGroup().Path().AsView(), source->Name(),
                       [&](StringView, StringView stream, StreamEncoding encoding)
                       {
                           if (UniquePtr<IStream> data = source->ReadData(stream))
                           {
                               Array<byte> bytes;
                               bytes.Resize(static_cast<usize>(data->Size()));
                               if (data->Read(bytes.Data(), bytes.Size()) == bytes.Size())
                               {
                                   (void)dest.WriteData(
                                       stream, Span<const byte>{bytes.Data(), bytes.Size()}, encoding);
                               }
                           }
                       });
        return Status{};
    }

    Status ContentDatabase::RenameInstance(const Guid& id, StringView newName)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        if (newName.IsEmpty() || newName == instance->Name())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (instance->OwningGroup().GetInstance(newName) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String folder = instance->OwningGroup().Path();
        const String oldEnvelope = instance->EnvelopePath();

        // Sidecars first: "<old>.<stream>.<suffix>" becomes "<new>.<stream>.<suffix>".
        ForEachSidecar(*m_mount, folder.AsView(), instance->Name(),
                       [&](StringView fileName, StringView, StreamEncoding)
                       {
                           String renamed(newName);
                           renamed.Append(fileName.SubStr(instance->Name().Size(),
                                                          fileName.Size() - instance->Name().Size()));
                           (void)writable->Move(JoinPath(folder.AsView(), fileName).AsView(),
                                                JoinPath(folder.AsView(), renamed.AsView()).AsView());
                       });

        // The envelope may not exist yet (instance created, never written); that is fine.
        instance->m_name = String(newName);
        instance->OwningGroup().ResortChildren();
        if (m_mount->Exists(oldEnvelope.AsView()))
        {
            const Status moved =
                writable->Move(oldEnvelope.AsView(), instance->EnvelopePath().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::MoveInstance(const Guid& id, Group& to)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        Group& from = instance->OwningGroup();
        if (&from == &to)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (to.GetInstance(instance->Name()) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String fromFolder = from.Path();
        const String toFolder = to.Path();
        const String oldEnvelope = instance->EnvelopePath();
        if (!toFolder.IsEmpty() && !m_mount->Exists(toFolder.AsView()))
        {
            (void)writable->CreateDirectory(toFolder.AsView());
        }
        // Sidecars first, as a rename does: each "<name>.<stream>.<suffix>" moves.
        ForEachSidecar(*m_mount, fromFolder.AsView(), instance->Name(),
                       [&](StringView fileName, StringView, StreamEncoding)
                       {
                           (void)writable->Move(JoinPath(fromFolder.AsView(), fileName).AsView(),
                                                JoinPath(toFolder.AsView(), fileName).AsView());
                       });

        // Out of its group and into the other, in its name order (the envelope may not exist
        // yet: an instance created and never written).
        from.RemoveInstance(instance);
        instance->m_group = &to;
        usize at = to.m_instances.Size();
        for (usize i = 0; i < to.m_instances.Size(); ++i)
        {
            if (Group::NameLess(instance->Name(), to.m_instances[i]->Name()))
            {
                at = i;
                break;
            }
        }
        to.m_instances.Insert(at, instance);
        Touch();
        if (m_mount->Exists(oldEnvelope.AsView()))
        {
            const Status moved = writable->Move(oldEnvelope.AsView(), instance->EnvelopePath().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::RenameGroup(Group& group, StringView newName)
    {
        if (group.Parent() == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        } // the root
        if (newName.IsEmpty() || newName == group.Name())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (group.Parent()->GetGroup(newName) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String oldPath = group.Path();
        group.m_name = String(newName);
        group.Parent()->ResortChildren();
        // The directory may not exist yet (group created, nothing written under it).
        if (!oldPath.IsEmpty() && m_mount->Exists(oldPath.AsView()))
        {
            const Status moved = writable->Move(oldPath.AsView(), group.Path().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::DeleteInstance(const Guid& id)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        // The envelope and every sidecar.
        (void)writable->Delete(instance->EnvelopePath().AsView());
        const String folder = instance->OwningGroup().Path();
        ForEachSidecar(*m_mount, folder.AsView(), instance->Name(),
                       [&](StringView fileName, StringView, StreamEncoding)
                       { (void)writable->Delete(JoinPath(folder.AsView(), fileName).AsView()); });

        m_byGuid.Remove(id);
        instance->OwningGroup().RemoveInstance(instance);
        for (usize i = 0; i < m_allInstances.Size(); ++i)
        {
            if (m_allInstances[i] == instance)
            {
                m_allInstances.RemoveAtSwap(i);
                break;
            }
        }
        m_allocator->Delete(instance);
        return Status{};
    }

    bool ContentDatabase::ReadHeader(IStream& stream, Header& out) const
    {
        UniquePtr<SerializerContext> ctx = m_factory(stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return false;
        }
        Serializer& ar = *ctx->serializer;
        ar.Key("guid");
        ar.GuidValue(out.id);
        ar.Key("typeNamespace");
        ar.Text(out.typeNamespace);
        ar.Key("typeName");
        ar.Text(out.typeName);
        if (!ar.IsOk())
        {
            return false;
        }
        // The source file, which a self-describing header carries (Instance::SourceName); one
        // written before it does not, and its record learns it on first asking.
        out.named = ar.IsSelfDescribing() && ar.HasKey("source");
        if (out.named)
        {
            ar.Key("source");
            ar.Text(out.source);
            out.named = ar.IsOk();
        }
        return true;
    }

    namespace
    {
        void Note(Array<Guid>& list, const Guid& id)
        {
            for (const Guid& listed : list)
            {
                if (listed == id)
                {
                    return;
                }
            }
            list.PushBack(id);
        }
    }

    void ContentDatabase::Refresh(Span<const String> paths, ContentChanges& out)
    {
        for (const String& path : paths)
        {
            const StringView view = path.AsView();
            if (EndsWith(view, m_extension.AsView()))
            {
                RefreshEnvelope(view, view.SubStr(0, view.Size() - m_extension.Size()), out);
            }
            else
            {
                RefreshDataStream(view, out);
            }
        }
    }

    void ContentDatabase::RefreshEnvelope(StringView path, StringView recordPath, ContentChanges& out)
    {
        Instance* existing = GetInstance(recordPath);
        UniquePtr<IStream> stream = m_mount->Open(path, FileMode::Read);
        if (!stream)
        {
            if (existing != nullptr)
            {
                const Guid gone = existing->Id();
                ForgetInstance(existing);
                Note(out.removed, gone);
            }
            return;
        }
        Header header;
        if (!ReadHeader(*stream, header) || header.id.IsNil())
        {
            out.unreadable.PushBack(String(path));
            return;
        }

        if (existing != nullptr && existing->Id() == header.id && existing->TypeNamespace() == header.typeNamespace.AsView() &&
            existing->TypeName() == header.typeName.AsView())
        {
            if (header.named)
            {
                existing->SetSourceName(header.source.AsView());
            }
            else
            {
                existing->ForgetSourceName();
            }
            Touch();
            Note(out.changed, header.id);
            return;
        }

        // A new record, or one whose identity or type changed under the same path.
        bool sameId = false;
        if (existing != nullptr)
        {
            const Guid previous = existing->Id();
            sameId = previous == header.id;
            ForgetInstance(existing);
            if (!sameId)
            {
                Note(out.removed, previous);
            }
        }
        Group* group = m_root;
        StringView name = recordPath;
        const usize slash = recordPath.FindLast(utf8char('/'));
        if (slash != StringView::kNotFound)
        {
            usize start = 0;
            for (usize i = 0; i <= slash; ++i)
            {
                if (i == slash || recordPath[i] == utf8char('/'))
                {
                    const StringView part = recordPath.SubStr(start, i - start);
                    if (!part.IsEmpty())
                    {
                        group = group->CreateGroup(part);
                    }
                    start = i + 1;
                }
            }
            name = recordPath.SubStr(slash + 1, recordPath.Size() - slash - 1);
        }
        Instance* instance = group->AddInstance(header.id, name, header.typeNamespace.AsView(), header.typeName.AsView());
        if (instance != nullptr && header.named)
        {
            instance->SetSourceName(header.source.AsView());
        }
        Note(sameId ? out.changed : out.added, header.id);
    }

    // A data stream's sidecar is "<record path>.<stream>.data" (text) or ".bin" (binary).
    void ContentDatabase::RefreshDataStream(StringView path, ContentChanges& out)
    {
        // The envelope's own extension is not a sidecar (the record refresh handles it).
        const StringView owner = OwnerPathOf(path, {});
        if (owner.IsEmpty())
        {
            return;
        }
        if (Instance* instance = GetInstance(owner))
        {
            for (const Guid& added : out.added)
            {
                if (added == instance->Id())
                {
                    return; // new this refresh: added says it all
                }
            }
            Note(out.changed, instance->Id());
        }
    }

    void ContentDatabase::ForgetInstance(Instance* instance)
    {
        const Guid id = instance->Id();
        instance->OwningGroup().RemoveInstance(instance);
        for (usize i = 0; i < m_allInstances.Size(); ++i)
        {
            if (m_allInstances[i] == instance)
            {
                m_allInstances.RemoveAtSwap(i);
                break;
            }
        }
        Instance* const* indexed = m_byGuid.Find(id);
        if (indexed != nullptr && *indexed == instance)
        {
            m_byGuid.Remove(id);
            for (Instance* other : m_allInstances)
            {
                if (other->Id() == id)
                {
                    m_byGuid.InsertOrAssign(id, other);
                    break;
                }
            }
        }
        m_allocator->Delete(instance);
    }

    Status ContentDatabase::DeleteGroup(Group& group)
    {
        if (group.Parent() == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        } // the root
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        // Instances first (copy the id list, since DeleteInstance unlinks from m_instances),
        Array<Guid> ids;
        for (Instance* instance : group.Instances())
        {
            ids.PushBack(instance->Id());
        }
        for (const Guid& id : ids)
        {
            (void)DeleteInstance(id);
        }
        // then child groups, bottom-up (copied, since the recursion unlinks from m_groups).
        Array<Group*> children;
        for (Group* child : group.Groups())
        {
            children.PushBack(child);
        }
        for (Group* child : children)
        {
            (void)DeleteGroup(*child);
        }

        // The directory may never have materialized (group created, nothing written).
        const String path = group.Path();
        if (!path.IsEmpty() && m_mount->Exists(path.AsView()))
        {
            const Status removed = writable->DeleteDirectory(path.AsView());
            if (!removed.IsOk())
            {
                return removed;
            }
        }

        // Unregister from the parent and the pool, then destroy.
        Touch();
        Group* parent = group.Parent();
        for (usize i = 0; i < parent->m_groups.Size(); ++i)
        {
            if (parent->m_groups[i] == &group)
            {
                parent->m_groups.RemoveAt(i);
                break;
            }
        }
        for (usize i = 0; i < m_allGroups.Size(); ++i)
        {
            if (m_allGroups[i] == &group)
            {
                m_allGroups.RemoveAtSwap(i);
                break;
            }
        }
        m_allocator->Delete(&group);
        return Status{};
    }
}
