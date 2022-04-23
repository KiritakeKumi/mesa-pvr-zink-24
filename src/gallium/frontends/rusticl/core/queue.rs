extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::context::*;
use self::rusticl_opencl_gen::*;

use std::sync::mpsc;
use std::sync::Arc;
use std::sync::Mutex;
use std::thread;
use std::thread::JoinHandle;

#[repr(C)]
pub struct Queue {
    pub base: CLObjectBase<CL_INVALID_COMMAND_QUEUE>,
    pub context: Arc<Context>,
    pub device: Arc<Device>,
    pub props: cl_command_queue_properties,
    pipe: Arc<PipeContext>,
    pending: Mutex<Vec<Arc<Event>>>,
    _thrd: Option<JoinHandle<()>>,
    chan_in: mpsc::Sender<Vec<Arc<Event>>>,
}

impl_cl_type_trait!(cl_command_queue, Queue, CL_INVALID_COMMAND_QUEUE);

impl Queue {
    pub fn new(
        context: &Arc<Context>,
        device: &Arc<Device>,
        props: cl_command_queue_properties,
    ) -> CLResult<Arc<Queue>> {
        let (tx_q, rx_t) = mpsc::channel::<Vec<Arc<Event>>>();
        Ok(Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            device: device.clone(),
            props: props,
            // we assume that memory allocation is the only possible failure. Any other failure reason
            // should be detected earlier (e.g.: checking for CAPs).
            pipe: device
                .screen()
                .create_context()
                .ok_or(CL_OUT_OF_HOST_MEMORY)?,
            pending: Mutex::new(Vec::new()),
            _thrd: Some(
                thread::Builder::new()
                    .name("rusticl queue thread".into())
                    .spawn(move || loop {
                        let r = rx_t.recv();
                        if r.is_err() {
                            break;
                        }
                        let new_events = r.unwrap();
                        for e in &new_events {
                            // all events should be processed, but we might have to wait on user
                            // events to happen
                            let err = e.deps.iter().map(|e| e.wait()).find(|s| *s < 0);
                            if let Some(err) = err {
                                // if a dependency failed, fail this event as well
                                e.set_user_status(err);
                            } else {
                                e.call();
                            }
                        }
                        for e in new_events {
                            e.wait();
                        }
                    })
                    .unwrap(),
            ),
            chan_in: tx_q,
        }))
    }

    pub fn context(&self) -> &Arc<PipeContext> {
        &self.pipe
    }

    pub fn queue(&self, e: &Arc<Event>) {
        self.pending.lock().unwrap().push(e.clone());
    }

    pub fn flush(&self, wait: bool) -> CLResult<()> {
        let mut p = self.pending.lock().unwrap();
        let last = p.last().map(|e| e.clone());
        // This should never ever error, but if it does return an error
        self.chan_in
            .send((*p).drain(0..).collect())
            .map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        if wait && last.is_some() {
            last.unwrap().wait();
        }
        Ok(())
    }
}

impl Drop for Queue {
    fn drop(&mut self) {
        // when deleting the application side object, we have to flush
        // From the OpenCL spec:
        // clReleaseCommandQueue performs an implicit flush to issue any previously queued OpenCL
        // commands in command_queue.
        // TODO: maybe we have to do it on every release?
        let _ = self.flush(true);
    }
}
