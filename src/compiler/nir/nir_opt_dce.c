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

static bool
is_dest_live(const nir_dest *dest, BITSET_WORD *defs_live)
{
   return !dest->is_ssa || BITSET_TEST(defs_live, dest->ssa.index);
}

static bool
mark_src_live(const nir_src *src, BITSET_WORD *defs_live)
{
   if (src->is_ssa) {
      BITSET_WORD old = defs_live[BITSET_BITWORD(src->ssa->index)];
      BITSET_WORD new_ = BITSET_SET(defs_live, src->ssa->index);
      return new_ != old;
   } else {
      if (src->reg.indirect)
         return mark_src_live(src->reg.indirect, defs_live);
      return false;
   }
}

static void
mark_indirect_live(const nir_dest *dest, BITSET_WORD *defs_live)
{
   if (!dest->is_ssa && dest->reg.indirect)
      mark_src_live(dest->reg.indirect, defs_live);
}

struct loop_state {
   bool phis_changed;
   nir_block *preheader;
};

static bool
dce_block(nir_block *block, BITSET_WORD *defs_live, struct loop_state *loop)
{
   bool progress = false;
   bool phis_changed = false;
   nir_foreach_instr_reverse_safe(instr, block) {
      bool live = false;
      switch (instr->type) {
      case nir_instr_type_alu: {
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         if (is_dest_live(&alu->dest.dest, defs_live)) {
            live = true;

            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
               mark_src_live(&alu->src[i].src, defs_live);

            mark_indirect_live(&alu->dest.dest, defs_live);
         }
         break;
      }
      case nir_instr_type_deref: {
         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (is_dest_live(&deref->dest, defs_live)) {
            live = true;

            if (deref->deref_type != nir_deref_type_var)
               mark_src_live(&deref->parent, defs_live);

            if (deref->deref_type == nir_deref_type_array ||
                deref->deref_type == nir_deref_type_ptr_as_array)
               mark_src_live(&deref->arr.index, defs_live);

            mark_indirect_live(&deref->dest, defs_live);
         }
         break;
      }
      case nir_instr_type_call: {
         nir_call_instr *call = nir_instr_as_call(instr);
         live = true;

         for (unsigned i = 0; i < call->num_params; i++)
            mark_src_live(&call->params[i], defs_live);
         break;
      }
      case nir_instr_type_tex: {
         nir_tex_instr *tex = nir_instr_as_tex(instr);
         if (is_dest_live(&tex->dest, defs_live)) {
            live = true;

            for (unsigned i = 0; i < tex->num_srcs; i++)
               mark_src_live(&tex->src[i].src, defs_live);

            mark_indirect_live(&tex->dest, defs_live);
         }
         break;
      }
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         const nir_intrinsic_info *info = &nir_intrinsic_infos[intrin->intrinsic];
         if (!(info->flags & NIR_INTRINSIC_CAN_ELIMINATE) ||
             (info->has_dest && is_dest_live(&intrin->dest, defs_live))) {
            live = true;

            for (unsigned i = 0; i < info->num_srcs; i++)
               mark_src_live(&intrin->src[i], defs_live);

            if (info->has_dest)
               mark_indirect_live(&intrin->dest, defs_live);
         }
         break;
      }
      case nir_instr_type_load_const: {
         nir_load_const_instr *lc = nir_instr_as_load_const(instr);
         live = BITSET_TEST(defs_live, lc->def.index);
         break;
      }
      case nir_instr_type_jump: {
         nir_jump_instr *jump = nir_instr_as_jump(instr);
         live = true;

         if (jump->type == nir_jump_goto_if)
            mark_src_live(&jump->condition, defs_live);
         break;
      }
      case nir_instr_type_ssa_undef: {
         nir_ssa_undef_instr *undef = nir_instr_as_ssa_undef(instr);
         live = BITSET_TEST(defs_live, undef->def.index);
         break;
      }
      case nir_instr_type_phi: {
         nir_phi_instr *phi = nir_instr_as_phi(instr);
         if (is_dest_live(&phi->dest, defs_live)) {
            live = true;

            nir_foreach_phi_src(src, phi) {
               phis_changed |= mark_src_live(&src->src, defs_live) &&
                               src->pred != loop->preheader;
            }

            mark_indirect_live(&phi->dest, defs_live);
         }
         break;
      }
      case nir_instr_type_parallel_copy: {
         nir_parallel_copy_instr *pc = nir_instr_as_parallel_copy(instr);
         nir_foreach_parallel_copy_entry(entry, pc) {
            if (is_dest_live(&entry->dest, defs_live)) {
               live = true;
               break;
            }
         }
         if (live) {
            nir_foreach_parallel_copy_entry(entry, pc) {
               mark_src_live(&entry->src, defs_live);
               mark_indirect_live(&entry->dest, defs_live);
            }
         }
         break;
      }
      }

      /* If we're not in a loop, remove it now if it's dead. If we are in a
       * loop, leave instructions to be removed later if they're still dead.
       */
      if (!loop->preheader && !live) {
         nir_instr_remove(instr);
         progress = true;
      } else if (loop->preheader) {
         instr->pass_flags = live;
      }
   }

   loop->phis_changed = phis_changed;
   return progress;
}

static bool
dce_cf_list(struct exec_list *cf_list, BITSET_WORD *defs_live,
            struct loop_state *parent_loop)
{
   bool progress = false;
   foreach_list_typed_reverse(nir_cf_node, cf_node, node, cf_list) {
      switch (cf_node->type) {
      case nir_cf_node_block: {
         nir_block *block = nir_cf_node_as_block(cf_node);
         progress |= dce_block(block, defs_live, parent_loop);
         break;
      }
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(cf_node);
         mark_src_live(&nif->condition, defs_live);
         progress |= dce_cf_list(&nif->else_list, defs_live, parent_loop);
         progress |= dce_cf_list(&nif->then_list, defs_live, parent_loop);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(cf_node);

         /* Fast path if the loop has no continues: we can remove instructions
          * as we mark the others live.
          */
         if (nir_loop_first_block(loop)->predecessors->entries == 1) {
            progress |= dce_cf_list(&loop->body, defs_live, parent_loop);
            break;
         }

         /* Mark instructions as live until there is no more progress. */
         struct loop_state inner_state;
         inner_state.preheader = nir_cf_node_as_block(nir_cf_node_prev(cf_node));
         inner_state.phis_changed = false;
         do {
            dce_cf_list(&loop->body, defs_live, &inner_state);
         } while (inner_state.phis_changed);

         /* We don't know how many times mark_cf_list() will repeat, so
          * remove instructions separately.
          */
         if (!parent_loop->preheader) {
            nir_foreach_block_in_cf_node(block, cf_node) {
               nir_foreach_instr_safe(instr, block) {
                  if (!instr->pass_flags) {
                     nir_instr_remove(instr);
                     progress = true;
                  }
               }
            }
         }
         break;
      }
      case nir_cf_node_function:
         unreachable("Invalid cf type");
      }
   }

   return progress;
}

static bool
nir_opt_dce_impl(nir_function_impl *impl)
{
   assert(impl->structured);

   BITSET_WORD *defs_live = rzalloc_array(NULL, BITSET_WORD,
                                          BITSET_WORDS(impl->ssa_alloc));

   struct loop_state loop;
   loop.preheader = NULL;
   bool progress = dce_cf_list(&impl->body, defs_live, &loop);

   ralloc_free(defs_live);

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

bool
nir_opt_dce(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader) {
      if (function->impl && nir_opt_dce_impl(function->impl))
         progress = true;
   }

   return progress;
}
