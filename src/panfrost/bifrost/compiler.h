/*
 * Copyright (C) 2020 Collabora Ltd.
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
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#ifndef __BIFROST_COMPILER_H
#define __BIFROST_COMPILER_H

#include "bifrost.h"
#include "bi_opcodes.h"
#include "compiler/nir/nir.h"
#include "panfrost/util/pan_ir.h"
#include "util/u_math.h"

/* Bifrost opcodes are tricky -- the same op may exist on both FMA and
 * ADD with two completely different opcodes, and opcodes can be varying
 * length in some cases. Then we have different opcodes for int vs float
 * and then sometimes even for different typesizes. Further, virtually
 * every op has a number of flags which depend on the op. In constrast
 * to Midgard where you have a strict ALU/LDST/TEX division and within
 * ALU you have strict int/float and that's it... here it's a *lot* more
 * involved. As such, we use something much higher level for our IR,
 * encoding "classes" of operations, letting the opcode details get
 * sorted out at emit time.
 *
 * Please keep this list alphabetized. Please use a dictionary if you
 * don't know how to do that.
 */

enum bi_class {
        BI_ADD,
        BI_ATEST,
        BI_BRANCH,
        BI_CMP,
        BI_BLEND,
        BI_BITWISE,
        BI_COMBINE,
        BI_CONVERT,
        BI_CSEL,
        BI_DISCARD,
        BI_FMA,
        BI_FMOV,
        BI_FREXP,
        BI_IMATH,
        BI_LOAD,
        BI_LOAD_UNIFORM,
        BI_LOAD_ATTR,
        BI_LOAD_VAR,
        BI_LOAD_VAR_ADDRESS,
        BI_LOAD_TILE,
        BI_MINMAX,
        BI_MOV,
        BI_REDUCE_FMA,
        BI_SELECT,
        BI_STORE,
        BI_STORE_VAR,
        BI_SPECIAL_ADD, /* _FAST on supported GPUs */
        BI_SPECIAL_FMA, /* _FAST on supported GPUs */
        BI_TABLE,
        BI_TEXS,
        BI_TEXC,
        BI_TEXC_DUAL,
        BI_ROUND,
        BI_IMUL,
        BI_ZS_EMIT,
        BI_NUM_CLASSES
};

/* Properties of a class... */
extern unsigned bi_class_props[BI_NUM_CLASSES];

/* abs/neg/clamp valid for a float op */
#define BI_MODS (1 << 0)

/* Accepts a bi_cond */
#define BI_CONDITIONAL (1 << 1)

/* Accepts a bi_round */
#define BI_ROUNDMODE (1 << 2)

/* Can be scheduled to FMA */
#define BI_SCHED_FMA (1 << 3)

/* Can be scheduled to ADD */
#define BI_SCHED_ADD (1 << 4)

/* Most ALU ops can do either, actually */
#define BI_SCHED_ALL (BI_SCHED_FMA | BI_SCHED_ADD)

/* Along with setting BI_SCHED_ADD, eats up the entire cycle, so FMA must be
 * nopped out. Used for _FAST operations. */
#define BI_SCHED_SLOW (1 << 5)

/* Swizzling allowed for the 8/16-bit source */
#define BI_SWIZZLABLE (1 << 6)

/* For scheduling purposes this is a high latency instruction and must be at
 * the end of a clause. Implies ADD */
#define BI_SCHED_HI_LATENCY (1 << 7)

/* Intrinsic is vectorized and acts with `vector_channels` components */
#define BI_VECTOR (1 << 8)

/* Use a data register for src0/dest respectively, bypassing the usual
 * register accessor. */
#define BI_DATA_REG_SRC (1 << 9)
#define BI_DATA_REG_DEST (1 << 10)

/* Quirk: cannot encode multiple abs on FMA in fp16 mode */
#define BI_NO_ABS_ABS_FP16_FMA (1 << 11)

/* It can't get any worse than csel4... can it? */
#define BIR_SRC_COUNT 4

/* BI_LD_VARY */
struct bi_load_vary {
        enum bi_sample interp_mode;
        enum bi_update update_mode;
        enum bi_varying_name var_id;
        unsigned index;
        bool immediate;
        bool special;
        bool reuse;
        bool flat;
};

/* BI_BRANCH encoding the details of the branch itself as well as a pointer to
 * the target. We forward declare bi_block since this is mildly circular (not
 * strictly, but this order of the file makes more sense I think)
 *
 * We define our own enum of conditions since the conditions in the hardware
 * packed in crazy ways that would make manipulation unweildly (meaning changes
 * based on slot swapping, etc), so we defer dealing with that until emit time.
 * Likewise, we expose NIR types instead of the crazy branch types, although
 * the restrictions do eventually apply of course. */

struct bi_block;

/* Sync with gen-pack.py */
enum bi_cond {
        BI_COND_ALWAYS = 0,
        BI_COND_LT,
        BI_COND_LE,
        BI_COND_GE,
        BI_COND_GT,
        BI_COND_EQ,
        BI_COND_NE,
};

/* Opcodes within a class */
enum bi_minmax_op {
        BI_MINMAX_MIN,
        BI_MINMAX_MAX
};

enum bi_bitwise_op {
        BI_BITWISE_AND,
        BI_BITWISE_OR,
        BI_BITWISE_XOR,
        BI_BITWISE_ARSHIFT,
};

enum bi_imath_op {
        BI_IMATH_ADD,
        BI_IMATH_SUB,
};

enum bi_imul_op {
        BI_IMUL_IMUL,
};

enum bi_table_op {
        /* fp32 log2() with low precision, suitable for GL or half_log2() in
         * CL. In the first argument, takes x. Letting u be such that x =
         * 2^{-m} u with m integer and 0.75 <= u < 1.5, returns
         * log2(u) / (u - 1). */

        BI_TABLE_LOG2_U_OVER_U_1_LOW,
};

enum bi_reduce_op {
        /* Takes two fp32 arguments and returns x + frexp(y). Used in
         * low-precision log2 argument reduction on newer models. */

        BI_REDUCE_ADD_FREXPM,
};

enum bi_frexp_op {
        BI_FREXPE_LOG,
};

enum bi_special_op {
        BI_SPECIAL_FRCP,
        BI_SPECIAL_FRSQ,

        /* fp32 exp2() with low precision, suitable for half_exp2() in CL or
         * exp2() in GL. In the first argument, it takes f2i_rte(x * 2^24). In
         * the second, it takes x itself. */
        BI_SPECIAL_EXP2_LOW,
        BI_SPECIAL_IABS,

        /* cubemap coordinates extraction helpers */
        BI_SPECIAL_CUBEFACE1,
        BI_SPECIAL_CUBEFACE2,
        BI_SPECIAL_CUBE_SSEL,
        BI_SPECIAL_CUBE_TSEL,

        /* Cross-lane permute, used to implement dFd{x,y} */
        BI_SPECIAL_CLPER_V6,
        BI_SPECIAL_CLPER_V7,
};

struct bi_bitwise {
        bool dest_invert;
        bool src1_invert;
        bool rshift; /* false for lshift */
};

struct bi_texture {
        /* Constant indices. Indirect would need to be in src[..] like normal,
         * we can reserve some sentinels there for that for future. */
        unsigned texture_index, sampler_index, varying_index;

        /* Should the LOD be computed based on neighboring pixels? Only valid
         * in fragment shaders. */
        bool compute_lod;
};

struct bi_clper {
        struct {
                enum bi_lane_op lane_op_mod;
                enum bi_inactive_result inactive_res;
        } clper;
        enum bi_subgroup subgroup_sz;
};

struct bi_attribute {
        unsigned index;
        bool immediate;
};

typedef struct {
        struct list_head link; /* Must be first */
        enum bi_class type;

        /* Indices, see pan_ssa_index etc. Note zero is special cased
         * to "no argument" */
        unsigned dest;
        unsigned src[BIR_SRC_COUNT];

        /* 32-bit word offset for destination, added to the register number in
         * RA when lowering combines */
        unsigned dest_offset;

        /* If one of the sources has BIR_INDEX_CONSTANT */
        union {
                uint64_t u64;
                uint32_t u32;
                uint16_t u16[2];
                uint8_t u8[4];
        } constant;

        /* Floating-point modifiers, type/class permitting. If not
         * allowed for the type/class, these are ignored. */
        enum bi_clamp clamp;
        bool src_abs[BIR_SRC_COUNT];
        bool src_neg[BIR_SRC_COUNT];

        /* Round mode (requires BI_ROUNDMODE) */
        enum bi_round round;

        /* Destination type. Usually the type of the instruction
         * itself, but if sources and destination have different
         * types, the type of the destination wins (so f2i would be
         * int). Zero if there is no destination. Bitsize included */
        nir_alu_type dest_type;

        /* Source types if required by the class */
        nir_alu_type src_types[BIR_SRC_COUNT];

        /* register_format if applicable */
        nir_alu_type format;

        /* If the source type is 8-bit or 16-bit such that SIMD is possible,
         * and the class has BI_SWIZZLABLE, this is a swizzle in the usual
         * sense. On non-SIMD instructions, it can be used for component
         * selection, so we don't have to special case extraction. */
        uint8_t swizzle[BIR_SRC_COUNT][NIR_MAX_VEC_COMPONENTS];

        /* For VECTOR ops, how many channels are written? */
        unsigned vector_channels;

        /* For texture ops, the skip bit. Set if helper invocations can skip
         * the operation. That is, set if the result of this texture operation
         * is never used for cross-lane operation (including texture
         * coordinates and derivatives) as determined by data flow analysis
         * (like Midgard) */
        bool skip;

        /* The comparison op. BI_COND_ALWAYS may not be valid. */
        enum bi_cond cond;

        /* For memory ops, base address */
        enum bi_seg segment;

        /* Can we spill the value written here? Used to prevent
         * useless double fills */
        bool no_spill;

        /* A class-specific op from which the actual opcode can be derived
         * (along with the above information) */

        union {
                enum bi_minmax_op minmax;
                enum bi_bitwise_op bitwise;
                enum bi_special_op special;
                enum bi_reduce_op reduce;
                enum bi_table_op table;
                enum bi_frexp_op frexp;
                enum bi_imath_op imath;
                enum bi_imul_op imul;

                /* For FMA/ADD, should we add a biased exponent? */
                bool mscale;
        } op;

        /* Union for class-specific information */
        union {
                enum bi_sem minmax;
                struct bi_load_vary load_vary;
                struct bi_block *branch_target;

                /* For BLEND -- the location 0-7 */
                unsigned blend_location;

                struct bi_bitwise bitwise;
                struct bi_texture texture;
                struct bi_clper special;
                struct bi_attribute attribute;
        };
} bi_instruction;

/* Swizzles across bytes in a 32-bit word. Expresses swz in the XML directly.
 * To express widen, use the correpsonding replicated form, i.e. H01 = identity
 * for widen = none, H00 for widen = h0, B1111 for widen = b1. For lane, also
 * use the replicated form (interpretation is governed by the opcode). For
 * 8-bit lanes with two channels, use replicated forms for replicated forms
 * (TODO: what about others?). For 8-bit lanes with four channels using
 * matching form (TODO: what about others?).
 */

enum bi_swizzle {
        /* 16-bit swizzle ordering deliberate for fast compute */
        BI_SWIZZLE_H00 = 0, /* = B0101 */
        BI_SWIZZLE_H01 = 1, /* = B0123 = W0 */
        BI_SWIZZLE_H10 = 2, /* = B2301 */
        BI_SWIZZLE_H11 = 3, /* = B2323 */

        /* replication order should be maintained for fast compute */
        BI_SWIZZLE_B0000 = 4, /* single channel (replicate) */
        BI_SWIZZLE_B1111 = 5,
        BI_SWIZZLE_B2222 = 6,
        BI_SWIZZLE_B3333 = 7,

        /* totally special for explicit pattern matching */
        BI_SWIZZLE_B0011 = 8, /* +SWZ.v4i8 */
        BI_SWIZZLE_B2233 = 9, /* +SWZ.v4i8 */
        BI_SWIZZLE_B1032 = 10, /* +SWZ.v4i8 */
        BI_SWIZZLE_B3210 = 11, /* +SWZ.v4i8 */

        BI_SWIZZLE_B0022 = 12, /* for b02 lanes */
};

enum bi_index_type {
        BI_INDEX_NULL = 0,
        BI_INDEX_NORMAL = 1,
        BI_INDEX_REGISTER = 2,
        BI_INDEX_CONSTANT = 3,
        BI_INDEX_PASS = 4,
        BI_INDEX_FAU = 5
};

typedef struct {
        uint32_t value;

        /* modifiers, should only be set if applicable for a given instruction.
         * For *IDP.v4i8, abs plays the role of sign. For bitwise ops where
         * applicable, neg plays the role of not */
        bool abs : 1;
        bool neg : 1;

        enum bi_swizzle swizzle : 4;
        uint32_t offset : 2;
        bool reg : 1;
        enum bi_index_type type : 3;
} bi_index;

static inline bi_index
bi_get_index(unsigned value, bool is_reg, unsigned offset)
{
        return (bi_index) {
                .type = BI_INDEX_NORMAL,
                .value = value,
                .swizzle = BI_SWIZZLE_H01,
                .offset = offset,
                .reg = is_reg,
        };
}

static inline bi_index
bi_register(unsigned reg)
{
        assert(reg < 64);

        return (bi_index) {
                .type = BI_INDEX_REGISTER,
                .swizzle = BI_SWIZZLE_H01,
                .value = reg
        };
}

static inline bi_index
bi_imm_u32(uint32_t imm)
{
        return (bi_index) {
                .type = BI_INDEX_CONSTANT,
                .swizzle = BI_SWIZZLE_H01,
                .value = imm
        };
}

static inline bi_index
bi_imm_f32(float imm)
{
        return bi_imm_u32(fui(imm));
}

static inline bi_index
bi_null()
{
        return (bi_index) { .type = BI_INDEX_NULL };
}

static inline bi_index
bi_zero()
{
        return bi_imm_u32(0);
}

static inline bi_index
bi_passthrough(enum bifrost_packed_src value)
{
        return (bi_index) {
                .type = BI_INDEX_PASS,
                .swizzle = BI_SWIZZLE_H01,
                .value = value
        };
}

/* Extracts a word from a vectored index */
static inline bi_index
bi_word(bi_index idx, unsigned component)
{
        idx.offset += component;
        return idx;
}

/* Helps construct swizzles */
static inline bi_index
bi_half(bi_index idx, bool upper)
{
        assert(idx.swizzle == BI_SWIZZLE_H01);
        idx.swizzle = upper ? BI_SWIZZLE_H11 : BI_SWIZZLE_H00;
        return idx;
}

static inline bi_index
bi_byte(bi_index idx, unsigned lane)
{
        assert(idx.swizzle == BI_SWIZZLE_H01);
        assert(lane < 4);
        idx.swizzle = BI_SWIZZLE_B0000 + lane;
        return idx;
}

static inline bi_index
bi_abs(bi_index idx)
{
        assert(idx.type != BI_INDEX_CONSTANT);
        idx.abs = true;
        return idx;
}

static inline bi_index
bi_neg(bi_index idx)
{
        assert(idx.type != BI_INDEX_CONSTANT);
        idx.neg ^= true;
        return idx;
}

/* For bitwise instructions */
#define bi_not(x) bi_neg(x)

static inline bi_index
bi_imm_u8(uint8_t imm)
{
        return bi_byte(bi_imm_u32(imm), 0);
}

static inline bi_index
bi_imm_u16(uint16_t imm)
{
        return bi_half(bi_imm_u32(imm), false);
}

static inline bool
bi_is_null(bi_index idx)
{
        return idx.type == BI_INDEX_NULL;
}

/* Compares equivalence as references. Does not compare offsets, swizzles, or
 * modifiers. In other words, this forms bi_index equivalence classes by
 * partitioning memory. E.g. -abs(foo[1].yx) == foo.xy but foo != bar */

static inline bool
bi_is_equiv(bi_index left, bi_index right)
{
        return (left.type == right.type) &&
                (left.reg == right.reg) &&
                (left.value == right.value);
}

#define BI_MAX_DESTS 2
#define BI_MAX_SRCS 4

typedef struct {
        /* Must be first */
        struct list_head link;

        /* Link for the use chain */
        struct list_head use;

        enum bi_opcode op;

        /* Data flow */
        bi_index dest[BI_MAX_DESTS];
        bi_index src[BI_MAX_SRCS];

        /* For a branch */
        struct bi_block *branch_target;

        /* These don't fit neatly with anything else.. */
        enum bi_register_format register_format;
        enum bi_vecsize vecsize;

        /* Can we spill the value written here? Used to prevent
         * useless double fills */
        bool no_spill;

        /* Everything after this MUST NOT be accessed directly, since
         * interpretation depends on opcodes */

        /* Destination modifiers */
        union {
                enum bi_clamp clamp;
                bool saturate;
                bool not_result;
        };

        /* Immediates. All seen alone in an instruction, except for varying/texture
         * which are specified jointly for VARTEX */
        union {
                uint32_t shift;
                uint32_t fill;
                uint32_t index;
                uint32_t table;
                uint32_t attribute_index;

                struct {
                        uint32_t varying_index;
                        uint32_t sampler_index;
                        uint32_t texture_index;
                };

                /* TEXC, ATOM_CX: # of staging registers used */
                uint32_t sr_count;
        };

        /* Modifiers specific to particular instructions are thrown in a union */
        union {
                enum bi_adj adj; /* FEXP_TABLE.u4 */
                enum bi_atom_opc atom_opc; /* atomics */
                enum bi_func func; /* FPOW_SC_DET */
                enum bi_function function; /* LD_VAR_FLAT */
                enum bi_mode mode; /* FLOG_TABLE */
                enum bi_mux mux; /* MUX */
                enum bi_precision precision; /* FLOG_TABLE */
                enum bi_sem sem; /* FMAX, FMIN */
                enum bi_source source; /* LD_GCLK */
                bool scale; /* VN_ASST2, FSINCOS_OFFSET */
                bool offset; /* FSIN_TABLE, FOCS_TABLE */
                bool divzero; /* FRSQ_APPROX, FRSQ */
                bool mask; /* CLZ */
                bool threads; /* IMULD, IMOV_FMA */
                bool combine; /* BRANCHC */
                bool format; /* LEA_TEX */

                struct {
                        bool skip; /* VAR_TEX, TEXS, TEXC */
                        bool lod_mode; /* TEXS */
                };

                struct {
                        enum bi_special special; /* FADD_RSCALE, FMA_RSCALE */
                        enum bi_round round; /* FMA, converts, FADD, _RSCALE, etc */
                };

                struct {
                        enum bi_result_type result_type; /* FCMP, ICMP */
                        enum bi_cmpf cmpf; /* CSEL, FCMP, ICMP, BRANCH */
                };

                struct {
                        enum bi_stack_mode stack_mode; /* JUMP_EX */
                        bool test_mode;
                };

                struct {
                        enum bi_seg seg; /* LOAD, STORE, SEG_ADD, SEG_SUB */
                        bool preserve_null; /* SEG_ADD, SEG_SUB */
                        enum bi_extend extend; /* LOAD, IMUL */
                };

                struct {
                        enum bi_sample sample; /* LD_VAR */
                        enum bi_update update; /* LD_VAR */
                        enum bi_varying_name varying_name; /* LD_VAR_SPECIAL */
                };

                struct {
                        enum bi_subgroup subgroup; /* WMASK, CLPER */
                        enum bi_inactive_result inactive_result; /* CLPER */
                        enum bi_lane_op lane_op; /* CLPER */
                };

                struct {
                        bool z; /* ZS_EMIT */
                        bool stencil; /* ZS_EMIT */
                };

                struct {
                        bool h; /* VN_ASST1.f16 */
                        bool l; /* VN_ASST1.f16 */
                };

                struct {
                        bool bytes2; /* RROT_DOUBLE, FRSHIFT_DOUBLE */
                        bool result_word;
                };

                struct {
                        bool sqrt; /* FREXPM */
                        bool log; /* FREXPM */
                };
        };
} bi_instr;

/* Represents the assignment of slots for a given bi_bundle */

typedef struct {
        /* Register to assign to each slot */
        unsigned slot[4];

        /* Read slots can be disabled */
        bool enabled[2];

        /* Configuration for slots 2/3 */
        struct bifrost_reg_ctrl_23 slot23;

        /* Fast-Access-Uniform RAM index */
        uint8_t fau_idx;

        /* Whether writes are actually for the last instruction */
        bool first_instruction;
} bi_registers;

/* A bi_bundle contains two paired instruction pointers. If a slot is unfilled,
 * leave it NULL; the emitter will fill in a nop. Instructions reference
 * registers via slots which are assigned per bundle.
 */

typedef struct {
        uint8_t fau_idx;
        bi_registers regs;
        bi_instruction *fma;
        bi_instruction *add;
} bi_bundle;

struct bi_block;

typedef struct {
        struct list_head link;

        /* Link back up for branch calculations */
        struct bi_block *block;

        /* A clause can have 8 instructions in bundled FMA/ADD sense, so there
         * can be 8 bundles. */

        unsigned bundle_count;
        bi_bundle bundles[8];

        /* For scoreboarding -- the clause ID (this is not globally unique!)
         * and its dependencies in terms of other clauses, computed during
         * scheduling and used when emitting code. Dependencies expressed as a
         * bitfield matching the hardware, except shifted by a clause (the
         * shift back to the ISA's off-by-one encoding is worked out when
         * emitting clauses) */
        unsigned scoreboard_id;
        uint8_t dependencies;

        /* See ISA header for description */
        enum bifrost_flow flow_control;

        /* Can we prefetch the next clause? Usually it makes sense, except for
         * clauses ending in unconditional branches */
        bool next_clause_prefetch;

        /* Assigned data register */
        unsigned staging_register;

        /* Corresponds to the usual bit but shifted by a clause */
        bool staging_barrier;

        /* Constants read by this clause. ISA limit. Must satisfy:
         *
         *      constant_count + bundle_count <= 13
         *
         * Also implicitly constant_count <= bundle_count since a bundle only
         * reads a single constant.
         */
        uint64_t constants[8];
        unsigned constant_count;

        /* Branches encode a constant offset relative to the program counter
         * with some magic flags. By convention, if there is a branch, its
         * constant will be last. Set this flag to indicate this is required.
         */
        bool branch_constant;

        /* What type of high latency instruction is here, basically */
        unsigned message_type;
} bi_clause;

typedef struct bi_block {
        pan_block base; /* must be first */

        /* If true, uses clauses; if false, uses instructions */
        bool scheduled;
        struct list_head clauses; /* list of bi_clause */
} bi_block;

typedef struct {
       nir_shader *nir;
       gl_shader_stage stage;
       struct list_head blocks; /* list of bi_block */
       struct panfrost_sysvals sysvals;
       uint32_t quirks;
       unsigned arch;
       unsigned tls_size;

       /* Is internally a blend shader? Depends on stage == FRAGMENT */
       bool is_blend;

       /* Blend constants */
       float blend_constants[4];

       /* Blend return offsets */
       uint32_t blend_ret_offsets[8];

       /* Blend tile buffer conversion desc */
       uint64_t blend_desc;

       /* During NIR->BIR */
       nir_function_impl *impl;
       bi_block *current_block;
       bi_block *after_block;
       bi_block *break_block;
       bi_block *continue_block;
       bool emitted_atest;
       nir_alu_type *blend_types;

       /* For creating temporaries */
       unsigned temp_alloc;

       /* Analysis results */
       bool has_liveness;

       /* Stats for shader-db */
       unsigned instruction_count;
       unsigned loop_count;
       unsigned spills;
       unsigned fills;
} bi_context;

static inline bi_instruction *
bi_emit(bi_context *ctx, bi_instruction ins)
{
        bi_instruction *u = rzalloc(ctx, bi_instruction);
        memcpy(u, &ins, sizeof(ins));
        list_addtail(&u->link, &ctx->current_block->base.instructions);
        return u;
}

static inline bi_instruction *
bi_emit_before(bi_context *ctx, bi_instruction *tag, bi_instruction ins)
{
        bi_instruction *u = rzalloc(ctx, bi_instruction);
        memcpy(u, &ins, sizeof(ins));
        list_addtail(&u->link, &tag->link);
        return u;
}

static inline void
bi_remove_instruction(bi_instruction *ins)
{
        list_del(&ins->link);
}

/* If high bits are set, instead of SSA/registers, we have specials indexed by
 * the low bits if necessary.
 *
 *  Fixed register: do not allocate register, do not collect $200.
 *  Uniform: access a uniform register given by low bits.
 *  Constant: access the specified constant (specifies a bit offset / shift)
 *  Zero: special cased to avoid wasting a constant
 *  Passthrough: a bifrost_packed_src to passthrough T/T0/T1
 */

#define BIR_INDEX_REGISTER (1 << 31)
#define BIR_INDEX_CONSTANT (1 << 30)
#define BIR_INDEX_PASS     (1 << 29)
#define BIR_INDEX_FAU      (1 << 28)

/* Shift everything away */
#define BIR_INDEX_ZERO (BIR_INDEX_CONSTANT | 64)

enum bir_fau {
        BIR_FAU_ZERO = 0,
        BIR_FAU_LANE_ID = 1,
        BIR_FAU_WRAP_ID = 2,
        BIR_FAU_CORE_ID = 3,
        BIR_FAU_FB_EXTENT = 4,
        BIR_FAU_ATEST_PARAM = 5,
        BIR_FAU_SAMPLE_POS_ARRAY = 6,
        BIR_FAU_BLEND_0 = 8,
        /* blend descs 1 - 7 */
        BIR_FAU_TYPE_MASK = 15,
        BIR_FAU_UNIFORM = (1 << 7),
        BIR_FAU_HI = (1 << 8),
};

static inline bi_index
bi_fau(enum bir_fau value, bool hi)
{
        return (bi_index) {
                .type = BI_INDEX_FAU,
                .value = value,
                .swizzle = BI_SWIZZLE_H01,
                .offset = hi ? 1 : 0
        };
}

/* Keep me synced please so we can check src & BIR_SPECIAL */

#define BIR_SPECIAL        (BIR_INDEX_REGISTER | BIR_INDEX_CONSTANT | \
                            BIR_INDEX_PASS | BIR_INDEX_FAU)

static inline unsigned
bi_max_temp(bi_context *ctx)
{
        unsigned alloc = MAX2(ctx->impl->reg_alloc, ctx->impl->ssa_alloc);
        return ((alloc + 2 + ctx->temp_alloc) << 1);
}

static inline bi_index
bi_temp(bi_context *ctx)
{
        unsigned alloc = (ctx->impl->ssa_alloc + ctx->temp_alloc++);
        return bi_get_index(alloc, false, 0);
}

static inline bi_index
bi_temp_reg(bi_context *ctx)
{
        unsigned alloc = (ctx->impl->reg_alloc + ctx->temp_alloc++);
        return bi_get_index(alloc, true, 0);
}

static inline unsigned
bi_make_temp(bi_context *ctx)
{
        return (ctx->impl->ssa_alloc + 1 + ctx->temp_alloc++) << 1;
}

static inline unsigned
bi_make_temp_reg(bi_context *ctx)
{
        return ((ctx->impl->reg_alloc + ctx->temp_alloc++) << 1) | PAN_IS_REG;
}

/* Inline constants automatically, will be lowered out by bi_lower_fau where a
 * constant is not allowed. load_const_to_scalar gaurantees that this makes
 * sense */

static inline bi_index
bi_src_index(nir_src *src)
{
        if (nir_src_is_const(*src))
                return bi_imm_u32(nir_src_as_uint(*src));
        else if (src->is_ssa)
                return bi_get_index(src->ssa->index, false, 0);
        else {
                assert(!src->reg.indirect);
                return bi_get_index(src->reg.reg->index, true, 0);
        }
}

static inline bi_index
bi_dest_index(nir_dest *dst)
{
        if (dst->is_ssa)
                return bi_get_index(dst->ssa.index, false, 0);
        else {
                assert(!dst->reg.indirect);
                return bi_get_index(dst->reg.reg->index, true, 0);
        }
}

static inline unsigned
bi_get_node(bi_index index)
{
        if (bi_is_null(index) || index.type != BI_INDEX_NORMAL)
                return ~0;
        else
                return (index.value << 1) | index.reg;
}

static inline bi_index
bi_node_to_index(unsigned node, unsigned node_count)
{
        assert(node < node_count);
        assert(node_count < ~0);

        return bi_get_index(node >> 1, node & PAN_IS_REG, 0);
}

/* Iterators for Bifrost IR */

#define bi_foreach_block(ctx, v) \
        list_for_each_entry(pan_block, v, &ctx->blocks, link)

#define bi_foreach_block_from(ctx, from, v) \
        list_for_each_entry_from(pan_block, v, from, &ctx->blocks, link)

#define bi_foreach_block_from_rev(ctx, from, v) \
        list_for_each_entry_from_rev(pan_block, v, from, &ctx->blocks, link)

#define bi_foreach_instr_in_block(block, v) \
        list_for_each_entry(bi_instruction, v, &(block)->base.instructions, link)

#define bi_foreach_instr_in_block_rev(block, v) \
        list_for_each_entry_rev(bi_instruction, v, &(block)->base.instructions, link)

#define bi_foreach_instr_in_block_safe(block, v) \
        list_for_each_entry_safe(bi_instruction, v, &(block)->base.instructions, link)

#define bi_foreach_instr_in_block_safe_rev(block, v) \
        list_for_each_entry_safe_rev(bi_instruction, v, &(block)->base.instructions, link)

#define bi_foreach_instr_in_block_from(block, v, from) \
        list_for_each_entry_from(bi_instruction, v, from, &(block)->base.instructions, link)

#define bi_foreach_instr_in_block_from_rev(block, v, from) \
        list_for_each_entry_from_rev(bi_instruction, v, from, &(block)->base.instructions, link)

#define bi_foreach_clause_in_block(block, v) \
        list_for_each_entry(bi_clause, v, &(block)->clauses, link)

#define bi_foreach_clause_in_block_safe(block, v) \
        list_for_each_entry_safe(bi_clause, v, &(block)->clauses, link)

#define bi_foreach_clause_in_block_from(block, v, from) \
        list_for_each_entry_from(bi_clause, v, from, &(block)->clauses, link)

#define bi_foreach_clause_in_block_from_rev(block, v, from) \
        list_for_each_entry_from_rev(bi_clause, v, from, &(block)->clauses, link)

#define bi_foreach_instr_global(ctx, v) \
        bi_foreach_block(ctx, v_block) \
                bi_foreach_instr_in_block((bi_block *) v_block, v)

#define bi_foreach_instr_global_safe(ctx, v) \
        bi_foreach_block(ctx, v_block) \
                bi_foreach_instr_in_block_safe((bi_block *) v_block, v)

/* Based on set_foreach, expanded with automatic type casts */

#define bi_foreach_predecessor(blk, v) \
        struct set_entry *_entry_##v; \
        bi_block *v; \
        for (_entry_##v = _mesa_set_next_entry(blk->base.predecessors, NULL), \
                v = (bi_block *) (_entry_##v ? _entry_##v->key : NULL);  \
                _entry_##v != NULL; \
                _entry_##v = _mesa_set_next_entry(blk->base.predecessors, _entry_##v), \
                v = (bi_block *) (_entry_##v ? _entry_##v->key : NULL))

#define bi_foreach_src(ins, v) \
        for (unsigned v = 0; v < ARRAY_SIZE(ins->src); ++v)

static inline bi_instruction *
bi_prev_op(bi_instruction *ins)
{
        return list_last_entry(&(ins->link), bi_instruction, link);
}

static inline bi_instruction *
bi_next_op(bi_instruction *ins)
{
        return list_first_entry(&(ins->link), bi_instruction, link);
}

static inline pan_block *
pan_next_block(pan_block *block)
{
        return list_first_entry(&(block->link), pan_block, link);
}

/* BIR manipulation */

bool bi_has_arg(bi_instr *ins, bi_index arg);
uint16_t bi_bytemask_of_read_components(bi_instr *ins, bi_index node);
unsigned bi_writemask(bi_instr *ins);

void bi_print_instr(bi_instr *I, FILE *fp);
void bi_print_slots(bi_registers *regs, FILE *fp);
void bi_print_bundle(bi_bundle *bundle, FILE *fp);
void bi_print_clause(bi_clause *clause, FILE *fp);
void bi_print_block(bi_block *block, FILE *fp);
void bi_print_shader(bi_context *ctx, FILE *fp);

/* BIR passes */

bool bi_opt_dead_code_eliminate(bi_context *ctx, bi_block *block);
void bi_schedule(bi_context *ctx);
void bi_register_allocate(bi_context *ctx);

bi_clause *
bi_singleton(void *memctx, bi_instr *ins,
                bi_block *block,
                unsigned scoreboard_id,
                unsigned dependencies,
                bool osrb);

/* Liveness */

void bi_compute_liveness(bi_context *ctx);
void bi_liveness_ins_update(uint16_t *live, bi_instr *ins, unsigned max);
void bi_invalidate_liveness(bi_context *ctx);

/* Layout */

bool bi_can_insert_bundle(bi_clause *clause, bool constant);
unsigned bi_clause_quadwords(bi_clause *clause);
signed bi_block_offset(bi_context *ctx, bi_clause *start, bi_block *target);

/* Code emit */

void bi_pack(bi_context *ctx, struct util_dynarray *emission);

unsigned bi_pack_fma(bi_instr *I,
                enum bifrost_packed_src src0,
                enum bifrost_packed_src src1,
                enum bifrost_packed_src src2,
                enum bifrost_packed_src src3);
unsigned bi_pack_add(bi_instr *I,
                enum bifrost_packed_src src0,
                enum bifrost_packed_src src1,
                enum bifrost_packed_src src2,
                enum bifrost_packed_src src3);

/* Like in NIR, for use with the builder */

enum bi_cursor_option {
    bi_cursor_after_block,
    bi_cursor_before_instr,
    bi_cursor_after_instr
};

typedef struct {
    enum bi_cursor_option option;

    union {
        bi_block *block;
        bi_instr *instr;
    };
} bi_cursor;

static inline bi_cursor
bi_after_block(bi_block *block)
{
    return (bi_cursor) {
        .option = bi_cursor_after_block,
        .block = block
    };
}

static inline bi_cursor
bi_before_instr(bi_instr *instr)
{
    return (bi_cursor) {
        .option = bi_cursor_before_instr,
        .instr = instr
    };
}

static inline bi_cursor
bi_after_instr(bi_instr *instr)
{
    return (bi_cursor) {
        .option = bi_cursor_after_instr,
        .instr = instr
    };
}

/* IR builder in terms of cursor infrastructure */

typedef struct {
    bi_context *shader;
    bi_cursor cursor;
} bi_builder;

/* Insert an instruction at the cursor and move the cursor */

static inline void
bi_builder_insert(bi_cursor *cursor, bi_instr *I)
{
    switch (cursor->option) {
    case bi_cursor_after_instr:
        list_add(&I->link, &cursor->instr->link);
        cursor->instr = I;
        return;

    case bi_cursor_after_block:
        list_addtail(&I->link, &cursor->block->base.instructions);
        cursor->option = bi_cursor_after_instr;
        cursor->instr = I;
        return;

    case bi_cursor_before_instr:
        list_addtail(&I->link, &cursor->instr->link);
        cursor->option = bi_cursor_after_instr;
        cursor->instr = I;
        return;
    }

    unreachable("Invalid cursor option");
}

#endif
