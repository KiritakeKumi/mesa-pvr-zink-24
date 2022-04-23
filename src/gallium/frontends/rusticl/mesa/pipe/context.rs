extern crate mesa_rust_gen;

use crate::compiler::nir::*;
use crate::pipe::fence::*;
use crate::pipe::resource::*;
use crate::pipe::screen::*;
use crate::pipe::transfer::*;

use self::mesa_rust_gen::*;

use std::os::raw::*;
use std::ptr;
use std::ptr::*;
use std::sync::Arc;

pub struct PipeContext {
    pipe: NonNull<pipe_context>,
    screen: Arc<PipeScreen>,
}

impl PipeContext {
    pub(super) fn new(context: *mut pipe_context, screen: &Arc<PipeScreen>) -> Option<Arc<Self>> {
        let s = Self {
            pipe: NonNull::new(context)?,
            screen: screen.clone(),
        };

        if !has_required_cbs(unsafe { s.pipe.as_ref() }) {
            assert!(false, "Context missing features. This should never happen!");
            return None;
        }

        Some(Arc::new(s))
    }

    pub fn buffer_subdata(
        &self,
        res: &PipeResource,
        offset: c_uint,
        data: *const c_void,
        size: c_uint,
    ) {
        unsafe {
            self.pipe.as_ref().buffer_subdata.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                pipe_map_flags::PIPE_MAP_WRITE.0, // TODO PIPE_MAP_x
                offset,
                size,
                data,
            )
        }
    }

    pub fn resource_copy_region(
        &self,
        src: &PipeResource,
        src_offset: i32,
        dst: &PipeResource,
        dst_offset: u32,
        size: i32,
    ) {
        let b = pipe_box {
            width: size,
            height: 1,
            depth: 1,
            x: src_offset,
            ..Default::default()
        };

        unsafe {
            self.pipe.as_ref().resource_copy_region.unwrap()(
                self.pipe.as_ptr(),
                dst.pipe(),
                0,
                dst_offset,
                0,
                0,
                src.pipe(),
                0,
                &b,
            )
        }
    }

    pub fn buffer_map(
        self: &Arc<Self>,
        res: &PipeResource,
        offset: i32,
        size: i32,
        block: bool,
    ) -> PipeTransfer {
        let mut b = pipe_box::default();
        let mut out: *mut pipe_transfer = ptr::null_mut();

        b.x = offset;
        b.width = size;
        b.height = 1;
        b.depth = 1;

        let flags = match block {
            false => pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED,
            true => pipe_map_flags(0),
        };

        let ptr = unsafe {
            self.pipe.as_ref().buffer_map.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                0,
                flags.0,
                &b,
                &mut out,
            )
        };

        PipeTransfer::new(out, ptr, self)
    }

    pub(super) fn buffer_unmap(&self, tx: *mut pipe_transfer) {
        unsafe { self.pipe.as_ref().buffer_unmap.unwrap()(self.pipe.as_ptr(), tx) };
    }

    pub fn blit(&self, src: &PipeResource, dst: &PipeResource) {
        let mut blit_info = pipe_blit_info::default();
        blit_info.src.resource = src.pipe();
        blit_info.dst.resource = dst.pipe();

        println!("blit not implemented!");

        unsafe { self.pipe.as_ref().blit.unwrap()(self.pipe.as_ptr(), &blit_info) }
    }

    pub fn create_compute_state(
        &self,
        nir: &NirShader,
        input_mem: u32,
        local_mem: u32,
    ) -> *mut c_void {
        let state = pipe_compute_state {
            ir_type: pipe_shader_ir::PIPE_SHADER_IR_NIR,
            prog: nir.dup_for_driver().cast(),
            req_input_mem: input_mem,
            req_local_mem: local_mem,
            req_private_mem: 0,
        };
        unsafe { self.pipe.as_ref().create_compute_state.unwrap()(self.pipe.as_ptr(), &state) }
    }

    pub fn bind_compute_state(&self, state: *mut c_void) {
        unsafe { self.pipe.as_ref().bind_compute_state.unwrap()(self.pipe.as_ptr(), state) }
    }

    pub fn delete_compute_state(&self, state: *mut c_void) {
        unsafe { self.pipe.as_ref().delete_compute_state.unwrap()(self.pipe.as_ptr(), state) }
    }

    pub fn launch_grid(
        &self,
        work_dim: u32,
        block: [u32; 3],
        grid: [u32; 3],
        grid_base: [u32; 3],
        input: &[u8],
    ) {
        let info = pipe_grid_info {
            pc: 0,
            input: input.as_ptr().cast(),
            work_dim: work_dim,
            block: block,
            last_block: [0; 3],
            grid: grid,
            grid_base: grid_base,
            indirect: ptr::null_mut(),
            indirect_offset: 0,
        };
        unsafe { self.pipe.as_ref().launch_grid.unwrap()(self.pipe.as_ptr(), &info) }
    }

    pub fn set_global_binding(&self, res: &[Option<Arc<PipeResource>>], out: &mut [*mut u32]) {
        let mut res: Vec<_> = res
            .iter()
            .map(|o| o.as_ref().map_or(ptr::null_mut(), |r| r.pipe()))
            .collect();
        unsafe {
            self.pipe.as_ref().set_global_binding.unwrap()(
                self.pipe.as_ptr(),
                0,
                res.len() as u32,
                res.as_mut_ptr(),
                out.as_mut_ptr(),
            )
        }
    }

    pub fn clear_global_binding(&self, count: u32) {
        unsafe {
            self.pipe.as_ref().set_global_binding.unwrap()(
                self.pipe.as_ptr(),
                0,
                count,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        }
    }

    pub fn memory_barrier(&self, barriers: u32) {
        unsafe { self.pipe.as_ref().memory_barrier.unwrap()(self.pipe.as_ptr(), barriers) }
    }

    pub fn flush(&self) -> PipeFence {
        unsafe {
            let mut fence = ptr::null_mut();
            self.pipe.as_ref().flush.unwrap()(self.pipe.as_ptr(), &mut fence, 0);
            PipeFence::new(fence, &self.screen)
        }
    }
}

impl Drop for PipeContext {
    fn drop(&mut self) {
        unsafe {
            self.pipe.as_ref().destroy.unwrap()(self.pipe.as_ptr());
        }
    }
}

fn has_required_cbs(c: &pipe_context) -> bool {
    c.destroy.is_some()
        && c.bind_compute_state.is_some()
        && c.blit.is_some()
        && c.buffer_map.is_some()
        && c.buffer_subdata.is_some()
        && c.buffer_unmap.is_some()
        && c.create_compute_state.is_some()
        && c.delete_compute_state.is_some()
        && c.flush.is_some()
        && c.launch_grid.is_some()
        && c.memory_barrier.is_some()
        && c.resource_copy_region.is_some()
        && c.set_global_binding.is_some()
}
