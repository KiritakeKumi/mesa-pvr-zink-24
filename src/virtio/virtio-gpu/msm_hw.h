/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MSM_HW_H_
#define MSM_HW_H_

struct virgl_renderer_capset_msm {
   uint32_t wire_format_version;
   /* Underlying drm device version: */
   uint32_t version_major;
   uint32_t version_minor;
   uint32_t version_patchlevel;
   uint32_t has_cached_coherent;
};

/**
 * General protocol notes:
 * 1) Request (req) messages are generally sent over DRM_VIRTGPU_EXECBUFFER
 *    but can also be sent via DRM_VIRTGPU_RESOURCE_CREATE_BLOB (in which
 *    case they are processed by the host before ctx->get_blob())
 * 2) Response (rsp) messages are returned via shmem->rsp_mem, at an offset
 *    specified by the guest in the req message.  Not all req messages have
 *    a rsp.
 * 3) Host and guest could have different pointer sizes, ie. 32b guest and
 *    64b host, or visa versa, so similar to kernel uabi, req and rsp msgs
 *    should be explicitly padded to avoid 32b vs 64b struct padding issues
 */

/**
 * Defines the layout of shmem buffer used for host->guest communication.
 */
struct msm_shmem {
   /**
    * The sequence # of last cmd processed by the host
    */
   uint32_t seqno;

   /* TODO maybe separate flags for host to increment when:
    * a) host CPU error (like async SUBMIT failed, etc)
    * b) global reset count, if it hasn't incremented guest
    *    can skip synchonous getparam..
    */
   uint16_t error;
   uint16_t rsp_mem_len;

   /**
    * Memory to use for response messages.  The offset to use for the
    * response message is allocated by the guest, and specified by
    * msm_ccmd_req:rsp_off.
    */
   uint8_t rsp_mem[0x4000-8];
};

#define DEFINE_CAST(parent, child)                                             \
   static inline struct child *to_##child(const struct parent *x)              \
   {                                                                           \
      return (struct child *)x;                                                \
   }

/*
 * Possible cmd types for "command stream", ie. payload of EXECBUF ioctl:
 */
enum msm_ccmd {
   MSM_CCMD_NOP = 1,         /* No payload, can be used to sync with host */
   MSM_CCMD_IOCTL_SIMPLE,
   MSM_CCMD_GEM_NEW,
   MSM_CCMD_GEM_INFO,
   MSM_CCMD_GEM_CPU_PREP,
   MSM_CCMD_GEM_SET_NAME,
   MSM_CCMD_GEM_SUBMIT,
   MSM_CCMD_GEM_UPLOAD,
   MSM_CCMD_SUBMITQUEUE_QUERY,
   MSM_CCMD_WAIT_FENCE,
   MSM_CCMD_LAST,
};

struct msm_ccmd_req {
   uint32_t cmd : 8;
   uint32_t len : 24;

   /* Pad header to multiple of 64b and leave room for expansion.. must be
    * zero.
    */
   uint32_t __pad;

   uint32_t seqno;

   /* Offset into shmem ctrl buffer to write response.  The host ensures
    * that it doesn't write outside the bounds of the ctrl buffer, but
    * otherwise it is up to the guest to manage allocation of where responses
    * should be written in the ctrl buf.
    */
   uint32_t rsp_off;
};

#define MSM_CCMD(_cmd, _len) (struct msm_ccmd_req){ \
       .cmd = MSM_CCMD_##_cmd,                      \
       .len = (_len),                               \
   }

/* TODO all rsp's have an 'int32_t ret', so maybe we should just have a
 * common header for rsp..
 */

/*
 * MSM_CCMD_IOCTL_SIMPLE
 *
 * Forward simple/flat IOC_RW or IOC_W ioctls.  Limited ioctls are supported.
 */
struct msm_ccmd_ioctl_simple_req {
   struct msm_ccmd_req hdr;

   uint32_t cmd;
   uint8_t payload[];
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_ioctl_simple_req)

struct msm_ccmd_ioctl_simple_rsp {
   /* ioctl return value, interrupted syscalls are handled on the host without
    * returning to the guest.
    */
   int32_t ret;

   /* The output payload for IOC_RW ioctls, the payload is the same size as
    * msm_context_cmd_ioctl_simple_req.
    *
    * For IOC_W ioctls (userspace writes, kernel reads) this is zero length.
    */
   uint8_t payload[];
};

/*
 * MSM_CCMD_GEM_NEW
 *
 * GEM buffer allocation, maps to DRM_MSM_GEM_NEW plus DRM_MSM_GEM_INFO to
 * get the BO's iova (to avoid extra guest<->host round trip)
 */
struct msm_ccmd_gem_new_req {
   struct msm_ccmd_req hdr;

   uint64_t size;
   uint32_t flags;
   uint32_t blob_id;
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_new_req)

struct msm_ccmd_gem_new_rsp {
   int32_t ret;
   uint32_t host_handle; /* host side GEM handle, used for cmdstream submit */
   uint64_t iova;
};

/*
 * MSM_CCMD_GEM_INFO
 *
 * Returns similar information as MSM_CCMD_GEM_NEW, but for imported BO's,
 * which don't have a blob_id in our context, but do have a resource-id
 */
struct msm_ccmd_gem_info_req {
   struct msm_ccmd_req hdr;
   uint32_t res_id;
   uint32_t blob_mem;   // TODO do we need this?
   uint32_t blob_id;    // TODO do we need this?
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_info_req)

struct msm_ccmd_gem_info_rsp {
   int32_t ret;
   uint32_t host_handle; /* host side GEM handle, used for cmdstream submit */
   uint64_t iova;
   uint32_t pad;
   uint32_t size;        /* true size of bo on host side */
};

/*
 * MSM_CCMD_GEM_CPU_PREP
 *
 * Maps to DRM_MSM_GEM_CPU_PREP
 *
 * Note: currently this uses a relative timeout mapped to absolute timeout
 * on the host, because I don't think we can rely on monotonic time being
 * aligned between host and guest.  This has the slight drawback of not
 * handling interrupted syscalls on the guest side, but since the actual
 * waiting happens on the host side (after guest execbuf ioctl returns)
 * this shouldn't be *that* much of a problem.
 *
 * If we could rely on host and guest times being aligned, we could use
 * MSM_CCMD_IOCTL_SIMPLE instead
 */
struct msm_ccmd_gem_cpu_prep_req {
   struct msm_ccmd_req hdr;
   uint32_t host_handle;
   uint32_t op;
   uint64_t timeout;
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_cpu_prep_req)

struct msm_ccmd_gem_cpu_prep_rsp {
   int32_t ret;
};

/*
 * MSM_CCMD_GEM_SET_NAME
 *
 * Maps to DRM_MSM_GEM_INFO:MSM_INFO_SET_NAME
 *
 * No response.
 */
struct msm_ccmd_gem_set_name_req {
   struct msm_ccmd_req hdr;
   uint32_t host_handle;
   /* Note: packet size aligned to 4 bytes, so the string name may
    * be shorter than the packet header indicates.
    */
   uint32_t len;
   uint8_t  payload[];
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_set_name_req)

/*
 * MSM_CCMD_GEM_SUBMIT
 *
 * Maps to DRM_MSM_GEM_SUBMIT
 *
 * The actual for-reals cmdstream submission.  Note this intentionally
 * does not support relocs, since we already require a non-ancient
 * kernel.
 *
 * Note, no in/out fence-fd, that synchronization is handled on guest
 * kernel side (ugg).. need to come up with a better story for fencing.
 * We probably need to sort something out for that to handle syncobjs.
 *
 * Note that the bo handles referenced are the host handles, so that
 * they can be directly passed to the host kernel without translation.
 *
 * TODO we can pack the payload tighter (and enforce no-relocs) if we
 * defined our own structs, at the cost of host userspace having to
 * do a bit more work.  Is it worth it?  It could probably be done
 * without extra overhead in guest userspace..
 *
 * No response.
 */
struct msm_ccmd_gem_submit_req {
   struct msm_ccmd_req hdr;
   uint32_t flags;
   uint32_t queue_id;
   uint32_t nr_bos;
   uint32_t nr_cmds;

   /**
    * What userspace expects the next seqno fence to be.  To avoid having
    * to wait for host, the guest tracks what it expects to be the next
    * returned seqno fence.  This is passed to guest just for error
    * checking.
    */
   uint32_t fence;

   /**
    * Payload is first an array of 'struct drm_msm_gem_submit_bo' of
    * length determined by nr_bos (note that handles are host handles),
    * followed by an array of 'struct drm_msm_gem_submit_cmd' of length
    * determined by nr_cmds
    */
   int8_t   payload[];
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_submit_req)

/*
 * MSM_CCMD_GEM_UPLOAD
 *
 * Upload data to a GEM buffer
 *
 * No response.
 */
struct msm_ccmd_gem_upload_req {
   struct msm_ccmd_req hdr;
   uint32_t host_handle;
   /* Note: packet size aligned to 4 bytes, so the string name may
    * be shorter than the packet header indicates.
    */
   uint32_t len;
   uint8_t  payload[];
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_gem_upload_req)

/*
 * MSM_CCMD_SUBMITQUEUE_QUERY
 *
 * Maps to DRM_MSM_SUBMITQUEUE_QUERY
 */
struct msm_ccmd_submitqueue_query_req {
   struct msm_ccmd_req hdr;

   uint32_t queue_id;
   uint32_t param;
   uint32_t len;   /* size of payload in rsp */
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_submitqueue_query_req)

struct msm_ccmd_submitqueue_query_rsp {
   int32_t  ret;
   uint32_t out_len;
   uint8_t  payload[];
};

/*
 * MSM_CCMD_WAIT_FENCE
 *
 * Maps to DRM_MSM_WAIT_FENCE
 *
 * Note: currently this uses a relative timeout mapped to absolute timeout
 * on the host, because I don't think we can rely on monotonic time being
 * aligned between host and guest.  This has the slight drawback of not
 * handling interrupted syscalls on the guest side, but since the actual
 * waiting happens on the host side (after guest execbuf ioctl returns)
 * this shouldn't be *that* much of a problem.
 *
 * If we could rely on host and guest times being aligned, we could use
 * MSM_CCMD_IOCTL_SIMPLE instead
 */
struct msm_ccmd_wait_fence_req {
   struct msm_ccmd_req hdr;
   uint32_t queue_id;
   uint32_t fence;
   uint64_t timeout;
};
DEFINE_CAST(msm_ccmd_req, msm_ccmd_wait_fence_req)

struct msm_ccmd_wait_fence_rsp {
   int32_t ret;
};

#endif /* MSM_HW_H_ */
