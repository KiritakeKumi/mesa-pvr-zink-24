/*
 * Copyright (C) 2021 Icecream95
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

/* A nodearray is an array type that is either sparse or dense, depending on
 * the number of elements.
 *
 * When the number of elements is over a threshold (max_sparse), the dense
 * mode is used, and the nodearray is simply a container for an array with an
 * 8-bit element per node.
 *
 * In sparse mode, the array has 32-bit elements, with a 24-bit node index
 * and an 8-bit value. The nodes are always sorted, so that a binary search
 * can be used to find elements. Nonexistent elements are treated as zero.
 *
 * Function names follow ARM instruction names: orr does *elem |= value.
 *
 * Although it's probably already fast enough, the datastructure could be sped
 * up a lot, especially when NEON is available, by making the sparse mode
 * store sixteen adjacent values, so that adding new keys also allocates
 * nearby keys, and to allow for vectorising iteration, as can be done when in
 * the dense mode.
 */

#ifndef __BIFROST_NODEARRAY_H
#define __BIFROST_NODEARRAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
// Defined in compiler.h
typedef struct {
        union {
                uint64_t *sparse;
                uint16_t *dense;
        }
        unsigned size; // either 64-bit or 16-bit elements
        unsigned sparse_capacity;
} nodearray;
*/

/* Align sizes to 16-bytes for SIMD purposes */
#define NODEARRAY_DENSE_ALIGN(x) ALIGN_POT(x, 16)

#define nodearray_sparse_foreach(buf, elem) \
   for (uint64_t *elem = (buf)->sparse; \
        elem < (buf)->sparse + (buf)->size; elem++)

#define nodearray_dense_foreach(buf, elem) \
   for (uint16_t *elem = (buf)->dense; \
        elem < (buf)->dense + (buf)->size; elem++)

#define nodearray_dense_foreach_64(buf, elem) \
   for (uint64_t *elem = (uint64_t *)(buf)->dense; \
        (uint16_t *)elem < (buf)->dense + (buf)->size; elem++)

static inline bool
nodearray_sparse(const nodearray *a)
{
        return a->sparse_capacity != ~0U;
}

static inline void
nodearray_init(nodearray *a)
{
        memset(a, 0, sizeof(nodearray));
}

static inline void
nodearray_reset(nodearray *a)
{
        free(a->sparse);
        nodearray_init(a);
}

static inline uint64_t
nodearray_encode(unsigned key, uint16_t value)
{
        return ((uint64_t) key << 16) | value;
}

static inline unsigned
nodearray_key(const uint64_t *elem)
{
        return *elem >> 16;
}

static inline uint16_t
nodearray_value(const uint64_t *elem)
{
        return *elem & 0xffff;
}

static inline unsigned
nodearray_sparse_search(const nodearray *a, uint64_t key, uint64_t **elem)
{
        assert(nodearray_sparse(a) && a->size);

        uint64_t *data = a->sparse;

        /* Encode the key using the highest possible value, so that the
         * matching node must be encoded lower than this */
        uint64_t skey = nodearray_encode(key, 0xffff);

        unsigned left = 0;
        unsigned right = a->size - 1;

        if (data[right] <= skey)
                left = right;

        while (left != right) {
                /* No need to worry about overflow, we couldn't have more than
                 * 2^24 elements */
                unsigned probe = (left + right + 1) / 2;

                if (data[probe] > skey)
                        right = probe - 1;
                else
                        left = probe;
        }

        *elem = data + left;
        return left;
}

static inline void
nodearray_orr(nodearray *a, unsigned key, uint16_t value,
              unsigned max_sparse, unsigned max)
{
        assert(key < (1 << 24));
        assert(key < max);

        if (!value)
                return;

        if (nodearray_sparse(a)) {
                unsigned size = a->size;

                unsigned left = 0;

                if (size) {
                        /* First, binary search for key */
                        uint64_t *elem;
                        left = nodearray_sparse_search(a, key, &elem);

                        if (nodearray_key(elem) == key) {
                                *elem |= value;
                                return;
                        }

                        /* We insert before `left`, so increment it if it's
                         * out of order */
                        if (nodearray_key(elem) < key)
                                ++left;
                }

                if (size < max_sparse && (size + 1) < max / 4) {
                        /* We didn't find it, but we know where to insert it. */

                        uint64_t *data = a->sparse;
                        uint64_t *data_move = data + left;

                        bool realloc = (++a->size) > a->sparse_capacity;

                        if (realloc) {
                                a->sparse_capacity = MIN2(MAX2(a->sparse_capacity * 2, 64), max / 4);

                                a->sparse = (uint64_t *)malloc(a->sparse_capacity * sizeof(uint64_t));

                                if (left)
                                        memcpy(a->sparse, data, left * sizeof(uint64_t));
                        }

                        uint64_t *elem = a->sparse + left;

                        if (left != size)
                                memmove(elem + 1, data_move, (size - left) * sizeof(uint64_t));

                        *elem = nodearray_encode(key, value);

                        if (realloc)
                                free(data);

                        return;
                }

                /* There are too many elements, so convert to a dense array */
                nodearray old = *a;

                a->dense = (uint16_t *)calloc(NODEARRAY_DENSE_ALIGN(max), sizeof(uint16_t));
                a->size = max;
                a->sparse_capacity = ~0U;

                uint16_t *data = a->dense;

                nodearray_sparse_foreach(&old, x) {
                        unsigned key = nodearray_key(x);
                        uint16_t value = nodearray_value(x);

                        assert(key < max);
                        data[key] = value;
                }

                free(old.sparse);
        }

        a->dense[key] |= value;
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
