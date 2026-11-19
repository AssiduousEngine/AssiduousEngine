// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A scene's entity events: a domain tells an entity's listeners something happened to it (a
// contact, an animation ending) through the scene, with no bridge between the domain and them.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    // Keeps what each post told it.
    class RecordingSink final : public IEntityEventSink
    {
    public:
        struct Heard
        {
            Scene* scene = nullptr;
            EntityHandle target;
            String name;
            usize argCount = 0;
            EntityHandle other;  // an entity argument, first
            f64 speed = 0.0;     // then a number
        };
        Array<Heard> heard;
        int* order = nullptr; // where this sink's turn is written, to check the order of sinks
        int turn = 0;

        void OnEntityEvent(Scene& scene, const EntityEvent& event) override
        {
            Heard h;
            h.scene = &scene;
            h.target = event.target;
            h.name = String(event.name);
            h.argCount = event.args.Size();
            if (event.args.Size() == 2u)
            {
                h.other = event.args[0].Get<EntityHandle>();
                h.speed = event.args[1].Get<f64>();
            }
            heard.PushBack(Move(h));
            if (order != nullptr)
                turn = ++*order;
        }
    };
}

TEST_CASE("entity events: a post reaches every sink at once, in the order they were added")
{
    Scene scene(DefaultAllocator(), u8"events");
    const EntityHandle pebble = scene.CreateEntity(u8"pebble");
    const EntityHandle floor = scene.CreateEntity(u8"floor");
    CHECK_FALSE(scene.WantsEntityEvents()); // no sink: a poster builds nothing

    RecordingSink first;
    RecordingSink second;
    int order = 0;
    first.order = &order;
    second.order = &order;
    scene.AddEntityEventSink(first);
    scene.AddEntityEventSink(second);
    scene.AddEntityEventSink(first); // once only
    CHECK(scene.WantsEntityEvents());

    // An entity travels as an argument like any other value.
    const Variant args[] = {Variant::From<EntityHandle>(floor), Variant::From<f64>(3.5)};
    scene.PostEntityEvent(EntityEvent{pebble, u8"contactBegin", Span<const Variant>(args, 2)});
    REQUIRE(first.heard.Size() == 1u);
    REQUIRE(second.heard.Size() == 1u);
    CHECK(first.turn == 1);
    CHECK(second.turn == 2);
    const RecordingSink::Heard& h = first.heard[0];
    CHECK(h.scene == &scene);
    CHECK(h.target == pebble);
    CHECK(h.name == u8"contactBegin");
    CHECK(h.argCount == 2u);
    CHECK(h.other == floor);
    CHECK(h.speed == 3.5);

    // Removed, a sink hears no more; the scene stores nothing for it meanwhile.
    scene.RemoveEntityEventSink(first);
    scene.PostEntityEvent(EntityEvent{pebble, u8"animationFinished", {}}); // no arguments at all
    CHECK(first.heard.Size() == 1u);
    REQUIRE(second.heard.Size() == 2u);
    CHECK(second.heard[1].argCount == 0u);
    scene.RemoveEntityEventSink(second);
    CHECK_FALSE(scene.WantsEntityEvents());
}
