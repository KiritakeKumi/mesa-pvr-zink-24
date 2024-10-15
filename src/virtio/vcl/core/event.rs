/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::Context;
use crate::dev::renderer::Vcl;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::slice;
use std::sync::Arc;

impl_cl_type_trait!(cl_event, Event, CL_INVALID_EVENT);

pub struct Event {
    base: CLObjectBase<CL_INVALID_EVENT>,
    pub context: Arc<Context>,
}

impl Event {
    pub fn new(context: &Arc<Context>) -> Arc<Event> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
        })
    }

    pub fn new_user(context: &Arc<Context>) -> CLResult<Arc<Event>> {
        let event = Self::new(context);
        Vcl::get().call_clCreateUserEventMESA(context.get_handle(), &mut event.get_handle())?;
        Ok(event)
    }

    pub fn from_cl_arr(events: *const cl_event, num_events: u32) -> CLResult<Vec<Arc<Event>>> {
        if !events.is_null() && num_events > 0 {
            let s = unsafe { slice::from_raw_parts(events, num_events as usize) };
            s.iter().map(|e| e.get_arc()).collect()
        } else {
            Ok(Vec::default())
        }
    }
}
