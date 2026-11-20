// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The behavior kinds, as C# declares them: a class the engine runs derives from one, and may have
// any name. A RunBehavior lives as long as the run (made with nothing), a SceneBehavior with its
// scene (made with the Scene), an EntityBehavior with its entity (made with the Entity). Each place
// that takes a class takes only its kind. The kinds have no members: the handlers (onStart,
// onUpdate, on<Event>, ...) are found by name, so a class declares only those it needs.

namespace Engine;

public abstract class RunBehavior
{
}

public abstract class SceneBehavior
{
}

public abstract class EntityBehavior
{
}
