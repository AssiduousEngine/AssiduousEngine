// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Sequence - `sequence.pipeline` (tooling).
//
// The source asset for a sequence and its builder. Sequences are authored in the editor (no OS-file
// importer): the asset holds the sequence as the editor edits it, and the builder writes it as the
// cooked SequenceSource that the runtime factory (foundation.sequence.resource) loads. The asset
// carries nothing the editor alone needs yet, so the cook writes the sequence as it is.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module sequence.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content; // CreateSequenceAsset
import foundation.sequence;
import foundation.sequence.resource;

using namespace foundation::core;

export namespace pipeline
{
    namespace sequence = foundation::sequence;

    /// Source asset: the authored sequence.
    class SequenceAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(SequenceAsset, pipeline::Asset)
    public:
        sequence::Sequence sequence;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            // v1 gave a property track's target its source; a v0 sequence's targets are components.
            sequence::SerializeSequence(ar, sequence, ar.Mode() == SerializeMode::Read && ar.Version() < 1);
        }
    };

    /// Cooks the authored asset to the SequenceSource product.
    class SequenceAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &SequenceAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &sequence::SequenceSource::StaticType(); }
        [[nodiscard]] u32 Version() const override { return 2; }
        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override
        {
            const SequenceAsset& authored = static_cast<const SequenceAsset&>(asset);
            sequence::SequenceSource cooked;
            cooked.sequence = authored.sequence; // the tracks are shared, not copied: the write only reads them
            return ctx.output->WriteObject(cooked);
        }
    };

    /// Registers the asset type (Pipeline domain) and the cooked source, resource and track kinds
    /// (Runtime domain).
    inline void RegisterSequenceAssets()
    {
        sequence::RegisterSequenceResource();
        GlobalTypeRegistry().Register(SequenceAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<SequenceAsset>();
    }

    /// An empty sequence in `target`, named `name` made unique (empty: "Sequence").
    [[nodiscard]] foundation::content::Instance* CreateSequenceAsset(foundation::content::Group* target, StringView name);
    /// File > New's sequence creator (pipeline.registration composes every domain's).
    void RegisterSequenceCreators(AssetCreatorRegistry& registry);
}
