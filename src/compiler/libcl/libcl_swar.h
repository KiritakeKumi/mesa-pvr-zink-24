/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "libcl.h"

/*
 * On some ISAs, 32-bit operations are the same performance as 16-bit. It is
 * still possible to improve performance of 16-bit vectors using
 * SIMD-within-a-register techniques. It is challenging to autovectorize this
 * class of code as there are often subtle correctness invariants that the
 * compiler cannot statically prove. The helpers in this file therefore add
 * helpers for all vectorizable operations, documenting their preconditions, and
 * asserting the preconditions hold in debug build for additional security.
 */

/* Bitwise operations can always be vectorized */
static inline ushort2
swar_not(ushort2 a)
{
   return as_ushort2(~as_uint(a));
}

#define DEFINE_BINOP(name, op)                                                 \
   static inline ushort2 swar_##name(ushort2 a, ushort2 b)                     \
   {                                                                           \
      return as_ushort2(as_uint(a) op as_uint(b));                             \
   }

DEFINE_BINOP(and, &)
DEFINE_BINOP(or, |)
DEFINE_BINOP(xor, ^)

#undef DEFINE_BINOP

/* Addition vectorizes if the bottom elements do not overflow. */
static inline ushort2
swar_add(ushort2 a, ushort2 b)
{
   assert((a.x + b.x) >= a.x && "SWAR addition must not overflow");
   return as_ushort2(as_uint(a) + as_uint(b));
}

/* Subtraction vectorizes if no elements overflow. */
static inline ushort2
swar_sub(ushort2 a, ushort2 b)
{
   assert(all(a >= b) && "SWAR subtraction must not overflow");
   return as_ushort2(as_uint(a) - as_uint(b));
}

/* Shifts and multiplies vectorize if bottom elements do no elements overflow.
 * Additionally, the shift must be the same for all elements.
 */
static inline ushort2
swar_shl(ushort2 a, uint shift)
{
   assert(((a.x << shift) >> shift) && "SWAR left-shift must not overflow");
   return as_ushort2(as_uint(a) << shift);
}

static inline ushort2
swar_mul(ushort2 a, uint mul)
{
   assert((((uint)a.x) * mul) <= 0xFFFF && "SWAR multiplies must not overflow");
   return as_ushort2(as_uint(a) * mul);
}

/* Right-shifts vectorize if the top elements does not overflow.
 * Additionally, the shift must be the same for all elements.
 */
static inline ushort2
swar_shr(ushort2 a, uint shift)
{
   assert(((a.y >> shift) << shift) && "SWAR right-shift must not overflow");
   return as_ushort2(as_uint(a) >> shift);
}
