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
import foundation.gui.code; // the text page's problems
import foundation.scene;
import foundation.run;
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

    // ---- the text page --------------------------------------------------------------------------

    DialogueTextPage::DialogueTextPage(editor::EditorContext& context, editor::app::AssetPageServices services,
                                       foundation::content::Instance& instance, const DialogueSettings& settings)
        : CodeAssetPage(context, Move(services), instance, DialogueAsset::StaticType(), kPath), m_settings(&settings)
    {
        m_model = MakeUnique<gui::TreeModel>(Allocator());
        m_outline = Find<gui::TreeView>(u8"dialogue-outline");
        if (m_outline != nullptr)
        {
            m_outline->SetModel(m_model.Get());
            DialogueTextPage* self = this;
            m_outline->SetOnSelectionChanged([self](gui::ModelIndex index)
                                             {
                                                 if (!self->m_showing && index.IsValid())
                                                 {
                                                     self->GoToConversation(static_cast<usize>(index.InternalId));
                                                 }
                                             });
        }
        m_previewButton = Find<gui::ButtonBase>(u8"dialogue-preview");
        m_previewState = Find<gui::Label>(u8"dialogue-preview-state");
        m_previewAnswers = Find<gui::UIWidget>(u8"dialogue-preview-answers");
        DialogueTextPage* self = this;
        if (m_previewButton != nullptr)
        {
            m_previewButton->SetOnClick([self]()
                                        {
                                            if (self->IsPreviewing())
                                            {
                                                self->StopPreview();
                                            }
                                            else
                                            {
                                                (void)self->StartPreview();
                                            }
                                        });
        }
        if (auto* advance = Find<gui::ButtonBase>(u8"dialogue-preview-advance"))
        {
            advance->SetOnClick([self]()
                                {
                                    if (self->m_preview)
                                    {
                                        (void)self->m_preview->Advance();
                                    }
                                });
        }
        if (auto* done = Find<gui::ButtonBase>(u8"dialogue-preview-done"))
        {
            done->SetOnClick([self]()
                             {
                                 if (self->m_preview)
                                 {
                                     (void)self->m_preview->Done();
                                 }
                             });
        }
        OpenCode(u8"dialogue");
        ShowPreview();
    }

    DialogueTextPage::~DialogueTextPage()
    {
        StopPreview();
        if (m_outline != nullptr)
        {
            m_outline->SetModel(nullptr);
        }
    }

    Array<editor::app::CodeProblem> DialogueTextPage::CheckCode(StringView text)
    {
        Array<editor::app::CodeProblem> problems;
        m_conversations.Clear();
        editor::EditorProject* project = Context().Project();
        if (project == nullptr)
        {
            return problems;
        }
        const DialogueSourceReader read = [project](StringView fileName, String& out)
        {
            Result<Array<byte>> bytes = ReadFile(project->SourceFile(fileName).AsView());
            if (!bytes.HasValue())
            {
                return false;
            }
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return true;
        };
        DialogueParse parse = CheckDialogueText(text, *m_settings, InstanceId(), project->SourceDb(), read, Allocator());
        m_builds = parse.Ok();
        for (const DialogueProblem& problem : parse.problems)
        {
            problems.PushBack(editor::app::CodeProblem{problem.warning ? gui::CodeSeverity::Warning : gui::CodeSeverity::Error,
                                                       problem.message, String{}, problem.line > 0 ? problem.line - 1 : 0, 0,
                                                       false});
        }
        for (const DialogueConversation& conversation : parse.conversations)
        {
            // Its header is the line before its first statement's, or its End's when it has none.
            u32 line = 0;
            for (const DialogueStep& step : conversation.steps)
            {
                if (step.sourceLine > 0 && (line == 0 || step.sourceLine < line))
                {
                    line = step.sourceLine;
                }
            }
            m_conversations.PushBack(Entry{conversation.id, line > 1 ? line - 1 : line});
        }
        m_built = Move(parse.conversations);
        m_builtLines = Move(parse.lines);
        return problems;
    }

    void DialogueTextPage::OnCodeChecked()
    {
        m_showing = true;
        m_model->Clear();
        for (const Entry& entry : m_conversations)
        {
            (void)m_model->AddNode(gui::kTreeModelRoot, entry.id.AsView());
        }
        m_showing = false;
    }

    String DialogueTextPage::CleanSummary() const
    {
        return m_conversations.Size() == 1 ? String(u8"No problems: 1 conversation")
                                           : Format(u8"No problems: {} conversations", m_conversations.Size());
    }

    void DialogueTextPage::GoToConversation(usize index)
    {
        if (index < m_conversations.Size())
        {
            m_chosen = index;
            ShowLine(m_conversations[index].line, 1);
        }
    }

    bool DialogueTextPage::IsOpen(const editor::ISceneEditorPage* page) const
    {
        for (const UniquePtr<editor::EditorPage>& open : Context().OpenPages())
        {
            if (open->Service<editor::ISceneEditorPage>() == page)
            {
                return true;
            }
        }
        return false;
    }

    bool DialogueTextPage::StartPreview(editor::ISceneEditorPage* scenePage)
    {
        StopPreview();
        if (scenePage == nullptr)
        {
            for (const UniquePtr<editor::EditorPage>& open : Context().OpenPages())
            {
                if (auto* found = open->Service<editor::ISceneEditorPage>())
                {
                    scenePage = found;
                    break;
                }
            }
        }
        if (scenePage == nullptr)
        {
            Context().Notify(editor::NoticeKind::Warning, u8"Preview: open a scene to play the conversation in");
            ShowPreview();
            return false;
        }
        m_startedSimulation = !scenePage->IsSimulating();
        if (m_startedSimulation)
        {
            scenePage->StartSimulation();
        }
        foundation::scene::Scene& scene = scenePage->EditContext().Scene();
        foundation::run::IRun* run = scene.Run();
        if (run == nullptr || !scenePage->IsSimulating())
        {
            Context().Notify(editor::NoticeKind::Warning, u8"Preview: the scene did not start");
            if (m_startedSimulation)
            {
                scenePage->StopSimulation();
            }
            m_startedSimulation = false;
            ShowPreview();
            return false;
        }
        m_previewPage = scenePage;
        if (!PlayPreview(scene, *run))
        {
            StopPreview();
            return false;
        }
        return true;
    }

    bool DialogueTextPage::PlayPreview(foundation::scene::Scene& scene, foundation::run::IRun& run)
    {
        m_preview.Reset();
        CheckNow(); // the text as it reads now
        if (!m_builds || m_built.IsEmpty())
        {
            Context().Notify(editor::NoticeKind::Warning, u8"Preview: the dialogue has errors, or no conversation");
            ShowPreview();
            return false;
        }
        // The text as it reads now, as the file the runner plays.
        RefPtr<DialogueResource> file = MakeRef<DialogueResource>(Allocator());
        file->conversations = m_built;
        file->lines = m_builtLines;
        if (const auto* asset = ObjectAs<DialogueAsset>())
        {
            file->cast = asset->cast;
        }
        m_preview = MakeUnique<DialogueRunService>(Allocator(), *m_settings, run);
        m_preview->Host().ShowStockBox(true);
        m_preview->Host().SetDialogue(InstanceId(), file);
        m_preview->Host().SetScene(&scene);
        const StringView conversation =
            m_chosen < m_conversations.Size() ? m_conversations[m_chosen].id.AsView() : StringView{};
        if (!m_preview->Play(InstanceId(), conversation))
        {
            m_preview.Reset();
            ShowPreview();
            return false;
        }
        ShowPreview();
        return true;
    }

    void DialogueTextPage::StopPreview()
    {
        const bool was = m_preview.Get() != nullptr;
        m_preview.Reset(); // the box and the fade go with its host
        if (m_startedSimulation && m_previewPage != nullptr && IsOpen(m_previewPage))
        {
            m_previewPage->StopSimulation();
        }
        m_previewPage = nullptr;
        m_startedSimulation = false;
        if (was)
        {
            ShowPreview();
        }
    }

    void DialogueTextPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        CodeAssetPage::OnUpdate(host, dt);
        if (!m_preview)
        {
            return;
        }
        if (m_previewPage != nullptr && (!IsOpen(m_previewPage) || !m_previewPage->IsSimulating()))
        {
            m_startedSimulation = false; // it closed or stopped on its own: nothing of it to stop
            StopPreview();
            return;
        }
        if (m_previewPage == nullptr || !m_previewPage->IsPaused())
        {
            m_preview->Update(dt);
        }
        if (m_preview && (m_preview->Waiting() != m_shownWait || m_preview->Box().AnswerCount() != m_shownAnswers))
        {
            ShowPreview();
        }
    }

    void DialogueTextPage::OnClose()
    {
        StopPreview();
        CodeAssetPage::OnClose();
    }

    String DialogueTextPage::PreviewState() const
    {
        if (!m_preview)
        {
            return String(u8"Not playing");
        }
        switch (m_preview->Waiting())
        {
        case DialogueWait::Line: return String(u8"A line shows: Advance goes on");
        case DialogueWait::Choice: return String(u8"A choice: take an answer");
        case DialogueWait::Time: return String(u8"Waiting");
        case DialogueWait::Staging: return String(u8"Staging: waiting for it to end");
        case DialogueWait::Command: return String(u8"A command waits: Done goes on");
        default: break;
        }
        return m_preview->IsPlaying() ? String(u8"Playing") : String(u8"Ended");
    }

    void DialogueTextPage::ShowPreview()
    {
        if (m_previewButton != nullptr)
        {
            m_previewButton->SetChecked(IsPreviewing());
        }
        if (m_previewState != nullptr)
        {
            m_previewState->SetText(PreviewState().AsView());
        }
        m_shownWait = m_preview ? m_preview->Waiting() : DialogueWait::Nothing;
        m_shownAnswers = m_preview ? m_preview->Box().AnswerCount() : 0;
        if (m_previewAnswers == nullptr)
        {
            return;
        }
        m_previewAnswers->RemoveAllChildren();
        for (usize i = 0; i < m_shownAnswers; ++i)
        {
            auto* button = Cast<gui::Button>(gui::Node::FromHandle(m_previewAnswers->AddElement(u8"Button")));
            if (button == nullptr)
            {
                continue;
            }
            button->SetText(m_preview->Box().AnswerText(i).AsView());
            DialogueTextPage* self = this;
            const i32 index = static_cast<i32>(i);
            button->SetOnClick([self, index]()
                               {
                                   if (self->m_preview)
                                   {
                                       (void)self->m_preview->Choose(index);
                                   }
                               });
        }
    }

    String DialogueTextPage::Outline() const
    {
        String out;
        for (const Entry& entry : m_conversations)
        {
            out.Append(entry.id.AsView());
            out.Append(u8'\n');
        }
        return out;
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

    namespace
    {
        // The text page, which also takes the plugin's settings.
        class TextPageFactory final : public editor::IEditorPageFactory
        {
        public:
            TextPageFactory(const editor::app::AssetPageServicesSource& services, const DialogueSettings& settings)
                : m_services(&services), m_settings(&settings)
            {
            }

            [[nodiscard]] const TypeInfo* PrimaryType() const override { return &DialogueAsset::StaticType(); }

            [[nodiscard]] UniquePtr<editor::EditorPage> CreatePage(editor::EditorContext& context,
                                                                   foundation::content::Instance& instance) override
            {
                IAllocator& allocator = editor::EditorRootAllocator();
                auto* page = allocator.New<DialogueTextPage>(context, m_services->Make(), instance, *m_settings);
                return UniquePtr<editor::EditorPage>(page, allocator);
            }

        private:
            const editor::app::AssetPageServicesSource* m_services;
            const DialogueSettings* m_settings;
        };
    }

    Array<u64> RegisterDialoguePages(editor::EditorContext& context, const editor::app::AssetPageServicesSource& services,
                                     const DialogueSettings& settings)
    {
        Array<u64> keys(context.Allocator());
        keys.PushBack(context.Pages().Register(MakeUnique<TextPageFactory>(context.Allocator(), services, settings)));
        keys.PushBack(context.Pages().Register(
            MakeUnique<PageFactory<DialogueCastPage, DialogueCastAsset>>(context.Allocator(), services)));
        keys.PushBack(context.Pages().Register(
            MakeUnique<PageFactory<DialogueAssetsPage, DialogueAssetListAsset>>(context.Allocator(), services)));
        return keys;
    }
}
