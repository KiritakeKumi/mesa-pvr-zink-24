extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::kernel::*;
use crate::core::program::*;

use self::mesa_rust_util::ptr::*;
use self::mesa_rust_util::string::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashSet;
use std::ptr;
use std::slice;
use std::sync::Arc;

impl CLInfo<cl_kernel_info> for cl_kernel {
    fn query(&self, q: cl_kernel_info, _: &[u8]) -> CLResult<Vec<u8>> {
        let kernel = self.get_ref()?;
        Ok(match q {
            CL_KERNEL_CONTEXT => {
                let ptr = Arc::as_ptr(&kernel.prog.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_KERNEL_FUNCTION_NAME => cl_prop::<&str>(&kernel.name),
            CL_KERNEL_NUM_ARGS => cl_prop::<cl_uint>(kernel.args.len() as cl_uint),
            CL_KERNEL_PROGRAM => {
                let ptr = Arc::as_ptr(&kernel.prog);
                cl_prop::<cl_program>(cl_program::from_ptr(ptr))
            }
            CL_KERNEL_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

impl CLInfoObj<cl_kernel_arg_info, cl_uint> for cl_kernel {
    fn query(&self, idx: cl_uint, q: cl_kernel_arg_info) -> CLResult<Vec<u8>> {
        let kernel = self.get_ref()?;
        Ok(match *q {
            CL_KERNEL_ARG_ACCESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_access_qualifier>(kernel.access_qualifier(idx))
            }
            CL_KERNEL_ARG_ADDRESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_address_qualifier>(kernel.address_qualifier(idx))
            }
            CL_KERNEL_ARG_NAME => cl_prop::<&str>(kernel.arg_name(idx)),
            CL_KERNEL_ARG_TYPE_NAME => cl_prop::<&str>(kernel.arg_type_name(idx)),
            CL_KERNEL_ARG_TYPE_QUALIFIER => {
                cl_prop::<cl_kernel_arg_type_qualifier>(kernel.type_qualifier(idx))
            }
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

impl CLInfoObj<cl_kernel_work_group_info, cl_device_id> for cl_kernel {
    fn query(&self, dev: cl_device_id, q: cl_kernel_work_group_info) -> CLResult<Vec<u8>> {
        let kernel = self.get_ref()?;
        let dev = dev.get_arc()?;
        Ok(match *q {
            CL_KERNEL_LOCAL_MEM_SIZE => cl_prop::<cl_ulong>(kernel.local_mem_size(&dev)),
            CL_KERNEL_PRIVATE_MEM_SIZE => cl_prop::<cl_ulong>(kernel.priv_mem_size(&dev)),
            // TODO
            CL_KERNEL_WORK_GROUP_SIZE => cl_prop::<usize>(1),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE)?,
        })
    }
}

const ZERO_ARR: [usize; 3] = [0; 3];
fn checked_kernel_work_arr(arr: *const usize, work_dim: cl_uint) -> &'static [usize] {
    if !arr.is_null() {
        unsafe { slice::from_raw_parts(arr, work_dim as usize) }
    } else {
        &ZERO_ARR
    }
}

fn get_devices_with_valid_build(p: &Arc<Program>) -> CLResult<Vec<&Arc<Device>>> {
    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for program.
    let devs: Vec<_> = p
        .devs
        .iter()
        .filter(|d| p.status(d) == CL_BUILD_SUCCESS as cl_build_status)
        .collect();
    if devs.is_empty() {
        Err(CL_INVALID_PROGRAM_EXECUTABLE)?;
    }
    Ok(devs)
}

pub fn create_kernel(
    program: cl_program,
    kernel_name: *const ::std::os::raw::c_char,
) -> CLResult<cl_kernel> {
    let p = program.get_arc()?;
    let name = c_string_to_string(kernel_name);

    // CL_INVALID_VALUE if kernel_name is NULL.
    if kernel_name.is_null() {
        Err(CL_INVALID_VALUE)?;
    }

    // CL_INVALID_KERNEL_NAME if kernel_name is not found in program.
    if !p.kernels().contains(&name) {
        Err(CL_INVALID_KERNEL_NAME)?;
    }

    // CL_INVALID_KERNEL_DEFINITION if the function definition for __kernel function given by
    // kernel_name such as the number of arguments, the argument types are not the same for all
    // devices for which the program executable has been built.
    let devs = get_devices_with_valid_build(&p)?;
    let nirs = p.nirs(&name);
    let kernel_args: HashSet<_> = devs.iter().map(|d| p.args(d, &name)).collect();
    if kernel_args.len() != 1 {
        Err(CL_INVALID_KERNEL_DEFINITION)?;
    }

    Ok(cl_kernel::from_arc(Kernel::new(
        name,
        p,
        nirs,
        kernel_args.into_iter().next().unwrap(),
    )))
}

pub fn create_kernels_in_program(
    program: cl_program,
    num_kernels: cl_uint,
    kernels: *mut cl_kernel,
    num_kernels_ret: *mut cl_uint,
) -> CLResult<()> {
    let p = program.get_arc()?;
    let devs = get_devices_with_valid_build(&p)?;

    // CL_INVALID_VALUE if kernels is not NULL and num_kernels is less than the number of kernels
    // in program.
    if !kernels.is_null() && p.kernels().len() > num_kernels as usize {
        Err(CL_INVALID_VALUE)?
    }

    let mut num_kernels = 0;
    for name in p.kernels() {
        let kernel_args: HashSet<_> = devs.iter().map(|d| p.args(d, &name)).collect();
        // Kernel objects are not created for any __kernel functions in program that do not have the
        // same function definition across all devices for which a program executable has been
        // successfully built.
        if kernel_args.len() != 1 {
            continue;
        }

        if !kernels.is_null() {
            // we just assume the client isn't stupid
            unsafe {
                let nirs = p.nirs(&name);
                kernels
                    .add(num_kernels as usize)
                    .write(cl_kernel::from_arc(Kernel::new(
                        name,
                        p.clone(),
                        nirs,
                        kernel_args.into_iter().next().unwrap(),
                    )));
            }
        }
        num_kernels += 1;
    }
    num_kernels_ret.write_checked(num_kernels);
    Ok(())
}

pub fn set_kernel_arg(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_size: usize,
    arg_value: *const ::std::os::raw::c_void,
) -> CLResult<()> {
    let k = kernel.get_arc()?;

    // CL_INVALID_ARG_INDEX if arg_index is not a valid argument index.
    if let Some(arg) = k.args.get(arg_index as usize) {
        // CL_INVALID_ARG_SIZE if arg_size does not match the size of the data type for an argument
        // that is not a memory object or if the argument is a memory object and
        // arg_size != sizeof(cl_mem) or if arg_size is zero and the argument is declared with the
        // local qualifier or if the argument is a sampler and arg_size != sizeof(cl_sampler).
        match arg.kind {
            KernelArgType::MemLocal => {
                if arg_size == 0 {
                    Err(CL_INVALID_ARG_SIZE)?;
                }
            }
            _ => {
                if arg.size != arg_size {
                    Err(CL_INVALID_ARG_SIZE)?;
                }
            }
        }

        // CL_INVALID_ARG_VALUE if arg_value specified is not a valid value.
        match arg.kind {
            // If the argument is declared with the local qualifier, the arg_value entry must be
            // NULL.
            KernelArgType::MemLocal => {
                if !arg_value.is_null() {
                    Err(CL_INVALID_ARG_VALUE)?;
                }
            }
            // If the argument is of type sampler_t, the arg_value entry must be a pointer to the
            // sampler object.
            KernelArgType::Constant | KernelArgType::Sampler => {
                if arg_value.is_null() {
                    Err(CL_INVALID_ARG_VALUE)?;
                }
            }
            _ => {}
        };

        // let's create the arg now
        let arg = unsafe {
            if arg.dead {
                KernelArgValue::None
            } else {
                match arg.kind {
                    KernelArgType::Constant => KernelArgValue::Constant(
                        slice::from_raw_parts(arg_value.cast(), arg_size).to_vec(),
                    ),
                    KernelArgType::MemConstant | KernelArgType::MemGlobal => {
                        let ptr: *const cl_mem = arg_value.cast();
                        if ptr.is_null() || (*ptr).is_null() {
                            KernelArgValue::None
                        } else {
                            KernelArgValue::MemObject((*ptr).get_ref()?)
                        }
                    }
                    KernelArgType::MemLocal => KernelArgValue::LocalMem(arg_size),
                    KernelArgType::Sampler => {
                        let ptr: *const cl_sampler = arg_value.cast();
                        KernelArgValue::Sampler((*ptr).get_ref()?)
                    }
                }
            }
        };
        k.values.get(arg_index as usize).unwrap().replace(Some(arg));
        Ok(())
    } else {
        Err(CL_INVALID_ARG_INDEX)
    }

    //• CL_INVALID_DEVICE_QUEUE for an argument declared to be of type queue_t when the specified arg_value is not a valid device queue object. This error code is missing before version 2.0.
    //• CL_INVALID_ARG_VALUE if the argument is an image declared with the read_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_WRITE_ONLY or if the image argument is declared with the write_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_READ_ONLY.
    //• CL_MAX_SIZE_RESTRICTION_EXCEEDED if the size in bytes of the memory object (if the argument is a memory object) or arg_size (if the argument is declared with local qualifier) exceeds a language- specified maximum size restriction for this argument, such as the MaxByteOffset SPIR-V decoration. This error code is missing before version 2.2.
}

pub fn enqueue_ndrange_kernel(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    work_dim: cl_uint,
    global_work_offset: *const usize,
    global_work_size: *const usize,
    local_work_size: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = command_queue.get_arc()?;
    let k = kernel.get_arc()?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and kernel are not the same
    if q.context != k.prog.context {
        Err(CL_INVALID_CONTEXT)?;
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built program executable available
    // for device associated with command_queue.
    if k.prog.status(&q.device) != CL_BUILD_SUCCESS as cl_build_status {
        Err(CL_INVALID_PROGRAM_EXECUTABLE)?;
    }

    // CL_INVALID_KERNEL_ARGS if the kernel argument values have not been specified.
    if k.values.iter().any(|v| v.borrow().is_none()) {
        Err(CL_INVALID_KERNEL_ARGS)?
    }

    // CL_INVALID_WORK_DIMENSION if work_dim is not a valid value (i.e. a value between 1 and
    // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS).
    if work_dim == 0 || work_dim > q.device.max_grid_dimensions() {
        Err(CL_INVALID_WORK_DIMENSION)?;
    }

    let global_work_size = checked_kernel_work_arr(global_work_size, work_dim);
    let local_work_size = checked_kernel_work_arr(local_work_size, work_dim);
    let global_work_offset = checked_kernel_work_arr(global_work_offset, work_dim);

    if q.device.address_bits() == 32 {
        for (s, o) in global_work_size.iter().zip(global_work_offset) {
            // CL_INVALID_GLOBAL_WORK_SIZE if any of the values specified in global_work_size[0], …
            // global_work_size[work_dim - 1] exceed the maximum value representable by size_t on
            // the device on which the kernel-instance will be enqueued.
            if *s > u32::MAX as usize {
                Err(CL_INVALID_GLOBAL_WORK_SIZE)?;
            }
            // CL_INVALID_GLOBAL_OFFSET if the value specified in global_work_size + the
            // corresponding values in global_work_offset for any dimensions is greater than the
            // maximum value representable by size t on the device on which the kernel-instance
            // will be enqueued
            if s + o > u32::MAX as usize {
                Err(CL_INVALID_GLOBAL_OFFSET)?;
            }
        }
    }

    // CL_INVALID_WORK_ITEM_SIZE if the number of work-items specified in any of
    // local_work_size[0], … local_work_size[work_dim - 1] is greater than the corresponding values
    // specified by CL_DEVICE_MAX_WORK_ITEM_SIZES[0], …, CL_DEVICE_MAX_WORK_ITEM_SIZES[work_dim - 1].
    if local_work_size.iter().gt(q.device.max_block_sizes().iter()) {
        Err(CL_INVALID_WORK_ITEM_SIZE)?;
    }

    // If global_work_size is NULL, or the value in any passed dimension is 0 then the kernel
    // command will trivially succeed after its event dependencies are satisfied and subsequently
    // update its completion event.
    let cb: EventSig = if global_work_size.contains(&0) {
        Box::new(|_| Ok(()))
    } else {
        k.launch(
            &q,
            work_dim,
            local_work_size,
            global_work_size,
            global_work_offset,
        )
    };

    let e = Event::new(&q, CL_COMMAND_NDRANGE_KERNEL, evs, cb);
    cl_event::leak_ref(event, &e);
    q.queue(&e);

    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and does not match the required work-group size for kernel in the program source.
    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and is not consistent with the required number of sub-groups for kernel in the program source.
    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and the total number of work-items in the work-group computed as local_work_size[0] × … local_work_size[work_dim - 1] is greater than the value specified by CL_KERNEL_WORK_GROUP_SIZE in the Kernel Object Device Queries table.
    //• CL_INVALID_WORK_GROUP_SIZE if the work-group size must be uniform and the local_work_size is not NULL, is not equal to the required work-group size specified in the kernel source, or the global_work_size is not evenly divisible by the local_work_size.
    //• CL_MISALIGNED_SUB_BUFFER_OFFSET if a sub-buffer object is specified as the value for an argument that is a buffer object and the offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue. This error code
    //• CL_INVALID_IMAGE_SIZE if an image object is specified as an argument value and the image dimensions (image width, height, specified or compute row and/or slice pitch) are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if an image object is specified as an argument value and the image format (image channel order and data type) is not supported by device associated with queue.
    //• CL_OUT_OF_RESOURCES if there is a failure to queue the execution instance of kernel on the command-queue because of insufficient resources needed to execute the kernel. For example, the explicitly specified local_work_size causes a failure to execute the kernel because of insufficient resources such as registers or local memory. Another example would be the number of read-only image args used in kernel exceed the CL_DEVICE_MAX_READ_IMAGE_ARGS value for device or the number of write-only and read-write image args used in kernel exceed the CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS value for device or the number of samplers used in kernel exceed CL_DEVICE_MAX_SAMPLERS for device.
    //• CL_MEM_OBJECT_ALLOCATION_FAILURE if there is a failure to allocate memory for data store associated with image or buffer objects specified as arguments to kernel.
    //• CL_INVALID_OPERATION if SVM pointers are passed as arguments to a kernel and the device does not support SVM or if system pointers are passed as arguments to a kernel and/or stored inside SVM allocations passed as kernel arguments and the device does not support fine grain system SVM allocations.
    Ok(())
}

pub fn enqueue_task(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    // clEnqueueTask is equivalent to calling clEnqueueNDRangeKernel with work_dim set to 1,
    // global_work_offset set to NULL, global_work_size[0] set to 1, and local_work_size[0] set to
    // 1.
    enqueue_ndrange_kernel(
        command_queue,
        kernel,
        1,
        ptr::null(),
        [1, 0, 0].as_ptr(),
        [1, 0, 0].as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}
