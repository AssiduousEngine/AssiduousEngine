// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Engine - the `navigation.engine` module.
//
// Scene integration for the navmesh core: the zone and agent components and the per-scene
// NavigationSceneSystem that loads baked zones, runs a Detour crowd per zone, and steers agents to
// targets. Bake-source collection and the editor bake action live in Navigation.Editor.

export module navigation.engine;

export import :components;
export import :subsystem;
