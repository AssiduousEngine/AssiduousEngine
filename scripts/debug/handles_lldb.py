# lldb summary for the engine's generational handles (foundation::core::Handle<Tag>).
#
# A handle shows its slot, its count and its pool, and, in a live process, the label its owner
# gave it and whether it is still live (an RHI resource's creation label, through the device that
# made it). Load it from lldb:
#
#     command script import scripts/debug/handles_lldb.py
#
# or add that line to ~/.lldbinit. The label comes from calling DebugHandleLabel in the
# process, so a core file shows the slot, count and pool only.

import re

import lldb

_INVALID_INDEX = 0xFFFFFFFF
_POOL_SHIFT = 24
_COUNT_MASK = (1 << _POOL_SHIFT) - 1


def _noun(tag):
    name = tag.strip().split("::")[-1]
    return name[:-3] if name.endswith("Tag") else name


def handle_summary(value, internal_dict):
    match = re.match(r"^foundation::core::Handle<(.+)>$", value.GetType().GetCanonicalType().GetName())
    noun = _noun(match.group(1)) if match else "Handle"
    index = value.GetChildMemberWithName("index").GetValueAsUnsigned()
    generation = value.GetChildMemberWithName("generation").GetValueAsUnsigned()
    if index == _INVALID_INDEX:
        return "%s handle (none)" % noun
    text = "%s handle slot %d count %d pool %d" % (noun, index, generation & _COUNT_MASK, generation >> _POOL_SHIFT)
    process = value.GetProcess()
    if process.IsValid() and process.GetState() == lldb.eStateStopped:
        answer = value.GetFrame().EvaluateExpression(
            '(const char*)DebugHandleLabel("%s", %du, %du)' % (noun, index, generation))
        if answer.GetError().Success():
            label = answer.GetSummary() or ""
            label = label.strip('"')
            if label:
                text += " " + label
    return text


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        'type summary add -x "^foundation::core::Handle<.+>$" -F handles_lldb.handle_summary')
