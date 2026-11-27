// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `editor.terrain` module.
//
// Terrain editing on the GUI: the scene view's brushes (sculpt, paint layers, cut holes), their
// panels, the splatmap thumbnails and the Terrain asset page.

export module terrain.editor;

export import :splatmap_thumbnail;
export import :asset_edits;
export import :sculpt;
export import :splat;
export import :hole;
export import :tool_panels;
export import :page;
export import :vegetation_pick;      // VegetationPick: the terrain footprint under the cursor (both brushes)
export import :vegetation_paint;     // VegetationPaintTool, RegisterVegetationViewportTools, RegisterVegetationToolPanels
export import :vegetation_scatter;   // VegetationScatterTool (Paint Props)
export import :vegetation_mask_page; // VegetationMaskPage
