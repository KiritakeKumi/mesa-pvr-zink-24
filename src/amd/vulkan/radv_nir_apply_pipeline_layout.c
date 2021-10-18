/*
 * Copyright © 2020 Valve Corporation
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
#include "nir.h"
#include "nir_builder.h"
#include "radv_private.h"
#include "radv_shader.h"
#include "radv_shader_args.h"

struct apply_layout_state {
   enum chip_class chip_class;
   uint32_t address32_hi;
   bool has_image_load_dcc_bug;

   const struct radv_shader_args *args;
   const struct radv_shader_info *info;
   const struct radv_pipeline_layout *pipeline_layout;
   nir_builder builder;
};

enum descriptor_type {
   DESC_IMAGE,
   DESC_FMASK,
   DESC_SAMPLER,
   DESC_BUFFER,
   DESC_PLANE_0,
   DESC_PLANE_1,
   DESC_PLANE_2,
};

static nir_ssa_def *
get_arg(nir_builder *b, unsigned size, struct ac_arg arg)
{
   return nir_load_scalar_arg_amd(b, size, .base = arg.arg_index);
}

static nir_ssa_def *
convert_pointer_to_64_bit(struct apply_layout_state *state, nir_ssa_def *ptr)
{
   nir_builder *b = &state->builder;
   return nir_pack_64_2x32_split(b, ptr, nir_imm_int(b, state->address32_hi));
}

static nir_ssa_def *
load_desc_ptr(struct apply_layout_state *state, unsigned set)
{
   nir_builder *b = &state->builder;
   const struct radv_userdata_locations *user_sgprs_locs = &state->info->user_sgprs_locs;
   if (user_sgprs_locs->shader_data[AC_UD_INDIRECT_DESCRIPTOR_SETS].sgpr_idx != -1) {
      nir_ssa_def *addr = get_arg(b, 1, state->args->descriptor_sets[0]);
      addr = convert_pointer_to_64_bit(state, addr);
      return nir_load_smem_amd(b, 1, addr, nir_imm_int(b, set * 4), .align_mul = 4);
   }

   assert(state->args->descriptor_sets[set].used);
   return get_arg(b, 1, state->args->descriptor_sets[set]);
}

static void
visit_vulkan_resource_index(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   nir_builder *b = &state->builder;
   unsigned desc_set = nir_intrinsic_desc_set(intrin);
   unsigned binding = nir_intrinsic_binding(intrin);
   struct radv_descriptor_set_layout *layout = state->pipeline_layout->set[desc_set].layout;
   unsigned offset = layout->binding[binding].offset;
   unsigned stride;

   nir_ssa_def *set_ptr;
   if (layout->binding[binding].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
       layout->binding[binding].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
      unsigned idx = state->pipeline_layout->set[desc_set].dynamic_offset_start +
                     layout->binding[binding].dynamic_offset_offset;
      set_ptr = get_arg(b, 1, state->args->ac.push_constants);
      offset = state->pipeline_layout->push_constant_size + idx * 16;
      stride = 16;
   } else {
      set_ptr = load_desc_ptr(state, desc_set);
      stride = layout->binding[binding].size;
   }

   nir_ssa_def *binding_ptr = nir_imul_imm(b, intrin->src[0].ssa, stride);
   nir_instr_as_alu(binding_ptr->parent_instr)->no_unsigned_wrap = true;

   binding_ptr = nir_iadd_imm(b, binding_ptr, offset);
   nir_instr_as_alu(binding_ptr->parent_instr)->no_unsigned_wrap = true;

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                            nir_vec3(b, set_ptr, binding_ptr, nir_imm_int(b, stride)));
   nir_instr_remove(&intrin->instr);
}

static void
visit_vulkan_resource_reindex(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   ASSERTED VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);
   assert(desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

   nir_builder *b = &state->builder;

   nir_ssa_def *binding_ptr = nir_channel(b, intrin->src[0].ssa, 1);
   nir_ssa_def *stride = nir_channel(b, intrin->src[0].ssa, 2);

   nir_ssa_def *index = nir_imul(b, intrin->src[1].ssa, stride);
   nir_instr_as_alu(index->parent_instr)->no_unsigned_wrap = true;

   binding_ptr = nir_iadd_nuw(b, binding_ptr, index);

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                            nir_vector_insert_imm(b, intrin->src[0].ssa, binding_ptr, 1));
   nir_instr_remove(&intrin->instr);
}

static void
visit_load_vulkan_descriptor(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   nir_builder *b = &state->builder;

   if (nir_intrinsic_desc_type(intrin) == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
      nir_ssa_def *addr =
         convert_pointer_to_64_bit(state, nir_iadd(b, nir_channel(b, intrin->src[0].ssa, 0),
                                                   nir_channel(b, intrin->src[0].ssa, 1)));
      nir_ssa_def *desc = nir_build_load_global(b, 1, 64, addr, .access = ACCESS_NON_WRITEABLE,
                                                .align_mul = 8, .align_offset = 0);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);
   } else {
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                               nir_vector_insert_imm(b, intrin->src[0].ssa, nir_imm_int(b, 0), 2));
   }
   nir_instr_remove(&intrin->instr);
}

static nir_ssa_def *
load_inline_buffer_descriptor(struct apply_layout_state *state, nir_ssa_def *rsrc)
{
   nir_builder *b = &state->builder;

   uint32_t desc_type =
      S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
      S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);
   if (state->chip_class >= GFX10) {
      desc_type |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_FLOAT) |
                   S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW) | S_008F0C_RESOURCE_LEVEL(1);
   } else {
      desc_type |= S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
                   S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
   }

   return nir_vec4(b, rsrc, nir_imm_int(b, S_008F04_BASE_ADDRESS_HI(state->address32_hi)),
                   nir_imm_int(b, 0xffffffff), nir_imm_int(b, desc_type));
}

static nir_ssa_def *
load_buffer_descriptor(struct apply_layout_state *state, nir_ssa_def *rsrc, unsigned access)
{
   nir_builder *b = &state->builder;

   nir_binding binding = nir_chase_binding(nir_src_for_ssa(rsrc));

   if (binding.success) {
      struct radv_descriptor_set_layout *layout =
         state->pipeline_layout->set[binding.desc_set].layout;
      if (layout->binding[binding.binding].type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
         rsrc = nir_iadd(b, nir_channel(b, rsrc, 0), nir_channel(b, rsrc, 1));
         return load_inline_buffer_descriptor(state, rsrc);
      }
   }

   if (access & ACCESS_NON_UNIFORM)
      return nir_iadd(b, nir_channel(b, rsrc, 0), nir_channel(b, rsrc, 1));

   nir_ssa_def *desc_set = convert_pointer_to_64_bit(state, nir_channel(b, rsrc, 0));
   return nir_load_smem_amd(b, 4, desc_set, nir_channel(b, rsrc, 1), .align_mul = 16);
}

static void
visit_get_ssbo_size(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   nir_builder *b = &state->builder;
   nir_ssa_def *rsrc = intrin->src[0].ssa;

   nir_ssa_def *size;
   if (nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM) {
      nir_ssa_def *ptr = nir_iadd(b, nir_channel(b, rsrc, 0), nir_channel(b, rsrc, 1));
      ptr = nir_iadd_imm(b, ptr, 4);
      ptr = convert_pointer_to_64_bit(state, ptr);
      size =
         nir_build_load_global(b, 4, 32, ptr, .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER,
                               .align_mul = 16, .align_offset = 4);
   } else {
      /* load the entire descriptor so it can be CSE'd */
      nir_ssa_def *ptr = convert_pointer_to_64_bit(state, nir_channel(b, rsrc, 0));
      nir_ssa_def *desc = nir_load_smem_amd(b, 4, ptr, nir_channel(b, rsrc, 1), .align_mul = 16);
      size = nir_channel(b, desc, 2);
   }

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, size);
   nir_instr_remove(&intrin->instr);
}

static nir_ssa_def *
get_sampler_desc(struct apply_layout_state *state, nir_deref_instr *deref,
                 enum descriptor_type desc_type, bool non_uniform, nir_tex_instr *tex, bool write)
{
   nir_builder *b = &state->builder;

   nir_variable *var = nir_deref_instr_get_variable(deref);
   assert(var);
   unsigned desc_set = var->data.descriptor_set;
   unsigned binding_index = var->data.binding;
   bool indirect = nir_deref_instr_has_indirect(deref);

   struct radv_descriptor_set_layout *layout = state->pipeline_layout->set[desc_set].layout;
   struct radv_descriptor_set_binding_layout *binding = &layout->binding[binding_index];

   if (desc_type == DESC_SAMPLER && binding->immutable_samplers_offset &&
       (!indirect || binding->immutable_samplers_equal)) {
      unsigned constant_index = 0;
      if (!binding->immutable_samplers_equal) {
         while (deref->deref_type != nir_deref_type_var) {
            assert(deref->deref_type == nir_deref_type_array);
            unsigned array_size = MAX2(glsl_get_aoa_size(deref->type), 1);
            constant_index += nir_src_as_uint(deref->arr.index) * array_size;
            deref = nir_deref_instr_parent(deref);
         }
      }

      const uint32_t *samplers = radv_immutable_samplers(layout, binding);
      return nir_imm_ivec4(b, samplers[constant_index * 4 + 0], samplers[constant_index * 4 + 1],
                           samplers[constant_index * 4 + 2], samplers[constant_index * 4 + 3]);
   }

   unsigned size = 8;
   unsigned offset = binding->offset;
   switch (desc_type) {
   case DESC_IMAGE:
   case DESC_PLANE_0:
      break;
   case DESC_FMASK:
   case DESC_PLANE_1:
      offset += 32;
      break;
   case DESC_SAMPLER:
      size = 4;
      if (binding->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
         offset += radv_combined_image_descriptor_sampler_offset(binding);
      break;
   case DESC_BUFFER:
      size = 4;
      break;
   case DESC_PLANE_2:
      size = 4;
      offset += 64;
      break;
   }

   nir_ssa_def *index = NULL;
   while (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);
      unsigned array_size = MAX2(glsl_get_aoa_size(deref->type), 1);
      array_size *= binding->size;

      nir_ssa_def *tmp = nir_imul_imm(b, deref->arr.index.ssa, array_size);
      if (tmp != deref->arr.index.ssa)
         nir_instr_as_alu(tmp->parent_instr)->no_unsigned_wrap = true;

      if (index) {
         index = nir_iadd(b, tmp, index);
         nir_instr_as_alu(index->parent_instr)->no_unsigned_wrap = true;
      } else {
         index = tmp;
      }

      deref = nir_deref_instr_parent(deref);
   }

   nir_ssa_def *index_offset = index ? nir_iadd_imm(b, index, offset) : nir_imm_int(b, offset);
   if (index && index_offset != index)
      nir_instr_as_alu(index_offset->parent_instr)->no_unsigned_wrap = true;

   if (non_uniform)
      return nir_iadd(b, load_desc_ptr(state, desc_set), index_offset);

   nir_ssa_def *addr = convert_pointer_to_64_bit(state, load_desc_ptr(state, desc_set));
   nir_ssa_def *desc = nir_load_smem_amd(b, size, addr, index_offset, .align_mul = size * 4u);

   /* 3 plane formats always have same size and format for plane 1 & 2, so
    * use the tail from plane 1 so that we can store only the first 16 bytes
    * of the last plane. */
   if (desc_type == DESC_PLANE_2) {
      nir_ssa_def *desc2 = get_sampler_desc(state, deref, DESC_PLANE_1, non_uniform, tex, write);

      nir_ssa_def *comp[8];
      for (unsigned i = 0; i < 4; i++)
         comp[i] = nir_channel(b, desc, i);
      for (unsigned i = 4; i < 8; i++)
         comp[i] = nir_channel(b, desc2, i);

      return nir_vec(b, comp, 8);
   } else if (desc_type == DESC_IMAGE && state->has_image_load_dcc_bug && !tex && !write) {
      nir_ssa_def *comp[8];
      for (unsigned i = 0; i < 8; i++)
         comp[i] = nir_channel(b, desc, i);

      /* WRITE_COMPRESS_ENABLE must be 0 for all image loads to workaround a
       * hardware bug.
       */
      comp[6] = nir_iand_imm(b, comp[6], C_00A018_WRITE_COMPRESS_ENABLE);

      return nir_vec(b, comp, 8);
   } else if (desc_type == DESC_SAMPLER && tex->op == nir_texop_tg4) {
      nir_ssa_def *comp[4];
      for (unsigned i = 0; i < 4; i++)
         comp[i] = nir_channel(b, desc, i);

      /* We want to always use the linear filtering truncation behaviour for
       * nir_texop_tg4, even if the sampler uses nearest/point filtering.
       */
      comp[0] = nir_iand_imm(b, comp[0], C_008F30_TRUNC_COORD);

      return nir_vec(b, comp, 4);
   }

   return desc;
}

static void
update_image_intrinsic(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   const enum glsl_sampler_dim dim = glsl_get_sampler_dim(deref->type);
   bool is_load = intrin->intrinsic == nir_intrinsic_image_deref_load ||
                  intrin->intrinsic == nir_intrinsic_image_deref_sparse_load;

   nir_ssa_def *desc =
      get_sampler_desc(state, deref, dim == GLSL_SAMPLER_DIM_BUF ? DESC_BUFFER : DESC_IMAGE,
                       nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM, NULL, !is_load);
   nir_rewrite_image_intrinsic(intrin, desc, true);
}

static void
apply_layout_to_intrin(struct apply_layout_state *state, nir_intrinsic_instr *intrin)
{
   nir_ssa_def *rsrc;
   switch (intrin->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      visit_vulkan_resource_index(state, intrin);
      break;
   case nir_intrinsic_vulkan_resource_reindex:
      visit_vulkan_resource_reindex(state, intrin);
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      visit_load_vulkan_descriptor(state, intrin);
      break;
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic_add:
   case nir_intrinsic_ssbo_atomic_imin:
   case nir_intrinsic_ssbo_atomic_umin:
   case nir_intrinsic_ssbo_atomic_fmin:
   case nir_intrinsic_ssbo_atomic_imax:
   case nir_intrinsic_ssbo_atomic_umax:
   case nir_intrinsic_ssbo_atomic_fmax:
   case nir_intrinsic_ssbo_atomic_and:
   case nir_intrinsic_ssbo_atomic_or:
   case nir_intrinsic_ssbo_atomic_xor:
   case nir_intrinsic_ssbo_atomic_exchange:
   case nir_intrinsic_ssbo_atomic_comp_swap:
      rsrc = load_buffer_descriptor(state, intrin->src[0].ssa, nir_intrinsic_access(intrin));
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[0], rsrc);
      break;
   case nir_intrinsic_store_ssbo:
      rsrc = load_buffer_descriptor(state, intrin->src[1].ssa, nir_intrinsic_access(intrin));
      nir_instr_rewrite_src_ssa(&intrin->instr, &intrin->src[1], rsrc);
      break;
   case nir_intrinsic_get_ssbo_size:
      visit_get_ssbo_size(state, intrin);
      break;
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_fmin:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_fmax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
   case nir_intrinsic_image_deref_atomic_fadd:
   case nir_intrinsic_image_deref_atomic_inc_wrap:
   case nir_intrinsic_image_deref_atomic_dec_wrap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
      update_image_intrinsic(state, intrin);
      break;
   default:
      break;
   }
}

static void
apply_layout_to_tex(struct apply_layout_state *state, nir_tex_instr *tex)
{
   nir_deref_instr *texture_deref_instr = NULL;
   nir_deref_instr *sampler_deref_instr = NULL;
   int plane = -1;

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_texture_deref:
         texture_deref_instr = nir_src_as_deref(tex->src[i].src);
         break;
      case nir_tex_src_sampler_deref:
         sampler_deref_instr = nir_src_as_deref(tex->src[i].src);
         break;
      case nir_tex_src_plane:
         plane = nir_src_as_int(tex->src[i].src);
         break;
      default:
         break;
      }
   }

   nir_ssa_def *image = NULL;
   nir_ssa_def *sampler = NULL;
   if (plane >= 0) {
      assert(tex->op != nir_texop_txf_ms && tex->op != nir_texop_samples_identical);
      assert(tex->sampler_dim != GLSL_SAMPLER_DIM_BUF);
      image = get_sampler_desc(state, texture_deref_instr, DESC_PLANE_0 + plane,
                               tex->texture_non_uniform, tex, false);
   } else if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF) {
      image = get_sampler_desc(state, texture_deref_instr, DESC_BUFFER, tex->texture_non_uniform,
                               tex, false);
   } else if (tex->op == nir_texop_fragment_mask_fetch_amd ||
              tex->op == nir_texop_samples_identical) {
      image = get_sampler_desc(state, texture_deref_instr, DESC_FMASK, tex->texture_non_uniform,
                               tex, false);
   } else {
      image = get_sampler_desc(state, texture_deref_instr, DESC_IMAGE, tex->texture_non_uniform,
                               tex, false);
   }

   if (sampler_deref_instr) {
      sampler = get_sampler_desc(state, sampler_deref_instr, DESC_SAMPLER, tex->sampler_non_uniform,
                                 tex, false);

      if (tex->sampler_dim < GLSL_SAMPLER_DIM_RECT && state->chip_class < GFX8) {
         /* Disable anisotropic filtering if BASE_LEVEL == LAST_LEVEL.
          *
          * GFX6-GFX7:
          *   If BASE_LEVEL == LAST_LEVEL, the shader must disable anisotropic
          *   filtering manually. The driver sets img7 to a mask clearing
          *   MAX_ANISO_RATIO if BASE_LEVEL == LAST_LEVEL. The shader must do:
          *     s_and_b32 samp0, samp0, img7
          *
          * GFX8:
          *   The ANISO_OVERRIDE sampler field enables this fix in TA.
          */
         /* TODO: This is unnecessary for combined image+sampler.
          * We can do this when updating the desc set. */
         nir_builder *b = &state->builder;
         nir_ssa_def *comp[4];
         for (unsigned i = 0; i < 4; i++)
            comp[i] = nir_channel(b, sampler, i);
         comp[0] = nir_iand(b, comp[0], nir_channel(b, image, 7));

         sampler = nir_vec(b, comp, 4);
      }
   }

   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_texture_deref:
         tex->src[i].src_type = nir_tex_src_texture_handle;
         nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src, image);
         break;
      case nir_tex_src_sampler_deref:
         tex->src[i].src_type = nir_tex_src_sampler_handle;
         nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src, sampler);
         break;
      default:
         break;
      }
   }
}

void
radv_nir_apply_pipeline_layout(nir_shader *shader, struct radv_device *device,
                               const struct radv_shader_args *args)
{
   struct apply_layout_state state;
   state.chip_class = args->chip_class;
   state.address32_hi = device->physical_device->rad_info.address32_hi;
   state.has_image_load_dcc_bug = device->physical_device->rad_info.has_image_load_dcc_bug;
   state.args = args;
   state.info = args->shader_info;
   state.pipeline_layout = args->layout;

   nir_foreach_function (function, shader) {
      if (!function->impl)
         continue;

      nir_builder_init(&state.builder, function->impl);

      /* Iterate in reverse so load_ubo lowering can look at
       * the vulkan_resource_index to tell if it's an inline
       * ubo.
       */
      nir_foreach_block_reverse (block, function->impl) {
         nir_foreach_instr_reverse_safe (instr, block) {
            state.builder.cursor = nir_before_instr(instr);

            if (instr->type == nir_instr_type_tex)
               apply_layout_to_tex(&state, nir_instr_as_tex(instr));
            else if (instr->type == nir_instr_type_intrinsic)
               apply_layout_to_intrin(&state, nir_instr_as_intrinsic(instr));
         }
      }

      nir_metadata_preserve(function->impl, nir_metadata_block_index | nir_metadata_dominance);
   }
}
