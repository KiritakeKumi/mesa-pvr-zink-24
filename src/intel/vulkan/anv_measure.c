/*
 * Copyright © 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "anv_measure.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common/intel_measure.h"
#include "util/debug.h"

struct anv_measure_batch {
   struct anv_bo *bo;
   struct list_head link;
   struct intel_measure_batch base;
};

void
anv_measure_device_init(struct anv_physical_device *device)
{
   switch (device->info.gen) {
   case 12:
      if (gen_device_info_is_12hp(&device->info))
         device->cmd_emit_timestamp = &gen125_cmd_emit_timestamp;
      else
         device->cmd_emit_timestamp = &gen12_cmd_emit_timestamp;
      break;
   case 11:
      device->cmd_emit_timestamp = &gen11_cmd_emit_timestamp;
      break;
   case 9:
      device->cmd_emit_timestamp = &gen9_cmd_emit_timestamp;
      break;
   case 8:
      device->cmd_emit_timestamp = &gen8_cmd_emit_timestamp;
      break;
   case 7:
      if (device->info.is_haswell)
         device->cmd_emit_timestamp = &gen75_cmd_emit_timestamp;
      else
         device->cmd_emit_timestamp = &gen7_cmd_emit_timestamp;
      break;
   default:
      assert(false);
   }

   /* initialise list of measure structures that await rendering */
   struct intel_measure_device *measure_device = &device->measure_device;
   pthread_mutex_init(&measure_device->mutex, NULL);
   list_inithead(&measure_device->queued_snapshots);

   measure_device->frame = 0;

   intel_measure_init(measure_device);
   struct intel_measure_config *config = measure_device->config;
   if (config == NULL)
      return;

   /* the final member of intel_measure_ringbuffer is a zero-length array of
    * intel_measure_buffered_result objects.  Allocate additional space for
    * the buffered objects based on the run-time configurable buffer_size
    */
   const size_t rb_bytes = sizeof(struct intel_measure_ringbuffer) +
      config->buffer_size * sizeof(struct intel_measure_buffered_result);
   struct intel_measure_ringbuffer * rb =
      vk_zalloc(&device->instance->vk.alloc,
                rb_bytes, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   measure_device->ringbuffer = rb;
}

static struct intel_measure_config*
config_from_command_buffer(struct anv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->device->physical->measure_device.config;
}

void
anv_measure_init(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_device *device = cmd_buffer->device;

   if (!config || !config->enabled) {
      cmd_buffer->measure = NULL;
      return;
   }

   /* the final member of anv_measure is a zero-length array of
    * intel_measure_snapshot objects.  Create additional space for the
    * snapshot objects based on the run-time configurable batch_size
    */
   const size_t batch_bytes = sizeof(struct anv_measure_batch) +
      config->batch_size * sizeof(struct intel_measure_snapshot);
   struct anv_measure_batch * measure =
      vk_alloc(&cmd_buffer->pool->alloc,
               batch_bytes, 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   memset(measure, 0, batch_bytes);
   VkResult result =
      anv_device_alloc_bo(device,
                          config->batch_size * sizeof(uint64_t),
                          ANV_BO_ALLOC_MAPPED,
                          0,
                          &measure->bo);
   assert(result == VK_SUCCESS);
   cmd_buffer->measure = measure;
}

static bool
anv_measure_ready(struct anv_device *device,
                  struct anv_measure_batch *measure)
{
   /* anv_device_bo_busy returns VK_NOT_READY if the bo is busy */
    return(VK_SUCCESS == anv_device_bo_busy(device, measure->bo));
}

/**
 * Collect snapshots from completed command buffers and submit them to
 * intel_measure for printing.
 */
static void
anv_measure_gather(struct anv_device *device)
{
   struct intel_measure_device *measure_device = &device->physical->measure_device;

   pthread_mutex_lock(&measure_device->mutex);

   /* iterate snapshots and collect if ready */
   while (!list_is_empty(&measure_device->queued_snapshots)) {
      struct anv_measure_batch *measure =
         list_first_entry(&measure_device->queued_snapshots,
                          struct anv_measure_batch, link);

      if (!anv_measure_ready(device, measure)) {
         /* command buffer has begun execution on the gpu, but has not
          * completed.
          */
         break;
      }

      uint64_t *map = anv_gem_mmap(device, measure->bo->gem_handle, 0,
                                   measure->base.index * sizeof(uint64_t), 0);

      if (map[0] == 0) {
         /* The first timestamp is still zero.  The Command buffer has not
          * begun execution on the gpu.  It was recently submitted, perhaps by
          * another thread.
          */
         anv_gem_munmap(device, map, measure->base.index * sizeof(uint64_t));
         break;
      }

      list_del(&measure->link);
      assert(measure->bo);
      assert(measure->base.index % 2 == 0);

      intel_measure_push_result(measure_device, &measure->base, map);

      anv_gem_munmap(device, map, measure->base.index * sizeof(uint64_t));
      measure->base.index = 0;
      measure->base.frame = 0;
   }

   intel_measure_print(measure_device, &device->info);
   pthread_mutex_unlock(&measure_device->mutex);
}

static void
anv_measure_start_snapshot(struct anv_cmd_buffer *cmd_buffer,
                           enum intel_measure_snapshot_type type,
                           const char *event_name,
                           uint32_t count)
{
   struct anv_batch *batch = &cmd_buffer->batch;
   struct anv_measure_batch *measure = cmd_buffer->measure;
   struct anv_physical_device *device = cmd_buffer->device->physical;
   struct intel_measure_device *measure_device = &device->measure_device;

   const unsigned device_frame = measure_device->frame;

   /* if the command buffer is not associated with a frame, associate it with
    * the most recent acquired frame
    */
   if (measure->base.frame == 0)
      measure->base.frame = device_frame;

   uintptr_t framebuffer = (uintptr_t)cmd_buffer->state.framebuffer;

   /* verify framebuffer has been properly tracked */
   assert(type == INTEL_SNAPSHOT_END ||
          framebuffer == measure->base.framebuffer ||
          framebuffer == 0 ); /* compute has no framebuffer */

   unsigned index = measure->base.index++;

   (*device->cmd_emit_timestamp)(batch, measure->bo, index * sizeof(uint64_t));

   if (event_name == NULL)
      event_name = intel_measure_snapshot_string(type);

   struct intel_measure_snapshot *snapshot = &(measure->base.snapshots[index]);
   memset(snapshot, 0, sizeof(*snapshot));
   snapshot->type = type;
   snapshot->count = (unsigned) count;
   snapshot->event_count = measure->base.event_count;
   snapshot->event_name = event_name;
   snapshot->framebuffer = framebuffer;

   if (type == INTEL_SNAPSHOT_COMPUTE && cmd_buffer->state.compute.pipeline) {
      snapshot->cs = (uintptr_t) cmd_buffer->state.compute.pipeline->cs;
   } else if (cmd_buffer->state.gfx.pipeline) {
      const struct anv_graphics_pipeline *pipeline =
         cmd_buffer->state.gfx.pipeline;
      snapshot->vs = (uintptr_t) pipeline->shaders[MESA_SHADER_VERTEX];
      snapshot->tcs = (uintptr_t) pipeline->shaders[MESA_SHADER_TESS_CTRL];
      snapshot->tes = (uintptr_t) pipeline->shaders[MESA_SHADER_TESS_EVAL];
      snapshot->gs = (uintptr_t) pipeline->shaders[MESA_SHADER_GEOMETRY];
      snapshot->fs = (uintptr_t) pipeline->shaders[MESA_SHADER_FRAGMENT];
   }
}

static void
anv_measure_end_snapshot(struct anv_cmd_buffer *cmd_buffer,
                         uint32_t event_count)
{
   struct anv_batch *batch = &cmd_buffer->batch;
   struct anv_measure_batch *measure = cmd_buffer->measure;
   struct anv_physical_device *device = cmd_buffer->device->physical;

   unsigned index = measure->base.index++;
   assert(index % 2 == 1);

   (*device->cmd_emit_timestamp)(batch, measure->bo, index * sizeof(uint64_t));

   struct intel_measure_snapshot *snapshot = &(measure->base.snapshots[index]);
   memset(snapshot, 0, sizeof(*snapshot));
   snapshot->type = INTEL_SNAPSHOT_END;
   snapshot->event_count = event_count;
}

static bool
state_changed(struct anv_cmd_buffer *cmd_buffer,
              enum intel_measure_snapshot_type type)
{
   uintptr_t vs=0, tcs=0, tes=0, gs=0, fs=0, cs=0;
   if (type == INTEL_SNAPSHOT_COMPUTE) {
      const struct anv_compute_pipeline *cs_pipe =
         cmd_buffer->state.compute.pipeline;
      assert(cs_pipe);
      cs = (uintptr_t)cs_pipe->cs;
   } else if (type == INTEL_SNAPSHOT_DRAW) {
      const struct anv_graphics_pipeline *gfx = cmd_buffer->state.gfx.pipeline;
      assert(gfx);
      vs = (uintptr_t) gfx->shaders[MESA_SHADER_VERTEX];
      tcs = (uintptr_t) gfx->shaders[MESA_SHADER_TESS_CTRL];
      tes = (uintptr_t) gfx->shaders[MESA_SHADER_TESS_EVAL];
      gs = (uintptr_t) gfx->shaders[MESA_SHADER_GEOMETRY];
      fs = (uintptr_t) gfx->shaders[MESA_SHADER_FRAGMENT];
   }
   /* else blorp, all programs NULL */

   return intel_measure_state_changed(&cmd_buffer->measure->base,
                                      vs, tcs, tes, gs, fs, cs);
}

void
_anv_measure_snapshot(struct anv_cmd_buffer *cmd_buffer,
                     enum intel_measure_snapshot_type type,
                     const char *event_name,
                     uint32_t count)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_measure_batch *measure = cmd_buffer->measure;

   assert(config);
   if (measure == NULL)
      return;

   assert(type != INTEL_SNAPSHOT_END);
   if (!state_changed(cmd_buffer, type)) {
      /* filter out this event */
      return;
   }

   /* increment event count */
   ++measure->base.event_count;
   if (measure->base.event_count == 1 ||
       measure->base.event_count == config->event_interval + 1) {
      /* the first event of an interval */

      if (measure->base.index % 2) {
         /* end the previous event */
         anv_measure_end_snapshot(cmd_buffer, measure->base.event_count - 1);
      }
      measure->base.event_count = 1;

      if (measure->base.index == config->batch_size) {
         /* Snapshot buffer is full.  The batch must be flushed before
          * additional snapshots can be taken.
          */
         static bool warned = false;
         if (unlikely(!warned)) {
            fprintf(config->file,
                    "WARNING: batch size exceeds INTEL_MEASURE limit: %d. "
                    "Data has been dropped. "
                    "Increase setting with INTEL_MEASURE=batch_size={count}\n",
                    config->batch_size);
         }

         warned = true;
         return;
      }

      anv_measure_start_snapshot(cmd_buffer, type, event_name, count);
   }
}

/**
 * Called when a command buffer is reset.  Re-initializes existing anv_measure
 * data structures.
 */
void
anv_measure_reset(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_device *device = cmd_buffer->device;
   struct anv_measure_batch *measure = cmd_buffer->measure;

   if (!config)
      return;

   if (!config->enabled) {
      cmd_buffer->measure = NULL;
      return;
   }

   if (!measure) {
      /* Capture has recently been enabled. Instead of resetting, a new data
       * structure must be allocated and initialized.
       */
      return anv_measure_init(cmd_buffer);
   }

   /* it is possible that the command buffer contains snapshots that have not
    * yet been processed
    */
   anv_measure_gather(device);

   assert(cmd_buffer->device != NULL);

   measure->base.index = 0;
   measure->base.framebuffer = 0;
   measure->base.frame = 0;
   measure->base.event_count = 0;

   anv_device_release_bo(device, measure->bo);
   VkResult result =
      anv_device_alloc_bo(device,
                          config->batch_size * sizeof(uint64_t),
                          ANV_BO_ALLOC_MAPPED,
                          0,
                          &measure->bo);
   assert(result == VK_SUCCESS);
}

void
anv_measure_destroy(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_measure_batch *measure = cmd_buffer->measure;
   struct anv_device *device = cmd_buffer->device;

   if (!config)
      return;
   if (measure == NULL)
      return;

   /* it is possible that the command buffer contains snapshots that have not
    * yet been processed
    */
   anv_measure_gather(device);

   anv_device_release_bo(device, measure->bo);
   vk_free(&cmd_buffer->pool->alloc, measure);
   cmd_buffer->measure = NULL;
}

static struct intel_measure_config*
config_from_device(struct anv_device *device)
{
   return device->physical->measure_device.config;
}

void
anv_measure_device_destroy(struct anv_physical_device *device)
{
   struct intel_measure_device *measure_device = &device->measure_device;
   struct intel_measure_config *config = measure_device->config;

   if (!config)
      return;

   if (measure_device->ringbuffer != NULL) {
      vk_free(&device->instance->vk.alloc, measure_device->ringbuffer);
      measure_device->ringbuffer = NULL;
   }
}

/**
 *  Hook for command buffer submission.
 */
void
_anv_measure_submit(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_measure_batch *measure = cmd_buffer->measure;
   struct intel_measure_device *measure_device = &cmd_buffer->device->physical->measure_device;

   if (!config)
      return;
   if (measure == NULL)
      return;

   if (measure->base.index == 0)
      /* no snapshots were started */
      return;

   /* finalize snapshots and enqueue them */
   static unsigned cmd_buffer_count = 0;
   measure->base.batch_count = p_atomic_inc_return(&cmd_buffer_count);

   if (measure->base.index %2 == 1) {
      anv_measure_end_snapshot(cmd_buffer, measure->base.event_count);
      measure->base.event_count = 0;
   }

   /* add to the list of submitted snapshots */
   pthread_mutex_lock(&measure_device->mutex);
   list_addtail(&measure->link, &measure_device->queued_snapshots);
   pthread_mutex_unlock(&measure_device->mutex);
}

/**
 *  Hook for the start of a frame.
 */
void
anv_measure_acquire(struct anv_device *device)
{
   struct intel_measure_config *config = config_from_device(device);
   struct intel_measure_device *measure_device = &device->physical->measure_device;

   if (!config)
      return;
   if (measure_device == NULL)
      return;

   intel_measure_frame_transition(p_atomic_inc_return(&measure_device->frame));

   /* iterate the queued snapshots and publish those that finished */
   anv_measure_gather(device);
}

void
_anv_measure_endcommandbuffer(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_measure_batch *measure = cmd_buffer->measure;

   if (!config)
      return;
   if (measure == NULL)
      return;
   if (measure->base.index % 2 == 0)
      return;

   anv_measure_end_snapshot(cmd_buffer, measure->base.event_count);
   measure->base.event_count = 0;
}

void
_anv_measure_beginrenderpass(struct anv_cmd_buffer *cmd_buffer)
{
   struct intel_measure_config *config = config_from_command_buffer(cmd_buffer);
   struct anv_measure_batch *measure = cmd_buffer->measure;

   if (!config)
      return;
   if (measure == NULL)
      return;

   if (measure->base.framebuffer == (uintptr_t) cmd_buffer->state.framebuffer)
      /* no change */
      return;

   bool filtering = (config->flags & (INTEL_MEASURE_RENDERPASS |
                                      INTEL_MEASURE_SHADER));
   if (filtering && measure->base.index % 2 == 1) {
      /* snapshot for previous renderpass was not ended */
      anv_measure_end_snapshot(cmd_buffer,
                               measure->base.event_count);
      measure->base.event_count = 0;
   }

   measure->base.framebuffer = (uintptr_t) cmd_buffer->state.framebuffer;
}
