// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Dialogue.Engine - `dialogue.engine`, the dialogue plugin's runtime half: conversations
/// written as text, played by a runner on the game's run, with the staging the sequencer gives.
/// Every host loads this part when a project enables the plugin.

export module dialogue.engine;

import dialogue.foundation; // the types it plays

export import :runner;
export import :run;
export import :facade;
export import :stockbox;
export import :linetrack;
