/*
 * Copyright © 2014 Intel Corporation
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
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"

/**
 * SSA-based copy propagation
 */

static bool
is_swizzleless_move(nir_alu_instr *instr)
{
   unsigned num_comp = instr->dest.dest.ssa.num_components;

   if (instr->src[0].src.ssa->num_components != num_comp)
      return false;

   if (instr->op == nir_op_mov) {
      for (unsigned i = 0; i < num_comp; i++) {
         if (instr->src[0].swizzle[i] != i)
            return false;
      }
   } else {
      for (unsigned i = 0; i < num_comp; i++) {
         if (instr->src[i].swizzle[0] != i ||
             instr->src[i].src.ssa != instr->src[0].src.ssa)
            return false;
      }
   }

   return true;
}

static bool is_copy(nir_alu_instr *instr)
{
   assert(instr->src[0].src.is_ssa);

   /* we handle modifiers in a separate pass */
   if (instr->op == nir_op_mov) {
      return !instr->dest.saturate &&
             !instr->src[0].abs &&
             !instr->src[0].negate;
   } else if (nir_op_is_vec(instr->op)) {
      for (unsigned i = 0; i < instr->dest.dest.ssa.num_components; i++) {
         if (instr->src[i].abs || instr->src[i].negate)
            return false;
      }
      return !instr->dest.saturate;
   } else {
      return false;
   }
}

static bool copy_propagate_alu(nir_alu_src *src, nir_alu_instr *copy)
{
   nir_ssa_def *def = NULL;
   nir_alu_instr *user = nir_instr_as_alu(src->src.parent_instr);
   unsigned comp = nir_ssa_alu_instr_src_components(user, src - user->src);

   if (copy->op == nir_op_mov) {
      def = copy->src[0].src.ssa;

      for (unsigned i = 0; i < comp; i++)
         src->swizzle[i] = copy->src[0].swizzle[src->swizzle[i]];
   } else {
      def = copy->src[src->swizzle[0]].src.ssa;

      for (unsigned i = 1; i < comp; i++) {
         if (copy->src[src->swizzle[i]].src.ssa != def)
            return false;
      }

      for (unsigned i = 0; i < comp; i++)
         src->swizzle[i] = copy->src[src->swizzle[i]].swizzle[0];
   }

   list_del(&src->src.use_link);
   list_addtail(&src->src.use_link, &def->uses);
   src->src.ssa = def;

   return true;
}

static bool copy_propagate(nir_src *src, nir_alu_instr *copy)
{
   if (!is_swizzleless_move(copy))
      return false;

   list_del(&src->use_link);
   list_addtail(&src->use_link, &copy->src[0].src.ssa->uses);
   src->ssa = copy->src[0].src.ssa;

   return true;
}

static bool copy_propagate_if(nir_src *src, nir_alu_instr *copy)
{
   if (!is_swizzleless_move(copy))
      return false;

   list_del(&src->use_link);
   list_addtail(&src->use_link, &copy->src[0].src.ssa->if_uses);
   src->ssa = copy->src[0].src.ssa;

   return true;
}

static bool
copy_prop_instr(nir_instr *instr)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);

   if (!is_copy(alu))
      return false;

   bool progress = false;

   nir_foreach_use_safe(src, &alu->dest.dest.ssa) {
      if (src->parent_instr->type == nir_instr_type_alu)
         progress |= copy_propagate_alu(container_of(src, nir_alu_src, src), alu);
      else
         progress |= copy_propagate(src, alu);
   }

   nir_foreach_if_use_safe(src, &alu->dest.dest.ssa)
      progress |= copy_propagate_if(src, alu);

   if (list_is_empty(&alu->dest.dest.ssa.uses) &&
       list_is_empty(&alu->dest.dest.ssa.if_uses) &&
       progress)
      nir_instr_remove(instr);

   return progress;
}

static bool
nir_copy_prop_impl(nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         progress |= copy_prop_instr(instr);
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

bool
nir_copy_prop(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl && nir_copy_prop_impl(function->impl))
         progress = true;
   }

   return progress;
}
