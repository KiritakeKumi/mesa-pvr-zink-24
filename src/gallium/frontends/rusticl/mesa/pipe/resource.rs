extern crate mesa_rust_gen;

use self::mesa_rust_gen::*;

use std::ptr;

pub struct PipeResource {
    pipe: *mut pipe_resource,
}

impl PipeResource {
    pub fn new(res: *mut pipe_resource) -> Option<Self> {
        if res.is_null() {
            return None;
        }

        Some(Self { pipe: res })
    }

    pub(super) fn pipe(&self) -> *mut pipe_resource {
        self.pipe
    }

    fn as_ref(&self) -> &pipe_resource {
        unsafe { self.pipe.as_ref().unwrap() }
    }

    pub fn pipe_image_view(&self) -> pipe_image_view {
        let u = if self.as_ref().target() == pipe_texture_target::PIPE_BUFFER {
            pipe_image_view__bindgen_ty_1 {
                buf: pipe_image_view__bindgen_ty_1__bindgen_ty_2 {
                    offset: 0,
                    size: self.as_ref().width0,
                },
            }
        } else {
            let mut tex = pipe_image_view__bindgen_ty_1__bindgen_ty_1::default();
            let mut array_size = self.as_ref().array_size;

            if array_size != 0 {
                array_size -= 1;
            }

            tex.set_first_layer(0);
            tex.set_last_layer(array_size.into());
            tex.set_level(0);

            pipe_image_view__bindgen_ty_1 { tex: tex }
        };

        pipe_image_view {
            resource: self.pipe(),
            format: self.as_ref().format(),
            access: 0,
            shader_access: PIPE_IMAGE_ACCESS_WRITE as u16,
            u: u,
        }
    }

    pub fn pipe_sampler_view_template(&self) -> pipe_sampler_view {
        let mut res = pipe_sampler_view::default();
        unsafe {
            u_sampler_view_default_template(&mut res, self.pipe, self.as_ref().format());
        }
        res
    }
}

impl Drop for PipeResource {
    fn drop(&mut self) {
        unsafe { pipe_resource_reference(&mut self.pipe, ptr::null_mut()) }
    }
}
