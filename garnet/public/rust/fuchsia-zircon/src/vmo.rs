// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Type-safe bindings for Zircon vmo objects.

use crate::ok;
use crate::{AsHandleRef, Handle, HandleBased, HandleRef, Status};
use bitflags::bitflags;
use fuchsia_zircon_sys as sys;
use std::ptr;

/// An object representing a Zircon
/// [virtual memory object](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/objects/vm_object.md).
///
/// As essentially a subtype of `Handle`, it can be freely interconverted.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[repr(transparent)]
pub struct Vmo(Handle);
impl_handle_based!(Vmo);

impl Vmo {
    /// Create a virtual memory object.
    ///
    /// Wraps the
    /// `zx_vmo_create`
    /// syscall. See the
    /// [Shared Memory: Virtual Memory Objects (VMOs)](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/concepts.md#Shared-Memory_Virtual-Memory-Objects-VMOs)
    /// for more information.
    pub fn create(size: u64) -> Result<Vmo, Status> {
        Vmo::create_with_opts(VmoOptions::from_bits_truncate(0), size)
    }

    /// Create a virtual memory object with options.
    ///
    /// Wraps the
    /// `zx_vmo_create`
    /// syscall, allowing options to be passed.
    pub fn create_with_opts(opts: VmoOptions, size: u64) -> Result<Vmo, Status> {
        let mut handle = 0;
        let status = unsafe { sys::zx_vmo_create(size, opts.bits(), &mut handle) };
        ok(status)?;
        unsafe { Ok(Vmo::from(Handle::from_raw(handle))) }
    }

    /// Read from a virtual memory object.
    ///
    /// Wraps the `zx_vmo_read` syscall.
    pub fn read(&self, data: &mut [u8], offset: u64) -> Result<(), Status> {
        unsafe {
            let status = sys::zx_vmo_read(self.raw_handle(), data.as_mut_ptr(), offset, data.len());
            ok(status)
        }
    }

    /// Write to a virtual memory object.
    ///
    /// Wraps the `zx_vmo_write` syscall.
    pub fn write(&self, data: &[u8], offset: u64) -> Result<(), Status> {
        unsafe {
            let status = sys::zx_vmo_write(self.raw_handle(), data.as_ptr(), offset, data.len());
            ok(status)
        }
    }

    /// Get the size of a virtual memory object.
    ///
    /// Wraps the `zx_vmo_get_size` syscall.
    pub fn get_size(&self) -> Result<u64, Status> {
        let mut size = 0;
        let status = unsafe { sys::zx_vmo_get_size(self.raw_handle(), &mut size) };
        ok(status).map(|()| size)
    }

    /// Attempt to change the size of a virtual memory object.
    ///
    /// Wraps the `zx_vmo_set_size` syscall.
    pub fn set_size(&self, size: u64) -> Result<(), Status> {
        let status = unsafe { sys::zx_vmo_set_size(self.raw_handle(), size) };
        ok(status)
    }

    /// Attempt to change the cache policy of a virtual memory object.
    ///
    /// Wraps the `zx_vmo_set_cache_policy` syscall.
    pub fn set_cache_policy(&self, cache_policy: sys::zx_cache_policy_t) -> Result<(), Status> {
        let status =
            unsafe { sys::zx_vmo_set_cache_policy(self.raw_handle(), cache_policy as u32) };
        ok(status)
    }

    /// Perform an operation on a range of a virtual memory object.
    ///
    /// Wraps the
    /// [zx_vmo_op_range](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/syscalls/vmo_op_range.md)
    /// syscall.
    pub fn op_range(&self, op: VmoOp, offset: u64, size: u64) -> Result<(), Status> {
        let status = unsafe {
            sys::zx_vmo_op_range(self.raw_handle(), op.into_raw(), offset, size, ptr::null_mut(), 0)
        };
        ok(status)
    }

    /// Create a new virtual memory object that clones a range of this one.
    ///
    /// Wraps the
    /// [zx_vmo_create_child](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/syscalls/vmo_create_child.md)
    /// syscall.
    pub fn create_child(
        &self,
        opts: VmoChildOptions,
        offset: u64,
        size: u64,
    ) -> Result<Vmo, Status> {
        let mut out = 0;
        let status = unsafe {
            sys::zx_vmo_create_child(self.raw_handle(), opts.bits(), offset, size, &mut out)
        };
        ok(status)?;
        unsafe { Ok(Vmo::from(Handle::from_raw(out))) }
    }

    /// Replace a VMO, adding execute rights.
    ///
    /// Wraps the
    /// [zx_vmo_replace_as_executable](https://fuchsia.googlesource.com/fuchsia/+/master/docs/zircon/syscalls/vmo_replace_as_executable.md)
    /// syscall.
    pub fn replace_as_executable(self) -> Result<Vmo, Status> {
        // TODO(SEC-42): Add resource argument for exec setter.
        let vmex = Handle::invalid();

        let mut out = 0;
        let status = unsafe {
            sys::zx_vmo_replace_as_executable(self.raw_handle(), vmex.raw_handle(), &mut out)
        };
        ok(status)?;
        unsafe { Ok(Vmo::from(Handle::from_raw(out))) }
    }
}

bitflags! {
    /// Options that may be used when creating a `Vmo`.
    #[repr(transparent)]
    pub struct VmoOptions: u32 {
        const RESIZABLE = sys::ZX_VMO_RESIZABLE;
    }
}

bitflags! {
    /// Options that may be used when creating a `Vmo` child.
    #[repr(transparent)]
    pub struct VmoChildOptions: u32 {
        const COPY_ON_WRITE = sys::ZX_VMO_CHILD_COPY_ON_WRITE;
        const RESIZABLE = sys::ZX_VMO_CHILD_RESIZABLE;
    }
}

/// VM Object opcodes
#[derive(Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[repr(transparent)]
pub struct VmoOp(u32);
impl VmoOp {
    pub fn from_raw(raw: u32) -> VmoOp {
        VmoOp(raw)
    }
    pub fn into_raw(self) -> u32 {
        self.0
    }
}

assoc_values!(VmoOp, [
    COMMIT =           sys::ZX_VMO_OP_COMMIT;
    DECOMMIT =         sys::ZX_VMO_OP_DECOMMIT;
    LOCK =             sys::ZX_VMO_OP_LOCK;
    UNLOCK =           sys::ZX_VMO_OP_UNLOCK;
    CACHE_SYNC =       sys::ZX_VMO_OP_CACHE_SYNC;
    CACHE_INVALIDATE = sys::ZX_VMO_OP_CACHE_INVALIDATE;
    CACHE_CLEAN =      sys::ZX_VMO_OP_CACHE_CLEAN;
    CACHE_CLEAN_INVALIDATE = sys::ZX_VMO_OP_CACHE_CLEAN_INVALIDATE;
]);

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Rights;

    #[test]
    fn vmo_get_size() {
        let size = 16 * 1024 * 1024;
        let vmo = Vmo::create(size).unwrap();
        assert_eq!(size, vmo.get_size().unwrap());
    }

    #[test]
    fn vmo_set_size() {
        // Use a multiple of page size to match VMOs page aligned size
        let start_size = 4096;
        let vmo = Vmo::create_with_opts(VmoOptions::RESIZABLE, start_size).unwrap();
        assert_eq!(start_size, vmo.get_size().unwrap());

        // Change the size and make sure the new size is reported
        let new_size = 8192;
        assert!(vmo.set_size(new_size).is_ok());
        assert_eq!(new_size, vmo.get_size().unwrap());
    }

    #[test]
    fn vmo_set_size_fails_on_non_resizable() {
        let size = 4096;
        let vmo = Vmo::create(size).unwrap();
        assert_eq!(size, vmo.get_size().unwrap());

        let new_size = 8192;
        assert_eq!(Err(Status::UNAVAILABLE), vmo.set_size(new_size));
        assert_eq!(size, vmo.get_size().unwrap());
    }

    #[test]
    fn vmo_read_write() {
        let mut vec1 = vec![0; 16];
        let vmo = Vmo::create(4096 as u64).unwrap();
        assert!(vmo.write(b"abcdef", 0).is_ok());
        assert!(vmo.read(&mut vec1, 0).is_ok());
        assert_eq!(b"abcdef", &vec1[0..6]);
        assert!(vmo.write(b"123", 2).is_ok());
        assert!(vmo.read(&mut vec1, 0).is_ok());
        assert_eq!(b"ab123f", &vec1[0..6]);

        // Read one byte into the vmo.
        assert!(vmo.read(&mut vec1, 1).is_ok());
        assert_eq!(b"b123f", &vec1[0..5]);
    }

    #[test]
    fn vmo_op_range_unsupported() {
        let vmo = Vmo::create(12).unwrap();
        assert_eq!(vmo.op_range(VmoOp::LOCK, 0, 1), Err(Status::NOT_SUPPORTED));
        assert_eq!(vmo.op_range(VmoOp::UNLOCK, 0, 1), Err(Status::NOT_SUPPORTED));
    }

    #[test]
    fn vmo_cache() {
        let vmo = Vmo::create(12).unwrap();

        // Cache operations should all succeed.
        assert_eq!(vmo.op_range(VmoOp::CACHE_SYNC, 0, 12), Ok(()));
        assert_eq!(vmo.op_range(VmoOp::CACHE_INVALIDATE, 0, 12), Ok(()));
        assert_eq!(vmo.op_range(VmoOp::CACHE_CLEAN, 0, 12), Ok(()));
        assert_eq!(vmo.op_range(VmoOp::CACHE_CLEAN_INVALIDATE, 0, 12), Ok(()));
    }

    #[test]
    fn vmo_create_child() {
        let original = Vmo::create(16).unwrap();
        assert!(original.write(b"one", 0).is_ok());

        // Clone the VMO, and make sure it contains what we expect.
        let clone = original.create_child(VmoChildOptions::COPY_ON_WRITE, 0, 16).unwrap();
        let mut read_buffer = vec![0; 16];
        assert!(clone.read(&mut read_buffer, 0).is_ok());
        assert_eq!(&read_buffer[0..3], b"one");

        // Writing to the original will not affect the clone.
        assert!(original.write(b"two", 0).is_ok());
        assert!(original.read(&mut read_buffer, 0).is_ok());
        assert_eq!(&read_buffer[0..3], b"two");
        assert!(clone.read(&mut read_buffer, 0).is_ok());
        assert_eq!(&read_buffer[0..3], b"one");

        // However, writing to the clone will not affect the original.
        assert!(clone.write(b"three", 0).is_ok());
        assert!(original.read(&mut read_buffer, 0).is_ok());
        assert_eq!(&read_buffer[0..3], b"two");
        assert!(clone.read(&mut read_buffer, 0).is_ok());
        assert_eq!(&read_buffer[0..5], b"three");
    }

    // TODO(SEC-42): In the near-ish future zx_vmo_replace_as_executable will be restricted and
    // will require either the process run in a job with some special policy or have access to a
    // special resource. We will either need to run the test binary with that capability or delete
    // this test at that point.
    #[test]
    fn vmo_replace_as_executeable() {
        let vmo = Vmo::create(16).unwrap();

        let info = vmo.as_handle_ref().basic_info().unwrap();
        assert!(!info.rights.contains(Rights::EXECUTE));

        let exec_vmo = vmo.replace_as_executable().unwrap();
        let info = exec_vmo.as_handle_ref().basic_info().unwrap();
        assert!(info.rights.contains(Rights::EXECUTE));
    }
}
