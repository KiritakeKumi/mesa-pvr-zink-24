/*
 * Copyright © 2020 Mike Blumenkrantz
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
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#ifndef ZINK_DESCRIPTOR_H
# define  ZINK_DESCRIPTOR_H
#include <vulkan/vulkan.h>
#include "util/u_dynarray.h"
#include "util/u_inlines.h"
#include "util/simple_mtx.h"

#include "zink_batch.h"

#ifndef ZINK_SHADER_COUNT
#define ZINK_SHADER_COUNT (PIPE_SHADER_TYPES - 1)
#endif

enum zink_descriptor_type {
   ZINK_DESCRIPTOR_TYPE_UBO,
   ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW,
   ZINK_DESCRIPTOR_TYPE_SSBO,
   ZINK_DESCRIPTOR_TYPE_IMAGE,
   ZINK_DESCRIPTOR_TYPES,
};


struct zink_descriptor_refs {
   struct util_dynarray refs;
};


/* hashes of all the named types in a given state */
struct zink_descriptor_state {
   bool valid[ZINK_DESCRIPTOR_TYPES];
   uint32_t state[ZINK_DESCRIPTOR_TYPES];
};

struct hash_table;

struct zink_context;
struct zink_image_view;
struct zink_program;
struct zink_resource;
struct zink_sampler;
struct zink_sampler_view;
struct zink_shader;
struct zink_screen;


struct zink_descriptor_state_key {
   bool exists[ZINK_SHADER_COUNT];
   uint32_t state[ZINK_SHADER_COUNT];
};

struct zink_descriptor_layout_key {
   unsigned num_descriptors;
   VkDescriptorSetLayoutBinding *bindings;
};

struct zink_descriptor_pool_key {
   struct zink_descriptor_layout_key *layout;
   unsigned num_type_sizes;
   VkDescriptorPoolSize *sizes;
};

struct zink_descriptor_reference {
   void **ref;
   bool *invalid;
};

bool
zink_descriptor_layouts_init(struct zink_context *ctx);

void
zink_descriptor_layouts_deinit(struct zink_context *ctx);

uint32_t
zink_get_sampler_view_hash(struct zink_context *ctx, struct zink_sampler_view *sampler_view, bool is_buffer);
uint32_t
zink_get_image_view_hash(struct zink_context *ctx, struct zink_image_view *image_view, bool is_buffer);
bool
zink_descriptor_util_alloc_sets(struct zink_screen *screen, VkDescriptorSetLayout dsl, VkDescriptorPool pool, VkDescriptorSet *sets, unsigned num_sets);
VkDescriptorSetLayout
zink_descriptor_util_layout_get(struct zink_context *ctx, enum zink_descriptor_type type,
                      VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
                      struct zink_descriptor_layout_key **layout_key);
bool
zink_descriptor_util_push_layouts_get(struct zink_context *ctx, VkDescriptorSetLayout *dsls, struct zink_descriptor_layout_key **layout_keys);
struct zink_resource *
zink_get_resource_for_descriptor(struct zink_context *ctx, enum zink_descriptor_type type, enum pipe_shader_type shader, int idx);

/* these two can't be called in lazy mode */
void
zink_descriptor_set_refs_clear(struct zink_descriptor_refs *refs, void *ptr);
void
zink_descriptor_set_recycle(struct zink_descriptor_set *zds);





bool
zink_descriptor_program_init(struct zink_context *ctx, struct zink_program *pg);

void
zink_descriptor_program_deinit(struct zink_screen *screen, struct zink_program *pg);

void
zink_descriptors_update(struct zink_context *ctx, bool is_compute);


void
zink_context_invalidate_descriptor_state(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned, unsigned);

uint32_t
zink_get_sampler_view_hash(struct zink_context *ctx, struct zink_sampler_view *sampler_view, bool is_buffer);
uint32_t
zink_get_image_view_hash(struct zink_context *ctx, struct zink_image_view *image_view, bool is_buffer);
struct zink_resource *
zink_get_resource_for_descriptor(struct zink_context *ctx, enum zink_descriptor_type type, enum pipe_shader_type shader, int idx);

void
zink_batch_descriptor_deinit(struct zink_screen *screen, struct zink_batch_state *bs);
void
zink_batch_descriptor_reset(struct zink_screen *screen, struct zink_batch_state *bs);
bool
zink_batch_descriptor_init(struct zink_screen *screen, struct zink_batch_state *bs);

bool
zink_descriptors_init(struct zink_context *ctx);

void
zink_descriptors_deinit(struct zink_context *ctx);

//LAZY
bool
zink_descriptor_program_init_lazy(struct zink_context *ctx, struct zink_program *pg);

void
zink_descriptor_program_deinit_lazy(struct zink_screen *screen, struct zink_program *pg);

void
zink_descriptors_update_lazy(struct zink_context *ctx, bool is_compute);


void
zink_context_invalidate_descriptor_state_lazy(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned, unsigned);

void
zink_batch_descriptor_deinit_lazy(struct zink_screen *screen, struct zink_batch_state *bs);
void
zink_batch_descriptor_reset_lazy(struct zink_screen *screen, struct zink_batch_state *bs);
bool
zink_batch_descriptor_init_lazy(struct zink_screen *screen, struct zink_batch_state *bs);

bool
zink_descriptors_init_lazy(struct zink_context *ctx);

void
zink_descriptors_deinit_lazy(struct zink_context *ctx);

#endif
