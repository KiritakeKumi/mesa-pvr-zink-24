/*
 * Copyright © 2020 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir_instr_set.h"
#include "nir_builder.h"

static bool
instruction_is_before(nir_instr *before, const nir_instr *after)
{
   if (before->block != after->block)
      return nir_block_dominates(before->block, after->block);

   for (/* empty */; before != NULL; before = nir_instr_next(before)) {
      if (before == after)
         return true;
   }

   return false;
}

static bool
dce_discard_condition_instr(nir_builder *b, nir_instr *instr,
                            UNUSED void *not_used)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *const intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic != nir_intrinsic_discard_if &&
       intrin->intrinsic != nir_intrinsic_terminate_if)
      return false;

   assert(intrin->src[0].is_ssa);

   /* There is nothing useful to be done if the source is a constant.  Other
    * optimization passes will either eliminate the intrinsic (i.e., when the
    * constant is false), or convert it to the non-_if form (i.e., when the
    * constant is true).  Either way, replacing later uses of the constant
    * with the constant false in this pass does not seem useful.
    */
   if (nir_src_is_const(intrin->src[0]))
      return false;

   bool progress = false;

   nir_foreach_use_safe(use, intrin->src[0].ssa) {
      nir_instr *user = use->parent_instr;

      /* If the discard_if instruction dominates the other use of the
       * condition, then the condition at the use can trivially be replaced
       * with false.
       */
      if (instr != user && instruction_is_before(instr, user)) {
         b->cursor = nir_before_instr(user);

         nir_instr_rewrite_src_ssa(user, use, nir_imm_false(b));
         progress = true;
      }
   }

   nir_foreach_if_use_safe(use, intrin->src[0].ssa) {
      nir_if *user = use->parent_if;

      nir_block *user_block = nir_cf_node_as_block(nir_cf_node_prev(&user->cf_node));
      if (nir_block_dominates(instr->block, user_block)) {
         b->cursor = nir_after_block(user_block);

         nir_if_rewrite_condition_ssa(user, use, nir_imm_false(b));
         progress = true;
      }
   }

   return progress;
}

bool
nir_opt_dce_discard_condition(nir_shader *shader)
{
   return nir_shader_instructions_pass_require_metadata(shader,
                                                        dce_discard_condition_instr,
                                                        nir_metadata_block_index |
                                                        nir_metadata_dominance,
                                                        nir_metadata_dominance,
                                                        NULL);
}

