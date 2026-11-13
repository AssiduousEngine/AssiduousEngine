// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the :asset_edits persists (see AssetEdits.cppm).

module;
#include "Core/Prelude.h"

module editor.terrain;

import foundation.core;
import foundation.content;              // ContentDatabase, Instance
import foundation.heightfield;          // Heightfield
import foundation.heightfield.resource; // HeightfieldSource, kHeightStream, kHoleStream
import foundation.terrain.resource;     // SplatWeights, SplatWeightsSource, kSplatStream, kSplatIndexStream
import heightfield.pipeline;            // HeightfieldAsset
import terrain.pipeline;                // SplatmapAsset

using namespace foundation::core;

namespace content = foundation::content;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;

namespace editor
{
    AssetEditPersist PersistHeightfield(RefPtr<hf::Heightfield> grid, const Guid& id)
    {
        return [grid, id](content::ContentDatabase& db) -> Status
        {
            content::Instance* inst = db.GetInstance(id);
            if (inst == nullptr || grid.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound}; // the heightfield's source went
            }
            // The source envelope is a HeightfieldAsset: read, modify and write it. Never the
            // cooked HeightfieldSource type, which would clobber the source asset.
            RefPtr<ISerializable> object = inst->ReadObject();
            auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
            if (asset == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            asset->fileName = {};
            asset->size = grid->Size();
            asset->worldSize = grid->WorldSize();
            asset->minY = grid->MinY();
            asset->maxY = grid->MaxY();
            if (const Status wrote = inst->WriteObject(*asset); !wrote.IsOk())
            {
                return wrote;
            }
            // The heights are written whichever tool edited, or an imported heightfield would
            // lose the heights from its image once its file name is cleared.
            if (const Status wrote = inst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid));
                !wrote.IsOk())
            {
                return wrote;
            }
            return inst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid));
        };
    }

    AssetEditPersist PersistSplatWeights(RefPtr<terrain::SplatWeights> weights, const Guid& id)
    {
        return [weights, id](content::ContentDatabase& db) -> Status
        {
            content::Instance* inst = db.GetInstance(id);
            if (inst == nullptr || weights.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            RefPtr<ISerializable> object = inst->ReadObject();
            auto* asset = Cast<pipeline::SplatmapAsset>(object.Get());
            if (asset == nullptr)
            {
                return Status{ErrorCode::InvalidArgument}; // not a splatmap asset
            }
            asset->fileName = {};
            asset->width = weights->Width();
            asset->height = weights->Height();
            if (const Status wrote = inst->WriteObject(*asset); !wrote.IsOk())
            {
                return wrote;
            }
            if (const Status wrote = inst->WriteData(terrain::kSplatStream, terrain::SplatWeightsSource::WeightBlob(*weights));
                !wrote.IsOk())
            {
                return wrote;
            }
            return inst->WriteData(terrain::kSplatIndexStream, terrain::SplatWeightsSource::IndexBlob(*weights));
        };
    }
}
