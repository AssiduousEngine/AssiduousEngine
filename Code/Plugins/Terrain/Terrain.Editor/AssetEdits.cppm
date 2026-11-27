// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :asset_edits partition.
//
// How a live terrain edit is written to its source on the page's save (IAssetEditSink). Every
// tool that edits one heightfield, or one splatmap, registers the same persist, which writes all
// of it: the edits registered per page and asset replace each other, so a persist that wrote only
// what its own tool changes would lose what another tool changed before it (a sculpt after a hole
// cut wrote the heights and dropped the holes).
//
// For GCC module hygiene, the pipeline and resource imports live in AssetEditsImpl.cpp.

module;
#include "Core/Prelude.h"

export module editor.terrain:asset_edits;

import foundation.core;
import foundation.heightfield;      // Heightfield
import foundation.terrain.resource; // SplatWeights
import editor.core;                 // AssetEditPersist

using namespace foundation::core;

export namespace editor
{
    /// Writes `grid` to the source HeightfieldAsset `id`: its envelope synced to the grid, with its
    /// file name cleared so the sidecars are what it cooks from (a re-import sets it again), then
    /// its heights and its holes.
    [[nodiscard]] AssetEditPersist PersistHeightfield(RefPtr<foundation::heightfield::Heightfield> grid, const Guid& id);

    /// Writes `weights` to the source SplatmapAsset `id`: its envelope synced to the weights, with
    /// its file name cleared, then its weights and its layer indices.
    [[nodiscard]] AssetEditPersist PersistSplatWeights(RefPtr<foundation::terrain::SplatWeights> weights, const Guid& id);
}
