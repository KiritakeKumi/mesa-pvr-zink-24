/*
 * Copyright © 2021 Valve Corporation
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
 */

#include "ac_nir.h"
#include "nir_builder.h"
#include "u_math.h"

typedef struct
{
   nir_variable *position_value_var;
   nir_variable *prim_exp_arg_var;

   bool passthrough;
   bool export_prim_id;
   bool early_prim_export;
   unsigned max_num_waves;
   unsigned num_vertices_per_primitives;
   unsigned provoking_vtx_idx;
   unsigned max_es_num_vertices;
   unsigned total_lds_bytes;
} lower_ngg_nogs_state;

typedef struct
{
   /* bitsize of this component (max 32), or 0 if it's never written at all */
   uint8_t bit_size : 6;
   /* output stream index  */
   uint8_t stream : 2;
} gs_output_component_info;

typedef struct
{
   nir_variable *output_vars[VARYING_SLOT_MAX][4];
   nir_variable *current_clear_primflag_idx_var;
   int const_out_vtxcnt[4];
   int const_out_prmcnt[4];
   unsigned max_num_waves;
   unsigned num_vertices_per_primitive;
   unsigned lds_addr_gs_out_vtx;
   unsigned lds_addr_gs_scratch;
   unsigned lds_bytes_per_gs_out_vertex;
   unsigned lds_offs_primflags;
   bool found_out_vtxcnt[4];
   bool output_compile_time_known;
   bool provoking_vertex_last;
   gs_output_component_info output_component_info[VARYING_SLOT_MAX][4];
} lower_ngg_gs_state;

typedef struct {
   nir_variable *pre_cull_position_value_var;
} remove_culling_shader_outputs_state;

typedef struct {
   bool position_output_found;
   nir_variable *pos_value_replacement;
} remove_extra_position_output_state;

typedef struct {
   nir_ssa_def *reduction_result;
   nir_ssa_def *excl_scan_result;
} wg_scan_result;

/* Per-vertex LDS layout of culling shaders */
enum {
   /* Position of the ES vertex (at the beginning for alignment reasons) */
   lds_es_pos_x = 0,
   lds_es_pos_y = 4,
   lds_es_pos_z = 8,
   lds_es_pos_w = 12,

   /* 1 when the vertex is accepted, 0 if it should be culled */
   lds_es_vertex_accepted = 16,
   /* ID of the thread which will export the current thread's vertex */
   lds_es_exporter_tid = 17,

   /* Repacked arguments - also listed separately for VS and TES */
   lds_es_arg_0 = 20,

   /* VS arguments which need to be repacked */
   lds_es_vs_vertex_id = 20,
   lds_es_vs_instance_id = 24,
   lds_es_max_bytes_vs = 28,

   /* TES arguments which need to be repacked */
   lds_es_tes_u = 20,
   lds_es_tes_v = 24,
   lds_es_tes_rel_patch_id = 28,
   lds_es_tes_patch_id = 32,
   lds_es_max_bytes_tes = 36,
};

static wg_scan_result
workgroup_reduce_and_exclusive_scan(nir_builder *b, nir_ssa_def *input_bool,
                                    unsigned lds_addr_base, unsigned max_num_waves)
{
   /* This performs a reduction along with an exclusive scan addition accross the workgroup.
    * Assumes that all lanes are enabled (exec = -1) where this is emitted.
    *
    * Input:  (1) divergent bool
    *             -- 1 if the lane has a live/valid vertex, 0 otherwise
    * Output: (1) result of a reduction over the entire workgroup,
    *             -- the total number of vertices emitted by the workgroup
    *         (2) result of an exclusive scan over the entire workgroup
    *             -- used for vertex compaction, in order to determine
    *                which lane should export the current lane's vertex
    */

   assert(input_bool->bit_size == 1);

   /* Reduce the boolean -- result is the number of live vertices in the current wave */
   nir_ssa_def *input_mask = nir_build_ballot(b, 1, 64, input_bool);
   nir_ssa_def *wave_reduction = nir_bit_count(b, input_mask);

   /* Take care of when we know in compile time that the maximum workgroup size is small */
   if (max_num_waves == 1) {
      wg_scan_result r = {
         .reduction_result = wave_reduction,
         .excl_scan_result = nir_build_mbcnt_amd(b, input_mask),
      };
      return r;
   }

   /* Number of LDS dwords written by all waves (if there is only 1, that is already handled above) */
   unsigned num_lds_dwords = max_num_waves;
   assert(num_lds_dwords >= 2 && num_lds_dwords <= 8);

   nir_ssa_def *wave_id = nir_build_load_subgroup_id(b);
   nir_ssa_def *dont_care = nir_ssa_undef(b, num_lds_dwords, 32);
   nir_if *if_first_lane = nir_push_if(b, nir_build_elect(b, 1));

   /* The first lane of each wave stores the result of its subgroup reduction to LDS (NGG scratch). */
   nir_ssa_def *wave_id_lds_addr = nir_imul_imm(b, wave_id, 4u);
   nir_build_store_shared(b, wave_reduction, wave_id_lds_addr, .base = lds_addr_base, .align_mul = 4u, .write_mask = 0x1u);

   /* Workgroup barrier: wait for all waves to finish storing their result */
   nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                         .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_shader_out|nir_var_mem_shared);

   /* Only the first lane of each wave loads every wave's results from LDS, to avoid bank conflicts */
   nir_ssa_def *reduction_vector = nir_build_load_shared(b, num_lds_dwords, 32, nir_imm_zero(b, 1, 32), .base = lds_addr_base, .align_mul = 16u);
   nir_pop_if(b, if_first_lane);

   reduction_vector = nir_if_phi(b, reduction_vector, dont_care);

   nir_ssa_def *reduction_per_wave[8] = {0};
   for (unsigned i = 0; i < num_lds_dwords; ++i) {
      nir_ssa_def *reduction_wave_i = nir_channel(b, reduction_vector, i);
      reduction_per_wave[i] = nir_build_read_first_invocation(b, reduction_wave_i);
   }

   nir_ssa_def *num_waves = nir_build_load_num_subgroups(b);
   nir_ssa_def *wg_reduction = reduction_per_wave[0];
   nir_ssa_def *wg_excl_scan_base = NULL;

   for (unsigned i = 0; i < num_lds_dwords; ++i) {
      /* Workgroup reduction:
       * Add the reduction results from all waves up to and including wave_count.
       */
      if (i != 0) {
         nir_ssa_def *should_add = nir_ige(b, num_waves, nir_imm_int(b, i + 1u));
         nir_ssa_def *addition = nir_bcsel(b, should_add, reduction_per_wave[i], nir_imm_zero(b, 1, 32));
         wg_reduction = nir_iadd_nuw(b, wg_reduction, addition);
      }

      /* Base of workgroup exclusive scan:
       * Add the reduction results from waves up to and excluding wave_id_in_tg.
       */
      if (i != (num_lds_dwords - 1u)) {
         nir_ssa_def *should_add = nir_ige(b, wave_id, nir_imm_int(b, i + 1u));
         nir_ssa_def *addition = nir_bcsel(b, should_add, reduction_per_wave[i], nir_imm_zero(b, 1, 32));
         wg_excl_scan_base = !wg_excl_scan_base ? addition : nir_iadd_nuw(b, wg_excl_scan_base, addition);
      }
   }

   nir_ssa_def *sg_excl_scan = nir_build_mbcnt_amd(b, input_mask);
   nir_ssa_def *wg_excl_scan = nir_iadd_nuw(b, wg_excl_scan_base, sg_excl_scan);

   wg_scan_result r = {
      .reduction_result = wg_reduction,
      .excl_scan_result = wg_excl_scan,
   };

   return r;
}

static nir_ssa_def *
pervertex_lds_addr(nir_builder *b, nir_ssa_def *vertex_idx, unsigned per_vtx_bytes)
{
   return nir_imul_imm(b, vertex_idx, per_vtx_bytes);
}

static nir_ssa_def *
emit_pack_ngg_prim_exp_arg(nir_builder *b, unsigned num_vertices_per_primitives,
                           nir_ssa_def *vertex_indices[3], nir_ssa_def *is_null_prim)
{
   nir_ssa_def *arg = vertex_indices[0];

   for (unsigned i = 0; i < num_vertices_per_primitives; ++i) {
      assert(vertex_indices[i]);

      if (i)
         arg = nir_ior(b, arg, nir_ishl(b, vertex_indices[i], nir_imm_int(b, 10u * i)));

      if (b->shader->info.stage == MESA_SHADER_VERTEX) {
         nir_ssa_def *edgeflag = nir_build_load_initial_edgeflag_amd(b, 32, nir_imm_int(b, i));
         arg = nir_ior(b, arg, nir_ishl(b, edgeflag, nir_imm_int(b, 10u * i + 9u)));
      }
   }

   if (is_null_prim) {
      if (is_null_prim->bit_size == 1)
         is_null_prim = nir_b2i32(b, is_null_prim);
      assert(is_null_prim->bit_size == 32);
      arg = nir_ior(b, arg, nir_ishl(b, is_null_prim, nir_imm_int(b, 31u)));
   }

   return arg;
}

static nir_ssa_def *
ngg_input_primitive_vertex_index(nir_builder *b, unsigned vertex)
{
   /* TODO: This is RADV specific. We'll need to refactor RADV and/or RadeonSI to match. */
   return nir_ubfe(b, nir_build_load_gs_vertex_offset_amd(b, .base = vertex / 2u * 2u),
                      nir_imm_int(b, (vertex % 2u) * 16u), nir_imm_int(b, 16u));
}

static nir_ssa_def *
emit_ngg_nogs_prim_exp_arg(nir_builder *b, lower_ngg_nogs_state *st)
{
   if (st->passthrough) {
      assert(!st->export_prim_id);
      return nir_build_load_packed_passthrough_primitive_amd(b);
   } else {
      nir_ssa_def *vtx_idx[3] = {0};

      vtx_idx[0] = ngg_input_primitive_vertex_index(b, 0);
      vtx_idx[1] = st->num_vertices_per_primitives >= 2
               ? ngg_input_primitive_vertex_index(b, 1)
               : nir_imm_zero(b, 1, 32);
      vtx_idx[2] = st->num_vertices_per_primitives >= 3
               ? ngg_input_primitive_vertex_index(b, 2)
               : nir_imm_zero(b, 1, 32);

      return emit_pack_ngg_prim_exp_arg(b, st->num_vertices_per_primitives, vtx_idx, NULL);
   }
}

static void
emit_ngg_nogs_prim_export(nir_builder *b, lower_ngg_nogs_state *st, nir_ssa_def *arg)
{
   nir_if *if_gs_thread = nir_push_if(b, nir_build_has_input_primitive_amd(b));
   {
      if (!arg)
         arg = emit_ngg_nogs_prim_exp_arg(b, st);

      if (st->export_prim_id && b->shader->info.stage == MESA_SHADER_VERTEX) {
         /* Copy Primitive IDs from GS threads to the LDS address corresponding to the ES thread of the provoking vertex. */
         nir_ssa_def *prim_id = nir_build_load_primitive_id(b);
         nir_ssa_def *provoking_vtx_idx = ngg_input_primitive_vertex_index(b, st->provoking_vtx_idx);
         nir_ssa_def *addr = pervertex_lds_addr(b, provoking_vtx_idx, 4u);

         nir_build_store_shared(b,  prim_id, addr, .write_mask = 1u, .align_mul = 4u);
      }

      nir_build_export_primitive_amd(b, arg);
   }
   nir_pop_if(b, if_gs_thread);
}

static void
emit_store_ngg_nogs_es_primitive_id(nir_builder *b)
{
   nir_ssa_def *prim_id = NULL;

   if (b->shader->info.stage == MESA_SHADER_VERTEX) {
      /* Workgroup barrier - wait for GS threads to store primitive ID in LDS. */
      nir_scoped_barrier(b, .execution_scope = NIR_SCOPE_WORKGROUP, .memory_scope = NIR_SCOPE_WORKGROUP,
                            .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_shader_out | nir_var_mem_shared);

      /* LDS address where the primitive ID is stored */
      nir_ssa_def *thread_id_in_threadgroup = nir_build_load_local_invocation_index(b);
      nir_ssa_def *addr =  pervertex_lds_addr(b, thread_id_in_threadgroup, 4u);

      /* Load primitive ID from LDS */
      prim_id = nir_build_load_shared(b, 1, 32, addr, .align_mul = 4u);
   } else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL) {
      /* Just use tess eval primitive ID, which is the same as the patch ID. */
      prim_id = nir_build_load_primitive_id(b);
   }

   nir_io_semantics io_sem = {
      .location = VARYING_SLOT_PRIMITIVE_ID,
      .num_slots = 1,
   };

   nir_build_store_output(b, prim_id, nir_imm_zero(b, 1, 32),
                          .base = io_sem.location,
                          .write_mask = 1u, .src_type = nir_type_uint32, .io_semantics = io_sem);
}

static bool
remove_culling_shader_output(nir_builder *b, nir_instr *instr, void *state)
{
   remove_culling_shader_outputs_state *s = (remove_culling_shader_outputs_state *) state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   /* These are not allowed in VS / TES */
   assert(intrin->intrinsic != nir_intrinsic_store_per_vertex_output &&
          intrin->intrinsic != nir_intrinsic_load_per_vertex_input);

   /* We are only interested in output stores now */
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   b->cursor = nir_before_instr(instr);

   /* Position output - store the value to a variable, remove output store */
   nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   if (io_sem.location == VARYING_SLOT_POS) {
      /* TODO: check if it's indirect, etc? */
      unsigned writemask = nir_intrinsic_write_mask(intrin);
      nir_ssa_def *store_val = intrin->src[0].ssa;
      nir_store_var(b, s->pre_cull_position_value_var, store_val, writemask);
   }

   /* Remove all output stores */
   nir_instr_remove(instr);
   return true;
}

static void
remove_culling_shader_outputs(nir_shader *culling_shader, lower_ngg_nogs_state *nogs_state, nir_variable *pre_cull_position_value_var)
{
   remove_culling_shader_outputs_state s = {
      .pre_cull_position_value_var = pre_cull_position_value_var,
   };

   nir_shader_instructions_pass(culling_shader, remove_culling_shader_output,
                                nir_metadata_block_index | nir_metadata_dominance, &s);

   /* Remove dead code resulting from the deleted outputs. */
   for (bool progress = true; progress; progress = false) {
      NIR_PASS(progress, culling_shader, nir_opt_dce);
      NIR_PASS(progress, culling_shader, nir_opt_dead_cf);
      NIR_PASS(progress, culling_shader, nir_opt_dead_write_vars);
   }
}

static void
rewrite_uses_to_var(nir_builder *b, nir_ssa_def *old_def, nir_variable *replacement_var, unsigned replacement_var_channel)
{
   if (old_def->parent_instr->type == nir_instr_type_load_const)
      return;

   b->cursor = nir_after_instr(old_def->parent_instr);
   if (b->cursor.instr->type == nir_instr_type_phi)
      b->cursor = nir_after_phis(old_def->parent_instr->block);

   nir_ssa_def *pos_val_rep = nir_load_var(b, replacement_var);
   nir_ssa_def *replacement = nir_channel(b, pos_val_rep, replacement_var_channel);

   if (old_def->num_components > 1) {
      /* old_def uses a swizzled vector component.
       * There is no way to replace the uses of just a single vector component,
       * so instead create a new vector and replace all uses of the old vector.
       */
      nir_ssa_def *old_def_elements[NIR_MAX_VEC_COMPONENTS] = {0};
      for (unsigned j = 0; j < old_def->num_components; ++j)
         old_def_elements[j] = nir_channel(b, old_def, j);
      replacement = nir_vec(b, old_def_elements, old_def->num_components);
   }

   nir_ssa_def_rewrite_uses_after(old_def, replacement, replacement->parent_instr);
}

static bool
remove_extra_pos_output(nir_builder *b, nir_instr *instr, void *state)
{
   remove_extra_position_output_state *s = (remove_extra_position_output_state *) state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   /* These are not allowed in VS / TES */
   assert(intrin->intrinsic != nir_intrinsic_store_per_vertex_output &&
          intrin->intrinsic != nir_intrinsic_load_per_vertex_input);

   /* We are only interested in output stores now */
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   if (io_sem.location != VARYING_SLOT_POS)
      return false;

   if (!s->position_output_found) {
      s->position_output_found = true;
      return false;
   }

   b->cursor = nir_before_instr(instr);

   /* In case other outputs use what we calculated for pos,
    * try to avoid calculating it again by rewriting the usages
    * of the store components here.
    */
   nir_ssa_def *store_val = intrin->src[0].ssa;
   unsigned store_pos_component = nir_intrinsic_component(intrin);

   nir_instr_remove(instr);

   if (store_val->parent_instr->type == nir_instr_type_alu) {
      /* If the output store uses a vector, rewrite uses of each vector element. */
      nir_alu_instr *alu = nir_instr_as_alu(store_val->parent_instr);
      if (nir_op_is_vec(alu->op)) {
         unsigned num_vec_src = 0;
         if (alu->op == nir_op_mov)
            num_vec_src = 1;
         else if (alu->op == nir_op_vec2)
            num_vec_src = 2;
         else if (alu->op == nir_op_vec3)
            num_vec_src = 3;
         else if (alu->op == nir_op_vec4)
            num_vec_src = 4;
         assert(num_vec_src);

         /* Remember the current components whose uses we wish to replace.
          * This is needed because rewriting one source can affect the others too.
          */
         nir_ssa_def *vec_comps[NIR_MAX_VEC_COMPONENTS] = {0};
         for (unsigned i = 0; i < num_vec_src; i++)
            vec_comps[i] = alu->src[i].src.ssa;

         for (unsigned i = 0; i < num_vec_src; i++)
            rewrite_uses_to_var(b, vec_comps[i], s->pos_value_replacement, store_pos_component + i);
      }
   } else {
      rewrite_uses_to_var(b, store_val, s->pos_value_replacement, store_pos_component);
   }

   return true;
}

static void
remove_extra_pos_outputs(nir_shader *shader, lower_ngg_nogs_state *nogs_state)
{
   remove_extra_position_output_state s = {
      .pos_value_replacement = nogs_state->position_value_var,
   };

   nir_shader_instructions_pass(shader, remove_extra_pos_output,
                                nir_metadata_block_index | nir_metadata_dominance, &s);
}

static void
add_deferred_attribute_culling(nir_builder *b, nir_cf_list *original_extracted_cf, lower_ngg_nogs_state *nogs_state)
{
   assert(b->shader->info.outputs_written & (1 << VARYING_SLOT_POS));

   /* TODO: use less LDS space when not every item in the layout is necessary */
   unsigned pervertex_lds_bytes = b->shader->info.stage == MESA_SHADER_VERTEX ? lds_es_max_bytes_vs : lds_es_max_bytes_tes;
   unsigned max_exported_args = b->shader->info.stage == MESA_SHADER_VERTEX ? 2 : 4;
   unsigned total_es_lds_bytes = pervertex_lds_bytes * 256; /* TODO: why doesn't nogs_state->max_es_num_vertices work here? */
   unsigned max_num_waves = nogs_state->max_num_waves;
   unsigned ngg_scratch_lds_base_addr = DIV_ROUND_UP(total_es_lds_bytes, 16u) * 16u;
   unsigned ngg_scratch_lds_bytes = max_num_waves * 4u;
   nogs_state->total_lds_bytes = ngg_scratch_lds_base_addr + ngg_scratch_lds_bytes;

   nir_function_impl *impl = nir_shader_get_entrypoint(b->shader);

   nir_variable *position_value_var = nogs_state->position_value_var;
   nir_variable *prim_exp_arg_var = nogs_state->prim_exp_arg_var;
   nir_variable *gs_accepted_var = nir_local_variable_create(impl, glsl_bool_type(), "gs_accepted");
   nir_variable *es_accepted_var = nir_local_variable_create(impl, glsl_bool_type(), "es_accepted");
   nir_variable *has_vtx_var = nir_local_variable_create(impl, glsl_bool_type(), "has_vtx");
   nir_variable *has_prm_var = nir_local_variable_create(impl, glsl_bool_type(), "has_prm");
   nir_variable *gs_vtxaddr_vars[3] = {
      nir_local_variable_create(impl, glsl_uint_type(), "gs_vtx0_idx"),
      nir_local_variable_create(impl, glsl_uint_type(), "gs_vtx1_idx"),
      nir_local_variable_create(impl, glsl_uint_type(), "gs_vtx2_idx"),
   };
   nir_variable *repacked_arg_vars[4] = {
      nir_local_variable_create(impl, glsl_uint_type(), "repacked_arg_0"),
      nir_local_variable_create(impl, glsl_uint_type(), "repacked_arg_1"),
      nir_local_variable_create(impl, glsl_uint_type(), "repacked_arg_2"),
      nir_local_variable_create(impl, glsl_uint_type(), "repacked_arg_3"),
   };

   b->cursor = nir_before_cf_list(&impl->body);

   /* We are going to reuse these several times, so why not calculate them at the beginning */
   nir_ssa_def *invocation_index = nir_build_load_local_invocation_index(b);
   nir_ssa_def *es_vertex_lds_addr = pervertex_lds_addr(b, invocation_index, pervertex_lds_bytes);
   nir_ssa_def *any_culling_enabled = nir_build_load_cull_any_enabled_amd(b);

   nir_if *if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
   {
      /* Initialize the position output variable to zeroes, in case not all VS/TES invocations store the output.
       * The spec doesn't require it, but we use (0, 0, 0, 1) because some games rely on that.
       */
      nir_store_var(b, position_value_var, nir_imm_vec4(b, 0.0f, 0.0f, 0.0f, 1.0f), 0xfu);

      /* Now reinsert a clone of the shader code */
      struct hash_table *remap_table = _mesa_pointer_hash_table_create(NULL);
      nir_cf_list_clone_and_reinsert(original_extracted_cf, &if_es_thread->cf_node, b->cursor, remap_table);
      _mesa_hash_table_destroy(remap_table, NULL);
      b->cursor = nir_after_cf_list(&if_es_thread->then_list);

      /* Remember the current thread's shader arguments */
      if (b->shader->info.stage == MESA_SHADER_VERTEX) {
         nir_store_var(b, repacked_arg_vars[0], nir_build_load_vertex_id_zero_base(b), 0x1u);
         nir_store_var(b, repacked_arg_vars[1], nir_build_load_instance_id(b), 0x1u);
      } else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL) {
         nir_ssa_def *tess_coord = nir_build_load_tess_coord(b);
         nir_store_var(b, repacked_arg_vars[0], nir_channel(b, tess_coord, 0), 0x1u);
         nir_store_var(b, repacked_arg_vars[1], nir_channel(b, tess_coord, 1), 0x1u);
         nir_store_var(b, repacked_arg_vars[2], nir_build_load_tess_rel_patch_id_amd(b), 0x1u);
         nir_store_var(b, repacked_arg_vars[3], nir_build_load_primitive_id(b), 0x1u);
      } else {
         unreachable("Should be VS or TES.");
      }

      /* Don't use LDS here if culling is not enabled */
      nir_if *if_cull_en = nir_push_if(b, any_culling_enabled);
      {
         /* Clear out the ES accepted flag in LDS */
         nir_build_store_shared(b, nir_imm_zero(b, 1, 8), es_vertex_lds_addr, .write_mask = 0x1u, .align_mul = 4, .base = lds_es_vertex_accepted);

         /* Store position components that are relevant to culling in LDS */
         nir_ssa_def *pre_cull_pos = nir_load_var(b, position_value_var);
         nir_ssa_def *pre_cull_w = nir_channel(b, pre_cull_pos, 3);
         nir_build_store_shared(b, pre_cull_w, es_vertex_lds_addr, .write_mask = 0x1u, .align_mul = 4, .base = lds_es_pos_w);
         nir_ssa_def *pre_cull_x_div_w = nir_fdiv(b, nir_channel(b, pre_cull_pos, 0), pre_cull_w);
         nir_build_store_shared(b, pre_cull_x_div_w, es_vertex_lds_addr, .write_mask = 0x1u, .align_mul = 4, .base = lds_es_pos_x);
         nir_ssa_def *pre_cull_y_div_w = nir_fdiv(b, nir_channel(b, pre_cull_pos, 1), pre_cull_w);
         nir_build_store_shared(b, pre_cull_y_div_w, es_vertex_lds_addr, .write_mask = 0x1u, .align_mul = 4, .base = lds_es_pos_y);
      }
      nir_pop_if(b, if_cull_en);
   }
   nir_pop_if(b, if_es_thread);

   nir_metadata_preserve(impl, nir_metadata_none);

   /* Remove all non-position outputs, and put the position output into the variable. */
   remove_culling_shader_outputs(b->shader, nogs_state, position_value_var);
   b->cursor = nir_after_cf_list(&impl->body);

   nir_if *if_cull_en = nir_push_if(b, any_culling_enabled);
   {
      nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                            .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_mem_shared);

      nir_store_var(b, gs_accepted_var, nir_imm_bool(b, false), 0x1u);

      nir_if *if_gs_thread = nir_push_if(b, nir_build_has_input_primitive_amd(b));
      {
         /* Load vertex indices from input VGPRs */
         nir_ssa_def *vtx_idx[3] = {0};
         for (unsigned vertex = 0; vertex < 3; ++vertex)
            vtx_idx[vertex] = ngg_input_primitive_vertex_index(b, vertex);

         /* Load positions of each vertex */
         nir_ssa_def *vtx_addr[3] = {0};
         nir_ssa_def *pos[3][4] = {0};
         for (unsigned vtx = 0; vtx < 3; ++vtx) {
            vtx_addr[vtx] = pervertex_lds_addr(b, vtx_idx[vtx], pervertex_lds_bytes);
            pos[vtx][0] = nir_build_load_shared(b, 1, 32, vtx_addr[vtx], .align_mul = 4u, .base = lds_es_pos_x);
            pos[vtx][1] = nir_build_load_shared(b, 1, 32, vtx_addr[vtx], .align_mul = 4u, .base = lds_es_pos_y);
            pos[vtx][3] = nir_build_load_shared(b, 1, 32, vtx_addr[vtx], .align_mul = 4u, .base = lds_es_pos_w);
            nir_store_var(b, gs_vtxaddr_vars[vtx], vtx_addr[vtx], 0x1u);
         }

         /* See if the current primitive is accepted */
         nir_ssa_def *accepted = ac_nir_cull_triangle(b, nir_imm_bool(b, true), pos);
         nir_store_var(b, gs_accepted_var, accepted, 0x1u);

         nir_if *if_gs_accepted = nir_push_if(b, accepted);
         {
            /* Store the accepted state to LDS for ES threads */
            for (unsigned vtx = 0; vtx < 3; ++vtx)
               nir_build_store_shared(b, nir_imm_intN_t(b, 0xff, 8), vtx_addr[vtx], .base = lds_es_vertex_accepted, .align_mul = 4u, .write_mask = 0x1u);
         }
         nir_pop_if(b, if_gs_accepted);
      }
      nir_pop_if(b, if_gs_thread);

      /* Workgroup barrier: wait for all waves to finish calculating and storing whether the vertices were accepted */
      nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                            .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_mem_shared);

      nir_store_var(b, es_accepted_var, nir_imm_bool(b, false), 0x1u);

      if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
      {
         nir_ssa_def *accepted = nir_build_load_shared(b, 1, 8u, es_vertex_lds_addr, .base = lds_es_vertex_accepted, .align_mul = 4u);
         nir_ssa_def *accepted_bool = nir_ine(b, accepted, nir_imm_intN_t(b, 0, 8));
         nir_store_var(b, es_accepted_var, accepted_bool, 0x1u);
      }
      nir_pop_if(b, if_es_thread);

      nir_ssa_def *es_accepted = nir_load_var(b, es_accepted_var);

      /* On all threads, perform a workgroup reduction + exclusive scan.
       * The reduction result is the total number of accepted threads in the workgroup.
       * The exclusive scan result is the ID of the thread which will export the current thread's vertex.
       */
      wg_scan_result wg_scan = workgroup_reduce_and_exclusive_scan(b, es_accepted, ngg_scratch_lds_base_addr, max_num_waves);
      nir_ssa_def *num_live_vertices_in_workgroup = wg_scan.reduction_result;
      nir_ssa_def *es_exporter_tid = wg_scan.excl_scan_result;

      /* TODO: add shortcut exit when all vertices are culled (currently terminate is only usable for PS) */

      nir_if *if_es_accepted = nir_push_if(b, es_accepted);
      {
         nir_ssa_def *exporter_addr = pervertex_lds_addr(b, es_exporter_tid, pervertex_lds_bytes);

         /* Store the exporter thread's index to the LDS space of the current thread so GS threads can load it */
         nir_build_store_shared(b, nir_u2u8(b, es_exporter_tid), es_vertex_lds_addr, .base = lds_es_exporter_tid, .align_mul = 1u, .write_mask = 0x1u);

         /* Store the current thread's position output to the exporter thread's LDS space */
         nir_ssa_def *pos = nir_load_var(b, position_value_var);
         nir_build_store_shared(b, pos, exporter_addr, .base = lds_es_pos_x, .align_mul = 4u, .write_mask = 0xfu);

         /* Store the current thread's repackable arguments to the exporter thread's LDS space */
         for (unsigned i = 0; i < max_exported_args; ++i) {
            nir_ssa_def *arg_val = nir_load_var(b, repacked_arg_vars[i]);
            nir_build_store_shared(b, arg_val, exporter_addr, .base = lds_es_arg_0 + 4u * i, .align_mul = 4u, .write_mask = 0x1u);
         }
      }
      nir_pop_if(b, if_es_accepted);

      /* If all vertices are culled, set primitive count to 0 as well (otherwise the HW hangs) */
      nir_ssa_def *num_exported_prims = nir_build_load_workgroup_num_input_primitives_amd(b);
      num_exported_prims = nir_bcsel(b, nir_ieq_imm(b, num_live_vertices_in_workgroup, 0u), nir_imm_int(b, 0u), num_exported_prims);

      nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_int(b, 0)));
      {
         /* Tell the final vertex and primitive count to the HW */
         nir_build_alloc_vertices_and_primitives_amd(b, num_live_vertices_in_workgroup, num_exported_prims);
      }
      nir_pop_if(b, if_wave_0);

      nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                            .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_mem_shared);

      nir_if *if_packed_es_thread = nir_push_if(b, nir_ilt(b, invocation_index, num_live_vertices_in_workgroup));
      {
         /* Read position from the current ES thread's LDS space (written by the exported vertex's ES thread) */
         nir_ssa_def *exported_pos = nir_build_load_shared(b, 4, 32, es_vertex_lds_addr, .base = lds_es_pos_x, .align_mul = 4u);
         nir_store_var(b, position_value_var, exported_pos, 0xfu);

         /* Read the repacked arguments */
         for (unsigned i = 0; i < max_exported_args; ++i) {
            nir_ssa_def *arg_val = nir_build_load_shared(b, 1, 32, es_vertex_lds_addr, .base = lds_es_arg_0 + 4u * i, .align_mul = 4u);
            nir_store_var(b, repacked_arg_vars[i], arg_val, 0x1u);
         }
      }
      nir_pop_if(b, if_packed_es_thread);

      nir_store_var(b, prim_exp_arg_var, nir_imm_int(b, 1 << 31), 0x1u);

      nir_if *if_gs_accepted = nir_push_if(b, nir_load_var(b, gs_accepted_var));
      {
         nir_ssa_def *exporter_vtx_indices[3] = {0};

         /* Load the index of the ES threads that will export the current GS thread's vertices */
         for (unsigned v = 0; v < 3; ++v) {
            nir_ssa_def *vtx_addr = nir_load_var(b, gs_vtxaddr_vars[v]);
            nir_ssa_def *exporter_vtx_idx = nir_build_load_shared(b, 1, 8, vtx_addr, .base = lds_es_exporter_tid, .align_mul = 1u);
            exporter_vtx_indices[v] = nir_u2u32(b, exporter_vtx_idx);
         }

         nir_ssa_def *prim_exp_arg = emit_pack_ngg_prim_exp_arg(b, 3, exporter_vtx_indices, NULL);
         nir_store_var(b, prim_exp_arg_var, prim_exp_arg, 0x1u);
      }
      nir_pop_if(b, if_gs_accepted);

      /* Calculate the number of vertices and primitives left in the current wave */
      nir_ssa_def *has_vtx_after_culling = nir_ilt(b, invocation_index, num_live_vertices_in_workgroup);
      nir_ssa_def *has_prm_after_culling = nir_ilt(b, invocation_index, num_exported_prims);
      nir_store_var(b, has_vtx_var, has_vtx_after_culling, 0x1u);
      nir_store_var(b, has_prm_var, has_prm_after_culling, 0x1u);

   }
   nir_push_else(b, if_cull_en);
   {
      nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_int(b, 0)));
      {
         nir_ssa_def *vtx_cnt = nir_build_load_workgroup_num_input_vertices_amd(b);
         nir_ssa_def *prim_cnt = nir_build_load_workgroup_num_input_primitives_amd(b);
         nir_build_alloc_vertices_and_primitives_amd(b, vtx_cnt, prim_cnt);
      }
      nir_pop_if(b, if_wave_0);
      nir_store_var(b, prim_exp_arg_var, emit_ngg_nogs_prim_exp_arg(b, nogs_state), 0x1u);
      nir_store_var(b, has_vtx_var, nir_build_has_input_vertex_amd(b), 0x1u);
      nir_store_var(b, has_prm_var, nir_build_has_input_primitive_amd(b), 0x1u);
   }
   nir_pop_if(b, if_cull_en);

   /* These "overwrite" intrinsics must be at the top level,
    * otherwise they can mess up the backend's control flow (eg. ACO's SSA).
    *
    * TODO:
    * A cleaner solution would be to simply replace all usages of these args
    * with the load of the variables.
    * However, this wouldn't work right now because the backend uses the arguments
    * for purposes not expressed in NIR, eg. VS input loads, etc.
    * This can change if VS input loads and other stuff are lowered to eg. load_buffer_amd.
    */

   /* Overwrite the shader arguments. */
   if (b->shader->info.stage == MESA_SHADER_VERTEX)
      nir_build_overwrite_vs_arguments_amd(b,
         nir_load_var(b, repacked_arg_vars[0]), nir_load_var(b, repacked_arg_vars[1]));
   else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL)
      nir_build_overwrite_tes_arguments_amd(b,
         nir_load_var(b, repacked_arg_vars[0]), nir_load_var(b, repacked_arg_vars[1]),
         nir_load_var(b, repacked_arg_vars[2]), nir_load_var(b, repacked_arg_vars[3]));
   else
      unreachable("Should be VS or TES.");

   nir_ssa_def *vertices_left_in_wave = nir_bit_count(b, nir_build_ballot(b, 1, 64, nir_load_var(b, has_vtx_var)));
   nir_ssa_def *primitives_left_in_wave = nir_bit_count(b, nir_build_ballot(b, 1, 64, nir_load_var(b, has_prm_var)));
   nir_build_overwrite_subgroup_num_vertices_and_primitives_amd(b, vertices_left_in_wave, primitives_left_in_wave);
}

static bool
can_use_deferred_attribute_culling(nir_shader *shader)
{
   /* When the shader writes memory, it is difficult to guarantee correctness.
    * Future work:
    * - if only write-only SSBOs are used
    * - if we can prove that non-position outputs don't rely on memory stores
    * then may be okay to keep the memory stores in the 1st shader part, and delete them from the 2nd.
    */
   if (shader->info.writes_memory)
      return false;

   /* When the shader relies on the subgroup invocation ID, we'd break it, because the ID changes after the culling.
    * Future work: try to save this to LDS and reload, but it can still be broken in subtle ways.
    */
   if (BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_SUBGROUP_INVOCATION))
      return false;

   return true;
}

ac_nir_ngg_config
ac_nir_lower_ngg_nogs(nir_shader *shader,
                      unsigned max_num_es_vertices,
                      unsigned num_vertices_per_primitives,
                      unsigned max_workgroup_size,
                      unsigned wave_size,
                      bool consider_culling,
                      bool consider_passthrough,
                      bool export_prim_id,
                      bool provoking_vtx_last)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl);
   assert(max_num_es_vertices && max_workgroup_size && wave_size);

   bool can_cull = consider_culling && (num_vertices_per_primitives == 3) &&
                   can_use_deferred_attribute_culling(shader);
   bool passthrough = consider_passthrough && !can_cull &&
                      !(shader->info.stage == MESA_SHADER_VERTEX && export_prim_id);

   nir_variable *position_value_var = nir_local_variable_create(impl, glsl_vec4_type(), "position_value");
   nir_variable *prim_exp_arg_var = nir_local_variable_create(impl, glsl_uint_type(), "prim_exp_arg");

   lower_ngg_nogs_state state = {
      .passthrough = passthrough,
      .export_prim_id = export_prim_id,
      .early_prim_export = exec_list_is_singular(&impl->body),
      .num_vertices_per_primitives = num_vertices_per_primitives,
      .provoking_vtx_idx = provoking_vtx_last ? (num_vertices_per_primitives - 1) : 0,
      .position_value_var = position_value_var,
      .prim_exp_arg_var = prim_exp_arg_var,
      .max_num_waves = DIV_ROUND_UP(max_workgroup_size, wave_size),
      .max_es_num_vertices = max_num_es_vertices,
   };

   /* We need LDS space when VS needs to export the primitive ID. */
   if (shader->info.stage == MESA_SHADER_VERTEX && export_prim_id)
      state.total_lds_bytes = max_num_es_vertices * 4u;

   unsigned lds_bytes_if_culling_off = state.total_lds_bytes;

   nir_cf_list extracted;
   nir_cf_extract(&extracted, nir_before_cf_list(&impl->body), nir_after_cf_list(&impl->body));

   nir_builder builder;
   nir_builder *b = &builder; /* This is to avoid the & */
   nir_builder_init(b, impl);
   b->cursor = nir_before_cf_list(&impl->body);

   if (!can_cull) {
      /* Allocate export space on wave 0 - confirm to the HW that we want to use all possible space */
      nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_int(b, 0)));
      {
         nir_ssa_def *vtx_cnt = nir_build_load_workgroup_num_input_vertices_amd(b);
         nir_ssa_def *prim_cnt = nir_build_load_workgroup_num_input_primitives_amd(b);
         nir_build_alloc_vertices_and_primitives_amd(b, vtx_cnt, prim_cnt);
      }
      nir_pop_if(b, if_wave_0);

      /* Take care of early primitive export, otherwise just pack the primitive export argument */
      if (state.early_prim_export)
         emit_ngg_nogs_prim_export(b, &state, NULL);
      else
         nir_store_var(b, prim_exp_arg_var, emit_ngg_nogs_prim_exp_arg(b, &state), 0x1u);
   } else {
      add_deferred_attribute_culling(b, &extracted, &state);
      b->cursor = nir_after_cf_list(&impl->body);

      if (state.early_prim_export)
         emit_ngg_nogs_prim_export(b, &state, nir_load_var(b, state.prim_exp_arg_var));
   }

   nir_if *if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
   {
      if (can_cull) {
         nir_ssa_def *pos_val = nir_load_var(b, state.position_value_var);
         nir_io_semantics io_sem = { .location = VARYING_SLOT_POS, .num_slots = 1 };
         nir_build_store_output(b, pos_val, nir_imm_int(b, 0), .base = VARYING_SLOT_POS, .component = 0, .io_semantics = io_sem, .write_mask = 0xfu);
      }

      /* Run the actual shader */
      nir_cf_reinsert(&extracted, b->cursor);
      b->cursor = nir_after_cf_list(&if_es_thread->then_list);

      /* Export all vertex attributes (except primitive ID) */
      nir_build_export_vertex_amd(b);

      /* Export primitive ID (in case of early primitive export or TES) */
      if (state.export_prim_id && (state.early_prim_export || shader->info.stage != MESA_SHADER_VERTEX))
         emit_store_ngg_nogs_es_primitive_id(b);
   }
   nir_pop_if(b, if_es_thread);

   /* Take care of late primitive export */
   if (!state.early_prim_export) {
      emit_ngg_nogs_prim_export(b, &state, nir_load_var(b, prim_exp_arg_var));
      if (state.export_prim_id && shader->info.stage == MESA_SHADER_VERTEX) {
         if_es_thread = nir_push_if(b, nir_build_has_input_vertex_amd(b));
         emit_store_ngg_nogs_es_primitive_id(b);
         nir_pop_if(b, if_es_thread);
      }
   }

   if (can_cull) {
      remove_extra_pos_outputs(shader, &state);
   }

   nir_metadata_preserve(impl, nir_metadata_none);
   nir_validate_shader(shader, "after emitting NGG VS/TES");

   /* Cleanup */
   nir_lower_vars_to_ssa(shader);
   nir_remove_dead_variables(shader, nir_var_function_temp, NULL);
   nir_opt_undef(shader);

   nir_validate_shader(shader, "after cleaning up NGG VS/TES");

   shader->info.shared_size = state.total_lds_bytes;

   ac_nir_ngg_config ret = {
      .can_cull = can_cull,
      .passthrough = passthrough,
      .lds_bytes_if_culling_off = lds_bytes_if_culling_off,
   };

   return ret;
}

static nir_ssa_def *
ngg_gs_out_vertex_addr(nir_builder *b, nir_ssa_def *out_vtx_idx, lower_ngg_gs_state *s)
{
   unsigned write_stride_2exp = ffs(MAX2(b->shader->info.gs.vertices_out, 1)) - 1;

   /* gs_max_out_vertices = 2^(write_stride_2exp) * some odd number */
   if (write_stride_2exp) {
      nir_ssa_def *row = nir_ushr_imm(b, out_vtx_idx, 5);
      nir_ssa_def *swizzle = nir_iand_imm(b, row, (1u << write_stride_2exp) - 1u);
      out_vtx_idx = nir_ixor(b, out_vtx_idx, swizzle);
   }

   nir_ssa_def *out_vtx_offs = nir_imul_imm(b, out_vtx_idx, s->lds_bytes_per_gs_out_vertex);
   return nir_iadd_imm_nuw(b, out_vtx_offs, s->lds_addr_gs_out_vtx);
}

static nir_ssa_def *
ngg_gs_emit_vertex_addr(nir_builder *b, nir_ssa_def *gs_vtx_idx, lower_ngg_gs_state *s)
{
   nir_ssa_def *tid_in_tg = nir_build_load_local_invocation_index(b);
   nir_ssa_def *gs_out_vtx_base = nir_imul_imm(b, tid_in_tg, b->shader->info.gs.vertices_out);
   nir_ssa_def *out_vtx_idx = nir_iadd_nuw(b, gs_out_vtx_base, gs_vtx_idx);

   return ngg_gs_out_vertex_addr(b, out_vtx_idx, s);
}

static void
ngg_gs_clear_primflags(nir_builder *b, nir_ssa_def *num_vertices, unsigned stream, lower_ngg_gs_state *s)
{
   nir_ssa_def *zero_u8 = nir_imm_zero(b, 1, 8);
   nir_store_var(b, s->current_clear_primflag_idx_var, num_vertices, 0x1u);

   nir_loop *loop = nir_push_loop(b);
   {
      nir_ssa_def *current_clear_primflag_idx = nir_load_var(b, s->current_clear_primflag_idx_var);
      nir_if *if_break = nir_push_if(b, nir_uge(b, current_clear_primflag_idx, nir_imm_int(b, b->shader->info.gs.vertices_out)));
      {
         nir_jump(b, nir_jump_break);
      }
      nir_push_else(b, if_break);
      {
         nir_ssa_def *emit_vtx_addr = ngg_gs_emit_vertex_addr(b, current_clear_primflag_idx, s);
         nir_build_store_shared(b, zero_u8, emit_vtx_addr, .base = s->lds_offs_primflags + stream, .align_mul = 1, .write_mask = 0x1u);
         nir_store_var(b, s->current_clear_primflag_idx_var, nir_iadd_imm_nuw(b, current_clear_primflag_idx, 1), 0x1u);
      }
      nir_pop_if(b, if_break);
   }
   nir_pop_loop(b, loop);
}

static void
ngg_gs_shader_query(nir_builder *b, nir_intrinsic_instr *intrin, lower_ngg_gs_state *s)
{
   nir_if *if_shader_query = nir_push_if(b, nir_build_load_shader_query_enabled_amd(b));
   nir_ssa_def *num_prims_in_wave = NULL;

   /* Calculate the "real" number of emitted primitives from the emitted GS vertices and primitives.
    * GS emits points, line strips or triangle strips.
    * Real primitives are points, lines or triangles.
    */
   if (nir_src_is_const(intrin->src[0]) && nir_src_is_const(intrin->src[1])) {
      unsigned gs_vtx_cnt = nir_src_as_uint(intrin->src[0]);
      unsigned gs_prm_cnt = nir_src_as_uint(intrin->src[1]);
      unsigned total_prm_cnt = gs_vtx_cnt - gs_prm_cnt * (s->num_vertices_per_primitive - 1u);
      nir_ssa_def *num_threads = nir_bit_count(b, nir_build_ballot(b, 1, 64, nir_imm_bool(b, true)));
      num_prims_in_wave = nir_imul_imm(b, num_threads, total_prm_cnt);
   } else {
      nir_ssa_def *gs_vtx_cnt = intrin->src[0].ssa;
      nir_ssa_def *prm_cnt = intrin->src[1].ssa;
      if (s->num_vertices_per_primitive > 1)
         prm_cnt = nir_iadd_nuw(b, nir_imul_imm(b, prm_cnt, -1u * (s->num_vertices_per_primitive - 1)), gs_vtx_cnt);
      num_prims_in_wave = nir_build_reduce(b, prm_cnt, .reduction_op = nir_op_iadd);
   }

   /* Store the query result to GDS using an atomic add. */
   nir_if *if_first_lane = nir_push_if(b, nir_build_elect(b, 1));
   nir_build_gds_atomic_add_amd(b, 32, num_prims_in_wave, nir_imm_int(b, 0), nir_imm_int(b, 0x100));
   nir_pop_if(b, if_first_lane);

   nir_pop_if(b, if_shader_query);
}

static bool
lower_ngg_gs_store_output(nir_builder *b, nir_intrinsic_instr *intrin, lower_ngg_gs_state *s)
{
   assert(nir_src_is_const(intrin->src[1]));
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned writemask = nir_intrinsic_write_mask(intrin);
   unsigned base = nir_intrinsic_base(intrin);
   unsigned component_offset = nir_intrinsic_component(intrin);
   unsigned base_offset = nir_src_as_uint(intrin->src[1]);
   nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);

   assert((base + base_offset) < VARYING_SLOT_MAX);

   nir_ssa_def *store_val = intrin->src[0].ssa;

   for (unsigned comp = 0; comp < 4; ++comp) {
      if (!(writemask & (1 << comp)))
         continue;
      unsigned stream = (io_sem.gs_streams >> (comp * 2)) & 0x3;
      if (!(b->shader->info.gs.active_stream_mask & (1 << stream)))
         continue;

      /* Small bitsize components consume the same amount of space as 32-bit components,
       * but 64-bit ones consume twice as many. (Vulkan spec 15.1.5)
       */
      unsigned num_consumed_components = MIN2(1, DIV_ROUND_UP(store_val->bit_size, 32));
      nir_ssa_def *element = nir_channel(b, store_val, comp);
      if (num_consumed_components > 1)
         element = nir_extract_bits(b, &element, 1, 0, num_consumed_components, 32);

      for (unsigned c = 0; c < num_consumed_components; ++c) {
         unsigned component_index =  (comp * num_consumed_components) + c + component_offset;
         unsigned base_index = base + base_offset + component_index / 4;
         component_index %= 4;

         /* Save output usage info */
         gs_output_component_info *info = &s->output_component_info[base_index][component_index];
         info->bit_size = MAX2(info->bit_size, MIN2(store_val->bit_size, 32));
         info->stream = stream;

         /* Store the current component element */
         nir_ssa_def *component_element = element;
         if (num_consumed_components > 1)
            component_element = nir_channel(b, component_element, c);
         if (component_element->bit_size != 32)
            component_element = nir_u2u32(b, component_element);

         nir_store_var(b, s->output_vars[base_index][component_index], component_element, 0x1u);
      }
   }

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_ngg_gs_emit_vertex_with_counter(nir_builder *b, nir_intrinsic_instr *intrin, lower_ngg_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);
   if (!(b->shader->info.gs.active_stream_mask & (1 << stream))) {
      nir_instr_remove(&intrin->instr);
      return true;
   }

   nir_ssa_def *gs_emit_vtx_idx = intrin->src[0].ssa;
   nir_ssa_def *current_vtx_per_prim = intrin->src[1].ssa;
   nir_ssa_def *gs_emit_vtx_addr = ngg_gs_emit_vertex_addr(b, gs_emit_vtx_idx, s);

   for (unsigned slot = 0; slot < VARYING_SLOT_MAX; ++slot) {
      unsigned packed_location = util_bitcount64((b->shader->info.outputs_written & BITFIELD64_MASK(slot)));

      for (unsigned comp = 0; comp < 4; ++comp) {
         gs_output_component_info *info = &s->output_component_info[slot][comp];
         if (info->stream != stream || !info->bit_size)
            continue;

         /* Store the output to LDS */
         nir_ssa_def *out_val = nir_load_var(b, s->output_vars[slot][comp]);
         if (info->bit_size != 32)
            out_val = nir_u2u(b, out_val, info->bit_size);

         nir_build_store_shared(b, out_val, gs_emit_vtx_addr, .base = packed_location * 16 + comp * 4, .align_mul = 4, .write_mask = 0x1u);

         /* Clear the variable that holds the output */
         nir_store_var(b, s->output_vars[slot][comp], nir_ssa_undef(b, 1, 32), 0x1u);
      }
   }

   /* Calculate and store per-vertex primitive flags based on vertex counts:
    * - bit 0: whether this vertex finishes a primitive (a real primitive, not the strip)
    * - bit 1: whether the primitive index is odd (if we are emitting triangle strips, otherwise always 0)
    * - bit 2: always 1 (so that we can use it for determining vertex liveness)
    */

   nir_ssa_def *completes_prim = nir_ige(b, current_vtx_per_prim, nir_imm_int(b, s->num_vertices_per_primitive - 1));
   nir_ssa_def *prim_flag = nir_bcsel(b, completes_prim, nir_imm_int(b, 0b101u), nir_imm_int(b, 0b100u));

   if (s->num_vertices_per_primitive == 3) {
      nir_ssa_def *odd = nir_iand_imm(b, current_vtx_per_prim, 1);
      prim_flag = nir_iadd_nuw(b, prim_flag, nir_ishl(b, odd, nir_imm_int(b, 1)));
   }

   nir_build_store_shared(b, nir_u2u8(b, prim_flag), gs_emit_vtx_addr, .base = s->lds_offs_primflags + stream, .align_mul = 4u, .write_mask = 0x1u);
   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_ngg_gs_end_primitive_with_counter(nir_builder *b, nir_intrinsic_instr *intrin, UNUSED lower_ngg_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   /* These are not needed, we can simply remove them */
   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_ngg_gs_set_vertex_and_primitive_count(nir_builder *b, nir_intrinsic_instr *intrin, lower_ngg_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);
   if (stream > 0 && !(b->shader->info.gs.active_stream_mask & (1 << stream))) {
      nir_instr_remove(&intrin->instr);
      return true;
   }

   s->found_out_vtxcnt[stream] = true;

   /* Clear the primitive flags of non-emitted vertices */
   if (!nir_src_is_const(intrin->src[0]) || nir_src_as_uint(intrin->src[0]) < b->shader->info.gs.vertices_out)
      ngg_gs_clear_primflags(b, intrin->src[0].ssa, stream, s);

   ngg_gs_shader_query(b, intrin, s);
   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_ngg_gs_intrinsic(nir_builder *b, nir_instr *instr, void *state)
{
   lower_ngg_gs_state *s = (lower_ngg_gs_state *) state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_output)
      return lower_ngg_gs_store_output(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_emit_vertex_with_counter)
      return lower_ngg_gs_emit_vertex_with_counter(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_end_primitive_with_counter)
      return lower_ngg_gs_end_primitive_with_counter(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count)
      return lower_ngg_gs_set_vertex_and_primitive_count(b, intrin, s);

   return false;
}

static void
lower_ngg_gs_intrinsics(nir_shader *shader, lower_ngg_gs_state *s)
{
   nir_shader_instructions_pass(shader, lower_ngg_gs_intrinsic, nir_metadata_none, s);
}

static void
ngg_gs_export_primitives(nir_builder *b, nir_ssa_def *max_num_out_prims, nir_ssa_def *tid_in_tg,
                         nir_ssa_def *exporter_tid_in_tg, nir_ssa_def *primflag_0,
                         lower_ngg_gs_state *s)
{
   nir_if *if_prim_export_thread = nir_push_if(b, nir_ilt(b, tid_in_tg, max_num_out_prims));

   /* Only bit 0 matters here - set it to 1 when the primitive should be null */
   nir_ssa_def *is_null_prim = nir_ixor(b, primflag_0, nir_imm_int(b, -1u));

   nir_ssa_def *vtx_indices[3] = {0};
   vtx_indices[s->num_vertices_per_primitive - 1] = exporter_tid_in_tg;
   if (s->num_vertices_per_primitive >= 2)
      vtx_indices[s->num_vertices_per_primitive - 2] = nir_isub(b, exporter_tid_in_tg, nir_imm_int(b, 1));
   if (s->num_vertices_per_primitive == 3)
      vtx_indices[s->num_vertices_per_primitive - 3] = nir_isub(b, exporter_tid_in_tg, nir_imm_int(b, 2));

   if (s->num_vertices_per_primitive == 3) {
      /* API GS outputs triangle strips, but NGG HW understands triangles.
       * We already know the triangles due to how we set the primitive flags, but we need to
       * make sure the vertex order is so that the front/back is correct, and the provoking vertex is kept.
       */

      nir_ssa_def *is_odd = nir_ubfe(b, primflag_0, nir_imm_int(b, 1), nir_imm_int(b, 1));
      if (!s->provoking_vertex_last) {
         vtx_indices[1] = nir_iadd(b, vtx_indices[1], is_odd);
         vtx_indices[2] = nir_isub(b, vtx_indices[2], is_odd);
      } else {
         vtx_indices[0] = nir_iadd(b, vtx_indices[0], is_odd);
         vtx_indices[1] = nir_isub(b, vtx_indices[1], is_odd);
      }
   }

   nir_ssa_def *arg = emit_pack_ngg_prim_exp_arg(b, s->num_vertices_per_primitive, vtx_indices, is_null_prim);
   nir_build_export_primitive_amd(b, arg);
   nir_pop_if(b, if_prim_export_thread);
}

static void
ngg_gs_export_vertices(nir_builder *b, nir_ssa_def *max_num_out_vtx, nir_ssa_def *tid_in_tg,
                       nir_ssa_def *out_vtx_lds_addr, lower_ngg_gs_state *s)
{
   nir_if *if_vtx_export_thread = nir_push_if(b, nir_ilt(b, tid_in_tg, max_num_out_vtx));
   nir_ssa_def *exported_out_vtx_lds_addr = out_vtx_lds_addr;

   if (!s->output_compile_time_known) {
      /* Vertex compaction.
       * The current thread will export a vertex that was live in another invocation.
       * Load the index of the vertex that the current thread will have to export.
       */
      nir_ssa_def *exported_vtx_idx = nir_build_load_shared(b, 1, 8, out_vtx_lds_addr, .base = s->lds_offs_primflags + 1, .align_mul = 1u);
      exported_out_vtx_lds_addr = ngg_gs_out_vertex_addr(b, nir_u2u32(b, exported_vtx_idx), s);
   }

   for (unsigned slot = 0; slot < VARYING_SLOT_MAX; ++slot) {
      if (!(b->shader->info.outputs_written & BITFIELD64_BIT(slot)))
         continue;

      unsigned packed_location = util_bitcount64((b->shader->info.outputs_written & BITFIELD64_MASK(slot)));
      nir_io_semantics io_sem = { .location = slot, .num_slots = 1 };

      for (unsigned comp = 0; comp < 4; ++comp) {
         gs_output_component_info *info = &s->output_component_info[slot][comp];
         if (info->stream != 0 || info->bit_size == 0)
            continue;

         nir_ssa_def *load = nir_build_load_shared(b, 1, info->bit_size, exported_out_vtx_lds_addr, .base = packed_location * 16u + comp * 4u, .align_mul = 4u);
         nir_build_store_output(b, load, nir_imm_int(b, 0), .write_mask = 0x1u, .base = slot, .component = comp, .io_semantics = io_sem);
      }
   }

   nir_build_export_vertex_amd(b);
   nir_pop_if(b, if_vtx_export_thread);
}

static void
ngg_gs_setup_vertex_compaction(nir_builder *b, nir_ssa_def *vertex_live, nir_ssa_def *tid_in_tg,
                               nir_ssa_def *exporter_tid_in_tg, lower_ngg_gs_state *s)
{
   assert(vertex_live->bit_size == 1);
   nir_if *if_vertex_live = nir_push_if(b, vertex_live);
   {
      /* Setup the vertex compaction.
       * Save the current thread's id for the thread which will export the current vertex.
       * We reuse stream 1 of the primitive flag of the other thread's vertex for storing this.
       */

      nir_ssa_def *exporter_lds_addr = ngg_gs_out_vertex_addr(b, exporter_tid_in_tg, s);
      nir_ssa_def *tid_in_tg_u8 = nir_u2u8(b, tid_in_tg);
      nir_build_store_shared(b, tid_in_tg_u8, exporter_lds_addr, .base = s->lds_offs_primflags + 1, .align_mul = 1u, .write_mask = 0x1u);
   }
   nir_pop_if(b, if_vertex_live);
}

static nir_ssa_def *
ngg_gs_load_out_vtx_primflag_0(nir_builder *b, nir_ssa_def *tid_in_tg, nir_ssa_def *vtx_lds_addr,
                               nir_ssa_def *max_num_out_vtx, lower_ngg_gs_state *s)
{
   nir_ssa_def *zero = nir_imm_int(b, 0);

   nir_if *if_outvtx_thread = nir_push_if(b, nir_ilt(b, tid_in_tg, max_num_out_vtx));
   nir_ssa_def *primflag_0 = nir_build_load_shared(b, 1, 8, vtx_lds_addr, .base = s->lds_offs_primflags, .align_mul = 4u);
   primflag_0 = nir_u2u32(b, primflag_0);
   nir_pop_if(b, if_outvtx_thread);

   return nir_if_phi(b, primflag_0, zero);
}

static void
ngg_gs_finale(nir_builder *b, lower_ngg_gs_state *s)
{
   nir_ssa_def *tid_in_tg = nir_build_load_local_invocation_index(b);
   nir_ssa_def *max_vtxcnt = nir_build_load_workgroup_num_input_vertices_amd(b);
   nir_ssa_def *max_prmcnt = max_vtxcnt; /* They are currently practically the same; both RADV and RadeonSI do this. */
   nir_ssa_def *out_vtx_lds_addr = ngg_gs_out_vertex_addr(b, tid_in_tg, s);

   if (s->output_compile_time_known) {
      /* When the output is compile-time known, the GS writes all possible vertices and primitives it can.
       * The gs_alloc_req needs to happen on one wave only, and needs to be before a workgroup barrier, otherwise Navi 10 hangs on it.
       */
      nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_zero(b, 1, 32)));
      nir_build_alloc_vertices_and_primitives_amd(b, max_vtxcnt, max_prmcnt);
      nir_pop_if(b, if_wave_0);
   }

   /* Workgroup barrier: wait for all GS threads to finish */
   nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                         .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_shader_out|nir_var_mem_shared);

   nir_ssa_def *out_vtx_primflag_0 = ngg_gs_load_out_vtx_primflag_0(b, tid_in_tg, out_vtx_lds_addr, max_vtxcnt, s);

   if (s->output_compile_time_known) {
      ngg_gs_export_primitives(b, max_vtxcnt, tid_in_tg, tid_in_tg, out_vtx_primflag_0, s);
      ngg_gs_export_vertices(b, max_vtxcnt, tid_in_tg, out_vtx_lds_addr, s);
      return;
   }

   /* When the output is not known in compile time: there are gaps between the output vertices data in LDS.
    * However, we need to make sure that the vertex exports are packed, meaning that there shouldn't be any gaps
    * between the threads that perform the exports. We solve this using a perform a workgroup reduction + scan.
    */
   nir_ssa_def *vertex_live = nir_ine(b, out_vtx_primflag_0, nir_imm_zero(b, 1, out_vtx_primflag_0->bit_size));
   wg_scan_result wg_scan = workgroup_reduce_and_exclusive_scan(b, vertex_live, s->lds_addr_gs_scratch, s->max_num_waves);

   /* Reduction result = total number of vertices emitted in the workgroup. */
   nir_ssa_def *workgroup_num_vertices = wg_scan.reduction_result;
   /* Exclusive scan result = the index of the thread in the workgroup that will export the current thread's vertex. */
   nir_ssa_def *exporter_tid_in_tg = wg_scan.excl_scan_result;

   /* When the workgroup emits 0 total vertices, we also must export 0 primitives (otherwise the HW can hang). */
   nir_ssa_def *any_output = nir_ine(b, workgroup_num_vertices, nir_imm_int(b, 0));
   max_prmcnt = nir_bcsel(b, any_output, max_prmcnt, nir_imm_int(b, 0));

   /* Allocate export space. We currently don't compact primitives, just use the maximum number. */
   nir_if *if_wave_0 = nir_push_if(b, nir_ieq(b, nir_build_load_subgroup_id(b), nir_imm_zero(b, 1, 32)));
   nir_build_alloc_vertices_and_primitives_amd(b, workgroup_num_vertices, max_prmcnt);
   nir_pop_if(b, if_wave_0);

   /* Vertex compaction. This makes sure there are no gaps between threads that export vertices. */
   ngg_gs_setup_vertex_compaction(b, vertex_live, tid_in_tg, exporter_tid_in_tg, s);

   /* Workgroup barrier: wait for all LDS stores to finish. */
   nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                        .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_shader_out|nir_var_mem_shared);

   ngg_gs_export_primitives(b, max_prmcnt, tid_in_tg, exporter_tid_in_tg, out_vtx_primflag_0, s);
   ngg_gs_export_vertices(b, workgroup_num_vertices, tid_in_tg, out_vtx_lds_addr, s);
}

void
ac_nir_lower_ngg_gs(nir_shader *shader,
                    unsigned wave_size,
                    unsigned max_workgroup_size,
                    unsigned esgs_ring_lds_bytes,
                    unsigned gs_out_vtx_bytes,
                    unsigned gs_total_out_vtx_bytes,
                    bool provoking_vertex_last)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl);

   lower_ngg_gs_state state = {
      .max_num_waves = DIV_ROUND_UP(max_workgroup_size, wave_size),
      .lds_addr_gs_out_vtx = esgs_ring_lds_bytes,
      .lds_addr_gs_scratch = ALIGN(esgs_ring_lds_bytes + gs_total_out_vtx_bytes, 16u),
      .lds_offs_primflags = gs_out_vtx_bytes,
      .lds_bytes_per_gs_out_vertex = gs_out_vtx_bytes + 4u,
      .provoking_vertex_last = provoking_vertex_last,
   };

   unsigned lds_scratch_bytes = state.max_num_waves * 4u;
   unsigned total_lds_bytes = state.lds_addr_gs_scratch + lds_scratch_bytes;
   shader->info.shared_size = total_lds_bytes;

   nir_gs_count_vertices_and_primitives(shader, state.const_out_vtxcnt, state.const_out_prmcnt, 4u);
   state.output_compile_time_known = state.const_out_vtxcnt[0] == shader->info.gs.vertices_out &&
                                     state.const_out_prmcnt[0] != -1;

   if (!state.output_compile_time_known)
      state.current_clear_primflag_idx_var = nir_local_variable_create(impl, glsl_uint_type(), "current_clear_primflag_idx");

   if (shader->info.gs.output_primitive == GL_POINTS)
      state.num_vertices_per_primitive = 1;
   else if (shader->info.gs.output_primitive == GL_LINE_STRIP)
      state.num_vertices_per_primitive = 2;
   else if (shader->info.gs.output_primitive == GL_TRIANGLE_STRIP)
      state.num_vertices_per_primitive = 3;
   else
      unreachable("Invalid GS output primitive.");

   /* Extract the full control flow. It is going to be wrapped in an if statement. */
   nir_cf_list extracted;
   nir_cf_extract(&extracted, nir_before_cf_list(&impl->body), nir_after_cf_list(&impl->body));

   nir_builder builder;
   nir_builder *b = &builder; /* This is to avoid the & */
   nir_builder_init(b, impl);
   b->cursor = nir_before_cf_list(&impl->body);

   /* Workgroup barrier: wait for ES threads */
   nir_scoped_barrier(b, .execution_scope=NIR_SCOPE_WORKGROUP, .memory_scope=NIR_SCOPE_WORKGROUP,
                         .memory_semantics=NIR_MEMORY_ACQ_REL, .memory_modes=nir_var_shader_out|nir_var_mem_shared);

   /* Wrap the GS control flow. */
   nir_if *if_gs_thread = nir_push_if(b, nir_build_has_input_primitive_amd(b));

   /* Create and initialize output variables */
   for (unsigned slot = 0; slot < VARYING_SLOT_MAX; ++slot) {
      for (unsigned comp = 0; comp < 4; ++comp) {
         state.output_vars[slot][comp] = nir_local_variable_create(impl, glsl_uint_type(), "output");
      }
   }

   nir_cf_reinsert(&extracted, b->cursor);
   b->cursor = nir_after_cf_list(&if_gs_thread->then_list);
   nir_pop_if(b, if_gs_thread);

   /* Lower the GS intrinsics */
   lower_ngg_gs_intrinsics(shader, &state);
   b->cursor = nir_after_cf_list(&impl->body);

   if (!state.found_out_vtxcnt[0]) {
      fprintf(stderr, "Could not find set_vertex_and_primitive_count for stream 0. This would hang your GPU.");
      abort();
   }

   /* Emit the finale sequence */
   ngg_gs_finale(b, &state);
   nir_validate_shader(shader, "after emitting NGG GS");

   /* Cleanup */
   nir_lower_vars_to_ssa(shader);
   nir_remove_dead_variables(shader, nir_var_function_temp, NULL);
   nir_metadata_preserve(impl, nir_metadata_none);
}
