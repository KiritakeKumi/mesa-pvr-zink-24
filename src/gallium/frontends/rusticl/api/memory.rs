#![allow(non_upper_case_globals)]

extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::memory::*;
use crate::*;

use self::mesa_rust_util::ptr::*;
use self::rusticl_opencl_gen::*;

use std::cmp::Ordering;
use std::os::raw::c_void;
use std::ptr;
use std::sync::Arc;

fn validate_mem_flags(flags: cl_mem_flags, images: bool) -> CLResult<()> {
    let mut valid_flags = cl_bitfield::from(
        CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY | CL_MEM_KERNEL_READ_AND_WRITE,
    );

    if !images {
        valid_flags |= cl_bitfield::from(
            CL_MEM_USE_HOST_PTR
                | CL_MEM_ALLOC_HOST_PTR
                | CL_MEM_COPY_HOST_PTR
                | CL_MEM_HOST_WRITE_ONLY
                | CL_MEM_HOST_READ_ONLY
                | CL_MEM_HOST_NO_ACCESS,
        );
    }

    let read_write_group =
        cl_bitfield::from(CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);

    let alloc_host_group = cl_bitfield::from(CL_MEM_ALLOC_HOST_PTR | CL_MEM_USE_HOST_PTR);

    let copy_host_group = cl_bitfield::from(CL_MEM_COPY_HOST_PTR | CL_MEM_USE_HOST_PTR);

    let host_read_write_group =
        cl_bitfield::from(CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS);

    if (flags & !valid_flags != 0)
        || (flags & read_write_group).count_ones() > 1
        || (flags & alloc_host_group).count_ones() > 1
        || (flags & copy_host_group).count_ones() > 1
        || (flags & host_read_write_group).count_ones() > 1
    {
        return Err(CL_INVALID_VALUE);
    }
    Ok(())
}

fn filter_image_access_flags(flags: cl_mem_flags) -> cl_mem_flags {
    flags
        & (CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY | CL_MEM_KERNEL_READ_AND_WRITE)
            as cl_mem_flags
}

fn inherit_mem_flags(mut flags: cl_mem_flags, mem: &Mem) -> cl_mem_flags {
    let read_write_mask = cl_bitfield::from(
        CL_MEM_READ_WRITE |
      CL_MEM_WRITE_ONLY |
      CL_MEM_READ_ONLY |
      // not in spec, but...
      CL_MEM_KERNEL_READ_AND_WRITE,
    );
    let host_ptr_mask =
        cl_bitfield::from(CL_MEM_USE_HOST_PTR | CL_MEM_ALLOC_HOST_PTR | CL_MEM_COPY_HOST_PTR);
    let host_mask =
        cl_bitfield::from(CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS);

    // For CL_MEM_OBJECT_IMAGE1D_BUFFER image type, or an image created from another memory object
    // (image or buffer)...
    //
    // ... if the CL_MEM_READ_WRITE, CL_MEM_READ_ONLY or CL_MEM_WRITE_ONLY values are not
    // specified in flags, they are inherited from the corresponding memory access qualifiers
    // associated with mem_object. ...
    if flags & read_write_mask == 0 {
        flags |= mem.flags & read_write_mask;
    }

    // ... The CL_MEM_USE_HOST_PTR, CL_MEM_ALLOC_HOST_PTR and CL_MEM_COPY_HOST_PTR values cannot
    // be specified in flags but are inherited from the corresponding memory access qualifiers
    // associated with mem_object. ...
    flags &= !host_ptr_mask;
    flags |= mem.flags & host_ptr_mask;

    // ... If the CL_MEM_HOST_WRITE_ONLY, CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS values
    // are not specified in flags, they are inherited from the corresponding memory access
    // qualifiers associated with mem_object.
    if flags & host_mask == 0 {
        flags |= mem.flags & host_mask;
    }

    flags
}

fn image_type_valid(image_type: cl_mem_object_type) -> bool {
    CL_IMAGE_TYPES.contains(&image_type)
}

fn validate_addressing_mode(addressing_mode: cl_addressing_mode) -> CLResult<()> {
    match addressing_mode {
        CL_ADDRESS_NONE
        | CL_ADDRESS_CLAMP_TO_EDGE
        | CL_ADDRESS_CLAMP
        | CL_ADDRESS_REPEAT
        | CL_ADDRESS_MIRRORED_REPEAT => Ok(()),
        _ => Err(CL_INVALID_VALUE),
    }
}

fn validate_filter_mode(filter_mode: cl_filter_mode) -> CLResult<()> {
    match filter_mode {
        CL_FILTER_NEAREST | CL_FILTER_LINEAR => Ok(()),
        _ => Err(CL_INVALID_VALUE),
    }
}

fn validate_host_ptr(host_ptr: *mut ::std::os::raw::c_void, flags: cl_mem_flags) -> CLResult<()> {
    // CL_INVALID_HOST_PTR if host_ptr is NULL and CL_MEM_USE_HOST_PTR or CL_MEM_COPY_HOST_PTR are
    // set in flags
    if host_ptr.is_null()
        && flags & (cl_mem_flags::from(CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR)) != 0
    {
        return Err(CL_INVALID_HOST_PTR);
    }

    // or if host_ptr is not NULL but CL_MEM_COPY_HOST_PTR or CL_MEM_USE_HOST_PTR are not set in
    // flags.
    if !host_ptr.is_null()
        && flags & (cl_mem_flags::from(CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR)) == 0
    {
        return Err(CL_INVALID_HOST_PTR);
    }

    Ok(())
}

fn validate_matching_buffer_flags<'a>(mem: &'a Mem, flags: cl_mem_flags) -> CLResult<()> {
    // CL_INVALID_VALUE if an image is being created from another memory object (buffer or image)
    // under one of the following circumstances:
    //
    // 1) mem_object was created with CL_MEM_WRITE_ONLY and
    //    flags specifies CL_MEM_READ_WRITE or CL_MEM_READ_ONLY,
    if bit_check(mem.flags, CL_MEM_WRITE_ONLY) && bit_check(flags, CL_MEM_READ_WRITE | CL_MEM_READ_ONLY) ||
      // 2) mem_object was created with CL_MEM_READ_ONLY and
      //    flags specifies CL_MEM_READ_WRITE or CL_MEM_WRITE_ONLY,
      bit_check(mem.flags, CL_MEM_READ_ONLY) && bit_check(flags, CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY) ||
      // 3) flags specifies CL_MEM_USE_HOST_PTR or CL_MEM_ALLOC_HOST_PTR or CL_MEM_COPY_HOST_PTR.
      bit_check(flags, CL_MEM_USE_HOST_PTR | CL_MEM_ALLOC_HOST_PTR | CL_MEM_COPY_HOST_PTR) ||
      // CL_INVALID_VALUE if an image is being created from another memory object (buffer or image)
      // and mem_object was created with CL_MEM_HOST_WRITE_ONLY and flags specifies CL_MEM_HOST_READ_ONLY
      bit_check(mem.flags, CL_MEM_HOST_WRITE_ONLY) && bit_check(flags, CL_MEM_HOST_READ_ONLY) ||
      // or if mem_object was created with CL_MEM_HOST_READ_ONLY and flags specifies CL_MEM_HOST_WRITE_ONLY
      bit_check(mem.flags, CL_MEM_HOST_READ_ONLY) && bit_check(flags, CL_MEM_HOST_WRITE_ONLY) ||
      // or if mem_object was created with CL_MEM_HOST_NO_ACCESS and_flags_ specifies CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_WRITE_ONLY.
      bit_check(mem.flags, CL_MEM_HOST_NO_ACCESS) && bit_check(flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_WRITE_ONLY)
    {
        Err(CL_INVALID_VALUE)?
    }

    Ok(())
}

impl CLInfo<cl_mem_info> for cl_mem {
    fn query(&self, q: cl_mem_info) -> CLResult<Vec<u8>> {
        let mem = self.get_ref()?;
        Ok(match *q {
            CL_MEM_ASSOCIATED_MEMOBJECT => {
                let ptr = match mem.parent.as_ref() {
                    // Note we use as_ptr here which doesn't increase the reference count.
                    Some(parent) => Arc::as_ptr(parent),
                    None => ptr::null(),
                };
                cl_prop::<cl_mem>(cl_mem::from_ptr(ptr))
            }
            CL_MEM_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&mem.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_MEM_FLAGS => cl_prop::<cl_mem_flags>(mem.flags),
            // TODO debugging feature
            CL_MEM_MAP_COUNT => cl_prop::<cl_uint>(0),
            CL_MEM_HOST_PTR => cl_prop::<*mut c_void>(mem.host_ptr),
            CL_MEM_OFFSET => cl_prop::<usize>(mem.offset),
            CL_MEM_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_MEM_SIZE => cl_prop::<usize>(mem.size),
            CL_MEM_TYPE => cl_prop::<cl_mem_object_type>(mem.mem_type),
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_buffer(
    context: cl_context,
    flags: cl_mem_flags,
    size: usize,
    host_ptr: *mut ::std::os::raw::c_void,
) -> CLResult<cl_mem> {
    let c = context.get_arc()?;

    // CL_INVALID_VALUE if values specified in flags are not valid as defined in the Memory Flags table.
    validate_mem_flags(flags, false)?;

    // CL_INVALID_BUFFER_SIZE if size is 0
    if size == 0 {
        return Err(CL_INVALID_BUFFER_SIZE);
    }

    // ... or if size is greater than CL_DEVICE_MAX_MEM_ALLOC_SIZE for all devices in context.
    for dev in &c.devs {
        if checked_compare(size, Ordering::Greater, dev.max_mem_alloc()) {
            return Err(CL_INVALID_BUFFER_SIZE);
        }
    }

    validate_host_ptr(host_ptr, flags)?;

    Ok(cl_mem::from_arc(Mem::new_buffer(
        &c, flags, size, host_ptr,
    )?))
}

pub fn create_sub_buffer(
    buffer: cl_mem,
    mut flags: cl_mem_flags,
    buffer_create_type: cl_buffer_create_type,
    buffer_create_info: *const ::std::os::raw::c_void,
) -> CLResult<cl_mem> {
    let b = buffer.get_arc()?;

    // CL_INVALID_MEM_OBJECT if buffer ... is a sub-buffer object.
    if b.parent.is_some() {
        Err(CL_INVALID_MEM_OBJECT)?
    }

    validate_matching_buffer_flags(&b, flags)?;

    flags = inherit_mem_flags(flags, &b);
    validate_mem_flags(flags, false)?;

    let (offset, size) = match buffer_create_type {
        CL_BUFFER_CREATE_TYPE_REGION => {
            // buffer_create_info is a pointer to a cl_buffer_region structure specifying a region of
            // the buffer.
            // CL_INVALID_VALUE if value(s) specified in buffer_create_info (for a given
            // buffer_create_type) is not valid or if buffer_create_info is NULL.
            let region = unsafe { buffer_create_info.cast::<cl_buffer_region>().as_ref() }
                .ok_or(CL_INVALID_VALUE)?;

            // CL_INVALID_BUFFER_SIZE if the size field of the cl_buffer_region structure passed in
            // buffer_create_info is 0.
            if region.size == 0 {
                Err(CL_INVALID_BUFFER_SIZE)?
            }

            // CL_INVALID_VALUE if the region specified by the cl_buffer_region structure passed in
            // buffer_create_info is out of bounds in buffer.
            if region.origin + region.size > b.size {
                Err(CL_INVALID_VALUE)?
            }

            (region.origin, region.size)
        }
        // CL_INVALID_VALUE if the value specified in buffer_create_type is not valid.
        _ => Err(CL_INVALID_VALUE)?,
    };

    Ok(cl_mem::from_arc(Mem::new_sub_buffer(
        &b, flags, offset, size,
    )))

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if there are no devices in context associated with buffer for which the origin field of the cl_buffer_region structure passed in buffer_create_info is aligned to the CL_DEVICE_MEM_BASE_ADDR_ALIGN value.
}

pub fn set_mem_object_destructor_callback(
    memobj: cl_mem,
    pfn_notify: Option<MemCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<()> {
    let m = memobj.get_ref()?;

    // CL_INVALID_VALUE if pfn_notify is NULL.
    if pfn_notify.is_none() {
        return Err(CL_INVALID_VALUE);
    }

    m.cbs
        .lock()
        .unwrap()
        .push(cl_closure!(|m| pfn_notify(m, user_data)));
    Ok(())
}

fn validate_image_format<'a>(
    image_format: *const cl_image_format,
) -> CLResult<(&'a cl_image_format, u8)> {
    // CL_INVALID_IMAGE_FORMAT_DESCRIPTOR ... if image_format is NULL.
    let format = unsafe { image_format.as_ref() }.ok_or(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?;

    let channels = match format.image_channel_order {
        CL_R | CL_A | CL_DEPTH | CL_LUMINANCE | CL_INTENSITY => 1,

        CL_RG | CL_RA | CL_Rx => 2,

        CL_RGB | CL_RGx | CL_sRGB => 3,

        CL_RGBA | CL_ARGB | CL_BGRA | CL_ABGR | CL_RGBx | CL_sRGBA | CL_sBGRA | CL_sRGBx => 4,

        _ => return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?,
    };

    let channel_size = match format.image_channel_data_type {
        CL_SNORM_INT8 | CL_UNORM_INT8 | CL_SIGNED_INT8 | CL_UNSIGNED_INT8 => 1,

        CL_SNORM_INT16 | CL_UNORM_INT16 | CL_SIGNED_INT16 | CL_UNSIGNED_INT16 | CL_HALF_FLOAT
        | CL_UNORM_SHORT_565 | CL_UNORM_SHORT_555 => 2,

        CL_SIGNED_INT32
        | CL_UNSIGNED_INT32
        | CL_FLOAT
        | CL_UNORM_INT_101010
        | CL_UNORM_INT_101010_2 => 4,

        _ => return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?,
    };

    let packed = [
        CL_UNORM_SHORT_565,
        CL_UNORM_SHORT_555,
        CL_UNORM_INT_101010,
        CL_UNORM_INT_101010,
    ]
    .contains(&format.image_channel_data_type);

    // special validation
    let valid_combination = match format.image_channel_data_type {
        CL_UNORM_SHORT_565 | CL_UNORM_SHORT_555 | CL_UNORM_INT_101010 => {
            [CL_RGB, CL_RGBx].contains(&format.image_channel_data_type)
        }
        CL_UNORM_INT_101010_2 => format.image_channel_data_type == CL_RGBA,
        _ => true,
    };
    if !valid_combination {
        return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }

    Ok((
        format,
        if packed {
            channel_size
        } else {
            channels * channel_size
        },
    ))
}

fn validate_image_desc<'a>(
    image_desc: *const cl_image_desc,
    host_ptr: *mut ::std::os::raw::c_void,
    elem_size: usize,
    devs: &Vec<Arc<Device>>,
) -> CLResult<cl_image_desc> {
    // CL_INVALID_IMAGE_DESCRIPTOR if values specified in image_desc are not valid
    const err: cl_int = CL_INVALID_IMAGE_DESCRIPTOR;

    // CL_INVALID_IMAGE_DESCRIPTOR ... if image_desc is NULL.
    let mut desc = *unsafe { image_desc.as_ref() }.ok_or(err)?;

    // image_type describes the image type and must be either CL_MEM_OBJECT_IMAGE1D,
    // CL_MEM_OBJECT_IMAGE1D_BUFFER, CL_MEM_OBJECT_IMAGE1D_ARRAY, CL_MEM_OBJECT_IMAGE2D,
    // CL_MEM_OBJECT_IMAGE2D_ARRAY, or CL_MEM_OBJECT_IMAGE3D.
    let (dims, array) = match desc.image_type {
        CL_MEM_OBJECT_IMAGE1D | CL_MEM_OBJECT_IMAGE1D_BUFFER => (1, false),
        CL_MEM_OBJECT_IMAGE1D_ARRAY => (1, true),
        CL_MEM_OBJECT_IMAGE2D => (2, false),
        CL_MEM_OBJECT_IMAGE2D_ARRAY => (2, true),
        CL_MEM_OBJECT_IMAGE3D => (3, false),
        _ => Err(err)?,
    };

    // image_width is the width of the image in pixels. For a 2D image and image array, the image
    // width must be a value ≥ 1 and ≤ CL_DEVICE_IMAGE2D_MAX_WIDTH. For a 3D image, the image width
    // must be a value ≥ 1 and ≤ CL_DEVICE_IMAGE3D_MAX_WIDTH. For a 1D image buffer, the image width
    // must be a value ≥ 1 and ≤ CL_DEVICE_IMAGE_MAX_BUFFER_SIZE. For a 1D image and 1D image array,
    // the image width must be a value ≥ 1 and ≤ CL_DEVICE_IMAGE2D_MAX_WIDTH.
    //
    // image_height is the height of the image in pixels. This is only used if the image is a 2D or
    // 3D image, or a 2D image array. For a 2D image or image array, the image height must be a
    // value ≥ 1 and ≤ CL_DEVICE_IMAGE2D_MAX_HEIGHT. For a 3D image, the image height must be a
    // value ≥ 1 and ≤ CL_DEVICE_IMAGE3D_MAX_HEIGHT.
    //
    // image_depth is the depth of the image in pixels. This is only used if the image is a 3D image
    // and must be a value ≥ 1 and ≤ CL_DEVICE_IMAGE3D_MAX_DEPTH.
    if desc.image_width < 1
        || desc.image_height < 1 && dims >= 2
        || desc.image_depth < 1 && dims >= 3
        || desc.image_array_size < 1 && array
    {
        Err(err)?
    }

    let max_size = if dims == 3 {
        devs.iter().map(|d| d.image_3d_size()).min()
    } else if desc.image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER {
        devs.iter().map(|d| d.image_buffer_size()).min()
    } else {
        devs.iter().map(|d| d.image_2d_size()).min()
    }
    .unwrap();
    let max_array = devs.iter().map(|d| d.image_array_size()).min().unwrap();

    // CL_INVALID_IMAGE_SIZE if image dimensions specified in image_desc exceed the maximum image
    // dimensions described in the Device Queries table for all devices in context.
    if desc.image_width > max_size
        || desc.image_height > max_size && dims >= 2
        || desc.image_depth > max_size && dims >= 3
        || desc.image_array_size > max_array && array
    {
        Err(CL_INVALID_IMAGE_SIZE)?
    }

    // num_mip_levels and num_samples must be 0.
    if desc.num_mip_levels != 0 || desc.num_samples != 0 {
        Err(err)?
    }

    // mem_object may refer to a valid buffer or image memory object. mem_object can be a buffer
    // memory object if image_type is CL_MEM_OBJECT_IMAGE1D_BUFFER or CL_MEM_OBJECT_IMAGE2D.
    // mem_object can be an image object if image_type is CL_MEM_OBJECT_IMAGE2D. Otherwise it must
    // be NULL.
    //
    // TODO: cl_khr_image2d_from_buffer is an optional feature
    let p = unsafe { &desc.anon_1.mem_object };
    if !p.is_null() {
        let p = p.get_ref()?;
        if !match desc.image_type {
            CL_MEM_OBJECT_IMAGE1D_BUFFER => p.is_buffer(),
            CL_MEM_OBJECT_IMAGE2D => !p.is_buffer(),
            _ => true,
        } {
            Err(CL_INVALID_OPERATION)?
        }
    }

    // image_row_pitch is the scan-line pitch in bytes. This must be 0 if host_ptr is NULL and can
    // be either 0 or ≥ image_width × size of element in bytes if host_ptr is not NULL. If host_ptr
    // is not NULL and image_row_pitch = 0, image_row_pitch is calculated as image_width × size of
    // element in bytes. If image_row_pitch is not 0, it must be a multiple of the image element
    // size in bytes. For a 2D image created from a buffer, the pitch specified (or computed if
    // pitch specified is 0) must be a multiple of the maximum of the
    // CL_DEVICE_IMAGE_PITCH_ALIGNMENT value for all devices in the context associated with the
    // buffer specified by mem_object that support images.
    //
    // image_slice_pitch is the size in bytes of each 2D slice in the 3D image or the size in bytes
    // of each image in a 1D or 2D image array. This must be 0 if host_ptr is NULL. If host_ptr is
    // not NULL, image_slice_pitch can be either 0 or ≥ image_row_pitch × image_height for a 2D
    // image array or 3D image and can be either 0 or ≥ image_row_pitch for a 1D image array. If
    // host_ptr is not NULL and image_slice_pitch = 0, image_slice_pitch is calculated as
    // image_row_pitch × image_height for a 2D image array or 3D image and image_row_pitch for a 1D
    // image array. If image_slice_pitch is not 0, it must be a multiple of the image_row_pitch.
    if host_ptr.is_null() {
        if desc.image_row_pitch != 0 || desc.image_slice_pitch != 0 {
            Err(err)?
        }
    } else {
        if desc.image_row_pitch == 0 {
            desc.image_row_pitch = desc.image_width * elem_size;
        } else if desc.image_row_pitch % elem_size != 0 {
            Err(err)?
        }

        if dims == 3 || array {
            let valid_slice_pitch =
                desc.image_row_pitch * if dims == 1 { 1 } else { desc.image_height };
            if desc.image_slice_pitch == 0 {
                desc.image_slice_pitch = valid_slice_pitch;
            } else {
                if desc.image_slice_pitch < valid_slice_pitch
                    || desc.image_slice_pitch % desc.image_row_pitch != 0
                {
                    Err(err)?
                }
            }
        }
    }

    Ok(desc)
}

fn desc_eq_no_buffer(a: &cl_image_desc, b: &cl_image_desc) -> bool {
    a.image_type == b.image_type
        && a.image_width == b.image_width
        && a.image_height == b.image_height
        && a.image_depth == b.image_depth
        && a.image_array_size == b.image_array_size
        && a.image_row_pitch == b.image_row_pitch
        && a.image_slice_pitch == b.image_slice_pitch
        && a.num_mip_levels == b.num_mip_levels
        && a.num_samples == b.num_samples
}

fn validate_buffer(
    desc: &cl_image_desc,
    mut flags: cl_mem_flags,
    format: &cl_image_format,
    host_ptr: *mut ::std::os::raw::c_void,
    elem_size: usize,
) -> CLResult<cl_mem_flags> {
    // CL_INVALID_IMAGE_DESCRIPTOR if values specified in image_desc are not valid
    const err: cl_int = CL_INVALID_IMAGE_DESCRIPTOR;
    let mem_object = unsafe { desc.anon_1.mem_object };

    // mem_object may refer to a valid buffer or image memory object. mem_object can be a buffer
    // memory object if image_type is CL_MEM_OBJECT_IMAGE1D_BUFFER or CL_MEM_OBJECT_IMAGE2D
    // mem_object can be an image object if image_type is CL_MEM_OBJECT_IMAGE2D. Otherwise it must
    // be NULL. The image pixels are taken from the memory objects data store. When the contents of
    // the specified memory objects data store are modified, those changes are reflected in the
    // contents of the image object and vice-versa at corresponding synchronization points.
    if !mem_object.is_null() {
        let mem = mem_object.get_ref()?;

        match mem.mem_type {
            CL_MEM_OBJECT_BUFFER => {
                match desc.image_type {
                    // For a 1D image buffer created from a buffer object, the image_width × size of
                    // element in bytes must be ≤ size of the buffer object.
                    CL_MEM_OBJECT_IMAGE1D_BUFFER => {
                        if desc.image_width * elem_size > mem.size {
                            Err(err)?
                        }
                    }
                    // For a 2D image created from a buffer object, the image_row_pitch × image_height
                    // must be ≤ size of the buffer object specified by mem_object.
                    CL_MEM_OBJECT_IMAGE2D => {
                        //TODO
                        //• CL_INVALID_IMAGE_FORMAT_DESCRIPTOR if a 2D image is created from a buffer and the row pitch and base address alignment does not follow the rules described for creating a 2D image from a buffer.
                        if desc.image_row_pitch * desc.image_height > mem.size {
                            Err(err)?
                        }
                    }
                    _ => Err(err)?,
                }
            }
            // For an image object created from another image object, the values specified in the
            // image descriptor except for mem_object must match the image descriptor information
            // associated with mem_object.
            CL_MEM_OBJECT_IMAGE2D => {
                if desc.image_type != mem.mem_type || !desc_eq_no_buffer(&desc, &mem.image_desc) {
                    Err(err)?
                }

                // CL_INVALID_IMAGE_FORMAT_DESCRIPTOR if a 2D image is created from a 2D image object
                // and the rules described above are not followed.

                // Creating a 2D image object from another 2D image object creates a new 2D image
                // object that shares the image data store with mem_object but views the pixels in the
                //  image with a different image channel order. Restrictions are:
                //
                // The image channel data type specified in image_format must match the image channel
                // data type associated with mem_object.
                if format.image_channel_data_type != mem.image_format.image_channel_data_type {
                    Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?
                }

                // The image channel order specified in image_format must be compatible with the image
                // channel order associated with mem_object. Compatible image channel orders are:
                if format.image_channel_order != mem.image_format.image_channel_order {
                    // in image_format | in  mem_object:
                    // CL_sBGRA | CL_BGRA
                    // CL_BGRA  | CL_sBGRA
                    // CL_sRGBA | CL_RGBA
                    // CL_RGBA  | CL_sRGBA
                    // CL_sRGB  | CL_RGB
                    // CL_RGB   | CL_sRGB
                    // CL_sRGBx | CL_RGBx
                    // CL_RGBx  | CL_sRGBx
                    // CL_DEPTH | CL_R
                    match (
                        format.image_channel_order,
                        mem.image_format.image_channel_order,
                    ) {
                        (CL_sBGRA, CL_BGRA)
                        | (CL_BGRA, CL_sBGRA)
                        | (CL_sRGBA, CL_RGBA)
                        | (CL_RGBA, CL_sRGBA)
                        | (CL_sRGB, CL_RGB)
                        | (CL_RGB, CL_sRGB)
                        | (CL_sRGBx, CL_RGBx)
                        | (CL_RGBx, CL_sRGBx)
                        | (CL_DEPTH, CL_R) => (),
                        _ => Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?,
                    }
                }
            }
            _ => Err(err)?,
        }

        // If the buffer object specified by mem_object was created with CL_MEM_USE_HOST_PTR, the
        // host_ptr specified to clCreateBuffer or clCreateBufferWithProperties must be aligned to
        // the maximum of the CL_DEVICE_IMAGE_BASE_ADDRESS_ALIGNMENT value for all devices in the
        // context associated with the buffer specified by mem_object that support images.
        if mem.flags & CL_MEM_USE_HOST_PTR as cl_mem_flags != 0 {
            for dev in &mem.context.devs {
                let addr_alignment = dev.image_base_address_alignment();
                if addr_alignment == 0 {
                    Err(CL_INVALID_OPERATION)?
                } else if !is_alligned(host_ptr, addr_alignment as usize) {
                    Err(err)?
                }
            }
        }

        validate_matching_buffer_flags(mem, flags)?;

        flags = inherit_mem_flags(flags, mem);
    // implied by spec
    } else if desc.image_type == CL_MEM_OBJECT_IMAGE1D_BUFFER {
        Err(err)?
    }

    Ok(flags)
}

impl CLInfo<cl_image_info> for cl_mem {
    fn query(&self, q: cl_image_info) -> CLResult<Vec<u8>> {
        let mem = self.get_ref()?;
        Ok(match *q {
            CL_IMAGE_ARRAY_SIZE => cl_prop::<usize>(mem.image_desc.image_array_size),
            CL_IMAGE_BUFFER => cl_prop::<cl_mem>(unsafe { mem.image_desc.anon_1.buffer }),
            CL_IMAGE_DEPTH => cl_prop::<usize>(mem.image_desc.image_depth),
            CL_IMAGE_ELEMENT_SIZE => cl_prop::<usize>(mem.image_elem_size.into()),
            CL_IMAGE_FORMAT => cl_prop::<cl_image_format>(mem.image_format),
            CL_IMAGE_HEIGHT => cl_prop::<usize>(mem.image_desc.image_height),
            CL_IMAGE_NUM_MIP_LEVELS => cl_prop::<cl_uint>(mem.image_desc.num_mip_levels),
            CL_IMAGE_NUM_SAMPLES => cl_prop::<cl_uint>(mem.image_desc.num_samples),
            CL_IMAGE_ROW_PITCH => cl_prop::<usize>(mem.image_desc.image_row_pitch),
            CL_IMAGE_SLICE_PITCH => cl_prop::<usize>(mem.image_desc.image_slice_pitch),
            CL_IMAGE_WIDTH => cl_prop::<usize>(mem.image_desc.image_width),
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_image(
    context: cl_context,
    mut flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_desc: *const cl_image_desc,
    host_ptr: *mut ::std::os::raw::c_void,
) -> CLResult<cl_mem> {
    let c = context.get_arc()?;

    // CL_INVALID_OPERATION if there are no devices in context that support images (i.e.
    // CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    c.devs
        .iter()
        .find(|d| d.image_supported())
        .ok_or(CL_INVALID_OPERATION)?;

    let (format, elem_size) = validate_image_format(image_format)?;
    let desc = validate_image_desc(image_desc, host_ptr, elem_size.into(), &c.devs)?;
    flags = validate_buffer(&desc, flags, format, host_ptr, elem_size.into())?;

    // For all image types except CL_MEM_OBJECT_IMAGE1D_BUFFER, if the value specified for flags is 0, the
    // default is used which is CL_MEM_READ_WRITE.
    if flags == 0 && desc.image_type != CL_MEM_OBJECT_IMAGE1D_BUFFER {
        flags = CL_MEM_READ_WRITE.into();
    }

    validate_mem_flags(flags, false)?;
    validate_host_ptr(host_ptr, flags)?;

    let filtered_flags = filter_image_access_flags(flags);
    // CL_IMAGE_FORMAT_NOT_SUPPORTED if there are no devices in context that support image_format.
    c.devs
        .iter()
        .filter_map(|d| d.formats.get(format))
        .filter_map(|f| f.get(&desc.image_type))
        .find(|f| *f & filtered_flags == filtered_flags)
        .ok_or(CL_IMAGE_FORMAT_NOT_SUPPORTED)?;

    Ok(cl_mem::from_arc(Mem::new_image(
        &c,
        desc.image_type,
        flags,
        format,
        desc,
        elem_size,
        host_ptr,
    )))
}

pub fn get_supported_image_formats(
    context: cl_context,
    flags: cl_mem_flags,
    image_type: cl_mem_object_type,
    num_entries: cl_uint,
    image_formats: *mut cl_image_format,
    num_image_formats: *mut cl_uint,
) -> CLResult<()> {
    let c = context.get_ref()?;

    // CL_INVALID_VALUE if flags
    validate_mem_flags(flags, true)?;

    // or image_type are not valid
    if !image_type_valid(image_type) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE ... if num_entries is 0 and image_formats is not NULL.
    if num_entries == 0 && !image_formats.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let mut res = Vec::<cl_image_format>::new();
    let filtered_flags = filter_image_access_flags(flags);
    for dev in &c.devs {
        for f in &dev.formats {
            let s = f.1.get(&image_type).unwrap_or(&0);

            if filtered_flags & s == filtered_flags {
                res.push(*f.0);
            }
        }
    }

    num_image_formats.write_checked(res.len() as cl_uint);
    unsafe { image_formats.copy_checked(res.as_ptr(), res.len()) };

    Ok(())
}

impl CLInfo<cl_sampler_info> for cl_sampler {
    fn query(&self, q: cl_sampler_info) -> CLResult<Vec<u8>> {
        let sampler = self.get_ref()?;
        Ok(match q {
            CL_SAMPLER_ADDRESSING_MODE => cl_prop::<cl_addressing_mode>(sampler.addressing_mode),
            CL_SAMPLER_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&sampler.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_SAMPLER_FILTER_MODE => cl_prop::<cl_filter_mode>(sampler.filter_mode),
            CL_SAMPLER_NORMALIZED_COORDS => cl_prop::<bool>(sampler.normalized_coords),
            CL_SAMPLER_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_sampler(
    context: cl_context,
    normalized_coords: cl_bool,
    addressing_mode: cl_addressing_mode,
    filter_mode: cl_filter_mode,
) -> CLResult<cl_sampler> {
    let c = context.get_arc()?;

    // CL_INVALID_OPERATION if images are not supported by any device associated with context (i.e.
    // CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    c.devs
        .iter()
        .find(|d| d.image_supported())
        .ok_or(CL_INVALID_OPERATION)?;

    // CL_INVALID_VALUE if addressing_mode, filter_mode, normalized_coords or a combination of these
    // arguements are not valid.
    validate_addressing_mode(addressing_mode)?;
    validate_filter_mode(filter_mode)?;

    let sampler = Sampler::new(
        &c,
        check_cl_bool(normalized_coords).ok_or(CL_INVALID_VALUE)?,
        addressing_mode,
        filter_mode,
    );
    Ok(cl_sampler::from_arc(sampler))
}

pub fn enqueue_write_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_write: cl_bool,
    offset: usize,
    cb: usize,
    ptr: *const ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = command_queue.get_arc()?;
    let b = buffer.get_arc()?;
    let block = check_cl_bool(blocking_write).ok_or(CL_INVALID_VALUE)?;

    // CL_INVALID_VALUE if the region being read or written specified by (offset, size) is out of
    // bounds or if ptr is a NULL value.
    if offset + cb > b.size || ptr.is_null() {
        Err(CL_INVALID_VALUE)?
    }

    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    // or if the context associated with command_queue and events in event_wait_list are not the
    // same.
    if b.context != q.context || evs.iter().find(|e| e.context != q.context).is_some() {
        Err(CL_INVALID_CONTEXT)?
    }

    // CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the read and write operations are blocking
    // and the execution status of any of the events in event_wait_list is a negative integer value.
    if block && evs.iter().find(|e| e.is_error()).is_some() {
        Err(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST)?
    }

    // CL_INVALID_OPERATION if clEnqueueWriteBuffer is called on buffer which has been created with
    // CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(b.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) {
        Err(CL_INVALID_OPERATION)?
    }

    let e = Event::new(
        &q,
        CL_COMMAND_WRITE_BUFFER,
        evs,
        Box::new(move |q| b.write_from_user(q, offset, ptr, cb)),
    );
    event.write_checked(cl_event::from_arc(e.clone()));
    q.queue(&e);
    if block {
        q.flush(true)?;
    }
    Ok(())

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue.
}

pub fn enqueue_read_buffer_rect(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_read: cl_bool,
    buffer_origin: *const usize,
    host_origin: *const usize,
    region: *const usize,
    mut buffer_row_pitch: usize,
    mut buffer_slice_pitch: usize,
    mut host_row_pitch: usize,
    mut host_slice_pitch: usize,
    ptr: *mut ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let block = check_cl_bool(blocking_read).ok_or(CL_INVALID_VALUE)?;
    let q = command_queue.get_arc()?;
    let buf = buffer.get_arc()?;
    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_OPERATION if clEnqueueReadBufferRect is called on buffer which has been created
    // with CL_MEM_HOST_WRITE_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(buf.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) {
        Err(CL_INVALID_OPERATION)?
    }

    // CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the read and write operations are blocking
    // and the execution status of any of the events in event_wait_list is a negative integer value.
    if block && evs.iter().find(|e| e.is_error()).is_some() {
        Err(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST)?
    }

    // CL_INVALID_VALUE if buffer_origin, host_origin, or region is NULL.
    if buffer_origin.is_null() ||
      host_origin.is_null() ||
      region.is_null() ||
      // CL_INVALID_VALUE if ptr is NULL.
      ptr.is_null()
    {
        Err(CL_INVALID_VALUE)?
    }

    let r = CLVec::from_raw_parts(region);
    let buf_ori = CLVec::from_raw_parts(buffer_origin);
    let host_ori = CLVec::from_raw_parts(host_origin);

    // CL_INVALID_VALUE if any region array element is 0.
    if r.contains(&0) ||
      // CL_INVALID_VALUE if buffer_row_pitch is not 0 and is less than region[0].
      buffer_row_pitch != 0 && buffer_row_pitch < r[0] ||
      // CL_INVALID_VALUE if host_row_pitch is not 0 and is less than region[0].
      host_row_pitch != 0 && host_row_pitch < r[0]
    {
        Err(CL_INVALID_VALUE)?
    }

    // If buffer_row_pitch is 0, buffer_row_pitch is computed as region[0].
    if buffer_row_pitch == 0 {
        buffer_row_pitch = r[0];
    }

    // If host_row_pitch is 0, host_row_pitch is computed as region[0].
    if host_row_pitch == 0 {
        host_row_pitch = r[0];
    }

    // CL_INVALID_VALUE if buffer_slice_pitch is not 0 and is less than region[1] × buffer_row_pitch and not a multiple of buffer_row_pitch.
    if buffer_slice_pitch != 0 && buffer_slice_pitch < r[1] * buffer_row_pitch && buffer_slice_pitch % buffer_row_pitch != 0 ||
      // CL_INVALID_VALUE if host_slice_pitch is not 0 and is less than region[1] × host_row_pitch and not a multiple of host_row_pitch.
      host_slice_pitch != 0 && host_slice_pitch < r[1] * host_row_pitch && host_slice_pitch % host_row_pitch != 0
    {
        Err(CL_INVALID_VALUE)?
    }

    // If buffer_slice_pitch is 0, buffer_slice_pitch is computed as region[1] × buffer_row_pitch.
    if buffer_slice_pitch == 0 {
        buffer_slice_pitch = r[1] * buffer_row_pitch;
    }

    // If host_slice_pitch is 0, host_slice_pitch is computed as region[1] × host_row_pitch.
    if host_slice_pitch == 0 {
        host_slice_pitch = r[1] * host_row_pitch
    }

    // CL_INVALID_VALUE if the region being read or written specified by (buffer_origin, region,
    // buffer_row_pitch, buffer_slice_pitch) is out of bounds.
    if (r + buf_ori) * [1, buffer_row_pitch, buffer_slice_pitch] > buf.size {
        Err(CL_INVALID_VALUE)?
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    // or if the context associated with command_queue and events in event_wait_list are not the
    // same.
    if q.context != buf.context || evs.iter().find(|e| e.context != q.context).is_some() {
        Err(CL_INVALID_CONTEXT)?
    }

    let e = Event::new(
        &q,
        CL_COMMAND_READ_BUFFER_RECT,
        evs,
        Box::new(move |q| {
            buf.read_to_user_rect(
                ptr,
                q,
                &r,
                &buf_ori,
                buffer_row_pitch,
                buffer_slice_pitch,
                &host_ori,
                host_row_pitch,
                host_slice_pitch,
            )
        }),
    );
    event.write_checked(cl_event::from_arc(e.clone()));
    q.queue(&e);
    if block {
        q.flush(true)?;
    }
    Ok(())

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue.
}

pub fn enqueue_write_buffer_rect(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_write: cl_bool,
    buffer_origin: *const usize,
    host_origin: *const usize,
    region: *const usize,
    mut buffer_row_pitch: usize,
    mut buffer_slice_pitch: usize,
    mut host_row_pitch: usize,
    mut host_slice_pitch: usize,
    ptr: *const ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let block = check_cl_bool(blocking_write).ok_or(CL_INVALID_VALUE)?;
    let q = command_queue.get_arc()?;
    let buf = buffer.get_arc()?;
    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_OPERATION if clEnqueueWriteBufferRect is called on buffer which has been created
    // with CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(buf.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) {
        Err(CL_INVALID_OPERATION)?
    }

    // CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the read and write operations are blocking
    // and the execution status of any of the events in event_wait_list is a negative integer value.
    if block && evs.iter().find(|e| e.is_error()).is_some() {
        Err(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST)?
    }

    // CL_INVALID_VALUE if buffer_origin, host_origin, or region is NULL.
    if buffer_origin.is_null() ||
      host_origin.is_null() ||
      region.is_null() ||
      // CL_INVALID_VALUE if ptr is NULL.
      ptr.is_null()
    {
        Err(CL_INVALID_VALUE)?
    }

    let r = CLVec::from_raw_parts(region);
    let buf_ori = CLVec::from_raw_parts(buffer_origin);
    let host_ori = CLVec::from_raw_parts(host_origin);

    // CL_INVALID_VALUE if any region array element is 0.
    if r.contains(&0) ||
      // CL_INVALID_VALUE if buffer_row_pitch is not 0 and is less than region[0].
      buffer_row_pitch != 0 && buffer_row_pitch < r[0] ||
      // CL_INVALID_VALUE if host_row_pitch is not 0 and is less than region[0].
      host_row_pitch != 0 && host_row_pitch < r[0]
    {
        Err(CL_INVALID_VALUE)?
    }

    // If buffer_row_pitch is 0, buffer_row_pitch is computed as region[0].
    if buffer_row_pitch == 0 {
        buffer_row_pitch = r[0];
    }

    // If host_row_pitch is 0, host_row_pitch is computed as region[0].
    if host_row_pitch == 0 {
        host_row_pitch = r[0];
    }

    // CL_INVALID_VALUE if buffer_slice_pitch is not 0 and is less than region[1] × buffer_row_pitch and not a multiple of buffer_row_pitch.
    if buffer_slice_pitch != 0 && buffer_slice_pitch < r[1] * buffer_row_pitch && buffer_slice_pitch % buffer_row_pitch != 0 ||
      // CL_INVALID_VALUE if host_slice_pitch is not 0 and is less than region[1] × host_row_pitch and not a multiple of host_row_pitch.
      host_slice_pitch != 0 && host_slice_pitch < r[1] * host_row_pitch && host_slice_pitch % host_row_pitch != 0
    {
        Err(CL_INVALID_VALUE)?
    }

    // If buffer_slice_pitch is 0, buffer_slice_pitch is computed as region[1] × buffer_row_pitch.
    if buffer_slice_pitch == 0 {
        buffer_slice_pitch = r[1] * buffer_row_pitch;
    }

    // If host_slice_pitch is 0, host_slice_pitch is computed as region[1] × host_row_pitch.
    if host_slice_pitch == 0 {
        host_slice_pitch = r[1] * host_row_pitch
    }

    // CL_INVALID_VALUE if the region being read or written specified by (buffer_origin, region,
    // buffer_row_pitch, buffer_slice_pitch) is out of bounds.
    if (r + buf_ori) * [1, buffer_row_pitch, buffer_slice_pitch] > buf.size {
        Err(CL_INVALID_VALUE)?
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    // or if the context associated with command_queue and events in event_wait_list are not the
    // same.
    if q.context != buf.context || evs.iter().find(|e| e.context != q.context).is_some() {
        Err(CL_INVALID_CONTEXT)?
    }

    let e = Event::new(
        &q,
        CL_COMMAND_WRITE_BUFFER_RECT,
        evs,
        Box::new(move |q| {
            buf.write_from_user_rect(
                ptr,
                q,
                &r,
                &host_ori,
                host_row_pitch,
                host_slice_pitch,
                &buf_ori,
                buffer_row_pitch,
                buffer_slice_pitch,
            )
        }),
    );
    event.write_checked(cl_event::from_arc(e.clone()));
    q.queue(&e);
    if block {
        q.flush(true)?;
    }
    Ok(())

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue.
}

pub fn enqueue_copy_buffer_rect(
    command_queue: cl_command_queue,
    src_buffer: cl_mem,
    dst_buffer: cl_mem,
    src_origin: *const usize,
    dst_origin: *const usize,
    region: *const usize,
    mut src_row_pitch: usize,
    mut src_slice_pitch: usize,
    mut dst_row_pitch: usize,
    mut dst_slice_pitch: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = command_queue.get_arc()?;
    let src = src_buffer.get_arc()?;
    let dst = dst_buffer.get_arc()?;
    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_VALUE if src_origin, dst_origin, or region is NULL.
    if src_origin.is_null() || dst_origin.is_null() || region.is_null() {
        Err(CL_INVALID_VALUE)?
    }

    let r = CLVec::from_raw_parts(region);
    let src_ori = CLVec::from_raw_parts(src_origin);
    let dst_ori = CLVec::from_raw_parts(dst_origin);

    // CL_INVALID_VALUE if any region array element is 0.
    if r.contains(&0) ||
      // CL_INVALID_VALUE if src_row_pitch is not 0 and is less than region[0].
      src_row_pitch != 0 && src_row_pitch < r[0] ||
      // CL_INVALID_VALUE if dst_row_pitch is not 0 and is less than region[0].
      dst_row_pitch != 0 && dst_row_pitch < r[0]
    {
        Err(CL_INVALID_VALUE)?
    }

    // If src_row_pitch is 0, src_row_pitch is computed as region[0].
    if src_row_pitch == 0 {
        src_row_pitch = r[0];
    }

    // If dst_row_pitch is 0, dst_row_pitch is computed as region[0].
    if dst_row_pitch == 0 {
        dst_row_pitch = r[0];
    }

    // CL_INVALID_VALUE if src_slice_pitch is not 0 and is less than region[1] × src_row_pitch
    if src_slice_pitch != 0 && src_slice_pitch < r[1] * src_row_pitch ||
      // CL_INVALID_VALUE if dst_slice_pitch is not 0 and is less than region[1] × dst_row_pitch
      dst_slice_pitch != 0 && dst_slice_pitch < r[1] * dst_row_pitch ||
      // if src_slice_pitch is not 0 and is not a multiple of src_row_pitch.
      src_slice_pitch != 0 && src_slice_pitch % src_row_pitch != 0 ||
      // if dst_slice_pitch is not 0 and is not a multiple of dst_row_pitch.
      dst_slice_pitch != 0 && dst_slice_pitch % dst_row_pitch != 0
    {
        Err(CL_INVALID_VALUE)?
    }

    // If src_slice_pitch is 0, src_slice_pitch is computed as region[1] × src_row_pitch.
    if src_slice_pitch == 0 {
        src_slice_pitch = r[1] * src_row_pitch;
    }

    // If dst_slice_pitch is 0, dst_slice_pitch is computed as region[1] × dst_row_pitch.
    if dst_slice_pitch == 0 {
        dst_slice_pitch = r[1] * dst_row_pitch;
    }

    // CL_INVALID_VALUE if src_buffer and dst_buffer are the same buffer object and src_slice_pitch
    // is not equal to dst_slice_pitch and src_row_pitch is not equal to dst_row_pitch.
    if src_buffer == dst_buffer
        && src_slice_pitch != dst_slice_pitch
        && src_row_pitch != dst_row_pitch
    {
        Err(CL_INVALID_VALUE)?
    }

    // CL_INVALID_VALUE if (src_origin, region, src_row_pitch, src_slice_pitch) or (dst_origin,
    // region, dst_row_pitch, dst_slice_pitch) require accessing elements outside the src_buffer
    // and dst_buffer buffer objects respectively.
    if (src_ori + r) * [1, src_row_pitch, src_slice_pitch] > src.size
        || (dst_ori + r) * [1, dst_row_pitch, dst_slice_pitch] > dst.size
    {
        Err(CL_INVALID_VALUE)?
    }

    // CL_MEM_COPY_OVERLAP if src_buffer and dst_buffer are the same buffer or sub-buffer object and
    // the source and destination regions overlap or if src_buffer and dst_buffer are different
    // sub-buffers of the same associated buffer object and they overlap.
    if src.has_same_parent(&dst)
        && check_copy_overlap(
            &src_ori,
            src.offset,
            &dst_ori,
            dst.offset,
            &r,
            src_row_pitch,
            src_slice_pitch,
        )
    {
        Err(CL_MEM_COPY_OVERLAP)?
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue, src_buffer and dst_buffer
    // are not the same or if the context associated with command_queue and events in
    // event_wait_list are not the same.
    if src.context != q.context
        || dst.context != q.context
        || evs.iter().find(|e| e.context != q.context).is_some()
    {
        Err(CL_INVALID_CONTEXT)?
    }

    let e = Event::new(
        &q,
        CL_COMMAND_COPY_BUFFER_RECT,
        evs,
        Box::new(move |q| {
            src.copy_to(
                &dst,
                q,
                &r,
                &src_ori,
                src_row_pitch,
                src_slice_pitch,
                &dst_ori,
                dst_row_pitch,
                dst_slice_pitch,
            )
        }),
    );
    event.write_checked(cl_event::from_arc(e.clone()));
    q.queue(&e);
    Ok(())

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if src_buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue.
}

pub fn enqueue_map_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_map: cl_bool,
    map_flags: cl_map_flags,
    offset: usize,
    size: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<*mut c_void> {
    let q = command_queue.get_arc()?;
    let b = buffer.get_arc()?;
    let block = check_cl_bool(blocking_map).ok_or(CL_INVALID_VALUE)?;

    // CL_INVALID_VALUE if region being mapped given by (offset, size) is out of bounds or if size
    // is 0
    if offset + size > b.size || size == 0 {
        Err(CL_INVALID_VALUE)?
    }

    // CL_INVALID_VALUE ... if values specified in map_flags are not valid.
    let valid_flags =
        cl_bitfield::from(CL_MAP_READ | CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION);
    let read_write_group = cl_bitfield::from(CL_MAP_READ | CL_MAP_WRITE);
    let invalidate_group = cl_bitfield::from(CL_MAP_WRITE_INVALIDATE_REGION);

    if (map_flags & !valid_flags != 0)
        || ((map_flags & read_write_group != 0) && (map_flags & invalidate_group != 0))
    {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_OPERATION if buffer has been created with CL_MEM_HOST_WRITE_ONLY or
    // CL_MEM_HOST_NO_ACCESS and CL_MAP_READ is set in map_flags
    if bit_check(b.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) &&
      bit_check(map_flags, CL_MAP_READ) ||
      // or if buffer has been created with CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS and
      // CL_MAP_WRITE or CL_MAP_WRITE_INVALIDATE_REGION is set in map_flags.
      bit_check(b.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) &&
      bit_check(map_flags, CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION)
    {
        Err(CL_INVALID_OPERATION)?
    }

    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the map operation is blocking and the
    // execution status of any of the events in event_wait_list is a negative integer value.
    if block && evs.iter().find(|e| e.is_error()).is_some() {
        Err(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST)?
    }

    // CL_INVALID_CONTEXT if context associated with command_queue and buffer are not the same or
    // if the context associated with command_queue and events in event_wait_list are not the same.
    if b.context != q.context || evs.iter().find(|e| e.context != q.context).is_some() {
        Err(CL_INVALID_CONTEXT)?
    }

    if !block || num_events_in_wait_list > 0 || !event.is_null() {
        println!("enqueue_map_buffer not implemented");
        Err(CL_MAP_FAILURE)?
    }

    if block {
        q.flush(true)?;
    }

    Ok(b.map(&q, offset, size))
    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for the device associated with queue. This error code is missing before version 1.1.
    // CL_MAP_FAILURE if there is a failure to map the requested region into the host address space. This error cannot occur for buffer objects created with CL_MEM_USE_HOST_PTR or CL_MEM_ALLOC_HOST_PTR.
    // CL_INVALID_OPERATION if mapping would lead to overlapping regions being mapped for writing.
}

pub fn enqueue_unmap_mem_object(
    command_queue: cl_command_queue,
    memobj: cl_mem,
    mapped_ptr: *mut ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = command_queue.get_arc()?;
    let m = memobj.get_ref()?;
    let evs = event_list_from_cl(num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and memobj are not the same or if
    // the context associated with command_queue and events in event_wait_list are not the same.
    if q.context != m.context || evs.iter().find(|e| e.context != q.context).is_some() {
        Err(CL_INVALID_CONTEXT)?
    }

    // CL_INVALID_VALUE if mapped_ptr is not a valid pointer returned by clEnqueueMapBuffer or
    // clEnqueueMapImage for memobj.
    if !m.unmap(mapped_ptr) {
        Err(CL_INVALID_VALUE)?
    }

    if num_events_in_wait_list > 0 || !event.is_null() {
        println!("enqueue_unmap_mem_object not implemented");
        Err(CL_OUT_OF_HOST_MEMORY)?
    }

    Ok(())
}
