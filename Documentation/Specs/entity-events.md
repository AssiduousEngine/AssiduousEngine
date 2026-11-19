# Entity events without bridges

> APPROVED and built 2026-11-19, with the decisions below settled. Read again on master at
> `3ebb9e4a8`, after the behavior naming landed; line numbers will drift.

## Goal

A domain tells an entity's behaviors that something happened to it (a contact, an animation
ending) without a bridge per domain, and without the domain depending on the behavior layer.
`Engine.Integration` and both of its bridges go.

## What exists today

Two bridges in `Code/Engine/Engine.Integration`, owned and installed by `DefaultApplication`:

| Bridge | Listens to | Calls |
|---|---|---|
| `ScriptPhysicsContactBridge` | `engine::physics::IContactListener` | `BehaviorSubsystem::DeliverContact` |
| `ScriptAnimationBridge` | `engine::animation::IAnimationListener` | `BehaviorSubsystem::DeliverEvent` |

Both do the same thing: turn a domain's struct into `(scene, entity, handler name, arguments)`
and hand it to the behavior layer. The contact bridge also translates `ContactKind` into
`ScriptContactKind`, an enum that exists only to mirror it.

They exist because the entry point for "send this entity an event" lives in `Engine.Behavior`,
which physics and animation must not depend on.

Storage and delivery are already in one place. `EntityBehaviorSystem` has one deferred queue,
`m_messages` (`BehaviorSubsystem.cppm:1387`), fed by three sources:

- `entity.send` from a behavior, through `EnqueueMessage`
- physics contacts, through the bridge and `EnqueueHandler`
- animation events, through the bridge and `EnqueueHandler`

`DrainMessages` empties it at the top of the behavior tick, where no VM call is active, capped
at 4,096 per drain (`kMaxMessagesPerDrain`). A handler that sends again is drained in the same
pass.

Outside tests, the bridges are the only code that registers a contact listener or an animation
listener. `DefaultApplication` installs them, and the editor embeds the same `DefaultApplication`,
so play, the Game tab and Simulate all reach behaviors through them.

What travels today: a contact gives each side `(other, point, normal, speed)`, a trigger `(other)`,
the other as the script `Entity`; `onAnimationFinished` gets the clip's guid and `onAnimationEvent`
the event's name.

## The proposal

Move the entry point down to `Foundation/Scene`, which physics, animation and the behavior
layer all link already.

```cpp
// foundation.scene
struct EntityEvent
{
    EntityHandle target;
    StringView name;          // "contactBegin", "animationFinished"
    Span<const Variant> args; // an entity among them is an EntityHandle
};

class IEntityEventSink
{
public:
    virtual ~IEntityEventSink() = default;
    /// Enqueue only: never call into a behavior from here.
    virtual void OnEntityEvent(Scene& scene, const EntityEvent& event) = 0;
};

// Scene
void AddEntityEventSink(IEntityEventSink& sink);
void RemoveEntityEventSink(IEntityEventSink& sink);
[[nodiscard]] bool WantsEntityEvents() const noexcept; // no sink: the publisher builds nothing
void PostEntityEvent(const EntityEvent& event);
```

**Publishers.** Physics posts `contactBegin`, `contactEnd`, `triggerEnter` and `triggerExit` to
each side of a contact. Animation posts `animationFinished` with the clip and `animationEvent`
with the event's name. Each checks `WantsEntityEvents()` first.

**The consumer.** `EntityBehaviorSystem` is a sink, added when the system joins its scene and
removed when it leaves. Its body is today's `EnqueueHandler` plus two conversions:

```cpp
void EntityBehaviorSystem::OnEntityEvent(scene::Scene&, const scene::EntityEvent& event)
{
    PendingMessage pending;
    pending.target = event.target;
    pending.handler = BuildMessageHandlerName(event.name); // "contactBegin" -> "onContactBegin"
    pending.args = ...; // event.args, each EntityHandle turned into the Entity facade
    m_messages.PushBack(Move(pending));
}
```

Native code can be a sink too, so this is not limited to scripted behaviors.

## Storage and delivery

The scene stores nothing. It holds a list of sinks, and `PostEntityEvent` calls them at once.
Each consumer keeps its own queue and delivers at its own safe point.

| | Today | Proposed |
|---|---|---|
| Publisher calls | its own listener interface | `scene.PostEntityEvent(target, name, args)` |
| In between | a bridge per domain | the scene's sink list |
| Lands in | `EnqueueHandler`, then `m_messages` | the same |
| Delivered by | `DrainMessages` at the behavior tick | the same |

So timing, ordering and the drain cap do not change.

## Not a second event bus

The run's `EventBus` and entity events answer different questions, and both stay:

- **`EventBus` (on the run):** a broadcast by name. It owns its queue and delivers at the run's
  drain to every subscriber of that name.
- **Entity events (on the scene):** addressed to one entity. Nothing is stored centrally.

## Alternatives not taken

- **Contacts on the run's `EventBus`.** A contact is addressed to two entities; on the bus every
  subscriber would get every contact and filter. The payload is one `Variant` and an
  `EntityContact` is larger than the 24 byte inline size, so each contact would allocate.
  Delivery would move from the behavior tick to the run's drain; whether that adds a frame was
  not checked.
- **A bridge inside each domain.** Physics' domain module would import `engine.behavior`, or the
  reverse. That is the dependency the bridges avoid, and it is still one bridge per domain.
- **The sink as a run service** (`run.Get<...>()`). Possible now that runs are formal. The scene
  is the better home: the events and the behavior system are both per scene, and an editing
  scene with no application run keeps working.
- **A queue owned by the scene**, delivered to sinks at a fixed point in the scene tick. It
  gives every consumer one order, at the cost of a second drain point, a second cap and a copy
  of every event's arguments. With one consumer it buys nothing. Revisit when a second consumer
  needs a shared order.

## What is removed

- `Code/Engine/Engine.Integration` and `Engine.Integration.Tests`
- `ScriptContactKind`, `BehaviorSubsystem::DeliverContact`, `DeliverContactSide`, `DeliverEvent`
- `m_contactBridge`, `m_animationBridge` and their install and uninstall calls in
  `DefaultApplication`
- `IContactListener` and `IAnimationListener` with their register and unregister functions;
  their tests (`OneShotTests.cpp`, `ContactBridgeTests.cpp`) assert on posted entity events
  instead
- The sentence in `EventBus.cppm`'s header that names `ScriptPhysicsContactBridge`

## Decided (2026-11-19)

1. **Event names are behavior API.** `contactBegin` gives `onContactBegin`, the rule `entity.send`
   follows (`BuildMessageHandlerName`). A test in `Code/Integration` pins the names, so renaming an
   event in a domain fails there before it breaks a game's behaviors.
2. **An entity travels as an argument**, an `EntityHandle` in its `Variant` like any other value,
   so an event carries none, one or several. `Variant::From` holds any copyable type without
   reflection, and only types registered in the global type registry reach the script backends,
   so no `EntityHandle` type appears on the script surface. The behavior layer turns each
   `EntityHandle` argument into the `Entity` facade, as `DeliverContactSide` does for a contact's
   other side today. (A field on `EntityEvent` for the other entity was considered and dropped:
   only contacts have one.)
3. **`IContactListener` and `IAnimationListener` go.** One way to hear these events: a native
   consumer is a sink.

## Order of work

Each step leaves the tree green.

1. The sink list and `PostEntityEvent` on `Scene`, with tests in `Scene.Tests`.
2. `EntityBehaviorSystem` becomes a sink; a backend-neutral test in `Code/Integration` posts an
   event and checks the handler ran.
3. Animation posts its two events; `ScriptAnimationBridge` is removed.
4. Physics posts contacts; `ScriptPhysicsContactBridge`, `ScriptContactKind` and
   `DeliverContact` are removed.
5. `Engine.Integration` is deleted, and the listener interfaces with it (decision 3).

## A cost this makes easy to fix

`PendingMessage` holds the handler as a `String` and the arguments as an `Array<Variant>`, so
each contact allocates twice per side today. The sink can skip an event whose target has no
behavior with that handler before building anything. Not measured.

## Risks

- **A sink that calls a behavior directly.** The rule is "enqueue only", as
  `IContactListener` states today. Assert that no VM call is entered from `PostEntityEvent`.
- **A sink removed during a post.** Sinks change only when a scene system joins or leaves;
  assert that this does not happen inside `PostEntityEvent`.
- **Sample projects.** Handler names in behaviors do not change, so no sample needs editing.
  The Integration.Mcp cook check confirms it; Lamplight (Pebble, Checkpoint, Exit) and PaperKid
  (Paper) use the handlers, so a playthrough of each checks delivery.
