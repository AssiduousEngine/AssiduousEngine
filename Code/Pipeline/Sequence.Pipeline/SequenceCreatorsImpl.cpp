// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Sequence.Pipeline - the sequence domain's New Asset creator: what File > New and asset_create
// make, with no editor.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module sequence.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance* CreateSequenceAsset(foundation::content::Group* target, StringView name)
    {
        if (target == nullptr)
        {
            return nullptr;
        }
        foundation::content::Instance* instance = target->CreateInstance(
            target->UniqueInstanceName(name.IsEmpty() ? StringView(u8"Sequence") : name).AsView(), SequenceAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        SequenceAsset asset;
        return instance->WriteObject(asset).IsOk() ? instance : nullptr;
    }

    void RegisterSequenceCreators(AssetCreatorRegistry& registry)
    {
        AssetCreator creator;
        creator.label = String(u8"Sequence");
        creator.category = String(u8"Animation");
        creator.type = &SequenceAsset::StaticType();
        creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
        { return CreateSequenceAsset(context.Target(), context.name); };
        registry.Register(Move(creator));
    }

    // The asset type's display name (the editor shows it as the asset's kind).
    REFLECT_MEMBERS(SequenceAsset, "rtti::pipeline::sequence")
    {
        builder.Attribute("displayName", String(u8"Sequence"));
    }
}
