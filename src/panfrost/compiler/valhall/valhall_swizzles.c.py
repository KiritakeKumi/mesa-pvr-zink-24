# Copyright © 2025 Collabora Ltd.
# SPDX-License-Identifier: MIT

TEMPLATE = """
#include "bi_swizzles.h"

uint32_t va_op_swizzles[BI_NUM_OPCODES][BI_MAX_SRCS] = {
% for opcode, src_swizzles in op_swizzles.items():
    [BI_OPCODE_${opcode.replace('.', '_').upper()}] = {
% for src in src_swizzles:
% for swizzle in src:
        (1 << BI_SWIZZLE_${swizzle.upper()}) |
% endfor
        0,
% endfor
    },
% endfor
};
"""

import sys
from valhall import valhall_parse_isa
from mako.template import Template

(instructions, immediates, enums, typesize, safe_name) = valhall_parse_isa()

# TODO: this whole thing can probably be replaced with valhall_opcodes?

op_swizzles = {}

def get_enum_values(name):
    return [value.value for value in enums[name].values]

swizzle_aliases = {
}

for instr in instructions:
    src_swizzles = []

    for src in instr.srcs:
        swizzles = []
        enum = None
        if src.swizzle:
            swizzles += get_enum_values(f'swizzles_{src.size}_bit')
        if src.lanes:
            swizzles += get_enum_values(f'lanes_{src.size}_bit')
        if src.halfswizzle:
            swizzles += get_enum_values(f'half_swizzles_{src.size}_bit')
        # TODO: I don't think this enum is actually correct
        if src.widen:
            swizzles += get_enum_values('widen')
        if src.lanes:
            swizzles += get_enum_values(f'lanes_{src.size}_bit')
        # TODO: do I need 'lane'?

        src_swizzles.append([s for s in swizzles if s != 'reserved'])

    op_swizzles[instr.name] = src_swizzles

print(Template(TEMPLATE).render(op_swizzles = op_swizzles))
