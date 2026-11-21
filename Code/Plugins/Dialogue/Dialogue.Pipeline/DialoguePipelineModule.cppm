// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Dialogue.Pipeline - `dialogue.pipeline`, the dialogue plugin's authoring half: the
/// format's parser, the declared commands' notation, the cook's checks, and the dialogue's assets,
/// their builders, importer and creators. The editor and the command-line tools load
/// this part when a project enables the plugin.

export module dialogue.pipeline;

export import :parse;
export import :check;
export import :assets;
