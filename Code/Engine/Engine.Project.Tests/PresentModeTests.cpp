// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The present mode a project's window starts in: kept by a save and a copy, and vsync for a
// manifest saved before the setting.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.xml.serialization;
import engine.project;

using namespace foundation::core;
namespace project = engine::project;

namespace
{
    [[nodiscard]] String Written(project::ProjectSettings& settings)
    {
        MemoryStream buffer;
        UniquePtr<SerializerContext> ctx = foundation::xml::XmlSerializerFactory()(buffer, SerializeMode::Write);
        REQUIRE(ctx);
        BeginVersionedPayload(*ctx->serializer, project::ProjectSettings::StaticType());
        settings.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
        const auto bytes = buffer.Bytes();
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()));
    }

    // The present mode a manifest reads as.
    [[nodiscard]] project::PresentMode Read(StringView xml)
    {
        MemoryStream buffer;
        (void)buffer.Write(reinterpret_cast<const u8*>(xml.Data()), xml.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        project::ProjectSettings settings;
        UniquePtr<SerializerContext> ctx = foundation::xml::XmlSerializerFactory()(buffer, SerializeMode::Read);
        REQUIRE(ctx);
        BeginVersionedPayload(*ctx->serializer, project::ProjectSettings::StaticType());
        settings.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        CHECK(ctx->serializer->IsOk());
        return settings.presentMode;
    }
}

TEST_CASE("project: the present mode is vsync by default, and a save and a copy keep another")
{
    project::ProjectSettings settings;
    CHECK(settings.presentMode == project::PresentMode::Vsync);

    settings.presentMode = project::PresentMode::Immediate;
    const String xml = Written(settings);
    CHECK(xml.AsView().Find(u8"presentMode") != StringView::kNotFound);
    CHECK(Read(xml.AsView()) == project::PresentMode::Immediate);

    project::ProjectSettings copy;
    REQUIRE(project::CopyProjectSettings(settings, copy).IsOk());
    CHECK(copy.presentMode == project::PresentMode::Immediate);
}

TEST_CASE("project: a manifest saved before the present mode reads vsync")
{
    project::ProjectSettings settings;
    settings.presentMode = project::PresentMode::Mailbox;
    String xml = Written(settings);
    // The same manifest without the setting, as one saved before it was.
    const usize at = xml.AsView().Find(u8"presentMode");
    REQUIRE(at != StringView::kNotFound);
    const usize lineStart = xml.AsView().SubStr(0, at).FindLast(u8'\n') + 1;
    const usize lineEnd = xml.AsView().Find(u8'\n', at);
    REQUIRE(lineEnd != StringView::kNotFound);
    String older(xml.AsView().SubStr(0, lineStart));
    older.Append(xml.AsView().SubStr(lineEnd + 1, xml.Size() - lineEnd - 1));
    CHECK(older.AsView().Find(u8"presentMode") == StringView::kNotFound);
    CHECK(Read(older.AsView()) == project::PresentMode::Vsync);
}
