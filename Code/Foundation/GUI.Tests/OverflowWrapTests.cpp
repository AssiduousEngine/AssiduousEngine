// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - overflow-wrap: a word wider than its whole line breaks where the line ends, between
// graphemes, instead of running out of its box. A word that fits on a line of its own moves to the
// next line first, as usual. The tests use DejaVu Sans Mono, where every character has the same
// width.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.gui;

using namespace foundation::core;
using namespace foundation::gui;
namespace fonts = foundation::fonts;

namespace
{
    struct DejaVu
    {
        fonts::TrueTypeFontService service{DefaultAllocator()};
        DejaVu()
        {
            fonts::TrueTypeFonts::Initialize();
            const String root = foundation::vfs::FindDataRoot();
            REQUIRE(service.LoadFont(u8"DejaVu", foundation::vfs::DataPath(root.AsView(), u8"Samples/Fonts/dejavu/DejaVuSansMono.ttf").AsView()) ==
                    fonts::FontLoadResult::Success);
        }
        fonts::CachedFont* Font() { return service.GetFont(u8"DejaVu", 16.0f); }
        f32 Advance() { return Font()->font->MeasureString(StringView(u8"a")); }
    };
    TextLayout Lay(DejaVu& font, StringView text, f32 maxWidth, bool breakWords)
    {
        TextLayoutStyle style;
        style.Font = font.Font();
        style.Family = String(u8"DejaVu");
        style.Size = 16.0f;
        style.MaxWidth = maxWidth;
        style.BreakWords = breakWords;
        TextDocument document;
        TextRun run;
        run.Text = String(text);
        document.Runs.PushBack(Move(run));
        return LayOutText(document, style);
    }
    // Checks that every line fits within `width` and that together the lines cover the text, each
    // starting where the last ended.
    void CheckCovers(const TextLayout& layout, usize size, f32 width)
    {
        REQUIRE(!layout.Lines.IsEmpty());
        CHECK(layout.Lines[0].Start == 0u);
        for (usize i = 0; i < layout.Lines.Size(); ++i)
        {
            CHECK(layout.Lines[i].Width <= width + 0.01f);
            if (i + 1 < layout.Lines.Size())
                CHECK(layout.Lines[i + 1].Start == layout.Lines[i].End);
        }
        CHECK(layout.Lines.Back().End == size);
    }
}

TEST_CASE("overflow wrap: without it a long word runs out of its line")
{
    DejaVu font;
    const f32 width = font.Advance() * 5.5f;
    const TextLayout layout = Lay(font, StringView(u8"abcdefghijklmnop"), width, false);
    REQUIRE(layout.Lines.Size() == 1u);
    CHECK(layout.Lines[0].Width > width);
}

TEST_CASE("overflow wrap: break-word breaks a word wider than its line where the line ends")
{
    DejaVu font;
    const f32 width = font.Advance() * 5.5f; // five characters to a line
    const TextLayout layout = Lay(font, StringView(u8"abcdefghijklmnop"), width, true);
    REQUIRE(layout.Lines.Size() == 4u);
    CHECK(layout.Lines[0].End == 5u);
    CheckCovers(layout, 16, width);
}

TEST_CASE("overflow wrap: a long word after a short one starts its own line, then breaks")
{
    DejaVu font;
    const f32 width = font.Advance() * 5.5f;
    const TextLayout layout = Lay(font, StringView(u8"hi abcdefghij"), width, true);
    REQUIRE(layout.Lines.Size() == 3u);
    CHECK(layout.Lines[0].End == 3u); // "hi " alone
    CHECK(layout.Lines[1].End == 8u);
    CheckCovers(layout, 13, width);
}

TEST_CASE("overflow wrap: a line narrower than a character still holds one, and a grapheme is not split")
{
    DejaVu font;
    const TextLayout narrow = Lay(font, StringView(u8"abc"), font.Advance() * 0.5f, true);
    CHECK(narrow.Lines.Size() == 3u);
    // e and a combining acute: one grapheme, two characters.
    const TextLayout marks = Lay(font, StringView(u8"aéb"), font.Advance() * 0.5f, true);
    REQUIRE(marks.Lines.Size() == 3u);
    CHECK(marks.Lines[1].Start == 1u);
    CHECK(marks.Lines[1].End == 4u);
}

TEST_CASE("overflow wrap: inherits from a sheet, and normal turns it off")
{
    DejaVu font;
    RefPtr<SceneRoot> root = MakeRef<SceneRoot>(DefaultAllocator(), MakeRef<GUIContext>(DefaultAllocator()));
    RefPtr<Panel> panel = MakeRef<Panel>(DefaultAllocator());
    RefPtr<Label> label = MakeRef<Label>(DefaultAllocator());
    root->AddChild(panel.Get());
    panel->AddChild(label.Get());
    panel->AddClass(StringView(u8"page"));
    label->SetFont(font.Font());
    label->AddClass(StringView(u8"text"));
    StyleManager styles;
    styles.SetStyleSheet(CSSParser::Parse(StringView(u8".page { overflow-wrap: break-word; }")));
    styles.ApplyTree(*root.Get());
    CHECK(label->GetTextStyle().BreakWords);
    CHECK(label->GetTextLayout().GetBreakWords());
    styles.SetStyleSheet(CSSParser::Parse(StringView(u8".page { overflow-wrap: anywhere; } .text { overflow-wrap: normal; }")));
    styles.ApplyTree(*root.Get());
    CHECK_FALSE(label->GetTextStyle().BreakWords);
}

namespace
{
    // A font every character of which is 10 wide, CJK included (no font in the repository has CJK
    // glyphs, and one without them measures them 0, so nothing would ever wrap).
    class EvenFont final : public fonts::IFont
    {
    public:
        [[nodiscard]] StringView FamilyName() const override { return u8"Even"; }
        [[nodiscard]] Span<const u8> SourceData() const override { return {}; }
        [[nodiscard]] fonts::FontMetrics Metrics() const override { return fonts::FontMetrics(12.0f, -4.0f, 0.0f, 16.0f, 1.0f); }
        [[nodiscard]] f32 PixelHeight() const override { return 16.0f; }
        [[nodiscard]] fonts::GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            fonts::GlyphInfo info;
            info.codepoint = codepoint;
            info.glyphIndex = codepoint;
            info.advanceWidth = 10.0f;
            return info;
        }
        [[nodiscard]] f32 GetKerning(i32, i32) const override { return 0.0f; }
        [[nodiscard]] bool HasGlyph(i32) const override { return true; }
        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            Array<fonts::GlyphPosition> positions;
            return MeasureString(text, positions);
        }
        [[nodiscard]] f32 MeasureString(StringView text, Array<fonts::GlyphPosition>& out) const override
        {
            f32 x = 0.0f;
            usize at = 0;
            while (at < text.Size())
            {
                const usize start = at;
                const u32 c = DecodeUtf8(text, at);
                fonts::GlyphPosition position;
                position.cluster = static_cast<i32>(start);
                position.clusterLength = static_cast<i32>(at - start);
                position.x = x;
                position.advance = 10.0f;
                position.glyphInfo = GetGlyphInfo(static_cast<i32>(c));
                out.PushBack(position);
                x += 10.0f;
            }
            return x;
        }
    };

    TextLayout LayEven(fonts::CachedFont& font, StringView text, f32 maxWidth)
    {
        TextLayoutStyle style;
        style.Font = &font;
        style.Family = String(u8"Even");
        style.Size = 16.0f;
        style.MaxWidth = maxWidth;
        TextDocument document;
        TextRun run;
        run.Text = String(text);
        document.Runs.PushBack(Move(run));
        return LayOutText(document, style);
    }
}

TEST_CASE("line breaks: CJK text wraps between its characters, and keeps its punctuation off a line's start")
{
    // Pieces ended only at spaces, so 150 characters of Chinese or Japanese were one word that
    // never wrapped and ran out of its box.
    fonts::CachedFont font(DefaultAllocator(), DefaultAllocator().New<EvenFont>(), nullptr);
    String text;
    for (int i = 0; i < 50; ++i)
    {
        text.Append(u8"漢字か");
    }
    const TextLayout layout = LayEven(font, text.AsView(), 105.0f); // ten characters to a line
    CHECK(layout.Lines.Size() == 15u);
    CheckCovers(layout, text.Size(), 105.0f);

    // "。" may not start a line: the break before it moves back one character.
    String punctuated;
    for (int i = 0; i < 10; ++i)
    {
        punctuated.Append(u8"漢");
    }
    punctuated.Append(u8"。字字");
    const TextLayout kept = LayEven(font, punctuated.AsView(), 105.0f);
    REQUIRE(kept.Lines.Size() == 2u);
    CHECK(punctuated.AsView().SubStr(kept.Lines[1].Start, 3) != StringView(u8"。"));
    CHECK(punctuated.AsView().SubStr(kept.Lines[1].Start, 3) == StringView(u8"漢"));

    // Latin text still breaks only at its spaces.
    CHECK(LayEven(font, StringView(u8"abcdefghijklmnopqrstuvwxyz"), 105.0f).Lines.Size() == 1u);
    CHECK_FALSE(IsLineBreakBetween(u8'a', u8'b'));
    CHECK(IsLineBreakBetween(0x6F22, 0x5B57));
    CHECK_FALSE(IsLineBreakBetween(0x300C, 0x6F22)); // nothing after an opening bracket
}
