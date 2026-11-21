// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Dialogue.Engine - `dialogue.engine`, the dialogue plugin's runtime half: conversations
/// written as text, played by a runner on the game's run, with the staging the sequencer gives.
/// Every host loads this part when a project enables the plugin.

export module dialogue.engine;

export import :settings;
export import :conversation;
export import :cast;
export import :resources;
export import :runner;
export import :run;
