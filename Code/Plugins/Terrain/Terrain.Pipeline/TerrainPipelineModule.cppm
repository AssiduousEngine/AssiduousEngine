// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Terrain.Pipeline - `terrain.pipeline`, the terrain plugin's assets: the terrain and the
/// splatmap, and the vegetation mask, with their builders, importers and creators.

export module terrain.pipeline;

export import :terrainasset;        // TerrainAsset, SplatmapAsset, their builders and the splatmap importer
export import :vegetationmaskasset; // VegetationMaskAsset, its builder and importer
