// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The quality level a game starts at: the player's saved choice while the game still lists it,
// else the manifest's default (a dist's is its export preset's) while listed, else the list's last;
// none at all when the game lists no levels (the engine's own highest).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import engine.project;

using namespace foundation::core;

TEST_CASE("project: the quality level a game starts at, in order of who chose it")
{
    engine::project::ProjectSettings settings;
    const Guid low{0x11, 0x12};
    const Guid medium{0x13, 0x14};
    const Guid high{0x15, 0x16};

    // No levels: none, whatever a save says.
    CHECK(settings.StartupQuality(Guid{}).IsNil());
    CHECK(settings.StartupQuality(low).IsNil());

    // Levels without a default: the list's last.
    settings.qualityLevelIds.PushBack(low);
    settings.qualityLevelIds.PushBack(medium);
    settings.qualityLevelIds.PushBack(high);
    CHECK(settings.StartupQuality(Guid{}) == high);

    // The manifest's default (the project's, or a dist's from its preset).
    settings.defaultQualityId = medium;
    CHECK(settings.StartupQuality(Guid{}) == medium);

    // The player's own choice over both.
    CHECK(settings.StartupQuality(low) == low);

    // A saved level the game no longer lists is ignored, and so is a default it does not list.
    const Guid gone{0x17, 0x18};
    CHECK(settings.StartupQuality(gone) == medium);
    settings.defaultQualityId = gone;
    CHECK(settings.StartupQuality(Guid{}) == high);
    CHECK(settings.IsQualityLevel(medium));
    CHECK_FALSE(settings.IsQualityLevel(gone));
}
