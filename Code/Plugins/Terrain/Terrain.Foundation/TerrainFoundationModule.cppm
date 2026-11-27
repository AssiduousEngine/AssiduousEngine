// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Terrain.Foundation - `terrain.foundation`, the terrain plugin's types: the terrain's
/// chunks and LOD, the cooked terrain and its splatmap, the vegetation mask and the scatter of
/// vegetation over the chunks. The Engine part draws them; the Pipeline part cooks into them.

export module terrain.foundation;

export import :chunks;          // the chunk grid, quadtree and LOD over a heightfield
export import :splatmap;        // SplatWeights (top-K rasters), brush cores, cooked source and factory
export import :resource;        // TerrainResource, its cooked source and factory
export import :vegetationmask;  // VegetationMask, PaintMask/EraseMask/SmoothMask, its source and factory
export import :vegetationlayer; // VegetationPlacement, ScatterLayer, LayerScatterHash
export import :scatter;         // ScatterChunk, ChunkSeed, DensityAtDistance, FadePrefix, ChunksTouchedBy
