// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The `Dialogue` facade and the notices' reflection for script.

module;

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module dialogue.engine;

import foundation.core;
import foundation.script;
import foundation.script.facades;
import engine.run; // SaveStoreHandle, what useSave takes

using namespace foundation::core;

namespace dialogue
{
    DialogueRunService* DialogueFacade::Resolve() { return foundation::script::CurrentService<DialogueRunService>(); }

    bool DialogueFacade::play(Guid dialogue) { return play(dialogue, String()); }

    bool DialogueFacade::play(Guid dialogue, String conversation)
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Play(dialogue, conversation.AsView());
    }

    bool DialogueFacade::isPlaying()
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->IsPlaying();
    }

    bool DialogueFacade::advance()
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Advance();
    }

    bool DialogueFacade::choose(i32 index)
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Choose(index);
    }

    bool DialogueFacade::done()
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Done();
    }

    void DialogueFacade::stop()
    {
        if (DialogueRunService* runner = Resolve())
        {
            runner->Stop();
        }
    }

    bool DialogueFacade::resume()
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Resume();
    }

    bool DialogueFacade::flag(String name)
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->Flag(name.AsView());
    }

    void DialogueFacade::setFlag(String name, bool on)
    {
        if (DialogueRunService* runner = Resolve())
        {
            runner->SetFlag(name.AsView(), on);
        }
    }

    void DialogueFacade::bind(String speaker, foundation::script::Entity entity)
    {
        if (DialogueRunService* runner = Resolve())
        {
            runner->Bind(speaker.AsView(), entity.Handle());
        }
    }

    bool DialogueFacade::useSave(engine::runtime::SaveStoreHandle store)
    {
        DialogueRunService* runner = Resolve();
        return runner != nullptr && runner->UseSave(store);
    }

    REFLECT_MEMBERS(DialogueFacade, "rtti::dialogue")
    {
        builder.Attribute("scriptName", "Dialogue");
        builder.Method<static_cast<bool (*)(Guid)>(&DialogueFacade::play)>("play", {"dialogue"});
        builder.Method<static_cast<bool (*)(Guid, String)>(&DialogueFacade::play)>("play", {"dialogue", "conversation"});
        builder.Method<&DialogueFacade::isPlaying>("isPlaying");
        builder.Method<&DialogueFacade::advance>("advance");
        builder.Method<&DialogueFacade::choose>("choose", {"index"});
        builder.Method<&DialogueFacade::done>("done");
        builder.Method<&DialogueFacade::stop>("stop");
        builder.Method<&DialogueFacade::resume>("resume");
        builder.Method<&DialogueFacade::flag>("flag", {"name"});
        builder.Method<&DialogueFacade::setFlag>("setFlag", {"name", "on"});
        builder.Method<&DialogueFacade::bind>("bind", {"speaker", "entity"});
        builder.Method<&DialogueFacade::useSave>("useSave", {"store"});
        builder.Constructor();
    }

    REFLECT_VALUE(DialogueLineNotice, "rtti::dialogue")
    {
        builder.Property<&DialogueLineNotice::dialogue>("dialogue");
        builder.Property<&DialogueLineNotice::conversation>("conversation");
        builder.Property<&DialogueLineNotice::speaker>("speaker");
        builder.Property<&DialogueLineNotice::shownName>("shownName");
        builder.Property<&DialogueLineNotice::mood>("mood");
        builder.Property<&DialogueLineNotice::portrait>("portrait");
        builder.Property<&DialogueLineNotice::text>("text");
        builder.Property<&DialogueLineNotice::lineId>("lineId");
    }

    REFLECT_VALUE(DialogueChoiceNotice, "rtti::dialogue")
    {
        builder.Property<&DialogueChoiceNotice::dialogue>("dialogue");
        builder.Property<&DialogueChoiceNotice::conversation>("conversation");
        builder.Method<&DialogueChoiceNotice::answerCount>("answerCount");
        builder.Method<&DialogueChoiceNotice::answer>("answer", {"index"});
        builder.Method<&DialogueChoiceNotice::answerLineId>("answerLineId", {"index"});
    }

    REFLECT_VALUE(DialogueCommandNotice, "rtti::dialogue")
    {
        builder.Property<&DialogueCommandNotice::dialogue>("dialogue");
        builder.Property<&DialogueCommandNotice::conversation>("conversation");
        builder.Property<&DialogueCommandNotice::name>("name");
        builder.Property<&DialogueCommandNotice::waits>("waits");
        builder.Property<&DialogueCommandNotice::leaves>("leaves");
        builder.Method<&DialogueCommandNotice::has>("has", {"name"});
        builder.Method<&DialogueCommandNotice::text>("text", {"name"});
        builder.Method<&DialogueCommandNotice::number>("number", {"name"});
        builder.Method<&DialogueCommandNotice::asset>("asset", {"name"});
        builder.Method<&DialogueCommandNotice::entity>("entity", {"name"});
    }

    REFLECT_VALUE(DialogueEndedNotice, "rtti::dialogue")
    {
        builder.Property<&DialogueEndedNotice::dialogue>("dialogue");
        builder.Property<&DialogueEndedNotice::conversation>("conversation");
        builder.Property<&DialogueEndedNotice::stopped>("stopped");
        builder.Property<&DialogueEndedNotice::left>("left");
    }

    namespace
    {
        // The notices as their scripts name them, with the types to emit.
        struct ScriptRoot
        {
            const TypeInfo* type;
            StringView name;
        };

        [[nodiscard]] Span<const ScriptRoot> Roots()
        {
            static const ScriptRoot roots[] = {
                {&TypeOf<DialogueLineNotice>(), u8"DialogueLineNotice"},
                {&TypeOf<DialogueChoiceNotice>(), u8"DialogueChoiceNotice"},
                {&TypeOf<DialogueCommandNotice>(), u8"DialogueCommandNotice"},
                {&TypeOf<DialogueEndedNotice>(), u8"DialogueEndedNotice"},
            };
            return Span<const ScriptRoot>(roots, 4);
        }
    }

    void RegisterDialogueScriptFacade()
    {
        RttiRegisterValue_DialogueLineNotice();
        RttiRegisterValue_DialogueChoiceNotice();
        RttiRegisterValue_DialogueCommandNotice();
        RttiRegisterValue_DialogueEndedNotice();
        GlobalTypeRegistry().Register(DialogueFacade::StaticType());
        foundation::script::RegisterExtraFacadeName(u8"Dialogue");
        for (const ScriptRoot& root : Roots())
        {
            GlobalTypeRegistry().Register(*root.type);
            foundation::script::RegisterExtraScriptRootType(root.type);
            foundation::script::RegisterExtraFacadeName(root.name);
        }
    }

    void RemoveDialogueScriptFacade()
    {
        foundation::script::RemoveExtraFacadeName(u8"Dialogue");
        for (const ScriptRoot& root : Roots())
        {
            foundation::script::RemoveExtraScriptRootType(root.type);
            foundation::script::RemoveExtraFacadeName(root.name);
        }
    }

}
