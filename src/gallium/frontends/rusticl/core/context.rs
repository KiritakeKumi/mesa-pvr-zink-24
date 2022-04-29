extern crate mesa_rust;
extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::device::*;
use crate::core::format::*;
use crate::core::util::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::resource::*;
use self::mesa_rust_util::properties::Properties;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::sync::Arc;
use std::sync::Mutex;

pub struct Context {
    pub base: CLObjectBase<CL_INVALID_CONTEXT>,
    pub devs: Vec<Arc<Device>>,
    pub properties: Properties<cl_context_properties>,
    pub dtors: Mutex<Vec<Box<dyn Fn(cl_context) -> ()>>>,
}

impl_cl_type_trait!(cl_context, Context, CL_INVALID_CONTEXT);

impl Context {
    pub fn new(
        devs: Vec<Arc<Device>>,
        properties: Properties<cl_context_properties>,
    ) -> Arc<Context> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            devs: devs,
            properties: properties,
            dtors: Mutex::new(Vec::new()),
        })
    }

    pub fn create_buffer(&self, size: usize) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer(adj_size)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }

    pub fn create_buffer_from_user(
        &self,
        size: usize,
        user_ptr: *mut c_void,
    ) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer_from_user(adj_size, user_ptr)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }

    pub fn create_texture(
        &self,
        desc: &cl_image_desc,
        format: &cl_image_format,
    ) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let width = desc
            .image_width
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let height = desc
            .image_height
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let depth = desc
            .image_depth
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let array_size = desc
            .image_array_size
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let target = cl_mem_type_to_texture_target(desc.image_type);
        let format = format.to_pipe_format().unwrap();

        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_texture(width, height, depth, array_size, target, format)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }

    pub fn create_texture_from_user(
        &self,
        desc: &cl_image_desc,
        format: &cl_image_format,
        user_ptr: *mut c_void,
    ) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let width = desc
            .image_width
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let height = desc
            .image_height
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let depth = desc
            .image_depth
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let array_size = desc
            .image_array_size
            .try_into()
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let target = cl_mem_type_to_texture_target(desc.image_type);
        let format = format.to_pipe_format().unwrap();

        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_texture_from_user(
                    width, height, depth, array_size, target, format, user_ptr,
                )
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        let cl = cl_context::from_ptr(self);
        self.dtors
            .lock()
            .unwrap()
            .iter()
            .rev()
            .for_each(|cb| cb(cl));
    }
}
