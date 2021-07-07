/*
 * Copyright (C) 2018 Alyssa Rosenzweig
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
 */

#ifndef __PAN_CMDSTREAM_H__
#define __PAN_CMDSTREAM_H__

#include "pipe/p_defines.h"
#include "pipe/p_state.h"

#include "midgard_pack.h"

#include "pan_job.h"

static inline enum mali_sample_pattern
panfrost_sample_pattern(unsigned samples)
{
        switch (samples) {
        case 1:  return MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
        case 4:  return MALI_SAMPLE_PATTERN_ROTATED_4X_GRID;
        case 8:  return MALI_SAMPLE_PATTERN_D3D_8X_GRID;
        case 16: return MALI_SAMPLE_PATTERN_D3D_16X_GRID;
        default: unreachable("Unsupported sample count");
        }
}

#endif /* __PAN_CMDSTREAM_H__ */
