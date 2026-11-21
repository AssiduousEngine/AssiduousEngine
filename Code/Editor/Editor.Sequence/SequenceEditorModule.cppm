// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `editor.sequence` module.
//
// Sequences are authored in the scene, in a panel under the scene page's viewport: the sequence
// document with its own history (:document), the preview that poses the scene with it
// (:preview), the document as the dopesheet's and the curve view's source (:source), and the
// panel on the GUI (:panel), which the scene page holds.

export module editor.sequence;

export import :document;
export import :preview;
export import :source;
export import :panel;
