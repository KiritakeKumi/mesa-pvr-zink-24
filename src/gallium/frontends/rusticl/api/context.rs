extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::device::get_devs_for_type;
use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::context::*;

use self::mesa_rust_util::properties::Properties;
use self::rusticl_opencl_gen::*;

use std::collections::HashSet;
use std::iter::FromIterator;
use std::slice;

impl CLInfo<cl_context_info> for cl_context {
    fn query(&self, q: cl_context_info) -> Result<Vec<u8>, cl_int> {
        let ctx = self.get_ref()?;
        Ok(match q {
            CL_CONTEXT_DEVICES => {
                cl_prop::<&Vec<cl_device_id>>(&ctx.devs.iter().map(|d| d.cl).collect())
            }
            CL_CONTEXT_NUM_DEVICES => cl_prop::<cl_uint>(ctx.devs.len() as u32),
            CL_CONTEXT_PROPERTIES => cl_prop::<&Vec<cl_context_properties>>(&ctx.properties),
            CL_CONTEXT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

pub fn create_context(
    properties: *const cl_context_properties,
    num_devices: cl_uint,
    devices: *const cl_device_id,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> Result<cl_context, cl_int> {
    check_cb(&pfn_notify, user_data)?;

    // CL_INVALID_VALUE if devices is NULL.
    if devices.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if num_devices is equal to zero.
    if num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_PROPERTY [...] if the same property name is specified more than once.
    let props = Properties::from_ptr(properties).ok_or(CL_INVALID_PROPERTY)?;
    for p in props.props {
        match p.0 as u32 {
            // CL_INVALID_PLATFORM [...] if platform value specified in properties is not a valid platform.
            CL_CONTEXT_PLATFORM => {
                (p.1 as cl_platform_id).check()?;
            }
            CL_CONTEXT_INTEROP_USER_SYNC => {
                check_cl_bool(p.1).ok_or(CL_INVALID_PROPERTY)?;
            }
            // CL_INVALID_PROPERTY if context property name in properties is not a supported property name
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    // Duplicate devices specified in devices are ignored.
    let set: HashSet<_> =
        HashSet::from_iter(unsafe { slice::from_raw_parts(devices, num_devices as usize) }.iter());
    let devs: Result<_, _> = set.into_iter().map(cl_device_id::check).collect();

    Ok(cl_context::from_arc(Context::new(
        devs?,
        Properties::from_ptr_raw(properties),
    )))
}

pub fn create_context_from_type(
    properties: *const cl_context_properties,
    device_type: cl_device_type,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> Result<cl_context, cl_int> {
    // CL_INVALID_DEVICE_TYPE if device_type is not a valid value.
    check_cl_device_type(device_type)?;

    let devs: Vec<_> = get_devs_for_type(device_type)
        .iter()
        .map(|d| d.cl)
        .collect();

    // CL_DEVICE_NOT_FOUND if no devices that match device_type and property values specified in properties were found.
    if devs.is_empty() {
        return Err(CL_DEVICE_NOT_FOUND);
    }

    // errors are essentially the same and we will always pass in a valid
    // device list, so that's fine as well.
    create_context(
        properties,
        devs.len() as u32,
        devs.as_ptr(),
        pfn_notify,
        user_data,
    )
}
