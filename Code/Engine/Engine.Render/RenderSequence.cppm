// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Render - the `:sequence` partition: the track kinds a sequence drives the view with.
///
/// CameraCutSequenceTrack: while one of its sections runs, its bound camera is the scene's view
/// (a claim on CameraComponentManager, which the camera pick reads before the primary camera).
/// The camera knows nothing of it; the render starts the view's history fresh when the camera it
/// is drawn from changes.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.render:sequence;

import foundation.core;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

export namespace engine::render
{
    namespace sequence = foundation::sequence;

    /// After properties (a camera's own transform keys) and animation, before events.
    inline constexpr i32 kCameraTrackOrder = 500;

    /// A span of a camera cut track's timeline: its camera is the view from `start` for
    /// `duration` seconds.
    struct CameraCutSection
    {
        f32 start = 0.0f;
        f32 duration = 0.0f;
    };

    /// Cuts the scene's view to the bound camera for each of its sections; between sections, and
    /// once the play lets go, the view goes back to whoever had it.
    class CameraCutSequenceTrack final : public sequence::SequenceTrack
    {
        RTTI_OBJECT(CameraCutSequenceTrack, sequence::SequenceTrack)
    public:
        Array<CameraCutSection> sections; // by start

        [[nodiscard]] f32 Duration() const noexcept override;
        [[nodiscard]] i32 Order() const noexcept override { return kCameraTrackOrder; }
        [[nodiscard]] UniquePtr<sequence::SequenceTrackRunner> Begin(const sequence::SequenceTrackContext& context) const override;
        void Serialize(ISerializer& ar) override;
        /// Whether a section runs at `time` (its end included, so a play that ends on a cut and
        /// holds keeps it).
        [[nodiscard]] bool CutsAt(f32 time) const noexcept;
    };

    /// Registers the render's track kinds. Idempotent.
    void RegisterRenderSequenceTracks();
}
