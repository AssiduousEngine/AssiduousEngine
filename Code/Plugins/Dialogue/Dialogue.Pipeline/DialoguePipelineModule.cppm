// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Dialogue.Pipeline - `dialogue.pipeline`, the dialogue plugin's authoring half: the
/// format's parser and the declared commands' notation. The editor and the command-line tools load
/// this part when a project enables the plugin.

export module dialogue.pipeline;

export import :parse;
