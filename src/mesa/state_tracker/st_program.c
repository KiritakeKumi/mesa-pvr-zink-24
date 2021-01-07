/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  *   Brian Paul
  */


#include "main/errors.h"

#include "main/hash.h"
#include "main/mtypes.h"
#include "program/prog_parameter.h"
#include "program/prog_print.h"
#include "program/prog_to_nir.h"
#include "program/programopt.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "draw/draw_context.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_shader_tokens.h"
#include "draw/draw_context.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_emulate.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_ureg.h"
#include "nir/nir_to_tgsi.h"

#include "util/u_memory.h"

#include "st_debug.h"
#include "st_cb_bitmap.h"
#include "st_cb_drawpixels.h"
#include "st_context.h"
#include "st_program.h"
#include "st_atifs_to_nir.h"
#include "st_nir.h"
#include "st_shader_cache.h"
#include "st_util.h"
#include "cso_cache/cso_context.h"


static void
destroy_program_variants(struct st_context *st, struct gl_program *target);

static void
set_affected_state_flags(uint64_t *states,
                         struct gl_program *prog,
                         uint64_t new_constants,
                         uint64_t new_sampler_views,
                         uint64_t new_samplers,
                         uint64_t new_images,
                         uint64_t new_ubos,
                         uint64_t new_ssbos,
                         uint64_t new_atomics)
{
   if (prog->Parameters->NumParameters)
      *states |= new_constants;

   if (prog->info.num_textures)
      *states |= new_sampler_views | new_samplers;

   if (prog->info.num_images)
      *states |= new_images;

   if (prog->info.num_ubos)
      *states |= new_ubos;

   if (prog->info.num_ssbos)
      *states |= new_ssbos;

   if (prog->info.num_abos)
      *states |= new_atomics;
}

/**
 * This determines which states will be updated when the shader is bound.
 */
void
st_set_prog_affected_state_flags(struct gl_program *prog)
{
   uint64_t *states;

   switch (prog->info.stage) {
   case MESA_SHADER_VERTEX:
      states = &((struct st_program*)prog)->affected_states;

      *states = ST_NEW_VS_STATE |
                ST_NEW_RASTERIZER |
                ST_NEW_VERTEX_ARRAYS;

      set_affected_state_flags(states, prog,
                               ST_NEW_VS_CONSTANTS,
                               ST_NEW_VS_SAMPLER_VIEWS,
                               ST_NEW_VS_SAMPLERS,
                               ST_NEW_VS_IMAGES,
                               ST_NEW_VS_UBOS,
                               ST_NEW_VS_SSBOS,
                               ST_NEW_VS_ATOMICS);
      break;

   case MESA_SHADER_TESS_CTRL:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_TCS_STATE;

      set_affected_state_flags(states, prog,
                               ST_NEW_TCS_CONSTANTS,
                               ST_NEW_TCS_SAMPLER_VIEWS,
                               ST_NEW_TCS_SAMPLERS,
                               ST_NEW_TCS_IMAGES,
                               ST_NEW_TCS_UBOS,
                               ST_NEW_TCS_SSBOS,
                               ST_NEW_TCS_ATOMICS);
      break;

   case MESA_SHADER_TESS_EVAL:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_TES_STATE |
                ST_NEW_RASTERIZER;

      set_affected_state_flags(states, prog,
                               ST_NEW_TES_CONSTANTS,
                               ST_NEW_TES_SAMPLER_VIEWS,
                               ST_NEW_TES_SAMPLERS,
                               ST_NEW_TES_IMAGES,
                               ST_NEW_TES_UBOS,
                               ST_NEW_TES_SSBOS,
                               ST_NEW_TES_ATOMICS);
      break;

   case MESA_SHADER_GEOMETRY:
      states = &(st_program(prog))->affected_states;

      *states = ST_NEW_GS_STATE |
                ST_NEW_RASTERIZER;

      set_affected_state_flags(states, prog,
                               ST_NEW_GS_CONSTANTS,
                               ST_NEW_GS_SAMPLER_VIEWS,
                               ST_NEW_GS_SAMPLERS,
                               ST_NEW_GS_IMAGES,
                               ST_NEW_GS_UBOS,
                               ST_NEW_GS_SSBOS,
                               ST_NEW_GS_ATOMICS);
      break;

   case MESA_SHADER_FRAGMENT:
      states = &((struct st_program*)prog)->affected_states;

      /* gl_FragCoord and glDrawPixels always use constants. */
      *states = ST_NEW_FS_STATE |
                ST_NEW_SAMPLE_SHADING |
                ST_NEW_FS_CONSTANTS;

      set_affected_state_flags(states, prog,
                               ST_NEW_FS_CONSTANTS,
                               ST_NEW_FS_SAMPLER_VIEWS,
                               ST_NEW_FS_SAMPLERS,
                               ST_NEW_FS_IMAGES,
                               ST_NEW_FS_UBOS,
                               ST_NEW_FS_SSBOS,
                               ST_NEW_FS_ATOMICS);
      break;

   case MESA_SHADER_COMPUTE:
      states = &((struct st_program*)prog)->affected_states;

      *states = ST_NEW_CS_STATE;

      set_affected_state_flags(states, prog,
                               ST_NEW_CS_CONSTANTS,
                               ST_NEW_CS_SAMPLER_VIEWS,
                               ST_NEW_CS_SAMPLERS,
                               ST_NEW_CS_IMAGES,
                               ST_NEW_CS_UBOS,
                               ST_NEW_CS_SSBOS,
                               ST_NEW_CS_ATOMICS);
      break;

   default:
      unreachable("unhandled shader stage");
   }
}


/**
 * Delete a shader variant.  Note the caller must unlink the variant from
 * the linked list.
 */
static void
delete_variant(struct st_context *st, struct st_variant *v, GLenum target)
{
   if (v->driver_shader) {
      if (target == GL_VERTEX_PROGRAM_ARB &&
          ((struct st_common_variant*)v)->key.is_draw_shader) {
         /* Draw shader. */
         draw_delete_vertex_shader(st->draw, v->driver_shader);
      } else if (st->has_shareable_shaders || v->st == st) {
         /* The shader's context matches the calling context, or we
          * don't care.
          */
         switch (target) {
         case GL_VERTEX_PROGRAM_ARB:
            st->pipe->delete_vs_state(st->pipe, v->driver_shader);
            break;
         case GL_TESS_CONTROL_PROGRAM_NV:
            st->pipe->delete_tcs_state(st->pipe, v->driver_shader);
            break;
         case GL_TESS_EVALUATION_PROGRAM_NV:
            st->pipe->delete_tes_state(st->pipe, v->driver_shader);
            break;
         case GL_GEOMETRY_PROGRAM_NV:
            st->pipe->delete_gs_state(st->pipe, v->driver_shader);
            break;
         case GL_FRAGMENT_PROGRAM_ARB:
            st->pipe->delete_fs_state(st->pipe, v->driver_shader);
            break;
         case GL_COMPUTE_PROGRAM_NV:
            st->pipe->delete_compute_state(st->pipe, v->driver_shader);
            break;
         default:
            unreachable("bad shader type in delete_basic_variant");
         }
      } else {
         /* We can't delete a shader with a context different from the one
          * that created it.  Add it to the creating context's zombie list.
          */
         enum pipe_shader_type type =
            pipe_shader_type_from_mesa(_mesa_program_enum_to_shader_stage(target));

         st_save_zombie_shader(v->st, type, v->driver_shader);
      }
   }

   free(v);
}

static void
st_unbind_program(struct st_context *st, struct st_program *p)
{
   /* Unbind the shader in cso_context and re-bind in st/mesa. */
   switch (p->Base.info.stage) {
   case MESA_SHADER_VERTEX:
      cso_set_vertex_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_VS_STATE;
      break;
   case MESA_SHADER_TESS_CTRL:
      cso_set_tessctrl_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_TCS_STATE;
      break;
   case MESA_SHADER_TESS_EVAL:
      cso_set_tesseval_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_TES_STATE;
      break;
   case MESA_SHADER_GEOMETRY:
      cso_set_geometry_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_GS_STATE;
      break;
   case MESA_SHADER_FRAGMENT:
      cso_set_fragment_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_FS_STATE;
      break;
   case MESA_SHADER_COMPUTE:
      cso_set_compute_shader_handle(st->cso_context, NULL);
      st->dirty |= ST_NEW_CS_STATE;
      break;
   default:
      unreachable("invalid shader type");
   }
}

/**
 * Free all basic program variants.
 */
void
st_release_variants(struct st_context *st, struct st_program *p)
{
   struct st_variant *v;

   /* If we are releasing shaders, re-bind them, because we don't
    * know which shaders are bound in the driver.
    */
   if (p->variants)
      st_unbind_program(st, p);

   for (v = p->variants; v; ) {
      struct st_variant *next = v->next;
      delete_variant(st, v, p->Base.Target);
      v = next;
   }

   p->variants = NULL;

   if (p->state.tokens) {
      ureg_free_tokens(p->state.tokens);
      p->state.tokens = NULL;
   }

   /* Note: Any setup of ->ir.nir that has had pipe->create_*_state called on
    * it has resulted in the driver taking ownership of the NIR.  Those
    * callers should be NULLing out the nir field in any pipe_shader_state
    * that might have this called in order to indicate that.
    *
    * GLSL IR and ARB programs will have set gl_program->nir to the same
    * shader as ir->ir.nir, so it will be freed by _mesa_delete_program().
    */
}

/**
 * Free all basic program variants and unref program.
 */
void
st_release_program(struct st_context *st, struct st_program **p)
{
   if (!*p)
      return;

   destroy_program_variants(st, &((*p)->Base));
   st_reference_prog(st, p, NULL);
}

void
st_finalize_nir_before_variants(struct nir_shader *nir)
{
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);
   if (nir->options->lower_all_io_to_temps ||
       nir->options->lower_all_io_to_elements ||
       nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_GEOMETRY) {
      NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, false);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, true);
   }

   /* st_nir_assign_vs_in_locations requires correct shader info. */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   st_nir_assign_vs_in_locations(nir);
}

static void
st_prog_to_nir_postprocess(struct st_context *st, nir_shader *nir,
                           struct gl_program *prog)
{
   struct pipe_screen *screen = st->screen;

   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   nir_validate_shader(nir, "after st/ptn lower_regs_to_ssa");

   NIR_PASS_V(nir, st_nir_lower_wpos_ytransform, prog, screen);
   NIR_PASS_V(nir, nir_lower_system_values);
   NIR_PASS_V(nir, nir_lower_compute_system_values, NULL);

   /* Optimise NIR */
   NIR_PASS_V(nir, nir_opt_constant_folding);
   st_nir_opts(nir);
   st_finalize_nir_before_variants(nir);

   if (st->allow_st_finalize_nir_twice)
      st_finalize_nir(st, prog, NULL, nir, true);

   nir_validate_shader(nir, "after st/glsl finalize_nir");
}

/**
 * Translate ARB (asm) program to NIR
 */
static nir_shader *
st_translate_prog_to_nir(struct st_context *st, struct gl_program *prog,
                         gl_shader_stage stage)
{
   const struct nir_shader_compiler_options *options =
      st_get_nir_compiler_options(st, prog->info.stage);

   /* Translate to NIR */
   nir_shader *nir = prog_to_nir(prog, options);

   st_prog_to_nir_postprocess(st, nir, prog);

   return nir;
}

void
st_prepare_vertex_program(struct st_program *stp)
{
   struct st_vertex_program *stvp = (struct st_vertex_program *)stp;

   stvp->num_inputs = 0;
   memset(stvp->input_to_index, ~0, sizeof(stvp->input_to_index));
   memset(stvp->result_to_output, ~0, sizeof(stvp->result_to_output));

   /* Determine number of inputs, the mappings between VERT_ATTRIB_x
    * and TGSI generic input indexes, plus input attrib semantic info.
    */
   for (unsigned attr = 0; attr < VERT_ATTRIB_MAX; attr++) {
      if ((stp->Base.info.inputs_read & BITFIELD64_BIT(attr)) != 0) {
         stvp->input_to_index[attr] = stvp->num_inputs;
         stvp->index_to_input[stvp->num_inputs] = attr;
         stvp->num_inputs++;

         if ((stp->Base.DualSlotInputs & BITFIELD64_BIT(attr)) != 0) {
            /* add placeholder for second part of a double attribute */
            stvp->index_to_input[stvp->num_inputs] = ST_DOUBLE_ATTRIB_PLACEHOLDER;
            stvp->num_inputs++;
         }
      }
   }
   /* pre-setup potentially unused edgeflag input */
   stvp->input_to_index[VERT_ATTRIB_EDGEFLAG] = stvp->num_inputs;
   stvp->index_to_input[stvp->num_inputs] = VERT_ATTRIB_EDGEFLAG;

   /* Compute mapping of vertex program outputs to slots. */
   unsigned num_outputs = 0;
   for (unsigned attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (stp->Base.info.outputs_written & BITFIELD64_BIT(attr))
         stvp->result_to_output[attr] = num_outputs++;
   }
   /* pre-setup potentially unused edgeflag output */
   stvp->result_to_output[VARYING_SLOT_EDGE] = num_outputs;
}

void
st_translate_stream_output_info(struct gl_program *prog)
{
   struct gl_transform_feedback_info *info = prog->sh.LinkedTransformFeedback;
   if (!info)
      return;

   /* Determine the (default) output register mapping for each output. */
   unsigned num_outputs = 0;
   ubyte output_mapping[VARYING_SLOT_TESS_MAX];
   memset(output_mapping, 0, sizeof(output_mapping));

   for (unsigned attr = 0; attr < VARYING_SLOT_MAX; attr++) {
      if (prog->info.outputs_written & BITFIELD64_BIT(attr))
         output_mapping[attr] = num_outputs++;
   }

   /* Translate stream output info. */
   struct pipe_stream_output_info *so_info =
      &((struct st_program*)prog)->state.stream_output;

   for (unsigned i = 0; i < info->NumOutputs; i++) {
      so_info->output[i].register_index =
         output_mapping[info->Outputs[i].OutputRegister];
      so_info->output[i].start_component = info->Outputs[i].ComponentOffset;
      so_info->output[i].num_components = info->Outputs[i].NumComponents;
      so_info->output[i].output_buffer = info->Outputs[i].OutputBuffer;
      so_info->output[i].dst_offset = info->Outputs[i].DstOffset;
      so_info->output[i].stream = info->Outputs[i].StreamId;
   }

   for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
      so_info->stride[i] = info->Buffers[i].Stride;
   }
   so_info->num_outputs = info->NumOutputs;
}

/**
 * Translate a vertex program.
 */
bool
st_translate_vertex_program(struct st_context *st,
                            struct st_program *stp)
{
   if (stp->Base.arb.IsPositionInvariant)
      _mesa_insert_mvp_code(st->ctx, &stp->Base);

   /* ARB_vp: */
   _mesa_remove_output_reads(&stp->Base, PROGRAM_OUTPUT);

   /* This determines which states will be updated when the assembly
    * shader is bound.
    */
   stp->affected_states = ST_NEW_VS_STATE |
                           ST_NEW_RASTERIZER |
                           ST_NEW_VERTEX_ARRAYS;

   if (stp->Base.Parameters->NumParameters)
      stp->affected_states |= ST_NEW_VS_CONSTANTS;

   if (stp->Base.nir)
      ralloc_free(stp->Base.nir);

   if (stp->serialized_nir) {
      free(stp->serialized_nir);
      stp->serialized_nir = NULL;
   }

   stp->state.type = PIPE_SHADER_IR_NIR;
   stp->Base.nir = st_translate_prog_to_nir(st, &stp->Base,
                                             MESA_SHADER_VERTEX);
   stp->Base.info = stp->Base.nir->info;

   st_prepare_vertex_program(stp);
   return true;
}

static struct nir_shader *
get_nir_shader(struct st_context *st, struct st_program *stp)
{
   if (stp->Base.nir) {
      nir_shader *nir = stp->Base.nir;

      /* The first shader variant takes ownership of NIR, so that there is
       * no cloning. Additional shader variants are always generated from
       * serialized NIR to save memory.
       */
      stp->Base.nir = NULL;
      assert(stp->serialized_nir && stp->serialized_nir_size);
      return nir;
   }

   struct blob_reader blob_reader;
   const struct nir_shader_compiler_options *options =
      st_get_nir_compiler_options(st, stp->Base.info.stage);

   blob_reader_init(&blob_reader, stp->serialized_nir, stp->serialized_nir_size);
   return nir_deserialize(NULL, options, &blob_reader);
}

static void
lower_ucp(struct st_context *st,
          struct nir_shader *nir,
          unsigned ucp_enables,
          struct gl_program_parameter_list *params)
{
   if (nir->info.outputs_written & VARYING_BIT_CLIP_DIST0)
      NIR_PASS_V(nir, nir_lower_clip_disable, ucp_enables);
   else {
      struct pipe_screen *screen = st->screen;
      bool can_compact = screen->get_param(screen,
                                           PIPE_CAP_NIR_COMPACT_ARRAYS);
      bool use_eye = st->ctx->_Shader->CurrentProgram[MESA_SHADER_VERTEX] != NULL;

      gl_state_index16 clipplane_state[MAX_CLIP_PLANES][STATE_LENGTH] = {{0}};
      for (int i = 0; i < MAX_CLIP_PLANES; ++i) {
         if (use_eye) {
            clipplane_state[i][0] = STATE_CLIPPLANE;
            clipplane_state[i][1] = i;
         } else {
            clipplane_state[i][0] = STATE_INTERNAL;
            clipplane_state[i][1] = STATE_CLIP_INTERNAL;
            clipplane_state[i][2] = i;
         }
         _mesa_add_state_reference(params, clipplane_state[i]);
      }

      if (nir->info.stage == MESA_SHADER_VERTEX) {
         NIR_PASS_V(nir, nir_lower_clip_vs, ucp_enables,
                    true, can_compact, clipplane_state);
      } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
         NIR_PASS_V(nir, nir_lower_clip_gs, ucp_enables,
                    can_compact, clipplane_state);
      }

      NIR_PASS_V(nir, nir_lower_io_to_temporaries,
                 nir_shader_get_entrypoint(nir), true, false);
      NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   }
}

static struct st_common_variant *
st_create_vp_variant(struct st_context *st,
                     struct st_program *stvp,
                     const struct st_common_variant_key *key)
{
   struct st_common_variant *vpv = CALLOC_STRUCT(st_common_variant);
   struct pipe_context *pipe = st->pipe;
   struct pipe_shader_state state = {0};

   static const gl_state_index16 point_size_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_POINT_SIZE_CLAMPED, 0 };
   struct gl_program_parameter_list *params = stvp->Base.Parameters;

   vpv->key = *key;

   state.stream_output = stvp->state.stream_output;

   bool finalize = false;

   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = get_nir_shader(st, stvp);
   if (key->clamp_color) {
      NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
      finalize = true;
   }
   if (key->passthrough_edgeflags) {
      NIR_PASS_V(state.ir.nir, nir_lower_passthrough_edgeflags);
      finalize = true;
   }

   if (key->lower_point_size) {
      _mesa_add_state_reference(params, point_size_state);
      NIR_PASS_V(state.ir.nir, nir_lower_point_size_mov,
                  point_size_state);
      stvp->affected_states |= ST_NEW_VS_CONSTANTS;
      finalize = true;
   }

   if (key->lower_ucp) {
      lower_ucp(st, state.ir.nir, key->lower_ucp, params);
      finalize = true;
   }

   if (finalize || !st->allow_st_finalize_nir_twice) {
      st_finalize_nir(st, &stvp->Base, stvp->shader_program, state.ir.nir,
                        true);

      /* Some of the lowering above may have introduced new varyings */
      nir_shader_gather_info(state.ir.nir,
                              nir_shader_get_entrypoint(state.ir.nir));
   }

   if (ST_DEBUG & DEBUG_PRINT_IR)
      nir_print_shader(state.ir.nir, stderr);

   /* If the driver wants TGSI, then translate before handing off. */
   if (st->pipe->screen->get_shader_param(st->pipe->screen,
                                          PIPE_SHADER_VERTEX,
                                          PIPE_SHADER_CAP_PREFERRED_IR) !=
         PIPE_SHADER_IR_NIR) {
      nir_shader *s = state.ir.nir;
      state.tokens = nir_to_tgsi(s, st->pipe->screen);
      state.type = PIPE_SHADER_IR_TGSI;
      ralloc_free(s);
   }

   if (key->is_draw_shader)
      vpv->base.driver_shader = draw_create_vertex_shader(st->draw, &state);
   else
      vpv->base.driver_shader = pipe->create_vs_state(pipe, &state);

   return vpv;
}


/**
 * Find/create a vertex program variant.
 */
struct st_common_variant *
st_get_vp_variant(struct st_context *st,
                  struct st_program *stp,
                  const struct st_common_variant_key *key)
{
   struct st_vertex_program *stvp = (struct st_vertex_program *)stp;
   struct st_common_variant *vpv;

   /* Search for existing variant */
   for (vpv = st_common_variant(stp->variants); vpv;
        vpv = st_common_variant(vpv->base.next)) {
      if (memcmp(&vpv->key, key, sizeof(*key)) == 0) {
         break;
      }
   }

   if (!vpv) {
      /* create now */
      vpv = st_create_vp_variant(st, stp, key);
      if (vpv) {
         vpv->base.st = key->st;

         unsigned num_inputs = stvp->num_inputs + key->passthrough_edgeflags;
         for (unsigned index = 0; index < num_inputs; ++index) {
            unsigned attr = stvp->index_to_input[index];
            if (attr == ST_DOUBLE_ATTRIB_PLACEHOLDER)
               continue;
            vpv->vert_attrib_mask |= 1u << attr;
         }

         /* insert into list */
         vpv->base.next = stp->variants;
         stp->variants = &vpv->base;
      }
   }

   return vpv;
}


/**
 * Translate a non-GLSL Mesa fragment shader into a NIR shader.
 */
bool
st_translate_fragment_program(struct st_context *st,
                              struct st_program *stfp)
{
   _mesa_remove_output_reads(&stfp->Base, PROGRAM_OUTPUT);
   if (st->ctx->Const.GLSLFragCoordIsSysVal)
      _mesa_program_fragment_position_to_sysval(&stfp->Base);

   /* This determines which states will be updated when the assembly
      * shader is bound.
      *
      * fragment.position and glDrawPixels always use constants.
      */
   stfp->affected_states = ST_NEW_FS_STATE |
                           ST_NEW_SAMPLE_SHADING |
                           ST_NEW_FS_CONSTANTS;

   if (stfp->ati_fs) {
      /* Just set them for ATI_fs unconditionally. */
      stfp->affected_states |= ST_NEW_FS_SAMPLER_VIEWS |
                                 ST_NEW_FS_SAMPLERS;
   } else {
      /* ARB_fp */
      if (stfp->Base.SamplersUsed)
         stfp->affected_states |= ST_NEW_FS_SAMPLER_VIEWS |
                                    ST_NEW_FS_SAMPLERS;
   }

   /* Translate to NIR. */
   if (!stfp->ati_fs) {
      nir_shader *nir =
         st_translate_prog_to_nir(st, &stfp->Base, MESA_SHADER_FRAGMENT);

      if (stfp->Base.nir)
         ralloc_free(stfp->Base.nir);
      if (stfp->serialized_nir) {
         free(stfp->serialized_nir);
         stfp->serialized_nir = NULL;
      }
      stfp->state.type = PIPE_SHADER_IR_NIR;
      stfp->Base.nir = nir;
   }

   return true;
}

static struct st_fp_variant *
st_create_fp_variant(struct st_context *st,
                     struct st_program *stfp,
                     const struct st_fp_variant_key *key)
{
   struct pipe_context *pipe = st->pipe;
   struct st_fp_variant *variant = CALLOC_STRUCT(st_fp_variant);
   struct pipe_shader_state state = {0};
   struct gl_program_parameter_list *params = stfp->Base.Parameters;
   static const gl_state_index16 texcoord_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_CURRENT_ATTRIB, VERT_ATTRIB_TEX0 };
   static const gl_state_index16 scale_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_PT_SCALE };
   static const gl_state_index16 bias_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_PT_BIAS };
   static const gl_state_index16 alpha_ref_state[STATE_LENGTH] =
      { STATE_INTERNAL, STATE_ALPHA_REF };

   if (!variant)
      return NULL;

   /* Translate ATI_fs to NIR at variant time because that's when we have the
    * texture types.
    */
   if (stfp->ati_fs) {
      const struct nir_shader_compiler_options *options =
         st_get_nir_compiler_options(st, MESA_SHADER_FRAGMENT);

      nir_shader *s = st_translate_atifs_program(stfp->ati_fs, key, &stfp->Base, options);

      st_prog_to_nir_postprocess(st, s, &stfp->Base);

      state.type = PIPE_SHADER_IR_NIR;
      state.ir.nir = s;
   } else if (stfp->state.type == PIPE_SHADER_IR_NIR) {
      state.type = PIPE_SHADER_IR_NIR;
      state.ir.nir = get_nir_shader(st, stfp);
   }

   bool finalize = false;

   if (key->clamp_color) {
      NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
      finalize = true;
   }

   if (key->lower_flatshade) {
      NIR_PASS_V(state.ir.nir, nir_lower_flatshade);
      finalize = true;
   }

   if (key->lower_alpha_func != COMPARE_FUNC_ALWAYS) {
      _mesa_add_state_reference(params, alpha_ref_state);
      NIR_PASS_V(state.ir.nir, nir_lower_alpha_test, key->lower_alpha_func,
                  false, alpha_ref_state);
      finalize = true;
   }

   if (key->lower_two_sided_color) {
      bool face_sysval = st->ctx->Const.GLSLFrontFacingIsSysVal;
      NIR_PASS_V(state.ir.nir, nir_lower_two_sided_color, face_sysval);
      finalize = true;
   }

   if (key->persample_shading) {
         nir_shader *shader = state.ir.nir;
         nir_foreach_shader_in_variable(var, shader)
            var->data.sample = true;
         finalize = true;
   }

   assert(!(key->bitmap && key->drawpixels));

   /* glBitmap */
   if (key->bitmap) {
      nir_lower_bitmap_options options = {0};

      variant->bitmap_sampler = ffs(~stfp->Base.SamplersUsed) - 1;
      options.sampler = variant->bitmap_sampler;
      options.swizzle_xxxx = st->bitmap.tex_format == PIPE_FORMAT_R8_UNORM;

      NIR_PASS_V(state.ir.nir, nir_lower_bitmap, &options);
      finalize = true;
   }

   /* glDrawPixels (color only) */
   if (key->drawpixels) {
      nir_lower_drawpixels_options options = {{0}};
      unsigned samplers_used = stfp->Base.SamplersUsed;

      /* Find the first unused slot. */
      variant->drawpix_sampler = ffs(~samplers_used) - 1;
      options.drawpix_sampler = variant->drawpix_sampler;
      samplers_used |= (1 << variant->drawpix_sampler);

      options.pixel_maps = key->pixelMaps;
      if (key->pixelMaps) {
         variant->pixelmap_sampler = ffs(~samplers_used) - 1;
         options.pixelmap_sampler = variant->pixelmap_sampler;
      }

      options.scale_and_bias = key->scaleAndBias;
      if (key->scaleAndBias) {
         _mesa_add_state_reference(params, scale_state);
         memcpy(options.scale_state_tokens, scale_state,
                  sizeof(options.scale_state_tokens));
         _mesa_add_state_reference(params, bias_state);
         memcpy(options.bias_state_tokens, bias_state,
                  sizeof(options.bias_state_tokens));
      }

      _mesa_add_state_reference(params, texcoord_state);
      memcpy(options.texcoord_state_tokens, texcoord_state,
               sizeof(options.texcoord_state_tokens));

      NIR_PASS_V(state.ir.nir, nir_lower_drawpixels, &options);
      finalize = true;
   }

   if (unlikely(key->external.lower_nv12 || key->external.lower_iyuv ||
                  key->external.lower_xy_uxvx || key->external.lower_yx_xuxv ||
                  key->external.lower_ayuv || key->external.lower_xyuv ||
                  key->external.lower_yuv)) {

      st_nir_lower_samplers(st->screen, state.ir.nir,
                              stfp->shader_program, &stfp->Base);

      nir_lower_tex_options options = {0};
      options.lower_y_uv_external = key->external.lower_nv12;
      options.lower_y_u_v_external = key->external.lower_iyuv;
      options.lower_xy_uxvx_external = key->external.lower_xy_uxvx;
      options.lower_yx_xuxv_external = key->external.lower_yx_xuxv;
      options.lower_ayuv_external = key->external.lower_ayuv;
      options.lower_xyuv_external = key->external.lower_xyuv;
      options.lower_yuv_external = key->external.lower_yuv;
      NIR_PASS_V(state.ir.nir, nir_lower_tex, &options);
      finalize = true;
   }

   if (finalize || !st->allow_st_finalize_nir_twice) {
      st_finalize_nir(st, &stfp->Base, stfp->shader_program, state.ir.nir,
                        false);
   }

   /* This pass needs to happen *after* nir_lower_sampler */
   if (unlikely(key->external.lower_nv12 || key->external.lower_iyuv ||
                  key->external.lower_xy_uxvx || key->external.lower_yx_xuxv ||
                  key->external.lower_ayuv || key->external.lower_xyuv ||
                  key->external.lower_yuv)) {
      NIR_PASS_V(state.ir.nir, st_nir_lower_tex_src_plane,
                  ~stfp->Base.SamplersUsed,
                  key->external.lower_nv12 || key->external.lower_xy_uxvx ||
                     key->external.lower_yx_xuxv,
                  key->external.lower_iyuv);
      finalize = true;
   }

   if (finalize || !st->allow_st_finalize_nir_twice) {
      /* Some of the lowering above may have introduced new varyings */
      nir_shader_gather_info(state.ir.nir,
                              nir_shader_get_entrypoint(state.ir.nir));

      struct pipe_screen *screen = st->screen;
      if (screen->finalize_nir)
         screen->finalize_nir(screen, state.ir.nir, false);
   }

   if (ST_DEBUG & DEBUG_PRINT_IR)
      nir_print_shader(state.ir.nir, stderr);

   /* If the driver wants TGSI, then translate before handing off. */
   if (st->pipe->screen->get_shader_param(st->pipe->screen,
                                          PIPE_SHADER_FRAGMENT,
                                          PIPE_SHADER_CAP_PREFERRED_IR) !=
         PIPE_SHADER_IR_NIR) {
      nir_shader *s = state.ir.nir;
      state.tokens = nir_to_tgsi(s, st->pipe->screen);
      state.type = PIPE_SHADER_IR_TGSI;
      ralloc_free(s);
   }

   variant->base.driver_shader = pipe->create_fs_state(pipe, &state);
   variant->key = *key;

   return variant;
}

/**
 * Translate fragment program if needed.
 */
struct st_fp_variant *
st_get_fp_variant(struct st_context *st,
                  struct st_program *stfp,
                  const struct st_fp_variant_key *key)
{
   struct st_fp_variant *fpv;

   /* Search for existing variant */
   for (fpv = st_fp_variant(stfp->variants); fpv;
        fpv = st_fp_variant(fpv->base.next)) {
      if (memcmp(&fpv->key, key, sizeof(*key)) == 0) {
         break;
      }
   }

   if (!fpv) {
      /* create new */
      fpv = st_create_fp_variant(st, stfp, key);
      if (fpv) {
         fpv->base.st = key->st;

         if (key->bitmap || key->drawpixels) {
            /* Regular variants should always come before the
             * bitmap & drawpixels variants, (unless there
             * are no regular variants) so that
             * st_update_fp can take a fast path when
             * shader_has_one_variant is set.
             */
            if (!stfp->variants) {
               stfp->variants = &fpv->base;
            } else {
               /* insert into list after the first one */
               fpv->base.next = stfp->variants->next;
               stfp->variants->next = &fpv->base;
            }
         } else {
            /* insert into list */
            fpv->base.next = stfp->variants;
            stfp->variants = &fpv->base;
         }
      }
   }

   return fpv;
}

/**
 * Get/create a basic program variant.
 */
struct st_variant *
st_get_common_variant(struct st_context *st,
                      struct st_program *prog,
                      const struct st_common_variant_key *key)
{
   struct pipe_context *pipe = st->pipe;
   struct st_variant *v;
   struct pipe_shader_state state = {0};
   struct gl_program_parameter_list *params = prog->Base.Parameters;

   /* Search for existing variant */
   for (v = prog->variants; v; v = v->next) {
      if (memcmp(&st_common_variant(v)->key, key, sizeof(*key)) == 0)
         break;
   }

   if (!v) {
      /* create new */
      v = (struct st_variant*)CALLOC_STRUCT(st_common_variant);
      if (v) {
         bool finalize = false;

         state.type = PIPE_SHADER_IR_NIR;
         state.ir.nir = get_nir_shader(st, prog);

         if (key->clamp_color) {
            NIR_PASS_V(state.ir.nir, nir_lower_clamp_color_outputs);
            finalize = true;
         }

         if (key->lower_ucp) {
            lower_ucp(st, state.ir.nir, key->lower_ucp, params);
            finalize = true;
         }

         if (key->lower_point_size) {
            static const gl_state_index16 point_size_state[STATE_LENGTH] =
               { STATE_INTERNAL, STATE_POINT_SIZE_CLAMPED, 0 };
            _mesa_add_state_reference(params, point_size_state);
            NIR_PASS_V(state.ir.nir, nir_lower_point_size_mov,
                        point_size_state);
            if (prog->Base.info.stage == MESA_SHADER_TESS_EVAL)
               prog->affected_states |= ST_NEW_TES_CONSTANTS;
            else if (prog->Base.info.stage == MESA_SHADER_GEOMETRY)
               prog->affected_states |= ST_NEW_GS_CONSTANTS;
            finalize = true;
         }
         state.stream_output = prog->state.stream_output;

         if (finalize || !st->allow_st_finalize_nir_twice) {
            st_finalize_nir(st, &prog->Base, prog->shader_program,
                              state.ir.nir, true);
         }

         if (ST_DEBUG & DEBUG_PRINT_IR)
            nir_print_shader(state.ir.nir, stderr);

         /* fill in new variant */
         switch (prog->Base.info.stage) {
         case MESA_SHADER_TESS_CTRL:
            v->driver_shader = pipe->create_tcs_state(pipe, &state);
            break;
         case MESA_SHADER_TESS_EVAL:
            v->driver_shader = pipe->create_tes_state(pipe, &state);
            break;
         case MESA_SHADER_GEOMETRY:
            v->driver_shader = pipe->create_gs_state(pipe, &state);
            break;
         case MESA_SHADER_COMPUTE: {
            struct pipe_compute_state cs = {0};
            cs.ir_type = state.type;
            cs.req_local_mem = prog->Base.info.cs.shared_size;

            if (state.type == PIPE_SHADER_IR_NIR)
               cs.prog = state.ir.nir;
            else
               cs.prog = state.tokens;

            v->driver_shader = pipe->create_compute_state(pipe, &cs);
            break;
         }
         default:
            assert(!"unhandled shader type");
            free(v);
            return NULL;
         }

         st_common_variant(v)->key = *key;
         v->st = key->st;

         /* insert into list */
         v->next = prog->variants;
         prog->variants = v;
      }
   }

   return v;
}


/**
 * Vert/Geom/Frag programs have per-context variants.  Free all the
 * variants attached to the given program which match the given context.
 */
static void
destroy_program_variants(struct st_context *st, struct gl_program *target)
{
   if (!target || target == &_mesa_DummyProgram)
      return;

   struct st_program *p = st_program(target);
   struct st_variant *v, **prevPtr = &p->variants;
   bool unbound = false;

   for (v = p->variants; v; ) {
      struct st_variant *next = v->next;
      if (v->st == st) {
         if (!unbound) {
            st_unbind_program(st, p);
            unbound = true;
         }

         /* unlink from list */
         *prevPtr = next;
         /* destroy this variant */
         delete_variant(st, v, target->Target);
      }
      else {
         prevPtr = &v->next;
      }
      v = next;
   }
}


/**
 * Callback for _mesa_HashWalk.  Free all the shader's program variants
 * which match the given context.
 */
static void
destroy_shader_program_variants_cb(void *data, void *userData)
{
   struct st_context *st = (struct st_context *) userData;
   struct gl_shader *shader = (struct gl_shader *) data;

   switch (shader->Type) {
   case GL_SHADER_PROGRAM_MESA:
      {
         struct gl_shader_program *shProg = (struct gl_shader_program *) data;
         GLuint i;

         for (i = 0; i < ARRAY_SIZE(shProg->_LinkedShaders); i++) {
            if (shProg->_LinkedShaders[i])
               destroy_program_variants(st, shProg->_LinkedShaders[i]->Program);
         }
      }
      break;
   case GL_VERTEX_SHADER:
   case GL_FRAGMENT_SHADER:
   case GL_GEOMETRY_SHADER:
   case GL_TESS_CONTROL_SHADER:
   case GL_TESS_EVALUATION_SHADER:
   case GL_COMPUTE_SHADER:
      break;
   default:
      assert(0);
   }
}


/**
 * Callback for _mesa_HashWalk.  Free all the program variants which match
 * the given context.
 */
static void
destroy_program_variants_cb(void *data, void *userData)
{
   struct st_context *st = (struct st_context *) userData;
   struct gl_program *program = (struct gl_program *) data;
   destroy_program_variants(st, program);
}


/**
 * Walk over all shaders and programs to delete any variants which
 * belong to the given context.
 * This is called during context tear-down.
 */
void
st_destroy_program_variants(struct st_context *st)
{
   /* If shaders can be shared with other contexts, the last context will
    * call DeleteProgram on all shaders, releasing everything.
    */
   if (st->has_shareable_shaders)
      return;

   /* ARB vert/frag program */
   _mesa_HashWalk(st->ctx->Shared->Programs,
                  destroy_program_variants_cb, st);

   /* GLSL vert/frag/geom shaders */
   _mesa_HashWalk(st->ctx->Shared->ShaderObjects,
                  destroy_shader_program_variants_cb, st);
}


/**
 * Compile one shader variant.
 */
static void
st_precompile_shader_variant(struct st_context *st,
                             struct gl_program *prog)
{
   switch (prog->Target) {
   case GL_VERTEX_PROGRAM_ARB: {
      struct st_program *p = (struct st_program *)prog;
      struct st_common_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;
      st_get_vp_variant(st, p, &key);
      break;
   }

   case GL_FRAGMENT_PROGRAM_ARB: {
      struct st_program *p = (struct st_program *)prog;
      struct st_fp_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;
      key.lower_alpha_func = COMPARE_FUNC_ALWAYS;
      if (p->ati_fs) {
         for (int i = 0; i < ARRAY_SIZE(key.texture_index); i++)
            key.texture_index[i] = TEXTURE_2D_INDEX;
      }
      st_get_fp_variant(st, p, &key);
      break;
   }

   case GL_TESS_CONTROL_PROGRAM_NV:
   case GL_TESS_EVALUATION_PROGRAM_NV:
   case GL_GEOMETRY_PROGRAM_NV:
   case GL_COMPUTE_PROGRAM_NV: {
      struct st_program *p = st_program(prog);
      struct st_common_variant_key key;

      memset(&key, 0, sizeof(key));

      key.st = st->has_shareable_shaders ? NULL : st;

      st_get_common_variant(st, p, &key);
      break;
   }

   default:
      assert(0);
   }
}

void
st_serialize_nir(struct st_program *stp)
{
   if (!stp->serialized_nir) {
      struct blob blob;
      size_t size;

      blob_init(&blob);
      nir_serialize(&blob, stp->Base.nir, false);
      blob_finish_get_buffer(&blob, &stp->serialized_nir, &size);
      stp->serialized_nir_size = size;
   }
}

void
st_finalize_program(struct st_context *st, struct gl_program *prog)
{
   if (st->current_program[prog->info.stage] == prog) {
      if (prog->info.stage == MESA_SHADER_VERTEX)
         st->dirty |= ST_NEW_VERTEX_PROGRAM(st, (struct st_program *)prog);
      else
         st->dirty |= ((struct st_program *)prog)->affected_states;
   }

   if (prog->nir) {
      nir_sweep(prog->nir);

      /* This is only needed for ARB_vp/fp programs and when the disk cache
       * is disabled. If the disk cache is enabled, GLSL programs are
       * serialized in write_nir_to_cache.
       */
      st_serialize_nir(st_program(prog));
   }

   /* Create Gallium shaders now instead of on demand. */
   if (ST_DEBUG & DEBUG_PRECOMPILE ||
       st->shader_has_one_variant[prog->info.stage])
      st_precompile_shader_variant(st, prog);
}
