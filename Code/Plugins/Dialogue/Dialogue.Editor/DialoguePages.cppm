// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Plugins::Dialogue.Editor - `dialogue.editor`, the dialogue plugin's editor pages, which only the
/// editor loads.
///
/// DialogueListPage: a page over an asset that is a list of items, each edited apart. The items are
/// a list beside the grid, by name; the list's bar adds an item and removes the chosen one, and its
/// search keeps the items whose name holds the text. The grid shows the chosen item's fields from
/// reflection, its asset slots offering only assets of their kind. Every edit is one undo step;
/// Save writes the asset and requests a cook. The asset's own rows are not shown: the items are
/// the asset.
///   * DialogueCastPage (Data/Editor/Dialogue/CastPage.xml): a cast's speakers, by id.
///   * DialogueAssetsPage (Data/Editor/Dialogue/AssetsPage.xml): a dialogue assets list's entries,
///     by name.

module;

#include "Core/Prelude.h"

export module dialogue.editor;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace dialogue
{
    class DialogueListPage : public editor::app::AssetObjectPage
    {
    public:
        ~DialogueListPage() override;

        /// Chooses item `item` (-1: none): the grid shows it and the list selects it.
        void Choose(i32 item);
        /// Adds an item after the others and chooses it.
        void Add();
        /// Removes the chosen item; false when none is chosen.
        bool RemoveChosen();
        /// Keeps the items whose name holds `text` (empty: every one).
        void Search(StringView text);

        // === For tests ===
        [[nodiscard]] i32 Chosen() const noexcept { return m_chosen; }
        /// The list's rows, one a line.
        [[nodiscard]] String List() const;
        [[nodiscard]] foundation::gui::IPropertySource& ItemRows() noexcept;

    protected:
        /// `section` keys the grid's section of the chosen item, titled `title`.
        DialogueListPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                         foundation::content::Instance& instance, const TypeInfo& assetType, StringView markupPath,
                         StringView title);

        /// Builds the list and binds the first item; a derived page's constructor calls it once its
        /// own state is set, since it asks the derived page.
        void Start();

        [[nodiscard]] virtual const TypeInfo& ItemType() const = 0;
        [[nodiscard]] virtual usize ItemCount() const = 0;
        [[nodiscard]] virtual void* ItemAt(usize item) = 0;
        /// The item's name in the list.
        [[nodiscard]] virtual String ItemLabel(usize item) const = 0;
        /// Adds a new item to the asset (inside an undo step).
        virtual void AddItem(ISerializable& asset) = 0;
        /// Removes item `item` from the asset (inside an undo step).
        virtual void RemoveItem(ISerializable& asset, usize item) = 0;

        void OnObjectChanged() override;

    private:
        void Bind();
        void ShowList();

        String m_title;
        UniquePtr<foundation::gui::ReflectedPropertySource> m_values; // the chosen item's rows
        UniquePtr<foundation::gui::TreeModel> m_model;
        Array<i32> m_rows; // the item each list row stands for, by node id
        i32 m_chosen = -1;
        bool m_showing = false; // set while code makes a choice, so it is not taken as the user's
        String m_search;
        editor::app::XmlPropertyTextFormat m_format;
        foundation::gui::TreeView* m_list = nullptr;
    };

    class DialogueCastPage final : public DialogueListPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/Dialogue/CastPage.xml";

        DialogueCastPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                         foundation::content::Instance& instance);

    protected:
        [[nodiscard]] const TypeInfo& ItemType() const override;
        [[nodiscard]] usize ItemCount() const override;
        [[nodiscard]] void* ItemAt(usize item) override;
        [[nodiscard]] String ItemLabel(usize item) const override;
        void AddItem(ISerializable& asset) override;
        void RemoveItem(ISerializable& asset, usize item) override;
    };

    class DialogueAssetsPage final : public DialogueListPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/Dialogue/AssetsPage.xml";

        DialogueAssetsPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                           foundation::content::Instance& instance);

    protected:
        [[nodiscard]] const TypeInfo& ItemType() const override;
        [[nodiscard]] usize ItemCount() const override;
        [[nodiscard]] void* ItemAt(usize item) override;
        [[nodiscard]] String ItemLabel(usize item) const override;
        void AddItem(ISerializable& asset) override;
        void RemoveItem(ISerializable& asset, usize item) override;
    };

    /// Adds the dialogue's pages to the editor's page registry; the keys to remove them by.
    [[nodiscard]] Array<u64> RegisterDialoguePages(editor::EditorContext& context,
                                                   const editor::app::AssetPageServicesSource& services);
}
