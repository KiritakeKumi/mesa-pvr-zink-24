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

#include "va_compiler.h"
#include "valhall.h"

/* This file contains the final passes of the compiler. Running after
 * scheduling and RA, the IR is now finalized, so we need to emit it to actual
 * bits on the wire (as well as fixup branches) */

static unsigned
va_pack_reg(bi_index idx)
{
   assert(idx.type == BI_INDEX_REGISTER);
   assert(idx.value < 64);
   return idx.value;
}

static unsigned
va_pack_src(bi_index idx)
{
   if (idx.type == BI_INDEX_REGISTER) {
      unsigned value = va_pack_reg(idx);
      if (idx.discard) value |= (1 << 6);
      return value;
   } else if (idx.type == BI_INDEX_FAU) {
      assert(idx.offset <= 1);

      unsigned val = (idx.value & 0x3F) << 1;
      if (idx.offset) val++;

      if (idx.value & BIR_FAU_IMMEDIATE)
         return (0x3 << 6) | val;
      else if (idx.value & BIR_FAU_UNIFORM)
         return (0x2 << 6) | val;
      else if (idx.value == BIR_FAU_LANE_ID)
         return (0x3 << 6) | (32+2);
      else if (idx.value == BIR_FAU_ATEST_PARAM)
         return (0x3 << 6) | (0x2A);
      else if (idx.value >= BIR_FAU_BLEND_0 && idx.value <= BIR_FAU_BLEND_0 + 8)
         return (0x3 << 6) | (0x30 + ((idx.value - BIR_FAU_BLEND_0) << 1) + idx.offset);
      else
         unreachable("TODO: handle fau");
   }

   unreachable("Invalid type");
}

static unsigned
va_pack_wrmask(enum bi_swizzle swz)
{
   switch (swz) {
   case BI_SWIZZLE_H00: return 0x1;
   case BI_SWIZZLE_H11: return 0x2;
   case BI_SWIZZLE_H01: return 0x3;
   default: unreachable("Invalid write mask");
   }
}

static unsigned
va_pack_dest(bi_index index)
{
   return va_pack_reg(index) | (va_pack_wrmask(index.swizzle) << 6);
}

static unsigned
va_pack_widen_f32(enum bi_swizzle swz)
{
   switch (swz) {
   case BI_SWIZZLE_H01: return 0;
   case BI_SWIZZLE_H00: return 1;
   case BI_SWIZZLE_H11: return 2;
   default: unreachable("Invalid widen");
   }
}

/* bits are reversed */
static unsigned
va_pack_swizzle_f16(enum bi_swizzle swz)
{
   switch (swz) {
   case BI_SWIZZLE_H00: return 0;
   case BI_SWIZZLE_H10: return 1;
   case BI_SWIZZLE_H01: return 2;
   case BI_SWIZZLE_H11: return 3;
   default: unreachable("Invalid swizzle");
   }
}

static unsigned
va_pack_widen(enum bi_swizzle swz, enum va_size size)
{
   if (size == VA_SIZE_8) {
      switch (swz) {
      case BI_SWIZZLE_H01: return 0;
      case BI_SWIZZLE_H00: return 2;
      case BI_SWIZZLE_H11: return 3;
      case BI_SWIZZLE_B0000: return 4;
      case BI_SWIZZLE_B1111: return 5;
      case BI_SWIZZLE_B2222: return 6;
      case BI_SWIZZLE_B3333: return 7;
      default: unreachable("Exotic swizzles not yet handled");
      }
   } else if (size == VA_SIZE_16) {
      switch (swz) {
      case BI_SWIZZLE_H00: return 0;
      case BI_SWIZZLE_H10: return 1;
      case BI_SWIZZLE_H01: return 2;
      case BI_SWIZZLE_H11: return 3;
      case BI_SWIZZLE_B0000: return 4;
      case BI_SWIZZLE_B1111: return 8;
      case BI_SWIZZLE_B2222: return 7;
      case BI_SWIZZLE_B3333: return 10;
      default: unreachable("Exotic swizzles not yet handled");
      }
   } else if (size == VA_SIZE_32) {
      switch (swz) {
      case BI_SWIZZLE_H01: return 0;
      case BI_SWIZZLE_H00: return 2;
      case BI_SWIZZLE_H11: return 3;
      case BI_SWIZZLE_B0000: return 4;
      case BI_SWIZZLE_B1111: return 5;
      case BI_SWIZZLE_B2222: return 6;
      case BI_SWIZZLE_B3333: return 7;
      default: unreachable("Invalid swizzle");
      }
   } else {
      unreachable("TODO: other type sizes");
   }
}

static unsigned
va_pack_shift_lanes(enum bi_swizzle swz)
{
   switch (swz) {
   case BI_SWIZZLE_H01: return 0; // b02
   case BI_SWIZZLE_B0000: return 4; // b00
   default: unreachable("todo: more shifts");
   }
}

static unsigned
va_pack_branch_lane(enum bi_swizzle swz)
{
   switch (swz) {
   case BI_SWIZZLE_H01: return 0;
   case BI_SWIZZLE_H00: return 1;
   case BI_SWIZZLE_H11: return 2;
   default: unreachable("Invalid branch lane");
   }
}

static uint64_t
va_pack_alu(const bi_instr *I)
{
   struct va_opcode_info info = valhall_opcodes[I->op];
   uint64_t hex = 0;

   switch (I->op) {
   /* Add FREXP flags */
   case BI_OPCODE_FREXPE_F32:
   case BI_OPCODE_FREXPE_V2F16:
   case BI_OPCODE_FREXPM_F32:
   case BI_OPCODE_FREXPM_V2F16:
      if (I->sqrt) hex |= 1ull << 24;
      if (I->log) hex |= 1ull << 25;
      break;

   /* Add mux type */
   case BI_OPCODE_MUX_I32:
   case BI_OPCODE_MUX_V2I16:
   case BI_OPCODE_MUX_V4I8:
      hex |= (uint64_t) I->mux << 32;
      break;

   /* Add .eq flag */
   case BI_OPCODE_BRANCHZ_I16:
   case BI_OPCODE_BRANCHZI:
      assert(I->cmpf == BI_CMPF_EQ || I->cmpf == BI_CMPF_NE);

      if (I->cmpf == BI_CMPF_EQ) hex |= (1ull << 36);
      break;

   /* Add arithmetic flag */
   case BI_OPCODE_RSHIFT_AND_I32:
   case BI_OPCODE_RSHIFT_AND_V2I16:
   case BI_OPCODE_RSHIFT_AND_V4I8:
   case BI_OPCODE_RSHIFT_OR_I32:
   case BI_OPCODE_RSHIFT_OR_V2I16:
   case BI_OPCODE_RSHIFT_OR_V4I8:
   case BI_OPCODE_RSHIFT_XOR_I32:
   case BI_OPCODE_RSHIFT_XOR_V2I16:
   case BI_OPCODE_RSHIFT_XOR_V4I8:
      hex |= (uint64_t) I->arithmetic << 34;
      break;

   default:
      break;
   }

   /* FMA_RSCALE.f32 special modes treated as extra opcodes */
   if (I->op == BI_OPCODE_FMA_RSCALE_F32) {
      assert(I->special < 4);
      hex |= ((uint64_t) I->special) << 48;
   }

   /* Add the normal destination or a placeholder */
   if (info.has_dest) {
      hex |= (uint64_t) va_pack_dest(I->dest[0]) << 40;
   } else if (info.nr_staging_dests == 0) {
      assert(bi_is_null(I->dest[0]));
      hex |= 0xC0ull << 40; /* Placeholder */
   }

   bool swap12 = va_swap_12(I->op);

   for (unsigned i = 0; i < info.nr_srcs; ++i) {
      unsigned logical_i = (swap12 && i == 1) ? 2 : (swap12 && i == 2) ? 1 : i;

      struct va_src_info src_info = info.srcs[i];
      enum va_size size = src_info.size;

      bi_index src = I->src[logical_i];
      hex |= (uint64_t) va_pack_src(src) << (8 * i);

      if (src_info.notted) {
         if (src.neg) hex |= (1ull << 35);
      } else if (src_info.absneg) {
         unsigned neg_offs = 32 + 2 + ((2 - i) * 2);
         unsigned abs_offs = 33 + 2 + ((2 - i) * 2);

         if (src.neg) hex |= 1ull << neg_offs;
         if (src.abs) hex |= 1ull << abs_offs;
      } else {
         assert(!src.neg && "Unexpected negate");
         assert(!src.abs && "Unexpected absolute value");
      }

      if (src_info.swizzle) {
         unsigned offs = 24 + ((2 - i) * 2);
         unsigned S = src.swizzle;
         assert(size == VA_SIZE_16 || size == VA_SIZE_32);

         uint64_t v = (size == VA_SIZE_32 ? va_pack_widen_f32(S) : va_pack_swizzle_f16(S));
         hex |= v << offs;
      } else if (src_info.widen) {
         unsigned offs = (i == 1) ? 26 : 36;
         hex |= (uint64_t) va_pack_widen(src.swizzle, src_info.size) << offs;
      } else if (src_info.lane) {
         unsigned offs = 28;
         assert(i == 0 && "todo: MKVEC");
         if (src_info.size == VA_SIZE_16) {
            hex |= (src.swizzle == BI_SWIZZLE_H11 ? 1 : 0) << offs;
         } else if (I->op == BI_OPCODE_BRANCHZ_I16) {
            hex |= ((uint64_t) va_pack_branch_lane(src.swizzle) << 37);
         } else {
            assert(src_info.size == VA_SIZE_8);
            unsigned comp = src.swizzle - BI_SWIZZLE_B0000;
            assert(comp < 4);
            hex |= (uint64_t) comp << offs;
         }
      } else if (src_info.lanes) {
         assert(src_info.size == VA_SIZE_8);
         assert(i == 1);
         hex |= (uint64_t) va_pack_shift_lanes(src.swizzle) << 26;
      } else if (src_info.combine) {
         /* Treat as swizzle, subgroup ops not yet supported */
         assert(src_info.size == VA_SIZE_32);
         assert(i == 0);
         hex |= (uint64_t) va_pack_widen_f32(src.swizzle) << 37;
      } else {
         assert(src.swizzle == BI_SWIZZLE_H01 && "Unexpected swizzle");
      }
   }

   if (info.clamp) hex |= (uint64_t) I->clamp << 32;
   if (info.round_mode) hex |= (uint64_t) I->round << 30;
   if (info.condition) hex |= (uint64_t) I->cmpf << 32;
   if (info.result_type) hex |= (uint64_t) I->result_type << 30;

   return hex;
}

static uint64_t
va_pack_byte_offset(const bi_instr *I)
{
   int16_t offset = I->byte_offset;
   assert(offset == I->byte_offset && "offset overflow");

   uint16_t offset_as_u16 = offset;
   return ((uint64_t) offset_as_u16) << 8;
}

static uint64_t
va_pack_load(const bi_instr *I, bool buffer_descriptor)
{
   const uint8_t load_lane_identity[8] = {
      0, 0, 0, 0, 4, 7, 6, 7
   };

   // load lane identity
   unsigned memory_size = (valhall_opcodes[I->op].exact >> 27) & 0x7;
   uint64_t hex = (uint64_t) load_lane_identity[memory_size] << 36;

   // unsigned
   hex |= (1ull << 39);

   if (!buffer_descriptor)
      hex |= va_pack_byte_offset(I);

   // staging write
   hex |= (uint64_t) va_pack_reg(I->dest[0]) << 40;
   hex |= (0x80ull << 40); // flags

   // 1-src
   hex |= (uint64_t) va_pack_src(I->src[0]) << 0;

   // 2-src for buffer
   if (buffer_descriptor)
      hex |= (uint64_t) va_pack_src(I->src[1]) << 8;

   return hex;
}

enum va_memory_access {
   VA_MEMORY_ACCESS_NONE = 0,
   VA_MEMORY_ACCESS_ISTREAM = 1,
   VA_MEMORY_ACCESS_ESTREAM = 2,
   VA_MEMORY_ACCESS_FORCE = 3,
};

static uint64_t
va_pack_memory_access(const bi_instr *I)
{
   switch (I->seg) {
   case BI_SEG_TL:   return VA_MEMORY_ACCESS_FORCE;
   case BI_SEG_POS:  return VA_MEMORY_ACCESS_ISTREAM;
   case BI_SEG_VARY: return VA_MEMORY_ACCESS_ESTREAM;
   default:          return VA_MEMORY_ACCESS_NONE;
   }
}

static uint64_t
va_pack_store(const bi_instr *I)
{
   /* Staging read */
   uint64_t hex = (uint64_t) va_pack_reg(I->src[0]) << 40;
   hex |= (0x40ull << 40); // flags

   /* Memory access */
   hex |= va_pack_memory_access(I) << 24;

   /* 1-src */
   hex |= (uint64_t) va_pack_src(I->src[1]) << 0;

   hex |= va_pack_byte_offset(I);

   return hex;
}

static unsigned
va_pack_lod_mode(enum bi_va_lod_mode mode)
{
   switch (mode) {
   case BI_VA_LOD_MODE_ZERO_LOD: return 0;
   case BI_VA_LOD_MODE_COMPUTED_LOD: return 1;
   case BI_VA_LOD_MODE_EXPLICIT: return 4;
   case BI_VA_LOD_MODE_COMPUTED_BIAS: return 5;
   case BI_VA_LOD_MODE_GRDESC: return 6;
   }

   unreachable("Invalid LOD mode");
}

static unsigned
va_pack_register_type(enum bi_register_format regfmt)
{
   switch (regfmt) {
   case BI_REGISTER_FORMAT_F16:
   case BI_REGISTER_FORMAT_F32:
      return 1;

   case BI_REGISTER_FORMAT_U16:
   case BI_REGISTER_FORMAT_U32:
      return 2;

   case BI_REGISTER_FORMAT_S16:
   case BI_REGISTER_FORMAT_S32:
      return 3;

   default:
      unreachable("Invalid register format");
   }
}

static uint64_t
va_pack_varying_format(const bi_instr *I)
{
   switch (I->register_format) {
   case BI_REGISTER_FORMAT_S32:
   case BI_REGISTER_FORMAT_U32: return 0;
   case BI_REGISTER_FORMAT_S16:
   case BI_REGISTER_FORMAT_U16: return 1;
   case BI_REGISTER_FORMAT_F32: return 2;
   case BI_REGISTER_FORMAT_F16: return 3;
   default: unreachable("unhandled register format");
   }
}

static uint64_t
va_pack_register_format(const bi_instr *I)
{
   switch (I->register_format) {
   case BI_REGISTER_FORMAT_F32: return 2;
   case BI_REGISTER_FORMAT_F16: return 3;
   case BI_REGISTER_FORMAT_S32: return 4;
   case BI_REGISTER_FORMAT_S16: return 5;
   case BI_REGISTER_FORMAT_U32: return 6;
   case BI_REGISTER_FORMAT_U16: return 7;
   default: unreachable("unhandled register format");
   }
}

static uint64_t
va_pack_typed_load(const bi_instr *I)
{
   uint64_t hex = va_pack_alu(I);

   /* Staging register - destination */
   hex |= (uint64_t) va_pack_reg(I->dest[0]) << 40;
   hex |= (0x80ull << 40);

   hex |= va_pack_register_format(I) << 24;
   hex |= ((uint64_t) I->vecsize << 28);

   return hex;
}

uint64_t
va_pack_instr(const bi_instr *I, unsigned action)
{
   enum va_immediate_mode fau_mode = va_select_fau_mode(I);
   struct va_opcode_info info = valhall_opcodes[I->op];

   unsigned meta = fau_mode | (action << 2);
   uint64_t hex = info.exact | (((uint64_t) meta) << 57);

   /* Staging register count */
   switch (I->op) {
   case BI_OPCODE_LOAD_I8:
   case BI_OPCODE_LOAD_I16:
   case BI_OPCODE_LOAD_I24:
   case BI_OPCODE_LOAD_I32:
   case BI_OPCODE_LOAD_I48:
   case BI_OPCODE_LOAD_I64:
   case BI_OPCODE_LOAD_I96:
   case BI_OPCODE_LOAD_I128:
   case BI_OPCODE_LD_BUFFER_I8:
   case BI_OPCODE_LD_BUFFER_I16:
   case BI_OPCODE_LD_BUFFER_I24:
   case BI_OPCODE_LD_BUFFER_I32:
   case BI_OPCODE_LD_BUFFER_I48:
   case BI_OPCODE_LD_BUFFER_I64:
   case BI_OPCODE_LD_BUFFER_I96:
   case BI_OPCODE_LD_BUFFER_I128:
   case BI_OPCODE_ATEST:
   case BI_OPCODE_LD_VAR_SPECIAL:
   case BI_OPCODE_LD_ATTR_IMM:
   case BI_OPCODE_LD_VAR_BUF_IMM_F16:
   case BI_OPCODE_LD_VAR_BUF_IMM_F32:
   case BI_OPCODE_LEA_BUF_IMM:
   case BI_OPCODE_LD_TILE:
      hex |= ((uint64_t) bi_count_write_registers(I, 0) << 33);

      /* Slot */
      hex |= (0ull << 30);
      break;
   case BI_OPCODE_STORE_I8:
   case BI_OPCODE_STORE_I16:
   case BI_OPCODE_STORE_I24:
   case BI_OPCODE_STORE_I32:
   case BI_OPCODE_STORE_I48:
   case BI_OPCODE_STORE_I64:
   case BI_OPCODE_STORE_I96:
   case BI_OPCODE_STORE_I128:
   case BI_OPCODE_BLEND:
   case BI_OPCODE_TEX_SINGLE:
      hex |= ((uint64_t) bi_count_read_registers(I, 0) << 33);

      /* Slot */
      hex |= (0ull << 30);
      break;
   default:
      break;
   }

   switch (I->op) {
   case BI_OPCODE_LOAD_I8:
   case BI_OPCODE_LOAD_I16:
   case BI_OPCODE_LOAD_I24:
   case BI_OPCODE_LOAD_I32:
   case BI_OPCODE_LOAD_I48:
   case BI_OPCODE_LOAD_I64:
   case BI_OPCODE_LOAD_I96:
   case BI_OPCODE_LOAD_I128:
      hex |= va_pack_load(I, false);
      break;

   case BI_OPCODE_LD_BUFFER_I8:
   case BI_OPCODE_LD_BUFFER_I16:
   case BI_OPCODE_LD_BUFFER_I24:
   case BI_OPCODE_LD_BUFFER_I32:
   case BI_OPCODE_LD_BUFFER_I48:
   case BI_OPCODE_LD_BUFFER_I64:
   case BI_OPCODE_LD_BUFFER_I96:
   case BI_OPCODE_LD_BUFFER_I128:
      hex |= va_pack_load(I, true);
      break;

   case BI_OPCODE_STORE_I8:
   case BI_OPCODE_STORE_I16:
   case BI_OPCODE_STORE_I24:
   case BI_OPCODE_STORE_I32:
   case BI_OPCODE_STORE_I48:
   case BI_OPCODE_STORE_I64:
   case BI_OPCODE_STORE_I96:
   case BI_OPCODE_STORE_I128:
      hex |= va_pack_store(I);
      break;

   case BI_OPCODE_LEA_BUF_IMM:
      /* Staging register - destination */
      hex |= (uint64_t) va_pack_reg(I->dest[0]) << 40;
      hex |= (0x80ull << 40); // flags
      hex |= va_pack_alu(I);

      /* Unknown */
      hex |= 0xD << 8;
      break;

   case BI_OPCODE_BRANCHZ_I16:
      hex |= va_pack_alu(I);
      hex |= ((uint64_t) I->branch_offset & BITFIELD_MASK(27)) << 8;
      break;

   case BI_OPCODE_BRANCHZI:
      hex |= va_pack_alu(I);
      hex |= (0x1ull << 40); // unknown
      break;

   case BI_OPCODE_IADD_IMM_I32:
   case BI_OPCODE_IADD_IMM_V2I16:
   case BI_OPCODE_IADD_IMM_V4I8:
   case BI_OPCODE_FADD_IMM_F32:
   case BI_OPCODE_FADD_IMM_V2F16:
      hex |= va_pack_alu(I);
      hex |= ((uint64_t) I->index) << 8;
      break;

   case BI_OPCODE_CLPER_I32:
      assert(I->inactive_result < 16);
      assert(I->lane_op < 4);
      assert(I->subgroup < 4);

      hex |= va_pack_alu(I);
      hex |= ((uint64_t) I->inactive_result) << 22;
      hex |= ((uint64_t) I->lane_op) << 32;
      hex |= ((uint64_t) I->subgroup) << 36;
      break;

   case BI_OPCODE_BLEND:
   {
      /* Blend descriptor */
      hex |= ((uint64_t) va_pack_src(I->src[2])) << 0;

      /* Target */
      hex |= (0ull << 8);

      /* Staging register #1 - coverage task */
      hex |= ((uint64_t) va_pack_reg(I->src[1])) << 16;

      /* Register format */
      hex |= va_pack_register_format(I) << 24;

      /* Vector size */
      unsigned vecsize = 4;
      hex |= ((uint64_t) (vecsize - 1) << 28);

      /* Staging register #2 - input colour */
      hex |= ((uint64_t) va_pack_reg(I->src[0])) << 40;
      hex |= (0x40ull << 40); // flags

      break;
   }

   case BI_OPCODE_TEX_SINGLE:
   {
      /* Image to read from */
      hex |= ((uint64_t) va_pack_src(I->src[1])) << 0;

      if (I->array_enable) hex |= (1ull << 10);
      if (I->texel_offset) hex |= (1ull << 11);
      if (I->shadow) hex |= (1ull << 12);
      if (I->skip) hex |= (1ull << 39);
      if (!bi_is_regfmt_16(I->register_format)) hex |= (1ull << 46);

      /* LOD mode */
      assert(I->va_lod_mode < 8);
      hex |= ((uint64_t) va_pack_lod_mode(I->va_lod_mode)) << 13;

      /* Staging register write */
      hex |= ((uint64_t) va_pack_reg(I->dest[0])) << 16;

      /* Write mask */
      hex |= (0xF << 22);

      /* Register type */
      hex |= ((uint64_t) va_pack_register_type(I->register_format)) << 26;

      /* Dimension */
      hex |= ((uint64_t) I->dimension) << 28;

      /* Staging write count */
      hex |= ((uint64_t) bi_count_write_registers(I, 0) - 1) << 36;

      /* Staging register read */
      hex |= ((uint64_t) va_pack_reg(I->src[0])) << 40;

      break;
   }

   case BI_OPCODE_ATEST:
   {
      /* Staging register - updated coverage mask */
      hex |= ((uint64_t) va_pack_reg(I->dest[0])) << 40;
      hex |= (0x80ull << 40); // flags

      hex |= va_pack_alu(I);
      break;
   }

   case BI_OPCODE_LD_VAR_BUF_IMM_F16:
   case BI_OPCODE_LD_VAR_BUF_IMM_F32:
   {
      hex |= va_pack_alu(I);

      /* Staging register - destination */
      hex |= (uint64_t) va_pack_reg(I->dest[0]) << 40;
      hex |= (0x80ull << 40);

      hex |= va_pack_varying_format(I) << 24;
      hex |= ((uint64_t) I->vecsize << 28);

      hex |= ((uint64_t) I->index) << 16;
      hex |= ((uint64_t) I->update) << 36;
      hex |= ((uint64_t) I->sample) << 38;
      break;
   }

   case BI_OPCODE_LD_VAR_SPECIAL:
   {
      hex |= va_pack_alu(I);

      /* Staging register - destination */
      hex |= (uint64_t) va_pack_reg(I->dest[0]) << 40;
      hex |= (0x80ull << 40);

      hex |= va_pack_varying_format(I) << 24;
      hex |= ((uint64_t) I->vecsize << 28);

      hex |= ((uint64_t) I->varying_name) << 12; /* instead of index */
      hex |= ((uint64_t) I->update) << 36;
      hex |= ((uint64_t) I->sample) << 38;
      break;
   }

   case BI_OPCODE_LD_ATTR_IMM:
   {
      hex |= va_pack_typed_load(I);

      hex |= ((uint64_t) I->table) << 16;
      hex |= ((uint64_t) I->attribute_index) << 20;
      break;
   }

   case BI_OPCODE_LD_TILE:
   {
      hex |= va_pack_typed_load(I);
      break;
   }

   case BI_OPCODE_LEA_ATTR_TEX:
   case BI_OPCODE_ST_CVT:
   case BI_OPCODE_ZS_EMIT:
      /* TODO: Pack thse ops. For now, nop them out */
      hex |= (0xc0ull << 40);
      break;

   default:
      if (!info.exact && I->op != BI_OPCODE_NOP) {
         bi_print_instr(I, stderr);
         fflush(stderr);
         unreachable("Opcode not packable on Valhall");
      }

      hex |= va_pack_alu(I);
      break;
   }

   return hex;
}

static bool
va_last_in_block(bi_block *block, bi_instr *I)
{
   return (I->link.next == &block->instructions);
}

static bool
va_should_return(bi_block *block, bi_instr *I)
{
   // TODO: return semantics wat?
   if (I->op == BI_OPCODE_BLEND)
      return true;

   /* Don't return within a block */
   if (!va_last_in_block(block, I))
      return false;

   /* Don't return if we're succeeded by instructions */
   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      bi_block *succ = block->successors[i];

      if (succ && !bi_is_terminal_block(succ))
         return false;
   }

   return true;
}

static unsigned
va_pack_action(bi_block *block, bi_instr *I)
{
   // TODO: make blend shaders work..
   if (I->op == BI_OPCODE_BRANCHZI)
      return 0x1;

   /* .return */
   if (va_should_return(block, I))
      return 0x7 | 0x8;

   /* .reconverge */
   if (va_last_in_block(block, I) && bi_reconverge_branches(block))
      return 0x2 | 0x8;

   /* TODO: Barrier, thread discard, ATEST */
   if (I->action)
      return I->action;

   /* TODO: Generalize waits */
   if (valhall_opcodes[I->op].nr_staging_dests > 0)
      return 0x1;

   /* Default - no action */
   return 0;
}

void
bi_pack_valhall(bi_context *ctx, struct util_dynarray *emission)
{
   unsigned orig_size = emission->size;

   bi_foreach_block(ctx, block) {
      bi_foreach_instr_in_block(block, I) {
         unsigned action = va_pack_action(block, I);
         uint64_t hex = va_pack_instr(I, action);
         util_dynarray_append(emission, uint64_t, hex);
      }
   }

   /* Pad with zeroes, but keep empty programs empty so they may be omitted
    * altogether. Failing to do this would result in a program containing only
    * zeroes, which is invalid and will raise an encoding fault.
    *
    * Also pad until we're aligned, so a secondary shader will start at an
    * aligned offset. This may not be strictly necessary but it's good practice.
    */
   if (orig_size != emission->size) {
      unsigned aligned = ALIGN_POT(emission->size, 64);
      unsigned count = MAX2(aligned - emission->size, 16);

      memset(util_dynarray_grow(emission, uint8_t, count), 0, count);
   }
}
