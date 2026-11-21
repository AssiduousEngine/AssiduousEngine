// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The dialogue's editor pages and their factories.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.editor;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import dialogue.engine;
import dialogue.pipeline;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace dialogue
{
    namespace
    {
        constexpr StringView kItemSection = u8"item";

        // The first of `base`, `base_2`, `base_3`... that `taken` does not answer true for.
        template <typename Taken>
        [[nodiscard]] String FreeName(StringView base, const Taken& taken)
        {
            if (!taken(base))
            {
                return String(base);
            }
            for (usize n = 2;; ++n)
            {
                String name = Format(u8"{}_{}", base, n);
                if (!taken(name.AsView()))
                {
                    return name;
                }
            }
        }
    }

    // ---- the list beside the grid -------------------------------------------------------------

    DialogueListPage::DialogueListPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                                       foundation::content::Instance& instance, const TypeInfo& assetType,
                                       StringView markupPath, StringView title)
        : editor::app::AssetObjectPage(context, Move(services), instance, assetType, markupPath), m_title(title),
          m_format(context.Allocator())
    {
    }

    void DialogueListPage::Start()
    {
        HideAssetRows(); // the asset is its items: the grid shows the chosen one
        DialogueListPage* self = this;
        m_model = MakeUnique<gui::TreeModel>(Allocator());
        m_list = Find<gui::TreeView>(u8"dialogue-list");
        if (auto* add = Find<gui::ButtonBase>(u8"dialogue-add"))
        {
            add->SetOnClick([self]() { self->Add(); });
        }
        if (auto* remove = Find<gui::ButtonBase>(u8"dialogue-remove"))
        {
            remove->SetOnClick([self]() { (void)self->RemoveChosen(); });
        }
        if (auto* search = Find<gui::TextField>(u8"dialogue-search"))
        {
            search->SetOnTextChanging([self](StringView text) { self->Search(text); });
        }
        if (m_list != nullptr)
        {
            m_list->SetModel(m_model.Get());
            m_list->SetOnSelectionChanged([self](gui::ModelIndex index)
                                          {
                                              if (self->m_showing || !index.IsValid())
                                              {
                                                  return;
                                              }
                                              const usize node = static_cast<usize>(index.InternalId);
                                              if (node < self->m_rows.Size())
                                              {
                                                  self->Choose(self->m_rows[node]);
                                              }
                                          });
        }

        // The chosen item's rows come from reflection, and each edit is one undo step.
        m_values = MakeUnique<gui::ReflectedPropertySource>(Allocator(), Allocator());
        m_values->SetTextFormat(&m_format);
        m_values->SetOnChange([self](const gui::ReflectedChange& change)
                              {
                                  const i32 item = self->m_chosen;
                                  (void)self->Commands().Execute(UniquePtr<editor::IEditorCommand>(
                                      editor::EditorRootAllocator().New<editor::app::ReflectedEditCommand>(
                                          self->ItemType(),
                                          [self, item]() -> void*
                                          {
                                              return item >= 0 && static_cast<usize>(item) < self->ItemCount()
                                                         ? self->ItemAt(static_cast<usize>(item))
                                                         : nullptr;
                                          },
                                          change, self->m_format, u8"edit_dialogue",
                                          [self]()
                                          {
                                              // A name may have changed: the list shows it.
                                              self->m_values->Refresh();
                                              self->ShowList();
                                          }),
                                      editor::EditorRootAllocator()));
                              });
        AddSection(kItemSection, m_title.AsView(), *m_values);
        Choose(ItemCount() > 0 ? 0 : -1);
    }

    DialogueListPage::~DialogueListPage()
    {
        if (m_list != nullptr)
        {
            m_list->SetModel(nullptr);
        }
    }

    gui::IPropertySource& DialogueListPage::ItemRows() noexcept { return *m_values; }

    void DialogueListPage::Bind()
    {
        if (m_chosen < 0 || static_cast<usize>(m_chosen) >= ItemCount())
        {
            m_values->SetObjects(Span<const Instance>{});
            return;
        }
        // The item's address is read again each time: the list moves as items come and go.
        const Instance item(ItemAt(static_cast<usize>(m_chosen)), &ItemType());
        m_values->SetObjects(Span<const Instance>(&item, 1u));
    }

    void DialogueListPage::Choose(i32 item)
    {
        m_chosen = item;
        Bind();
        ShowList();
    }

    void DialogueListPage::ShowList()
    {
        m_showing = true;
        m_model->Clear();
        m_rows.Clear();
        i32 chosen = -1;
        for (usize i = 0; i < ItemCount(); ++i)
        {
            const String label = ItemLabel(i);
            if (!m_search.IsEmpty() && !label.AsView().ContainsIgnoreCase(m_search.AsView()))
            {
                continue;
            }
            const i32 node = m_model->AddNode(gui::kTreeModelRoot, label.AsView());
            m_rows.PushBack(static_cast<i32>(i));
            if (static_cast<i32>(i) == m_chosen)
            {
                chosen = node;
            }
        }
        if (m_list != nullptr && chosen >= 0)
        {
            const gui::ItemId id = static_cast<gui::ItemId>(chosen);
            m_list->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
        }
        m_showing = false;
    }

    String DialogueListPage::List() const
    {
        String out;
        for (const i32 item : m_rows)
        {
            if (!out.IsEmpty())
            {
                out.Append(u8'\n');
            }
            out.Append(ItemLabel(static_cast<usize>(item)).AsView());
        }
        return out;
    }

    void DialogueListPage::OnObjectChanged()
    {
        // A whole edit, or its undo: the chosen item may be gone, or may have moved.
        if (m_chosen >= static_cast<i32>(ItemCount()))
        {
            m_chosen = static_cast<i32>(ItemCount()) - 1;
        }
        Bind();
        ShowList();
    }

    void DialogueListPage::Add()
    {
        if (ObjectAs<ISerializable>() == nullptr)
        {
            return;
        }
        DialogueListPage* self = this;
        EditWhole(u8"edit_dialogue", [self](ISerializable& asset) { self->AddItem(asset); });
        Choose(static_cast<i32>(ItemCount()) - 1);
    }

    bool DialogueListPage::RemoveChosen()
    {
        if (ObjectAs<ISerializable>() == nullptr || m_chosen < 0 || static_cast<usize>(m_chosen) >= ItemCount())
        {
            return false;
        }
        const usize item = static_cast<usize>(m_chosen);
        DialogueListPage* self = this;
        EditWhole(u8"edit_dialogue", [self, item](ISerializable& asset) { self->RemoveItem(asset, item); });
        return true;
    }

    void DialogueListPage::Search(StringView text)
    {
        m_search = String(Trim(text));
        ShowList();
    }

    // ---- the cast -------------------------------------------------------------------------------

    DialogueCastPage::DialogueCastPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                                       foundation::content::Instance& instance)
        : DialogueListPage(context, Move(services), instance, DialogueCastAsset::StaticType(), kPath, u8"Speaker")
    {
        Start();
    }

    const TypeInfo& DialogueCastPage::ItemType() const { return TypeOf<DialogueSpeaker>(); }

    usize DialogueCastPage::ItemCount() const
    {
        const auto* cast = ObjectAs<DialogueCastAsset>();
        return cast != nullptr ? cast->speakers.Size() : 0;
    }

    void* DialogueCastPage::ItemAt(usize item) { return &ObjectAs<DialogueCastAsset>()->speakers[item]; }

    String DialogueCastPage::ItemLabel(usize item) const
    {
        const DialogueSpeaker& speaker = ObjectAs<DialogueCastAsset>()->speakers[item];
        if (speaker.shownName.IsEmpty() || speaker.shownName == speaker.id)
        {
            return speaker.id;
        }
        return Format(u8"{}   {}", speaker.id, speaker.shownName);
    }

    void DialogueCastPage::AddItem(ISerializable& asset)
    {
        Array<DialogueSpeaker>& speakers = static_cast<DialogueCastAsset&>(asset).speakers;
        DialogueSpeaker speaker;
        speaker.id = FreeName(u8"speaker",
                              [&speakers](StringView id)
                              {
                                  for (const DialogueSpeaker& other : speakers)
                                  {
                                      if (other.id == id)
                                      {
                                          return true;
                                      }
                                  }
                                  return false;
                              });
        speaker.moods.PushBack(DialogueMood{String(u8"neutral"), Guid{}}); // every speaker has a default mood
        speakers.PushBack(Move(speaker));
    }

    void DialogueCastPage::RemoveItem(ISerializable& asset, usize item)
    {
        Array<DialogueSpeaker>& speakers = static_cast<DialogueCastAsset&>(asset).speakers;
        if (item < speakers.Size())
        {
            speakers.RemoveAt(item);
        }
    }

    // ---- the dialogue assets --------------------------------------------------------------------

    DialogueAssetsPage::DialogueAssetsPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                                           foundation::content::Instance& instance)
        : DialogueListPage(context, Move(services), instance, DialogueAssetListAsset::StaticType(), kPath, u8"Dialogue Asset")
    {
        Start();
    }

    const TypeInfo& DialogueAssetsPage::ItemType() const { return TypeOf<DialogueAssetEntry>(); }

    usize DialogueAssetsPage::ItemCount() const
    {
        const auto* list = ObjectAs<DialogueAssetListAsset>();
        return list != nullptr ? list->entries.Size() : 0;
    }

    void* DialogueAssetsPage::ItemAt(usize item) { return &ObjectAs<DialogueAssetListAsset>()->entries[item]; }

    String DialogueAssetsPage::ItemLabel(usize item) const { return ObjectAs<DialogueAssetListAsset>()->entries[item].name; }

    void DialogueAssetsPage::AddItem(ISerializable& asset)
    {
        Array<DialogueAssetEntry>& entries = static_cast<DialogueAssetListAsset&>(asset).entries;
        DialogueAssetEntry entry;
        entry.name = FreeName(u8"asset",
                              [&entries](StringView name)
                              {
                                  for (const DialogueAssetEntry& other : entries)
                                  {
                                      if (other.name == name)
                                      {
                                          return true;
                                      }
                                  }
                                  return false;
                              });
        entries.PushBack(Move(entry));
    }

    void DialogueAssetsPage::RemoveItem(ISerializable& asset, usize item)
    {
        Array<DialogueAssetEntry>& entries = static_cast<DialogueAssetListAsset&>(asset).entries;
        if (item < entries.Size())
        {
            entries.RemoveAt(item);
        }
    }

    // ---- registration ---------------------------------------------------------------------------

    namespace
    {
        // Makes `Page` over the asset type `Asset` with the page services as they are when it opens.
        template <typename Page, typename Asset>
        class PageFactory final : public editor::IEditorPageFactory
        {
        public:
            explicit PageFactory(const editor::app::AssetPageServicesSource& services) : m_services(&services) {}

            [[nodiscard]] const TypeInfo* PrimaryType() const override { return &Asset::StaticType(); }

            [[nodiscard]] UniquePtr<editor::EditorPage> CreatePage(editor::EditorContext& context,
                                                                   foundation::content::Instance& instance) override
            {
                IAllocator& allocator = editor::EditorRootAllocator();
                auto* page = allocator.New<Page>(context, m_services->Make(), instance);
                return UniquePtr<editor::EditorPage>(page, allocator);
            }

        private:
            const editor::app::AssetPageServicesSource* m_services;
        };
    }

    Array<u64> RegisterDialoguePages(editor::EditorContext& context, const editor::app::AssetPageServicesSource& services)
    {
        Array<u64> keys(context.Allocator());
        keys.PushBack(context.Pages().Register(
            MakeUnique<PageFactory<DialogueCastPage, DialogueCastAsset>>(context.Allocator(), services)));
        keys.PushBack(context.Pages().Register(
            MakeUnique<PageFactory<DialogueAssetsPage, DialogueAssetListAsset>>(context.Allocator(), services)));
        return keys;
    }
}
