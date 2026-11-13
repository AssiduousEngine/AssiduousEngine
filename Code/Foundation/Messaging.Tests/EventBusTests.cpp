// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The native event bus (foundation.messaging), exercised with no scripting at all, because a
// C++-only game has to be able to publish and subscribe with native callbacks.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.messaging;

using namespace foundation::core;
using namespace foundation::messaging;

TEST_CASE("event bus: publish is deferred; Drain delivers the payload to a native subscriber")
{
    EventBus bus;
    f64 got = 0.0;
    int calls = 0;
    (void)bus.Subscribe(StringHash(u8"score"),
                        [&](const Variant& v)
                        {
                            ++calls;
                            if (const f64* n = v.TryGet<f64>())
                            {
                                got = *n;
                            }
                        });
    bus.Publish(StringHash(u8"score"), Variant::From<f64>(7.0));
    CHECK(calls == 0); // deferred: nothing fires until Drain
    CHECK(bus.PendingCount() == 1u);

    bus.Drain();
    CHECK(calls == 1);
    CHECK(got == doctest::Approx(7.0));
    CHECK(bus.PendingCount() == 0u);
}

TEST_CASE("event bus: name filtering + subscription order + unsubscribe")
{
    EventBus bus;
    String order;
    (void)bus.Subscribe(StringHash(u8"A"), [&](const Variant&) { order += u8"1"; });
    (void)bus.Subscribe(StringHash(u8"A"), [&](const Variant&) { order += u8"2"; });
    const u32 handleB = bus.Subscribe(StringHash(u8"B"), [&](const Variant&) { order += u8"B"; });

    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"12")); // both A subscribers, in subscription order; never B

    bus.Unsubscribe(handleB);
    bus.Publish(StringHash(u8"B"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"12")); // B unsubscribed: no delivery
}

TEST_CASE("event bus: a handler's emit cascades in the SAME drain")
{
    EventBus bus;
    int aCalls = 0, bCalls = 0;
    (void)bus.Subscribe(StringHash(u8"A"),
                        [&](const Variant&)
                        {
                            ++aCalls;
                            bus.Publish(StringHash(u8"B"), Variant{}); // cascade
                        });
    (void)bus.Subscribe(StringHash(u8"B"), [&](const Variant&) { ++bCalls; });

    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(aCalls == 1);
    CHECK(bCalls == 1); // B delivered in the same Drain, not deferred to next frame
}

TEST_CASE("event bus: a runaway emit loop is bounded (Drain terminates, never hangs)")
{
    EventBus bus;
    int calls = 0;
    (void)bus.Subscribe(StringHash(u8"loop"),
                        [&](const Variant&)
                        {
                            ++calls;
                            bus.Publish(StringHash(u8"loop"), Variant{}); // re-emits forever
                        });
    bus.Publish(StringHash(u8"loop"), Variant{});
    bus.Drain(); // must terminate at the pass cap, not spin
    CHECK(calls >= 1);
    CHECK(calls <= static_cast<int>(kEventBusMaxDrainPasses)); // capped, not infinite
}

TEST_CASE("event bus: a handler that unsubscribes itself stays alive for its call, and the next still hears")
{
    EventBus bus;
    String order;
    u32 self = 0;
    self = bus.Subscribe(StringHash(u8"A"),
                         [&order, &bus, &self, tag = String(u8"1")](const Variant&)
                         {
                             bus.Unsubscribe(self);
                             order += tag; // its captures are still there after the unsubscribe
                         });
    (void)bus.Subscribe(StringHash(u8"A"), [&](const Variant&) { order += u8"2"; });
    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"122")); // the second event no longer reaches the first
    CHECK(bus.SubscriberCount() == 1u);
}

TEST_CASE("event bus: a subscription made by a handler joins after that event; many at once move nothing that runs")
{
    EventBus bus;
    int late = 0;
    String order;
    (void)bus.Subscribe(StringHash(u8"A"),
                        [&, tag = String(u8"a")](const Variant&)
                        {
                            for (int i = 0; i < 64; ++i) // enough to grow the list many times over
                            {
                                (void)bus.Subscribe(StringHash(u8"A"), [&late](const Variant&) { ++late; });
                            }
                            order += tag;
                        });
    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"aa"));
    CHECK(late == 64); // the first event's 64 hear the second; none hears the event it was made in
    CHECK(bus.SubscriberCount() == 129u);
}

TEST_CASE("event bus: Clear from a handler stops every delivery; a Drain from a handler leaves it to the running one")
{
    EventBus bus;
    String order;
    (void)bus.Subscribe(StringHash(u8"A"),
                        [&](const Variant&)
                        {
                            bus.Publish(StringHash(u8"B"), Variant{});
                            bus.Drain(); // nested: does nothing; B goes in the next pass
                            order += u8"A";
                        });
    (void)bus.Subscribe(StringHash(u8"B"), [&](const Variant&) { order += u8"B"; });
    bus.Publish(StringHash(u8"A"), Variant{});
    bus.Drain();
    CHECK(order == StringView(u8"AB"));

    EventBus closing;
    int after = 0;
    (void)closing.Subscribe(StringHash(u8"A"), [&closing](const Variant&) { closing.Clear(); });
    (void)closing.Subscribe(StringHash(u8"A"), [&after](const Variant&) { ++after; });
    closing.Publish(StringHash(u8"A"), Variant{});
    closing.Drain();
    CHECK(after == 0);
    CHECK(closing.SubscriberCount() == 0u);
}
