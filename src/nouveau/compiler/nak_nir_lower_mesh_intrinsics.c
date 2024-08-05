/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "nak_private.h"
#include "nir_builder.h"
#include "nir_format_convert.h"

#include "util/u_math.h"

struct isbe_info {
   unsigned range_base;
   unsigned range;
   unsigned stride;
   unsigned component_mask;
   unsigned num_components;
};

static nir_def *
load_isbe(nir_builder *b,
          nir_def *offset,
          struct nak_nir_isbe_flags flags,
          const struct isbe_info *info,
          unsigned bit_size) {

   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   const uint32_t access = flags.output ? 0 : ACCESS_CAN_REORDER;

   unsigned mask = info->component_mask;
   unsigned read_components = ffs(mask + 1) - 1;

   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   while (mask) {
      const unsigned c = ffs(mask) - 1;
      nir_def *c_offset = nir_iadd_imm(b, offset, c * info->stride);

      nir_def *data =  nir_isberd_nv(b, bit_size, c_offset,
                                     .flags = flags_u32,
                                     .access = access);
      comps[c] = data;

      mask &= ~BITFIELD_RANGE(c, 1);
   }

   return nir_vec(b, comps, read_components);
}

static void
store_isbe(nir_builder *b,
           nir_def *offset,
           nir_def *data,
           struct nak_nir_isbe_flags flags,
           const struct isbe_info *info) {
   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   unsigned mask = info->component_mask;

   while (mask) {
      const unsigned c = ffs(mask) - 1;
      nir_def *c_offset = nir_iadd_imm(b, offset, c * info->stride);
      nir_def *c_data = nir_channel(b, data, c);

      nir_isbewr_nv(b, c_data, c_offset,
                    .range_base = info->range_base,
                    .range = info->range,
                    .flags = flags_u32);

      mask &= ~BITFIELD_RANGE(c, 1);
   }
}

static bool
lower_mesh_io_intrin(nir_builder *b,
                    nir_intrinsic_instr *intrin,
                    const struct lower_mesh_intrinsics_ctx *ctx)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *vtx = NULL, *offset = NULL, *data = NULL;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_load_per_primitive_output:
      vtx = intrin->src[0].ssa;
      offset = intrin->src[1].ssa;
      break;

   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_per_primitive_output:
      data = intrin->src[0].ssa;
      vtx = intrin->src[1].ssa;
      offset = intrin->src[2].ssa;
      break;

   default:
      unreachable("unknown intrinsic");
   }

   const bool is_per_primitive = intrin->intrinsic == nir_intrinsic_load_per_primitive_output ||
                                 intrin->intrinsic == nir_intrinsic_store_per_primitive_output;

   const bool is_store = data != NULL;
   nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

   const bool is_primitive_indices = sem.location == VARYING_SLOT_PRIMITIVE_INDICES;
   const bool is_cull_primitive = sem.location == VARYING_SLOT_CULL_PRIMITIVE;

   const struct nak_nir_isbe_flags flags = {
      .mode = is_primitive_indices ? NAK_ISBE_MODE_MAP : NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = !is_primitive_indices,
      .per_primitive = is_per_primitive,
   };

   unsigned component = nir_intrinsic_component(intrin);
   unsigned base_addr = nak_varying_mesh_skew_attr_addr(sem.location);
   base_addr += 4 * component;

   struct isbe_info info = {
      .range_base = base_addr,
      .range = 0,
      .component_mask = is_store ? nir_intrinsic_write_mask(intrin) :
                           nir_component_mask(intrin->num_components),
      .num_components = intrin->num_components,
   };

   if (nir_src_is_const(nir_src_for_ssa(offset))) {
      unsigned const_offset = nir_src_as_uint(nir_src_for_ssa(offset));
      /* Tighten the range */
      info.range_base += const_offset * 16;
      info.range = 4 * intrin->num_components;

      if (const_offset != 0)
         offset = nir_imm_int(b, 0);
   } else {
      /* Offsets from NIR are in vec4's */
      offset = nir_imul_imm(b, offset, 16);
      info.range = (sem.num_slots - 1) * 16 + intrin->num_components * 4;
   }

   nir_def *isbe_offset;

   if (is_primitive_indices) {
      const unsigned vertices_per_prim = mesa_vertices_per_prim(b->shader->info.mesh.primitive_type);

      /* Indices are 8 bits on hardware */
      isbe_offset = nir_iadd(b, offset, nir_iadd(b, nir_imul(b, vtx, nir_imm_int(b, vertices_per_prim)), nir_imm_int(b, 4)));
      info.stride = 1;

      if (is_store) {
         data = nir_u2u8(b, data);
      }
   } else if (is_cull_primitive) {
      /* ViewportMask under per primitive (GS) has a special layout */
      isbe_offset = nir_iadd(b, offset, nir_iadd_imm(b, nir_imul_imm(b, vtx, 4), nak_mesh_skew_total_size(ctx)));
      info.stride = 4;

      if (is_store) {
         data = nir_b2i32(b, nir_inot(b, data));
      }
   } else {
      uint16_t skew_attr_offset = nak_mesh_skew_offset(ctx, sem.location, info.range_base, is_per_primitive);
      nir_def *skew_start_offset;
      uint16_t skew_group_size;

      if (is_per_primitive) {
         skew_start_offset = nir_imm_int(b, nak_mesh_skew_vert_total_size(ctx));
         skew_group_size = nak_mesh_skew_prim_group_size(ctx);
      } else {
         skew_start_offset = nir_imm_int(b, 0);
         skew_group_size = nak_mesh_skew_vert_group_size(ctx);
      }

      /* Readjust offset to take into account SKEW groups */
      nir_def *offset_comp_index = nir_udiv_imm(b, offset, info.num_components * 4);
      nir_def *offset_comp_rest = nir_umod_imm(b, offset, info.num_components * 4);
      nir_def *offset_ajusted = nir_iadd(b, nir_imul_imm(b, offset_comp_index, info.num_components  * 4 * NAK_MESH_SKEW_GROUP_COUNT),
                                         offset_comp_rest);

      skew_start_offset = nir_iadd(b, skew_start_offset, nir_imul(b, nir_udiv(b, vtx, nir_imm_int(b, 32)), nir_imm_int(b, skew_group_size)));

      isbe_offset = nir_iadd(b, nir_iadd(b, nir_iadd(b, nir_imul(b, nir_imod_imm(b, vtx, 32),
                                                            nir_imm_int(b, 4)),
                                                skew_start_offset),
                                nir_imm_int(b, skew_attr_offset)),
                             offset_ajusted);

      info.stride = 4 * NAK_MESH_SKEW_GROUP_COUNT;
   }

   if (is_store) {
      /* Viewport is remapped to viewport mask on mesh */
      if (sem.location == VARYING_SLOT_VIEWPORT &&
          info.range_base == NAK_ATTR_VIEWPORT_MASK) {
         data = nir_ishl(b, nir_imm_int(b, 1), data);
      }

      store_isbe(b, isbe_offset, data, flags, &info);
   } else {
      uint8_t bit_size = is_primitive_indices ? 8 : intrin->def.bit_size;

      nir_def *dst = load_isbe(b, isbe_offset, flags,
                               &info,
                               bit_size);

      if (intrin->def.bit_size == 1)
         dst = nir_i2b(b, dst);

      /* Viewport and CullPrimitive are remapped to viewport mask on mesh */
      if (info.range_base == NAK_ATTR_VIEWPORT_MASK) {
         if (sem.location == VARYING_SLOT_VIEWPORT)
            data = nir_ufind_msb_rev(b, data);
         else if (is_cull_primitive)
            data = nir_inot(b, data);
      }

      /* Handle indices conversion */
      if (is_primitive_indices) {
         nir_def *comps[NIR_MAX_VEC_COMPONENTS];

         for (unsigned c = 0; c < intrin->num_components; c++) {
            nir_def *c_data = nir_channel(b, dst, c);
            comps[c] = nir_u2u32(b, c_data);
         }

         dst = nir_vec(b, comps, intrin->num_components);
      }

      nir_def_rewrite_uses(&intrin->def, dst);
   }

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_set_vertex_and_primitive_count(nir_builder *b,
                                     nir_intrinsic_instr *intrin)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *primitive_count = intrin->src[1].ssa;
   nir_def *offset = nir_imm_int(b, 0x3);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_MAP,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   nir_isbewr_nv(b, primitive_count, offset,
                 .flags = flags_u32);

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_load_workgroup_index(nir_builder *b,
                           nir_intrinsic_instr *intrin,
                           bool from_skew)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(b->shader);

   b->cursor = nir_before_impl(impl);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = false,
      // TODO: only use skew when a task shader is not present
      .skew = true,
      .per_primitive = false,
   };
   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   nir_def *dst =  nir_isberd_nv(b, 32, nir_imm_int(b, 0),
                                 .flags = flags_u32);

   nir_def_rewrite_uses(&intrin->def, dst);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_load_num_workgroups(nir_builder *b,
                          nir_intrinsic_instr *intrin)
{
   /* If we are here, we have a task shader */
   /* XXX: We should probably always lower here */
   b->cursor = nir_before_instr(&intrin->instr);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = false,
      .skew = false,
      .per_primitive = false,
   };

   struct isbe_info info = {
      .stride = 4,
      .component_mask = nir_component_mask(intrin->def.num_components),
      .num_components = intrin->num_components,
   };

   nir_def *dst = load_isbe(b, nir_imm_int(b, 0x8),
                            flags, &info,
                            32);

   nir_def_rewrite_uses(&intrin->def, dst);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_load_shared(nir_builder *b,
                  nir_intrinsic_instr *intrin,
                  unsigned base_offset)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *offset = intrin->src[0].ssa;

   const uint8_t bit_size = intrin->def.bit_size;

   assert(bit_size == 32);

   const unsigned base = nir_intrinsic_base(intrin);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   /* XXX: Move this to nvk_nir_lower_mesh and add it to the offset */
   offset = nir_iadd_imm(b, offset, base_offset);
   offset = nir_iadd_imm(b, offset, base);

   struct isbe_info info = {
      .stride = bit_size / 8,
      .component_mask = nir_component_mask(intrin->def.num_components),
      .num_components = intrin->num_components,
   };

   nir_def *dst = load_isbe(b, offset,
                            flags, &info,
                            bit_size);

   nir_def_rewrite_uses(&intrin->def, dst);
   nir_instr_remove(&intrin->instr);

   return true;
}



static bool
lower_store_shared(nir_builder *b,
                   nir_intrinsic_instr *intrin,
                   unsigned base_offset)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *value = intrin->src[0].ssa;
   nir_def *offset = intrin->src[1].ssa;

   const uint8_t bit_size = value->bit_size;

   assert(bit_size == 32);

   const unsigned base = nir_intrinsic_base(intrin);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   /* XXX: Move this to nvk_nir_lower_mesh and add it to the offset */
   offset = nir_iadd_imm(b, offset, base_offset);
   offset = nir_iadd_imm(b, offset, base);

   struct isbe_info info = {
      .stride = bit_size / 8,
      .component_mask = nir_intrinsic_write_mask(intrin),
      .num_components = intrin->num_components,
   };

   store_isbe(b, offset, value, flags, &info);

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_shared_atomic(nir_builder *b,
                   nir_intrinsic_instr *intrin,
                   unsigned base_offset)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *offset = intrin->src[0].ssa;
   nir_def *value = intrin->src[1].ssa;

   const uint8_t bit_size = value->bit_size * value->num_components;

   assert(bit_size == 8 || bit_size == 32);

   const unsigned base = nir_intrinsic_base(intrin);
   const nir_atomic_op atomic_op = nir_intrinsic_atomic_op(intrin);
   const nir_op alu_op = nir_atomic_op_to_alu(atomic_op);

   /* TODO: xchg, cmpxchg, fcmpxchg, inc_wrap and dec_wrap */
   assert(alu_op != nir_num_opcodes);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   /* XXX: Move this to nvk_nir_lower_mesh and add it to the offset */
   offset = nir_iadd_imm(b, offset, base_offset);
   offset = nir_iadd_imm(b, offset, base);

   nir_def *read_value = nir_isberd_nv(b, bit_size, offset,
                                       .flags = flags_u32);
   nir_def *new_value = nir_build_alu(b, alu_op, read_value, value, NULL, NULL);
   nir_isbewr_nv(b, new_value, offset,
                  .flags = flags_u32);

   nir_def_rewrite_uses(&intrin->def, read_value);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_load_task_payload(nir_builder *b,
                  nir_intrinsic_instr *intrin,
                  bool from_task_shader)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *offset = intrin->src[0].ssa;

   const uint8_t bit_size = intrin->def.bit_size;

   assert(bit_size == 32);

   const unsigned base = nir_intrinsic_base(intrin);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = from_task_shader,
      .skew = false,
      .per_primitive = false,
   };

   offset = nir_iadd_imm(b, offset, base);

   struct isbe_info info = {
      .stride = bit_size / 8,
      .component_mask = nir_component_mask(intrin->def.num_components),
      .num_components = intrin->num_components,
   };

   nir_def *dst = load_isbe(b, offset,
                            flags, &info,
                            bit_size);

   nir_def_rewrite_uses(&intrin->def, dst);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_store_task_payload(nir_builder *b,
                   nir_intrinsic_instr *intrin)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *value = intrin->src[0].ssa;
   nir_def *offset = intrin->src[1].ssa;

   const uint8_t bit_size = value->bit_size;

   assert(bit_size == 32);

   const unsigned base = nir_intrinsic_base(intrin);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   offset = nir_iadd_imm(b, offset, base);

   struct isbe_info info = {
      .stride = bit_size / 8,
      .component_mask = nir_intrinsic_write_mask(intrin),
      .num_components = intrin->num_components,
   };

   store_isbe(b, offset, value, flags, &info);

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_mesh_intrin(nir_builder *b,
                  nir_intrinsic_instr *intrin,
                  void *cb_data)
{
   const struct lower_mesh_intrinsics_ctx *ctx = cb_data;

   // TODO: lower task payload accesses
   // TODO: lower launch_mesh_workgroups (Task only)

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_load_per_primitive_output:
   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_per_primitive_output:
      return lower_mesh_io_intrin(b, intrin, ctx);
   case nir_intrinsic_set_vertex_and_primitive_count:
      return lower_set_vertex_and_primitive_count(b, intrin);
   case nir_intrinsic_load_workgroup_index:
      return lower_load_workgroup_index(b, intrin, !ctx->has_task_shader);
   case nir_intrinsic_load_num_workgroups:
      return lower_load_num_workgroups(b, intrin);
   case nir_intrinsic_load_shared:
      return lower_load_shared(b, intrin, 0x20);
   case nir_intrinsic_store_shared:
      return lower_store_shared(b, intrin, 0x20);
   case nir_intrinsic_shared_atomic:
      return lower_shared_atomic(b, intrin, 0x20);
   case nir_intrinsic_load_task_payload:
      return lower_load_task_payload(b, intrin, false);
   default:
      return false;
   }
}

bool
nak_nir_lower_mesh_intrinsics(nir_shader *nir, struct lower_mesh_intrinsics_ctx *ctx)
{
   return nir_shader_intrinsics_pass(nir, lower_mesh_intrin,
                                     nir_metadata_block_index |
                                     nir_metadata_dominance,
                                     ctx);
}

static bool
lower_launch_mesh_workgroups(nir_builder *b,
                             nir_intrinsic_instr *intrin)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *dimensions = intrin->src[0].ssa;
   nir_def *x = nir_channel(b, dimensions, 0);
   nir_def *y = nir_channel(b, dimensions, 1);
   nir_def *z = nir_channel(b, dimensions, 2);
   nir_def *task_count = nir_imul(b, nir_imul(b, x, y), z);

   const struct nak_nir_isbe_flags flags = {
      .mode = NAK_ISBE_MODE_ATTR,
      .output = true,
      .skew = false,
      .per_primitive = false,
   };

   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   nir_isbewr_nv(b, task_count, nir_imm_int(b, 0x4),
                 .flags = flags_u32);
   nir_isbewr_nv(b, x, nir_imm_int(b, 0x8),
                 .flags = flags_u32);
   nir_isbewr_nv(b, y, nir_imm_int(b, 0xC),
                 .flags = flags_u32);
   nir_isbewr_nv(b, z, nir_imm_int(b, 0x10),
                 .flags = flags_u32);

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_task_intrin(nir_builder *b,
                  nir_intrinsic_instr *intrin,
                  void *cb_data)
{
   /* TODO: nir_intrinsic_task_payload_atomic */
   /* TODO: nir_intrinsic_task_payload_atomic_swap */

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_shared:
      return lower_load_shared(b, intrin, 0);
   case nir_intrinsic_store_shared:
      return lower_store_shared(b, intrin, 0);
   case nir_intrinsic_shared_atomic:
      return lower_shared_atomic(b, intrin, 0);
   case nir_intrinsic_load_task_payload:
      return lower_load_task_payload(b, intrin, true);
   case nir_intrinsic_store_task_payload:
      return lower_store_task_payload(b, intrin);
   case nir_intrinsic_load_workgroup_index:
      return lower_load_workgroup_index(b, intrin, true);
   case nir_intrinsic_launch_mesh_workgroups:
      return lower_launch_mesh_workgroups(b, intrin);
   default:
      return false;
   }
}

bool
nak_nir_lower_task_intrinsics(nir_shader *nir)
{
   return nir_shader_intrinsics_pass(nir, lower_task_intrin,
                                     nir_metadata_block_index |
                                     nir_metadata_dominance,
                                     NULL);
}
