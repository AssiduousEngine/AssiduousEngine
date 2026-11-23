# gdb pretty printer for the engine's generational handles (foundation::core::Handle<Tag>).
#
# A handle shows its slot, its count and its pool, and, in a live process, the label its owner
# gave it and whether it is still live (an RHI resource's creation label, through the device that
# made it). Load it from gdb:
#
#     source scripts/debug/handles_gdb.py
#
# or add that line to ~/.gdbinit. The label comes from calling DebugHandleLabel in the
# inferior, so a core file shows the slot, count and pool only.

import re

import gdb
import gdb.printing

_INVALID_INDEX = 0xFFFFFFFF
_POOL_SHIFT = 24
_COUNT_MASK = (1 << _POOL_SHIFT) - 1


def _noun(tag):
    """'foundation::rhi::BufferTag' -> 'Buffer'."""
    name = tag.strip().split("::")[-1]
    return name[:-3] if name.endswith("Tag") else name


class HandlePrinter:
    def __init__(self, value, noun):
        self.value = value
        self.noun = noun

    def to_string(self):
        index = int(self.value["index"])
        generation = int(self.value["generation"])
        if index == _INVALID_INDEX:
            return "%s handle (none)" % self.noun
        text = "%s handle slot %d count %d pool %d" % (
            self.noun, index, generation & _COUNT_MASK, generation >> _POOL_SHIFT)
        label = self._label(index, generation)
        return "%s %s" % (text, label) if label else text

    def _label(self, index, generation):
        try:
            if not gdb.selected_inferior().pid:
                return ""
            answer = gdb.parse_and_eval(
                '(const char*)DebugHandleLabel("%s", %du, %du)' % (self.noun, index, generation))
            return answer.string()
        except gdb.error:
            return ""  # a core file, or a binary without Core's labels


def _lookup(value):
    type_ = value.type.strip_typedefs()
    name = type_.tag or type_.name or ""
    match = re.match(r"^foundation::core::Handle<(.+)>$", name)
    if match is None:
        return None
    return HandlePrinter(value, _noun(match.group(1)))


gdb.pretty_printers.append(_lookup)
