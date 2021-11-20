/*
 * Copyright (C) 2021 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler.h"

/* This file contains the final passes of the compiler. Running after
 * scheduling and RA, the IR is now finalized, so we need to emit it to actual
 * bits on the wire (as well as fixup branches) */

static void
va_pack_instr(const bi_instr *I, struct util_dynarray *emission)
{
   uint64_t hex = (0xC0ull << 40); // NOP
   util_dynarray_append(emission, uint64_t, hex);
}

static bool
va_should_return(bi_block *block, bi_instr *I)
{
   /* Don't return within a block */
   if (I->link.next != &block->instructions)
      return false;

   /* Don't return if we're succeeded by instructions */
   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      bi_block *succ = block->successors[i];

      if (succ && !bi_is_terminal_block(succ))
         return false;
   }

   return true;
}

void
bi_pack_valhall(bi_context *ctx, struct util_dynarray *emission)
{
   bi_foreach_block(ctx, block) {
      bi_foreach_instr_in_block(block, I) {
         va_pack_instr(I, emission);
      }
   }
}
