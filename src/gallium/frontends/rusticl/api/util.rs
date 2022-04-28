extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::types::*;
use crate::core::event::*;

use self::mesa_rust_util::ptr::CheckedPtr;
use self::rusticl_opencl_gen::*;

use std::cmp;
use std::convert::TryInto;
use std::ffi::CStr;
use std::ffi::CString;
use std::mem::size_of;
use std::ops::BitAnd;
use std::os::raw::c_void;
use std::slice;
use std::sync::Arc;

pub trait CLInfo<I> {
    fn query(&self, q: I) -> Result<Vec<u8>, cl_int>;

    fn get_info(
        &self,
        param_name: I,
        param_value_size: usize,
        param_value: *mut ::std::os::raw::c_void,
        param_value_size_ret: *mut usize,
    ) -> Result<(), cl_int> {
        let d = self.query(param_name)?;
        let size: usize = d.len();

        // CL_INVALID_VALUE [...] if size in bytes specified by param_value_size is < size of return
        // type as specified in the Context Attributes table and param_value is not a NULL value.
        if param_value_size < size && !param_value.is_null() {
            return Err(CL_INVALID_VALUE);
        }

        // param_value_size_ret returns the actual size in bytes of data being queried by param_name.
        // If param_value_size_ret is NULL, it is ignored.
        param_value_size_ret.write_checked(size);

        // param_value is a pointer to memory where the appropriate result being queried is returned.
        // If param_value is NULL, it is ignored.
        unsafe {
            param_value.copy_checked(d.as_ptr() as *const c_void, size);
        }

        Ok(())
    }
}

pub trait CLInfoObj<I, O> {
    fn query(&self, o: O, q: I) -> Result<Vec<u8>, cl_int>;

    fn get_info_obj(
        &self,
        obj: O,
        param_name: I,
        param_value_size: usize,
        param_value: *mut ::std::os::raw::c_void,
        param_value_size_ret: *mut usize,
    ) -> Result<(), cl_int> {
        let d = self.query(obj, param_name)?;
        let size: usize = d.len();

        // CL_INVALID_VALUE [...] if size in bytes specified by param_value_size is < size of return
        // type as specified in the Context Attributes table and param_value is not a NULL value.
        if param_value_size < size && !param_value.is_null() {
            return Err(CL_INVALID_VALUE);
        }

        // param_value_size_ret returns the actual size in bytes of data being queried by param_name.
        // If param_value_size_ret is NULL, it is ignored.
        param_value_size_ret.write_checked(size);

        // param_value is a pointer to memory where the appropriate result being queried is returned.
        // If param_value is NULL, it is ignored.
        unsafe {
            param_value.copy_checked(d.as_ptr() as *const c_void, size);
        }

        Ok(())
    }
}

pub trait CLProp {
    fn cl_vec(&self) -> Vec<u8>;
}

macro_rules! cl_prop_for_type {
    ($ty: ty) => {
        impl CLProp for $ty {
            fn cl_vec(&self) -> Vec<u8> {
                self.to_ne_bytes().to_vec()
            }
        }
    };
}

macro_rules! cl_prop_for_struct {
    ($ty: ty) => {
        impl CLProp for $ty {
            fn cl_vec(&self) -> Vec<u8> {
                unsafe {
                    slice::from_raw_parts((self as *const Self) as *const u8, size_of::<Self>())
                }
                .to_vec()
            }
        }
    };
}

cl_prop_for_type!(cl_char);
cl_prop_for_type!(cl_int);
cl_prop_for_type!(cl_uint);
cl_prop_for_type!(cl_ulong);
cl_prop_for_type!(isize);
cl_prop_for_type!(usize);

cl_prop_for_struct!(cl_image_format);
cl_prop_for_struct!(cl_name_version);

impl CLProp for bool {
    fn cl_vec(&self) -> Vec<u8> {
        cl_prop::<cl_bool>(if *self { CL_TRUE } else { CL_FALSE })
    }
}

impl CLProp for String {
    fn cl_vec(&self) -> Vec<u8> {
        let mut c = self.clone();
        c.push('\0');
        c.into_bytes()
    }
}

impl CLProp for &str {
    fn cl_vec(&self) -> Vec<u8> {
        CString::new(*self)
            .or_else(|_| CString::new(b"\0".to_vec()))
            .unwrap()
            .into_bytes_with_nul()
    }
}

impl CLProp for &CStr {
    fn cl_vec(&self) -> Vec<u8> {
        self.to_bytes_with_nul().to_vec()
    }
}

impl<T> CLProp for Vec<T>
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<u8> {
        let mut res: Vec<u8> = Vec::new();
        for i in self {
            res.append(&mut i.cl_vec())
        }
        res
    }
}

impl<T> CLProp for &Vec<T>
where
    T: CLProp,
{
    fn cl_vec(&self) -> Vec<u8> {
        let mut res: Vec<u8> = Vec::new();
        for i in *self {
            res.append(&mut i.cl_vec())
        }
        res
    }
}

impl<T> CLProp for *const T {
    fn cl_vec(&self) -> Vec<u8> {
        (*self as usize).cl_vec()
    }
}

impl<T> CLProp for *mut T {
    fn cl_vec(&self) -> Vec<u8> {
        (*self as usize).cl_vec()
    }
}

pub fn cl_prop<T: CLProp>(v: T) -> Vec<u8> {
    v.cl_vec()
}

const CL_DEVICE_TYPES: u32 = CL_DEVICE_TYPE_ACCELERATOR
    | CL_DEVICE_TYPE_CPU
    | CL_DEVICE_TYPE_GPU
    | CL_DEVICE_TYPE_CUSTOM
    | CL_DEVICE_TYPE_DEFAULT;

pub fn check_cl_device_type(val: cl_device_type) -> Result<(), cl_int> {
    let v: u32 = val.try_into().or(Err(CL_INVALID_DEVICE_TYPE))?;
    if v == CL_DEVICE_TYPE_ALL || v & CL_DEVICE_TYPES == v {
        return Ok(());
    }
    Err(CL_INVALID_DEVICE_TYPE)
}

pub const CL_IMAGE_TYPES: [cl_mem_object_type; 6] = [
    CL_MEM_OBJECT_IMAGE1D,
    CL_MEM_OBJECT_IMAGE2D,
    CL_MEM_OBJECT_IMAGE3D,
    CL_MEM_OBJECT_IMAGE1D_ARRAY,
    CL_MEM_OBJECT_IMAGE2D_ARRAY,
    CL_MEM_OBJECT_IMAGE1D_BUFFER,
];

pub const fn cl_image_format(
    order: cl_channel_order,
    data_type: cl_channel_type,
) -> cl_image_format {
    cl_image_format {
        image_channel_order: order,
        image_channel_data_type: data_type,
    }
}

pub fn check_cl_bool<T: PartialEq + TryInto<cl_uint>>(val: T) -> Option<bool> {
    let c: u32 = val.try_into().ok()?;
    if c != CL_TRUE && c != CL_FALSE {
        return None;
    }
    Some(c == CL_TRUE)
}

pub fn event_list_from_cl<'a>(
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
) -> Result<Vec<Arc<Event>>, cl_int> {
    // CL_INVALID_EVENT_WAIT_LIST if event_wait_list is NULL and num_events_in_wait_list > 0, or
    // event_wait_list is not NULL and num_events_in_wait_list is 0, or if event objects in
    // event_wait_list are not valid events.
    if event_wait_list.is_null() && num_events_in_wait_list > 0
        || !event_wait_list.is_null() && num_events_in_wait_list == 0
    {
        Err(CL_INVALID_EVENT_WAIT_LIST)?
    }

    Ok(Event::from_cl_arr(
        event_wait_list,
        num_events_in_wait_list,
    )?)
}

pub fn check_cb<T>(cb: &Option<T>, user_data: *mut c_void) -> Result<(), cl_int> {
    // CL_INVALID_VALUE if pfn_notify is NULL but user_data is not NULL.
    if cb.is_none() && !user_data.is_null() {
        Err(CL_INVALID_VALUE)?;
    }

    Ok(())
}

pub fn checked_compare(a: usize, o: cmp::Ordering, b: u64) -> bool {
    if usize::BITS > u64::BITS {
        a.cmp(&(b as usize)) == o
    } else {
        (a as u64).cmp(&b) == o
    }
}

pub fn is_alligned<T>(ptr: *const T, alignment: usize) -> bool {
    ptr as usize & (alignment - 1) == 0
}

pub fn bit_check<A: BitAnd<Output = A> + PartialEq + Default, B: Into<A>>(a: A, b: B) -> bool {
    a & b.into() != A::default()
}

// Taken from "Appendix D: Checking for Memory Copy Overlap"
// src_offset and dst_offset are additions to support sub-buffers
pub fn check_copy_overlap(
    src_origin: &CLVec<usize>,
    src_offset: usize,
    dst_origin: &CLVec<usize>,
    dst_offset: usize,
    region: &CLVec<usize>,
    row_pitch: usize,
    slice_pitch: usize,
) -> bool {
    let slice_size = (region[1] - 1) * row_pitch + region[0];
    let block_size = (region[2] - 1) * slice_pitch + slice_size;
    let src_start =
        src_origin[2] * slice_pitch + src_origin[1] * row_pitch + src_origin[0] + src_offset;
    let src_end = src_start + block_size;
    let dst_start =
        dst_origin[2] * slice_pitch + dst_origin[1] * row_pitch + dst_origin[0] + dst_offset;
    let dst_end = dst_start + block_size;

    /* No overlap if dst ends before src starts or if src ends
     * before dst starts.
     */
    if (dst_end <= src_start) || (src_end <= dst_start) {
        return false;
    }

    /* No overlap if region[0] for dst or src fits in the gap
     * between region[0] and row_pitch.
     */
    {
        let src_dx = (src_origin[0] + src_offset) % row_pitch;
        let dst_dx = (dst_origin[0] + dst_offset) % row_pitch;
        if ((dst_dx >= src_dx + region[0]) && (dst_dx + region[0] <= src_dx + row_pitch))
            || ((src_dx >= dst_dx + region[0]) && (src_dx + region[0] <= dst_dx + row_pitch))
        {
            return false;
        }
    }

    /* No overlap if region[1] for dst or src fits in the gap
     * between region[1] and slice_pitch.
     */
    {
        let src_dy = (src_origin[1] * row_pitch + src_origin[0] + src_offset) % slice_pitch;
        let dst_dy = (dst_origin[1] * row_pitch + dst_origin[0] + dst_offset) % slice_pitch;
        if ((dst_dy >= src_dy + slice_size) && (dst_dy + slice_size <= src_dy + slice_pitch))
            || ((src_dy >= dst_dy + slice_size) && (src_dy + slice_size <= dst_dy + slice_pitch))
        {
            return false;
        }
    }

    /* Otherwise src and dst overlap. */
    true
}
