// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorCommandStack tests: execute, undo and redo; a failed execute is dropped; a new command
// truncates the redo tail; same-type commands merge (slider drags); group transactions undo and
// redo atomically, coalesce by type, and stay separate after LockGroup; the saved point, and a
// page's Discard returning to it.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import editor.core;

using namespace foundation::core;
using namespace editor;

namespace
{
    // Adds `delta` to a shared counter. TypeId "add"; merges by summing deltas.
    class AddCommand final : public IEditorCommand
    {
    public:
        AddCommand(i32& target, i32 delta, bool mergeable = false)
            : m_target(&target), m_delta(delta), m_mergeable(mergeable)
        {
        }

        [[nodiscard]] bool Execute() override
        {
            // Merged re-execution must first revert the previously-applied delta; track it.
            if (m_applied != 0)
            {
                *m_target -= m_applied;
            }
            *m_target += m_delta;
            m_applied = m_delta;
            return true;
        }
        void Undo() override
        {
            *m_target -= m_applied;
            m_applied = 0;
        }
        [[nodiscard]] StringView TypeId() const override { return u8"add"; }
        [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
        {
            if (!m_mergeable)
            {
                return false;
            }
            auto& prev = static_cast<AddCommand&>(previous);
            if (!prev.m_mergeable || prev.m_target != m_target)
            {
                return false;
            }
            prev.m_delta = m_delta; // absorb: the previous command applies the newest value
            return true;
        }

    private:
        i32* m_target;
        i32 m_delta;
        i32 m_applied = 0;
        bool m_mergeable;
    };

    class FailCommand final : public IEditorCommand
    {
    public:
        [[nodiscard]] bool Execute() override { return false; }
        void Undo() override {}
        [[nodiscard]] StringView TypeId() const override { return u8"fail"; }
    };

    UniquePtr<IEditorCommand> Add(i32& target, i32 delta, bool mergeable = false)
    {
        return UniquePtr<IEditorCommand>(
            DefaultAllocator().New<AddCommand>(target, delta, mergeable), DefaultAllocator());
    }
}

TEST_CASE("editor-commands: execute, undo, redo")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(!stack.CanUndo());
    CHECK(!stack.CanRedo());

    CHECK(stack.Execute(Add(value, 5)));
    CHECK(stack.Execute(Add(value, 3)));
    CHECK(value == 8);
    CHECK(stack.CanUndo());

    stack.Undo();
    CHECK(value == 5);
    CHECK(stack.CanRedo());

    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo();
    CHECK(value == 5);
    stack.Redo();
    CHECK(value == 8);
    CHECK(!stack.CanRedo());
}

TEST_CASE("editor-commands: failed Execute is dropped, not pushed")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(!stack.Execute(
        UniquePtr<IEditorCommand>(DefaultAllocator().New<FailCommand>(), DefaultAllocator())));
    CHECK(stack.Size() == 1); // only the add
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: new command truncates the redo tail")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    stack.Undo(); // value = 1, redo available
    CHECK(stack.CanRedo());

    CHECK(stack.Execute(Add(value, 10))); // truncates the +2 redo entry
    CHECK(value == 11);
    CHECK(!stack.CanRedo());

    stack.Undo();
    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("editor-commands: same-type merge coalesces a drag into one entry")
{
    EditorCommandStack stack;
    i32 value = 0;

    // A "slider drag": many mergeable sets result in a single undo entry.
    CHECK(stack.Execute(Add(value, 1, true)));
    CHECK(stack.Execute(Add(value, 2, true)));
    CHECK(stack.Execute(Add(value, 3, true)));
    CHECK(value == 3); // merged command re-applies the newest value
    CHECK(stack.Size() == 1);

    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo();
    CHECK(value == 3);
}

TEST_CASE("editor-commands: non-mergeable commands do not merge")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    CHECK(stack.Size() == 2);
}

TEST_CASE("editor-commands: groups undo and redo atomically")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"spawn");
    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    CHECK(!stack.CanUndo()); // undo/redo unavailable inside an open group
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo(); // the whole group
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo(); // the whole group
    CHECK(value == 3);
    CHECK(!stack.CanRedo());
}

TEST_CASE("editor-commands: consecutive same-type groups coalesce")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();

    stack.BeginGroup(u8"move"); // coalesces into the previous "move" group
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo(); // one undo reverts both
    CHECK(value == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("editor-commands: an empty group leaves no undo step - nothing ran, nothing to take back")
{
    EditorCommandStack stack;
    i32 value = 0;
    u32 changes = 0;
    stack.OnChanged = [&changes]() { ++changes; };

    stack.BeginGroup(u8"mcp");
    stack.EndGroup(); // nothing inside
    CHECK(stack.Size() == 0u);
    CHECK(!stack.CanUndo());
    CHECK(changes == 0u); // no notification: nothing changed

    // A group whose only command was refused is empty too; the step before it stands alone.
    CHECK(stack.Execute(Add(value, 5)));
    const usize sizeBefore = stack.Size();
    stack.BeginGroup(u8"mcp");
    CHECK(!stack.Execute(UniquePtr<IEditorCommand>(DefaultAllocator().New<FailCommand>(),
                                                   DefaultAllocator())));
    stack.EndGroup();
    CHECK(stack.Size() == sizeBefore);
    CHECK(stack.UndoIndex() == 0);
    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("editor-commands: LockGroup prevents coalescing")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();
    stack.LockGroup();

    stack.BeginGroup(u8"move"); // locked: stays a separate group
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo();
    CHECK(value == 1); // only the second group reverted
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: different-type groups do not coalesce")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();

    stack.BeginGroup(u8"rotate");
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    stack.Undo();
    CHECK(value == 1);
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: empty group round-trips undo/redo")
{
    EditorCommandStack stack;
    i32 value = 0;

    // An empty group as a stack marker (a fence, such as one around play mode).
    stack.BeginGroup(u8"fence");
    stack.EndGroup();
    CHECK(stack.Execute(Add(value, 1)));

    stack.Undo(); // the add
    CHECK(value == 0);
    stack.Undo(); // the empty group (no-op, but index moves)
    CHECK(!stack.CanUndo());

    stack.Redo(); // the empty group
    stack.Redo(); // the add
    CHECK(value == 1);
}

TEST_CASE("editor-commands: OnChanged fires on mutations")
{
    EditorCommandStack stack;
    i32 value = 0;
    i32 changes = 0;
    stack.OnChanged = [&changes]() { ++changes; };

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(changes == 1);
    stack.Undo();
    CHECK(changes == 2);
    stack.Redo();
    CHECK(changes == 3);
    stack.Clear();
    CHECK(changes == 4);
    CHECK(stack.Size() == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("command-stack: locked stack refuses execute/undo/redo (Simulate mode)")
{
    EditorCommandStack stack;
    i32 value = 0;
    auto makeSet = [&](i32 target)
    {
        struct SetCommand final : IEditorCommand
        {
            i32* slot;
            i32 to;
            i32 from = 0;
            SetCommand(i32* s, i32 t) : slot(s), to(t) {}
            bool Execute() override
            {
                from = *slot;
                *slot = to;
                return true;
            }
            void Undo() override { *slot = from; }
            [[nodiscard]] StringView TypeId() const override { return u8"test.set"; }
        };
        return UniquePtr<IEditorCommand>(DefaultAllocator().New<SetCommand>(&value, target),
                                         DefaultAllocator());
    };

    REQUIRE(stack.Execute(makeSet(1)));
    CHECK(value == 1);

    stack.SetLocked(true);
    CHECK_FALSE(stack.Execute(makeSet(2))); // refused: no runtime edits on the history
    CHECK(value == 1);
    CHECK_FALSE(stack.CanUndo()); // nothing offers what Undo refuses
    stack.Undo();
    CHECK(value == 1); // undo refused too

    stack.SetLocked(false);
    stack.Undo();
    CHECK(value == 0); // unlocked: history intact and working
    stack.Redo();
    CHECK(value == 1);
}

TEST_CASE("command-stack: the saved point follows undo and redo")
{
    EditorCommandStack stack;
    i32 value = 0;
    CHECK(stack.IsAtSaved()); // an empty history is what was loaded

    REQUIRE(stack.Execute(Add(value, 1)));
    REQUIRE(stack.Execute(Add(value, 2)));
    CHECK_FALSE(stack.IsAtSaved());
    stack.MarkSaved();
    CHECK(stack.IsAtSaved());

    REQUIRE(stack.Execute(Add(value, 4)));
    CHECK_FALSE(stack.IsAtSaved());
    stack.Undo();
    CHECK(stack.IsAtSaved()); // undone back to what the file holds
    stack.Undo();
    CHECK_FALSE(stack.IsAtSaved());
    CHECK(value == 1);

    // Past the save: going back to it redoes.
    CHECK(stack.ReturnToSaved());
    CHECK(value == 3);
    CHECK(stack.IsAtSaved());

    // After the save: going back to it undoes.
    stack.Redo();
    CHECK(value == 7);
    CHECK(stack.ReturnToSaved());
    CHECK(value == 3);

    // Dropping the history keeps the point saved when it was.
    stack.Clear();
    CHECK(stack.IsAtSaved());
    REQUIRE(stack.Execute(Add(value, 1)));
    stack.Clear();
    CHECK_FALSE(stack.IsAtSaved()); // the history is gone, and the change it made is not saved
}

TEST_CASE("command-stack: a step below the save removes the save from the history")
{
    EditorCommandStack stack;
    i32 value = 0;
    REQUIRE(stack.Execute(Add(value, 1)));
    REQUIRE(stack.Execute(Add(value, 2)));
    stack.MarkSaved(); // value 3, two steps
    stack.Undo();
    stack.Undo();
    REQUIRE(stack.Execute(Add(value, 10))); // the saved step is dropped
    REQUIRE(stack.Execute(Add(value, 20))); // the history is two steps long again
    CHECK(value == 30);
    CHECK_FALSE(stack.IsAtSaved()); // at the saved position, not the saved state
    CHECK_FALSE(stack.ReturnToSaved());
    CHECK(value == 30); // nothing changed
}

TEST_CASE("command-stack: a step merged into the saved one removes the save, and drops the redo tail")
{
    EditorCommandStack stack;
    i32 value = 0;
    REQUIRE(stack.Execute(Add(value, 1, true)));
    stack.MarkSaved();
    REQUIRE(stack.Execute(Add(value, 5, true))); // a drag continuing into the saved step
    CHECK(stack.Size() == 1);
    CHECK_FALSE(stack.IsAtSaved());
    CHECK_FALSE(stack.ReturnToSaved());

    // A merge is a new step: what was undone before it no longer follows.
    EditorCommandStack other;
    i32 n = 0;
    REQUIRE(other.Execute(Add(n, 1, true)));
    REQUIRE(other.Execute(Add(n, 100)));
    other.Undo();
    REQUIRE(other.Execute(Add(n, 2, true)));
    CHECK_FALSE(other.CanRedo());
    CHECK(n == 2);
}

TEST_CASE("command-stack: a group reopened over the save, or a save inside a group, is no saved point")
{
    EditorCommandStack stack;
    i32 value = 0;
    stack.BeginGroup(u8"drag");
    REQUIRE(stack.Execute(Add(value, 1)));
    stack.EndGroup();
    stack.MarkSaved();
    stack.BeginGroup(u8"drag"); // coalesces: the saved end marker is reopened
    REQUIRE(stack.Execute(Add(value, 1)));
    stack.EndGroup();
    CHECK_FALSE(stack.IsAtSaved());
    CHECK_FALSE(stack.ReturnToSaved());

    EditorCommandStack inside;
    inside.BeginGroup(u8"drag");
    REQUIRE(inside.Execute(Add(value, 1)));
    inside.MarkSaved(); // no undo or redo lands mid-group
    CHECK_FALSE(inside.IsAtSaved());
    inside.EndGroup();
    CHECK_FALSE(inside.ReturnToSaved());
}

TEST_CASE("command-stack: a locked stack does not go back to the save")
{
    EditorCommandStack stack;
    i32 value = 0;
    stack.MarkSaved();
    REQUIRE(stack.Execute(Add(value, 1)));
    stack.SetLocked(true);
    CHECK_FALSE(stack.CanRedo());
    CHECK_FALSE(stack.ReturnToSaved());
    CHECK(value == 1);
}

namespace
{
    // A page over one number: its "file" holds `stored`, and reading it again can be refused.
    class NumberPage final : public EditorPage
    {
    public:
        NumberPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"number"; }
        [[nodiscard]] Status Save() override
        {
            stored = value;
            ClearDirty();
            return Status{};
        }
        bool ReloadFromSource() override
        {
            ++reloads;
            if (!readable)
                return false;
            value = stored;
            Commands().Clear();
            ClearDirty();
            return true;
        }

        i32 value = 0;
        i32 stored = 0;
        i32 reloads = 0;
        bool readable = true;
    };
}

TEST_CASE("page: Discard goes back to the last save, not past it")
{
    NumberPage page;
    REQUIRE(page.Commands().Execute(Add(page.value, 1)));
    REQUIRE(page.Save().IsOk());
    REQUIRE(page.Commands().Execute(Add(page.value, 2)));
    CHECK(page.IsDirty());

    CHECK(page.DiscardChanges());
    CHECK(page.value == 1); // the saved step stays applied
    CHECK_FALSE(page.IsDirty());
    CHECK(page.reloads == 0); // the history reached the save

    // Undone past the save, Discard redoes back to it.
    REQUIRE(page.Commands().Execute(Add(page.value, 4)));
    REQUIRE(page.Save().IsOk());
    page.Commands().Undo();
    CHECK(page.value == 1);
    CHECK(page.IsDirty());
    CHECK(page.DiscardChanges());
    CHECK(page.value == 5);
    CHECK_FALSE(page.IsDirty());
}

TEST_CASE("page: undoing back to the save makes the page clean")
{
    NumberPage page;
    REQUIRE(page.Commands().Execute(Add(page.value, 1)));
    CHECK(page.IsDirty());
    page.Commands().Undo();
    CHECK_FALSE(page.IsDirty());
    page.MarkDirty(); // a change outside the history keeps it unsaved
    CHECK(page.IsDirty());
}

TEST_CASE("page: Discard reads the file again when the history does not reach the save")
{
    NumberPage page;
    REQUIRE(page.Commands().Execute(Add(page.value, 1)));
    REQUIRE(page.Save().IsOk());
    page.Commands().Undo();
    REQUIRE(page.Commands().Execute(Add(page.value, 7))); // drops the saved step
    CHECK(page.DiscardChanges());
    CHECK(page.reloads == 1);
    CHECK(page.value == 1);
    CHECK_FALSE(page.IsDirty());

    // A change made outside the history is undone only by reading the file.
    page.value = 9;
    page.MarkDirty();
    CHECK(page.DiscardChanges());
    CHECK(page.reloads == 2);
    CHECK(page.value == 1);

    // A file that does not read keeps the edits, and says so.
    page.readable = false;
    page.value = 9;
    page.MarkDirty();
    CHECK_FALSE(page.DiscardChanges());
    CHECK(page.value == 9);
    CHECK(page.IsDirty());
}

TEST_CASE("page: Discard on a locked history returns at once and keeps the edits")
{
    NumberPage page;
    REQUIRE(page.Commands().Execute(Add(page.value, 1)));
    page.Commands().SetLocked(true); // Simulate
    CHECK_FALSE(page.DiscardChanges()); // used to loop on CanUndo while Undo refused
    CHECK(page.value == 1);
    CHECK(page.IsDirty());
    CHECK(page.reloads == 0);
}
