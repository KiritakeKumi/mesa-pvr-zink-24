extern crate rusticl_opencl_gen;

use self::rusticl_opencl_gen::*;

use std::convert::TryFrom;
use std::convert::TryInto;
use std::slice;

#[macro_export]
macro_rules! cl_closure {
    (|$obj:ident| $cb:ident($($arg:ident$(,)?)*)) => {
        Box::new(
            unsafe {
                move|$obj| $cb.unwrap()($($arg,)*)
            }
        )
    }
}

macro_rules! cl_callback {
    ($cb:ident {
        $($p:ident : $ty:ty,)*
    }) => {
        #[allow(dead_code)]
        pub type $cb = unsafe extern "C" fn(
            $($p: $ty,)*
        );
    }
}

cl_callback!(
    CreateContextCB {
        errinfo: *const ::std::os::raw::c_char,
        private_info: *const ::std::ffi::c_void,
        cb: usize,
        user_data: *mut ::std::ffi::c_void,
    }
);

cl_callback!(
    DeleteContextCB {
        context: cl_context,
        user_data: *mut ::std::os::raw::c_void,
    }
);

cl_callback!(
    EventCB {
        event: cl_event,
        event_command_status: cl_int,
        user_data: *mut ::std::os::raw::c_void,
    }
);

cl_callback!(
    MemCB {
        memobj: cl_mem,
        user_data: *mut ::std::os::raw::c_void,
    }
);

cl_callback!(
    ProgramCB {
        program: cl_program,
        user_data: *mut ::std::os::raw::c_void,
    }
);

cl_callback!(
    SVMFreeCb {
        queue: cl_command_queue,
        num_svm_pointers: cl_uint,
        svm_pointers: *mut *mut ::std::os::raw::c_void,
        user_data: *mut ::std::os::raw::c_void,
    }
);

// a lot of APIs use 3 component vectors passed as C arrays
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct CLVec<T> {
    vals: [T; 3],
}

impl<T: Copy> CLVec<T> {
    pub fn new(vals: [T; 3]) -> Self {
        Self { vals: vals }
    }

    pub fn from_raw_parts(v: *const T) -> Self {
        Self {
            // unwrap is safe as the slice has three elements
            vals: unsafe { slice::from_raw_parts(v, 3) }.try_into().unwrap(),
        }
    }
}

impl<T: Default + Copy> Default for CLVec<T> {
    fn default() -> Self {
        Self {
            vals: [T::default(); 3],
        }
    }
}

// provides a ton of functions
impl<T> std::ops::Deref for CLVec<T> {
    type Target = [T; 3];

    fn deref(&self) -> &Self::Target {
        &self.vals
    }
}

impl<T> std::ops::DerefMut for CLVec<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.vals
    }
}

impl<T: Copy + std::ops::Add<Output = T>> std::ops::Add for CLVec<T> {
    type Output = Self;

    fn add(self, other: Self) -> Self {
        self + other.vals
    }
}

impl<T: Copy + std::ops::Add<Output = T>> std::ops::Add<[T; 3]> for CLVec<T> {
    type Output = Self;

    fn add(self, other: [T; 3]) -> Self {
        Self {
            vals: [self[0] + other[0], self[1] + other[1], self[2] + other[2]],
        }
    }
}

impl<T: Copy + std::ops::Sub<Output = T>> std::ops::Sub<[T; 3]> for CLVec<T> {
    type Output = Self;

    fn sub(self, other: [T; 3]) -> Self {
        Self {
            vals: [self[0] - other[0], self[1] - other[1], self[2] - other[2]],
        }
    }
}

impl<T> std::ops::Mul for CLVec<T>
where
    T: Copy + std::ops::Mul<Output = T> + std::ops::Add<Output = T>,
{
    type Output = T;

    fn mul(self, other: Self) -> T {
        self * other.vals
    }
}

impl<T> std::ops::Mul<[T; 3]> for CLVec<T>
where
    T: Copy + std::ops::Mul<Output = T> + std::ops::Add<Output = T>,
{
    type Output = T;

    fn mul(self, other: [T; 3]) -> T {
        self[0] * other[0] + self[1] * other[1] + self[2] * other[2]
    }
}

impl<S, T> TryInto<[T; 3]> for CLVec<S>
where
    S: Copy,
    T: TryFrom<S>,
    [T; 3]: TryFrom<Vec<T>>,
{
    type Error = cl_int;

    fn try_into(self) -> Result<[T; 3], cl_int> {
        let vec: Result<Vec<T>, _> = self
            .vals
            .iter()
            .map(|v| T::try_from(*v).map_err(|_| CL_OUT_OF_HOST_MEMORY))
            .collect();
        vec?.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)
    }
}
