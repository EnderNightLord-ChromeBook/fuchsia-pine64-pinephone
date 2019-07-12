// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Native process creation and program loading library.
//!
//! # Restrictions
//!
//! Most Fuchsia processes are not able to use this library.
//!
//! This library uses the [zx_process_create] syscall to create a new process in a job. Use of that
//! syscall requires that the job of the process using the syscall (not the job that the process is
//! being created in) be allowed to create processes. In concrete terms, the process using this
//! library must be in a job whose [ZX_POL_NEW_PROCESS job policy is
//! ZX_POL_ACTION_ALLOW][zx_job_set_policy].
//!
//! Most processes on Fuchsia run in jobs where this job policy is set to DENY and thus will not
//! be able to use this library.  Those processes should instead use the [fuchsia.process.Launcher]
//! FIDL service, which is itself implemented using this library. [fdio::spawn()],
//! [fdio::spawn_vmo()], and [fdio::spawn_etc()] provide simple interfaces to this service.
//!
//! # Example
//!
//! ```
//! let process_name = CString::new("my_process")?;
//! let job = /* job to create new process in */;
//! let executable = /* VMO with execute rights containing ELF executable */;
//! let pkg_directory = /* fidl::endpoints::ClientEnd for fuchsia.io.Directory */;
//! let out_directory = /* server end of zx::Channel */;
//! let other_handle = /* some arbitrary zx::Handle */;
//!
//! let builder = ProcessBuilder::new(&process_name, &job, executable)?;
//! builder.add_arguments(vec![process_name, CString::new("arg0")?]);
//! builder.add_environment_variables(vec![CString::new("VAR=VALUE")?]);
//! builder.add_namespace_entries(vec![NamespaceEntry{
//!     path: CString::new("/pkg")?,
//!     directory: package_directory,
//! }])?;
//! builder.add_handles(vec![
//!     StartupHandle{
//!         handle: out_directory.into_handle(),
//!         info: HandleInfo::new(HandleType::DirectoryRequest, 0),
//!     },
//!     StartupHandle{
//!         handle: other_handle,
//!         info: HandleInfo::new(HandleType::User0, 1),
//!     },
//! ])?;
//!
//! let built_process: BuiltProcess = builder.build()?;
//! let process: zx::Process = builder.start()?;
//! ```
//!
//! [zx_process_create]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/docs/syscalls/process_create.md
//! [zx_job_set_policy]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/docs/syscalls/job_set_policy.md
//! [fuchsia.process.Launcher]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/system/fidl/fuchsia-process/launcher.fidl
//
// TODO: Consider supporting this for processes that do not meet the above requirements (nearly
// all), where it can optionally build the process directly if able or delegate to a remote
// fuchsia.process.Launcher (possibly through fdio::spawn_etc, in which case it would just be an
// alternative front-end for that, similar to our C++ ProcessBuilder library, though that pulls in
// a dependency on fdio).

#![feature(async_await, await_macro)]
#![deny(missing_docs)]

pub use self::elf_load::ElfLoadError;
pub use self::elf_parse::ElfParseError;
pub use self::processargs::{ProcessargsError, StartupHandle};

mod elf_load;
mod elf_parse;
mod processargs;
mod util;

use {
    failure::Fail,
    fidl::endpoints::ClientEnd,
    fidl_fuchsia_io as fio, fidl_fuchsia_ldsvc as fldsvc,
    fuchsia_async::{self as fasync, TimeoutExt},
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon::{self as zx, AsHandleRef, DurationNum, HandleBased},
    futures::prelude::*,
    lazy_static::lazy_static,
    std::convert::TryFrom,
    std::default::Default,
    std::ffi::{CStr, CString},
    std::iter,
    std::mem,
};

/// A container for a single namespace entry, containing a path and a directory handle. Used as an
/// input to [ProcessBuilder::add_namespace_entries()].
pub struct NamespaceEntry {
    /// Namespace path.
    pub path: CString,

    /// Namespace directory handle.
    pub directory: ClientEnd<fio::DirectoryMarker>,
}

/// The main builder type for this crate. Collects inputs and creates a new process.
///
/// See top-level crate documentation for a usage example.
pub struct ProcessBuilder {
    executable: zx::Vmo,
    ldsvc: Option<fldsvc::LoaderProxy>,
    inner: BuilderInner,
}

// This is split into a separate struct so that we can borrow this to continue building the linker
// message after already partially moving fields out of ProcessBuilder.
struct BuilderInner {
    process: zx::Process,
    thread: zx::Thread,
    root_vmar: zx::Vmar,
    msg_contents: processargs::MessageContents,
}

/// A container for a fully built but not yet started (as in, its initial thread is not yet
/// running) process, with all related handles and metadata. Output of [ProcessBuilder::build()].
///
/// You can use this struct to start the process with [BuiltProcess::start()], which is a simple
/// wrapper around the [zx_process_start] syscall. You can optionally use the handles and
/// information in this struct to manipulate the process or its address space before starting it,
/// such as when creating a process in a debugger.
///
/// [zx_process_start]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/docs/syscalls/process_start.md
pub struct BuiltProcess {
    /// The newly created process.
    pub process: zx::Process,

    /// The root VMAR for the created process.
    pub root_vmar: zx::Vmar,

    /// The process's initial thread.
    pub thread: zx::Thread,

    /// The process's entry point.
    pub entry: usize,

    /// The initial thread's stack pointer.
    pub stack: usize,

    /// The bootstrap channel, to be passed to the process on start as arg1 in zx_process_start /
    /// zx::Process::start.
    pub bootstrap: zx::Channel,

    /// The base address of the VDSO in the process's VMAR, to be passed to the process on start as
    /// arg2 in zx_process_start / zx::Process::start.
    pub vdso_base: usize,

    /// The base address where the ELF executable, or the dynamic linker if the ELF was dynamically
    /// linked, was loaded in the process's VMAR.
    pub elf_base: usize,
}

impl ProcessBuilder {
    /// Create a new ProcessBuilder that can be used to create a new process under the given job
    /// with the given name and ELF64 executable (as a VMO).
    ///
    /// This job is only used to create the process and thus is not taken ownership of. To provide
    /// a default job handle to be passed to the new process, use [ProcessBuilder::add_handles()]
    /// with [HandleType::DefaultJob].
    ///
    /// The provided VMO must have the [zx::Rights::EXECUTE] right.
    ///
    /// # Errors
    ///
    /// Returns Err([ProcessBuilderError::CreateProcess]) if process creation fails, such as if the
    /// process using this is disallowed direct process creation rights through job policy. See
    /// top-level crate documentation for more details.
    pub fn new(
        name: &CStr,
        job: &zx::Job,
        executable: zx::Vmo,
    ) -> Result<ProcessBuilder, ProcessBuilderError> {
        if job.is_invalid_handle() {
            return Err(ProcessBuilderError::BadHandle("Invalid job handle"));
        }
        if executable.is_invalid_handle() {
            return Err(ProcessBuilderError::BadHandle("Invalid executable handle"));
        }

        // Creating the process immediately has the benefit that we fail fast if the calling
        // process does not have permission to create processes directly.
        let (process, root_vmar) = job
            .create_child_process(name.to_bytes())
            .map_err(ProcessBuilderError::CreateProcess)?;

        // Create the initial thread.
        let thread =
            process.create_thread(b"initial-thread").map_err(ProcessBuilderError::CreateThread)?;

        // Add duplicates of the process, VMAR, and thread handles to the bootstrap message.
        let msg_contents = processargs::MessageContents::default();
        let mut pb = ProcessBuilder {
            executable,
            ldsvc: None,
            inner: BuilderInner { process, thread, root_vmar, msg_contents },
        };
        pb.inner.msg_contents.handles.append(&mut pb.inner.common_message_handles()?);
        Ok(pb)
    }

    /// Sets the fuchsia.ldsvc.Loader service for the process.
    ///
    /// The loader service is used to load dynamic libraries if the executable is a dynamically
    /// linked ELF file (i.e. if it contains a PT_INTERP header), and is required for such
    /// executables. It will only be provided to the new process in this case. Otherwise, it is
    /// unused and has no effect.
    ///
    /// If no loader service has been provided and it is needed, process creation will fail. Note
    /// that this differs from the automatic fallback behavior of previous process creation
    /// libraries, which would clone the loader of the current process. This fallback is likely to
    /// fail in subtle and confusing ways. An appropriate loader service that has access to the
    /// libraries or interpreter must be provided.
    ///
    /// Note that [ProcessBuilder::add_handles()] will automatically pass a handle with type
    /// [HandleType::LdsvcLoader] to this function.
    ///
    /// If called multiple times (e.g. if a loader was initially provided through
    /// [ProcessBuilder::add_handles()] and you want to replace it), the new loader replaces the
    /// previous and the handle to the previous loader is dropped.
    pub fn set_loader_service(
        &mut self,
        ldsvc: ClientEnd<fldsvc::LoaderMarker>,
    ) -> Result<(), ProcessBuilderError> {
        if ldsvc.is_invalid_handle() {
            return Err(ProcessBuilderError::BadHandle("Invalid loader service handle"));
        }
        self.ldsvc =
            Some(ldsvc.into_proxy().map_err(|e| {
                ProcessBuilderError::Internal("Failed to get LoaderProxy", e.into())
            })?);
        Ok(())
    }

    /// Add arguments to the process's bootstrap message. Successive calls append (not replace)
    /// arguments.
    pub fn add_arguments(&mut self, mut args: Vec<CString>) {
        self.inner.msg_contents.args.append(&mut args);
    }

    /// Add environment variables to the process's bootstrap message. Successive calls append (not
    /// replace) environment variables.
    pub fn add_environment_variables(&mut self, mut vars: Vec<CString>) {
        self.inner.msg_contents.environment_vars.append(&mut vars);
    }

    /// Add handles to the process's bootstrap message. Successive calls append (not replace)
    /// handles.
    ///
    /// Each [StartupHandle] contains a [zx::Handle] object accompanied by a [HandleInfo] object
    /// that includes the handle type and a type/context-dependent argument.
    ///
    /// A [HandleType::LdsvcLoader] handle will automatically be passed along to
    /// [ProcessBuilder::set_loader_service()] if provided through this function.
    ///
    /// # Errors
    ///
    /// [HandleType::NamespaceDirectory] handles should not be added through this function since
    /// they must be accompanied with a path. Use [ProcessBuilder::add_namespace_entries()] for
    /// that instead.
    ///
    /// The following handle types cannot be added through this, as they are added automatically by
    /// the ProcessBuilder:
    /// * [HandleType::ProcessSelf]
    /// * [HandleType::ThreadSelf]
    /// * [HandleType::RootVmar]
    /// * [HandleType::LoadedVmar]
    /// * [HandleType::VdsoVmo]
    /// * [HandleType::StackVmo]
    /// * [HandleType::ExecutableVmo]
    pub fn add_handles(
        &mut self,
        mut startup_handles: Vec<StartupHandle>,
    ) -> Result<(), ProcessBuilderError> {
        // Do a bit of validation before adding to the bootstrap handles.
        for h in &startup_handles {
            if h.handle.is_invalid() {
                return Err(ProcessBuilderError::BadHandle("Invalid handle in startup handles"));
            }

            let t = h.info.handle_type();
            match t {
                HandleType::NamespaceDirectory => {
                    return Err(ProcessBuilderError::InvalidArg(
                        "Cannot add NamespaceDirectory handles directly, use add_namespace_entries"
                            .into(),
                    ));
                }
                HandleType::ProcessSelf
                | HandleType::ThreadSelf
                | HandleType::RootVmar
                | HandleType::LoadedVmar
                | HandleType::VdsoVmo
                | HandleType::StackVmo
                | HandleType::ExecutableVmo => {
                    return Err(ProcessBuilderError::InvalidArg(format!(
                        "Cannot add a {:?} handle directly, it will be automatically added",
                        t,
                    )));
                }
                _ => {}
            }
        }

        // Intentionally separate from validation so that we don't partially add namespace entries.
        for h in startup_handles.drain(..) {
            match h.info.handle_type() {
                HandleType::LdsvcLoader => {
                    // Automatically pass this to |set_loader_service| instead.
                    self.set_loader_service(ClientEnd::from(h.handle))?;
                }
                _ => {
                    self.inner.msg_contents.handles.push(h);
                }
            }
        }
        Ok(())
    }

    /// Add directories to the process's namespace.
    ///
    /// Successive calls append new namespace entries, not replace previous entries.
    ///
    /// Each [NamespaceEntry] contains a client connection to a fuchsia.io.Directory FIDL service
    /// and a path to add that directory to the process's namespace as.
    ///
    /// # Errors
    ///
    /// Returns Err([ProcessBuilderError::InvalidArg]) if the maximum number of namespace entries
    /// (2^16) was reached and the entry could not be added. This is exceedingly unlikely, and most
    /// likely if you are anywhere near this limit [ProcessBuilder::build()] will fail because the
    /// process's processargs startup messsage is over its own length limit.
    pub fn add_namespace_entries(
        &mut self,
        mut entries: Vec<NamespaceEntry>,
    ) -> Result<(), ProcessBuilderError> {
        // Namespace entries are split into a namespace path, that is included in the bootstrap
        // message (as the so-called "namespace table"), plus a NamespaceDirectory handle, where the arg
        // value is the index of the path in the namespace table.
        //
        // Check that the namespace table doesn't exceed 2^16 entries, since the HandleInfo arg is
        // only 16-bits. Realistically this will never matter - if you're anywhere near this
        // many entries, you're going to exceed the bootstrap message length limit - but Rust
        // encourages us (and makes it easy) to be safe about the edge case here.
        let mut idx = u16::try_from(self.inner.msg_contents.namespace_paths.len())
            .expect("namespace_paths.len should never be larger than a u16");
        let num_entries = u16::try_from(entries.len())
            .map_err(|_| ProcessBuilderError::InvalidArg("Too many namespace entries".into()))?;
        if idx.checked_add(num_entries).is_none() {
            return Err(ProcessBuilderError::InvalidArg(
                "Can't add namespace entries, limit reached".into(),
            ));
        }

        for entry in &entries {
            if entry.directory.is_invalid_handle() {
                return Err(ProcessBuilderError::BadHandle("Invalid handle in namespace entry"));
            }
        }

        // Intentionally separate from validation so that we don't partially add namespace entries=
        for entry in entries.drain(..) {
            self.inner.msg_contents.namespace_paths.push(entry.path);
            self.inner.msg_contents.handles.push(StartupHandle {
                handle: zx::Handle::from(entry.directory),
                info: HandleInfo::new(HandleType::NamespaceDirectory, idx),
            });
            idx += 1;
        }
        Ok(())
    }

    /// Build the new process using the data and handles provided to the ProcessBuilder.
    ///
    /// The return value of this function is a [BuiltProcess] struct which contains the new process
    /// and all the handles and data needed to start it, but the process' initial thread is not yet
    /// started. Use [BuiltProcess::start()] or the [zx_process_start] syscall to actually start
    /// it.
    ///
    /// # Errors
    ///
    /// There are many errors that could result during process loading and only some are listed
    /// here. See [ProcessBuilderError] for the various error types that can be returned.
    ///
    /// Returns Err([ProcessBuilderError::LoaderMissing]) if the ELF executable is dynamically
    /// linked (has a PT_INTERP program header) but no loader service has been provided through
    /// [ProcessBuilder::set_loader_service()] or [ProcessBuilder::add_handles()].
    ///
    /// [zx_process_start]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/docs/syscalls/process_start.md
    pub async fn build(mut self) -> Result<BuiltProcess, ProcessBuilderError> {
        // Parse the executable as an ELF64 file, reading in the headers we need. Done first since
        // this is most likely to be invalid and error out.
        let elf_headers = elf_parse::Elf64Headers::from_vmo(&self.executable)?;

        // Create bootstrap message channel.
        let (bootstrap_rd, bootstrap_wr) = zx::Channel::create()
            .map_err(|s| ProcessBuilderError::GenericStatus("Failed to create channel", s))?;

        // Check if main executable is dynamically linked, and handle appropriately.
        let loaded_elf;
        let _reserve_vmar; // VMAR destroyed on drop but otherwise unused, hence _ prefix.
        let dynamic;
        if let Some(interp_hdr) =
            elf_headers.program_header_with_type(elf_parse::SegmentType::Interp)?
        {
            // Dynamically linked so defer loading the main executable to the dynamic
            // linker/loader, which we load here instead.
            dynamic = true;

            // Check that a ldsvc.Loader service was provided.
            let ldsvc = self.ldsvc.ok_or(ProcessBuilderError::LoaderMissing())?;

            // A process using PT_INTERP might be loading a libc.so that supports sanitizers;
            // reserve the low address region for sanitizers to allocate shadow memory.
            //
            // The reservation VMAR ensures that the initial allocations & mappings made in this
            // function stay out of this area. It is destroyed automatically on Drop. The process's
            // own allocations can use the full address space.
            //
            // !! WARNING: This makes a specific address VMAR allocation, so it must come before
            // any elf_load::load_elf calls. !!
            _reserve_vmar = ReservationVmar::reserve_low_address_space(&self.inner.root_vmar)?;

            // Get the dynamic linker and map it into the process's address space.
            let ld_vmo = await!(get_dynamic_linker(&ldsvc, &self.executable, interp_hdr))?;
            let ld_headers = elf_parse::Elf64Headers::from_vmo(&ld_vmo)?;
            loaded_elf = elf_load::load_elf(&ld_vmo, &self.inner.root_vmar, &ld_headers)?;

            // Build the dynamic linker bootstrap message and write it to the bootstrap channel.
            // This message is written before the primary bootstrap message since it is consumed
            // first in the dynamic linker.
            let msg = self.inner.build_linker_message(ldsvc, self.executable, loaded_elf.vmar)?;
            msg.write(&bootstrap_wr).map_err(ProcessBuilderError::WriteBootstrapMessage)?;
        } else {
            // Statically linked but still position-independent (ET_DYN) ELF, load directly.
            dynamic = false;

            loaded_elf = elf_load::load_elf(&self.executable, &self.inner.root_vmar, &elf_headers)?;
            self.inner.msg_contents.handles.push(StartupHandle {
                handle: loaded_elf.vmar.into_handle(),
                info: HandleInfo::new(HandleType::LoadedVmar, 0),
            });
        }

        // Load the system vDSO into the process's address space and a handle to it to the
        // bootstrap message.
        let vdso_base = self.inner.load_system_vdso()?;

        // Calculate initial stack size.
        let stack_size;
        let stack_vmo_name;
        if dynamic {
            // Calculate the initial stack size for the dynamic linker. This factors in the size of
            // an extra handle for the stac) that hasn't yet been added to the message contents,
            // since creating the stack requires this size.
            stack_size = calculate_initial_linker_stack_size(&mut self.inner.msg_contents, 1)?;
            stack_vmo_name = format!("stack: msg of {:#x?}", stack_size);
        } else {
            // Set stack size from PT_GNU_STACK header, if present, or use the default. The dynamic
            // linker handles this for dynamically linked ELFs (above case).
            const ZIRCON_DEFAULT_STACK_SIZE: usize = 256 << 10; // 256KiB
            let mut ss = ("default", ZIRCON_DEFAULT_STACK_SIZE);
            if let Some(stack_hdr) =
                elf_headers.program_header_with_type(elf_parse::SegmentType::GnuStack)?
            {
                if stack_hdr.memsz > 0 {
                    ss = ("explicit", stack_hdr.memsz as usize);
                }
            }

            // Stack size must be page aligned.
            stack_size = util::page_end(ss.1);
            stack_vmo_name = format!("stack: {} {:#x?}", ss.0, stack_size);
        }

        // Allocate the initial thread's stack, map it, and add a handle to the bootstrap message.
        let stack_vmo_name =
            CString::new(stack_vmo_name).expect("Stack VMO name must not contain interior nul's");
        let stack_ptr = self.inner.create_stack(stack_size, &stack_vmo_name)?;

        // Build and send the primary bootstrap message.
        let msg = processargs::Message::build(self.inner.msg_contents)?;
        msg.write(&bootstrap_wr).map_err(ProcessBuilderError::WriteBootstrapMessage)?;

        Ok(BuiltProcess {
            process: self.inner.process,
            root_vmar: self.inner.root_vmar,
            thread: self.inner.thread,
            entry: loaded_elf.entry,
            stack: stack_ptr,
            bootstrap: bootstrap_rd,
            vdso_base: vdso_base,
            elf_base: loaded_elf.vmar_base,
        })
    }
}

/// Calculate the size of the initial stack to allocate for the dynamic linker, based on the given
/// processargs message contents.
///
/// The initial stack is used just for startup work in the dynamic linker and to hold the bootstrap
/// message, so we only attempt to make it only as big as needed. The size returned is based on the
/// stack space needed to read the bootstrap message with zx_channel_read, and thus includes the
/// message data itself plus the size of the handles (i.e. the size of N zx_handle_t's).
///
/// This also allows the caller to specify an number of "extra handles" to factor into the size
/// calculation. This allows the size to be calculated before all the real handles have been added
/// to the contents, for example if the size is needed to create those handles.
fn calculate_initial_linker_stack_size(
    msg_contents: &mut processargs::MessageContents,
    extra_handles: usize,
) -> Result<usize, ProcessBuilderError> {
    // Add N placeholder handles temporarily to factor in the size of handles that are not yet
    // added to the message contents.
    msg_contents.handles.extend(
        iter::repeat_with(|| StartupHandle {
            handle: zx::Handle::invalid(),
            info: HandleInfo::new(HandleType::User0, 0),
        })
        .take(extra_handles),
    );

    // Include both the message data size and the size of the handles since we're calculating the
    // stack space required to read the message.
    let num_handles = msg_contents.handles.len();
    let msg_stack_size = processargs::Message::calculate_size(msg_contents)?
        + num_handles * mem::size_of::<zx::sys::zx_handle_t>();
    msg_contents.handles.truncate(num_handles - extra_handles);

    // PTHREAD_STACK_MIN is defined by the C library in
    // //zircon/third_party/ulib/musl/include/limits.h. It is tuned enough to cover the dynamic
    // linker and C library startup code's stack usage (up until the point it switches to its own
    // stack in __libc_start_main), but leave a little space so for small bootstrap message sizes
    // the stack needs only one page.
    const PTHREAD_STACK_MIN: usize = 3072;
    Ok(util::page_end(msg_stack_size + PTHREAD_STACK_MIN))
}

impl BuilderInner {
    /// Build the bootstrap message for the dynamic linker, which uses the same processargs
    /// protocol as the message for the main process but somewhat different contents.
    ///
    /// The LoaderProxy provided must be ready to be converted to a Handle with into_channel(). In
    /// other words, there must be no other active clones of the proxy, no open requests, etc. The
    /// intention is that the user provides a handle only (perhaps wrapped in a ClientEnd) through
    /// [ProcessBuilder::set_loader_service()], not a Proxy, so the library can be sure this
    /// invariant is maintained and a failure is a library bug.
    fn build_linker_message(
        &self,
        ldsvc: fldsvc::LoaderProxy,
        executable: zx::Vmo,
        loaded_vmar: zx::Vmar,
    ) -> Result<processargs::Message, ProcessBuilderError> {
        // Don't need to use the ldsvc.Loader anymore; turn it back into into a raw handle so
        // we can pass it along in the dynamic linker bootstrap message.
        let ldsvc_hnd =
            ldsvc.into_channel().expect("Failed to get channel from LoaderProxy").into_zx_channel();

        let mut linker_msg_contents = processargs::MessageContents {
            // Argument strings are sent to the linker so that it can use argv[0] in messages it
            // prints.
            args: self.msg_contents.args.clone(),
            // Environment variables are sent to the linker so that it can see vars like LD_DEBUG.
            environment_vars: self.msg_contents.environment_vars.clone(),
            // Process namespace is not set up or used in the linker.
            namespace_paths: vec![],
            // Loader message includes a few special handles needed to do its job, plus a set of
            // handles common to both messages which are generated by this library.
            handles: vec![
                StartupHandle {
                    handle: ldsvc_hnd.into_handle(),
                    info: HandleInfo::new(HandleType::LdsvcLoader, 0),
                },
                StartupHandle {
                    handle: executable.into_handle(),
                    info: HandleInfo::new(HandleType::ExecutableVmo, 0),
                },
                StartupHandle {
                    handle: loaded_vmar.into_handle(),
                    info: HandleInfo::new(HandleType::LoadedVmar, 0),
                },
            ],
        };
        linker_msg_contents.handles.append(&mut self.common_message_handles()?);

        Ok(processargs::Message::build(linker_msg_contents)?)
    }

    /// Returns a vector of processargs message handles created by this library which are common to
    /// both the linker and main messages, duplicating handles as needed.
    fn common_message_handles(&self) -> Result<Vec<StartupHandle>, ProcessBuilderError> {
        let handles: &[(zx::HandleRef, &str, HandleType)] = &[
            (self.process.as_handle_ref(), "Failed to dup process handle", HandleType::ProcessSelf),
            (self.root_vmar.as_handle_ref(), "Failed to dup VMAR handle", HandleType::RootVmar),
            (self.thread.as_handle_ref(), "Failed to dup thread handle", HandleType::ThreadSelf),
        ];

        handles
            .iter()
            .map(|(h, err_str, handle_type)| {
                let handle = h
                    .duplicate(zx::Rights::SAME_RIGHTS)
                    .map_err(|s| ProcessBuilderError::GenericStatus(err_str, s))?;
                Ok(StartupHandle { handle, info: HandleInfo::new(*handle_type, 0) })
            })
            .collect()
    }

    /// Load the system vDSO VMO into the process's address space and a handle to it to the
    /// bootstrap message. Returns the base address that the vDSO was mapped into.
    fn load_system_vdso(&mut self) -> Result<usize, ProcessBuilderError> {
        let vdso_vmo = get_system_vdso_vmo()?;
        let vdso_headers = elf_parse::Elf64Headers::from_vmo(&vdso_vmo)?;
        let loaded_vdso = elf_load::load_elf(&vdso_vmo, &self.root_vmar, &vdso_headers)?;

        self.msg_contents.handles.push(StartupHandle {
            handle: vdso_vmo.into_handle(),
            info: HandleInfo::new(HandleType::VdsoVmo, 0),
        });

        Ok(loaded_vdso.vmar_base)
    }

    /// Allocate the initial thread's stack, map it, and add a handle to the bootstrap message.
    /// Returns the initial stack pointer for the process.
    ///
    /// Note that launchpad supported not allocating a stack at all, but that only happened if an
    /// explicit stack size of 0 is set. ProcessBuilder does not support overriding the stack size
    /// so a stack is always created.
    fn create_stack(
        &mut self,
        stack_size: usize,
        vmo_name: &CStr,
    ) -> Result<usize, ProcessBuilderError> {
        let stack_vmo = zx::Vmo::create(stack_size as u64).map_err(|s| {
            ProcessBuilderError::GenericStatus("Failed to create VMO for initial thread stack", s)
        })?;
        stack_vmo
            .set_name(&vmo_name)
            .map_err(|s| ProcessBuilderError::GenericStatus("Failed to set stack VMO name", s))?;
        let stack_flags = zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE;
        let stack_base = self
            .root_vmar
            .map(0, &stack_vmo, 0, stack_size, stack_flags)
            .map_err(|s| ProcessBuilderError::GenericStatus("Failed to map initial stack", s))?;
        let stack_ptr = compute_initial_stack_pointer(stack_base, stack_size);

        // Pass the stack VMO to the process. Our protocol with the new process is that we warrant
        // that this is the VMO from which the initial stack is mapped and that we've exactly
        // mapped the entire thing, so vm_object_get_size on this in concert with the initial SP
        // value tells it the exact bounds of its stack.
        self.msg_contents.handles.push(StartupHandle {
            handle: stack_vmo.into_handle(),
            info: HandleInfo::new(HandleType::StackVmo, 0),
        });

        Ok(stack_ptr)
    }
}

/// Returns an owned VMO handle to the system vDSO ELF image, duplicated from the handle provided
/// to this process through its own processargs bootstrap message.
fn get_system_vdso_vmo() -> Result<zx::Vmo, ProcessBuilderError> {
    lazy_static! {
        static ref VDSO_VMO: zx::Vmo = {
            zx::Vmo::from(
                fuchsia_runtime::take_startup_handle(HandleInfo::new(HandleType::VdsoVmo, 0))
                    .expect("Failed to take VDSO VMO startup handle"),
            )
        };
    }

    let vdso_dup = VDSO_VMO
        .duplicate_handle(zx::Rights::SAME_RIGHTS)
        .map_err(|s| ProcessBuilderError::GenericStatus("Failed to dup vDSO VMO handle", s))?;
    Ok(vdso_dup)
}

// Copied from //zircon/system/ulib/elf-psabi/include/lib/elf-psabi/sp.h, must be kept in sync with
// that.
fn compute_initial_stack_pointer(base: usize, size: usize) -> usize {
    // Assume stack grows down.
    let mut sp = base.checked_add(size).expect("Overflow in stack pointer calculation");

    // The x86-64 and AArch64 ABIs require 16-byte alignment.
    // The 32-bit ARM ABI only requires 8-byte alignment, but 16-byte alignment is preferable for
    // NEON so use it there too.
    sp &= 16usize.wrapping_neg();

    // The x86-64 ABI requires %rsp % 16 = 8 on entry.  The zero word at (%rsp) serves as the
    // return address for the outermost frame.
    #[cfg(target_arch = "x86_64")]
    {
        sp -= 8;
    }

    // The ARMv7 and ARMv8 ABIs both just require that SP be aligned, so just catch unknown archs.
    #[cfg(not(any(target_arch = "x86_64", target_arch = "arm", target_arch = "aarch64")))]
    {
        compile_error!("Unknown target_arch");
    }

    sp
}

/// Load the dynamic linker/loader specified in the PT_INTERP header via the fuchsia.ldsvc.Loader
/// handle.
async fn get_dynamic_linker<'a>(
    ldsvc: &'a fldsvc::LoaderProxy,
    executable: &'a zx::Vmo,
    interp_hdr: &'a elf_parse::Elf64ProgramHeader,
) -> Result<zx::Vmo, ProcessBuilderError> {
    // Read the dynamic linker name from the main VMO, based on the PT_INTERP header.
    let mut interp = vec![0u8; interp_hdr.filesz as usize];
    executable
        .read(&mut interp[..], interp_hdr.offset as u64)
        .map_err(|s| ProcessBuilderError::GenericStatus("Failed to read from VMO", s))?;
    let interp_str = std::str::from_utf8(&interp).map_err(|e| {
        ProcessBuilderError::Internal("Interp header contained invalid UTF8", e.into())
    })?;

    // Retrieve the dynamic linker as a VMO from fuchsia.ldsvc.Loader
    const LDSO_LOAD_TIMEOUT_SEC: i64 = 10;
    let load_fut = ldsvc
        .load_object(interp_str)
        .map_err(ProcessBuilderError::LoadDynamicLinker)
        .on_timeout(fasync::Time::after(LDSO_LOAD_TIMEOUT_SEC.seconds()), || {
            Err(ProcessBuilderError::LoadDynamicLinkerTimeout())
        });
    let (status, ld_vmo) = await!(load_fut)?;
    zx::Status::ok(status).map_err(|s| {
        ProcessBuilderError::GenericStatus(
            "Failed to load dynamic linker from fuchsia.ldsvc.Loader",
            s,
        )
    })?;
    Ok(ld_vmo.ok_or(ProcessBuilderError::GenericStatus(
        "load_object status was OK but no VMO",
        zx::Status::INTERNAL,
    ))?)
}

impl BuiltProcess {
    /// Start an already built process.
    ///
    /// This is a simple wrapper around the [zx_process_start] syscall that consumes the handles
    /// and data in the BuiltProcess struct as needed.
    ///
    /// [zx_process_start]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/docs/syscalls/process_start.md
    pub fn start(self) -> Result<zx::Process, ProcessBuilderError> {
        self.process
            .start(
                &self.thread,
                self.entry,
                self.stack,
                self.bootstrap.into_handle(),
                self.vdso_base,
            )
            .map_err(ProcessBuilderError::ProcessStart)?;
        Ok(self.process)
    }
}

struct ReservationVmar(zx::Vmar);

impl ReservationVmar {
    /// Reserve the lower half of the address space of the given VMAR by allocating another VMAR.
    ///
    /// The VMAR wrapped by this reservation is automatically destroyed when the reservation
    /// is dropped.
    fn reserve_low_address_space(vmar: &zx::Vmar) -> Result<ReservationVmar, ProcessBuilderError> {
        let info = vmar
            .info()
            .map_err(|s| ProcessBuilderError::GenericStatus("Failed to get VMAR info", s))?;

        // Reserve the lower half of the full address space, not just half of the VMAR length.
        // (base+len) represents the full address space, assuming this is used with a root VMAR and
        // length extends to the end of the address space, including a region the kernel reserves
        // at the start of the space.
        let reserve_size = util::page_end((info.base + info.len) / 2) - info.base;
        let (reserve_vmar, reserve_base) =
            vmar.allocate(0, reserve_size, zx::VmarFlags::SPECIFIC).map_err(|s| {
                ProcessBuilderError::GenericStatus("Failed to allocate reservation VMAR", s)
            })?;
        assert_eq!(reserve_base, info.base, "Reservation VMAR allocated at wrong address");

        Ok(ReservationVmar(reserve_vmar))
    }
}

// VMARs are not destroyed when the handle is closed (by dropping), so we must explicit destroy it
// to release the reservation and allow the created process to use the full address space.
impl Drop for ReservationVmar {
    fn drop(&mut self) {
        // This is safe because there are no mappings in the region and it is not a region in the
        // current process.
        unsafe {
            self.0.destroy().expect("Failed to destroy reservation VMAR");
        }
    }
}

/// Error type returned by ProcessBuilder methods.
#[allow(missing_docs)] // No docs on individual error variants.
#[derive(Fail, Debug)]
pub enum ProcessBuilderError {
    #[fail(display = "{}", _0)]
    InvalidArg(String),
    #[fail(display = "{}", _0)]
    BadHandle(&'static str),
    #[fail(display = "Failed to create process: {}", _0)]
    CreateProcess(#[cause] zx::Status),
    #[fail(display = "Failed to create thread: {}", _0)]
    CreateThread(#[cause] zx::Status),
    #[fail(display = "Failed to start process: {}", _0)]
    ProcessStart(#[cause] zx::Status),
    #[fail(display = "Failed to parse ELF: {}", _0)]
    ElfParse(#[cause] elf_parse::ElfParseError),
    #[fail(display = "Failed to load ELF: {}", _0)]
    ElfLoad(#[cause] elf_load::ElfLoadError),
    #[fail(display = "{}", _0)]
    Processargs(#[cause] processargs::ProcessargsError),
    #[fail(display = "{}: {}", _0, _1)]
    GenericStatus(&'static str, #[cause] zx::Status),
    #[fail(display = "{}: {}", _0, _1)]
    Internal(&'static str, #[cause] failure::Error),
    #[fail(
        display = "Failed to build process with dynamic ELF, missing fuchsia.ldsvc.Loader handle"
    )]
    LoaderMissing(),
    #[fail(display = "Failed to load dynamic linker from fuchsia.ldsvc.Loader: {}", _0)]
    LoadDynamicLinker(#[cause] fidl::Error),
    #[fail(display = "Timed out loading dynamic linker from fuchsia.ldsvc.Loader")]
    LoadDynamicLinkerTimeout(),
    #[fail(display = "Failed to write bootstrap message to channel: {}", _0)]
    WriteBootstrapMessage(#[cause] zx::Status),
}

impl ProcessBuilderError {
    /// Returns an appropriate zx::Status code for the given error.
    pub fn as_zx_status(&self) -> zx::Status {
        match self {
            ProcessBuilderError::InvalidArg(_) => zx::Status::INVALID_ARGS,
            ProcessBuilderError::BadHandle(_) => zx::Status::BAD_HANDLE,
            ProcessBuilderError::CreateProcess(s)
            | ProcessBuilderError::CreateThread(s)
            | ProcessBuilderError::ProcessStart(s)
            | ProcessBuilderError::GenericStatus(_, s)
            | ProcessBuilderError::WriteBootstrapMessage(s) => *s,
            ProcessBuilderError::ElfParse(e) => e.as_zx_status(),
            ProcessBuilderError::ElfLoad(e) => e.as_zx_status(),
            ProcessBuilderError::Processargs(e) => e.as_zx_status(),
            ProcessBuilderError::Internal(_, _) => zx::Status::INTERNAL,
            ProcessBuilderError::LoaderMissing() => zx::Status::INVALID_ARGS,
            ProcessBuilderError::LoadDynamicLinker(_) => zx::Status::NOT_FOUND,
            ProcessBuilderError::LoadDynamicLinkerTimeout() => zx::Status::TIMED_OUT,
        }
    }
}

impl From<elf_parse::ElfParseError> for ProcessBuilderError {
    fn from(err: elf_parse::ElfParseError) -> Self {
        ProcessBuilderError::ElfParse(err)
    }
}

impl From<elf_load::ElfLoadError> for ProcessBuilderError {
    fn from(err: elf_load::ElfLoadError) -> Self {
        ProcessBuilderError::ElfLoad(err)
    }
}

impl From<processargs::ProcessargsError> for ProcessBuilderError {
    fn from(err: processargs::ProcessargsError) -> Self {
        ProcessBuilderError::Processargs(err)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        failure::{err_msg, Error, ResultExt},
        fidl::endpoints::{Proxy, ServerEnd, ServiceMarker},
        fidl_fuchsia_io as fio,
        fidl_test_processbuilder::{UtilMarker, UtilProxy},
        fuchsia_async as fasync,
        fuchsia_vfs_pseudo_fs::{
            directory::entry::DirectoryEntry, file::simple::read_only, pseudo_directory,
        },
        std::fs::File,
        std::iter,
        std::mem,
        std::path::Path,
        zerocopy::LayoutVerified,
    };

    extern "C" {
        fn dl_clone_loader_service(handle: *mut zx::sys::zx_handle_t) -> zx::sys::zx_status_t;
    }

    // Clone the current loader service to provide to the new test processes.
    fn clone_loader_service() -> Result<ClientEnd<fldsvc::LoaderMarker>, zx::Status> {
        let mut raw = 0;
        let status = unsafe { dl_clone_loader_service(&mut raw) };
        zx::Status::ok(status)?;

        let handle = unsafe { zx::Handle::from_raw(raw) };
        Ok(ClientEnd::new(zx::Channel::from(handle)))
    }

    fn connect_util(client: &zx::Channel) -> Result<UtilProxy, Error> {
        let (proxy, server) = zx::Channel::create()?;
        fdio::service_connect_at(&client, UtilMarker::NAME, server)
            .context("failed to connect to util service")?;
        Ok(UtilProxy::from_channel(fasync::Channel::from_channel(proxy)?))
    }

    fn create_test_util_builder() -> Result<ProcessBuilder, Error> {
        const TEST_UTIL_BIN: &'static str = "/pkg/bin/process_builder_test_util";
        let binpath = Path::new(TEST_UTIL_BIN);
        let vmo = fdio::get_vmo_copy_from_file(&File::open(binpath)?)?.replace_as_executable()?;
        let job = fuchsia_runtime::job_default();

        let procname = CString::new(TEST_UTIL_BIN.to_owned())?;
        Ok(ProcessBuilder::new(&procname, &job, vmo)?)
    }

    // Common builder setup for all tests that start a test util process.
    fn setup_test_util_builder(set_loader: bool) -> Result<(ProcessBuilder, UtilProxy), Error> {
        let mut builder = create_test_util_builder()?;
        if set_loader {
            builder.add_handles(vec![StartupHandle {
                handle: clone_loader_service()?.into_handle(),
                info: HandleInfo::new(HandleType::LdsvcLoader, 0),
            }])?;
        }

        let (dir_client, dir_server) = zx::Channel::create()?;
        builder.add_handles(vec![StartupHandle {
            handle: dir_server.into_handle(),
            info: HandleInfo::new(HandleType::DirectoryRequest, 0),
        }])?;

        let proxy = connect_util(&dir_client)?;
        Ok((builder, proxy))
    }

    fn check_process_running(process: &zx::Process) -> Result<(), Error> {
        let info = process.info()?;
        assert_eq!(
            info,
            zx::ProcessInfo {
                return_code: 0,
                started: true,
                exited: false,
                debugger_attached: false
            }
        );
        Ok(())
    }

    async fn check_process_exited_ok(process: &zx::Process) -> Result<(), Error> {
        await!(fasync::OnSignals::new(process, zx::Signals::PROCESS_TERMINATED))?;

        let info = process.info()?;
        assert_eq!(
            info,
            zx::ProcessInfo {
                return_code: 0,
                started: true,
                exited: true,
                debugger_attached: false
            }
        );
        Ok(())
    }

    // These start_util_with_* tests cover the most common paths through ProcessBuilder and
    // exercise most of its functionality. They verifies that we can create a new process for a
    // "standard" dynamically linked executable and that we can provide arguments, environment
    // variables, namespace entries, and other handles to it through the startup processargs
    // message. The test communicates with the test util process it creates over a test-only FIDL
    // API to verify that arguments and whatnot were passed correctly.
    #[fasync::run_singlethreaded(test)]
    async fn start_util_with_args() -> Result<(), Error> {
        let test_args = vec!["arg0", "arg1", "arg2"];
        let test_args_cstr =
            test_args.iter().map(|s| CString::new(s.clone())).collect::<Result<_, _>>()?;

        let (mut builder, proxy) = setup_test_util_builder(true)?;
        builder.add_arguments(test_args_cstr);
        let process = await!(builder.build())?.start()?;
        check_process_running(&process)?;

        // Use the util protocol to confirm that the new process was set up correctly. A successful
        // connection to the util validates that handles are passed correctly to the new process,
        // since the DirectoryRequest handle made it.
        let proc_args = await!(proxy.get_arguments()).context("failed to get args from util")?;
        assert_eq!(proc_args, test_args);

        mem::drop(proxy);
        await!(check_process_exited_ok(&process))?;
        Ok(())
    }

    // Verify that a loader service handle is properly handled if passed directly to
    // set_loader_service instead of through add_handles.
    #[fasync::run_singlethreaded(test)]
    async fn set_loader_directly() -> Result<(), Error> {
        let test_args = vec!["arg0", "arg1", "arg2"];
        let test_args_cstr =
            test_args.iter().map(|s| CString::new(s.clone())).collect::<Result<_, _>>()?;

        let (mut builder, proxy) = setup_test_util_builder(false)?;
        builder.set_loader_service(clone_loader_service()?)?;
        builder.add_arguments(test_args_cstr);
        let process = await!(builder.build())?.start()?;
        check_process_running(&process)?;

        // Use the util protocol to confirm that the new process was set up correctly. A successful
        // connection to the util validates that handles are passed correctly to the new process,
        // since the DirectoryRequest handle made it.
        let proc_args = await!(proxy.get_arguments()).context("failed to get args from util")?;
        assert_eq!(proc_args, test_args);

        mem::drop(proxy);
        await!(check_process_exited_ok(&process))?;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn start_util_with_env() -> Result<(), Error> {
        let test_env = vec![("VAR1", "value2"), ("VAR2", "value2")];
        let test_env_cstr = test_env
            .iter()
            .map(|v| CString::new(format!("{}={}", v.0, v.1)))
            .collect::<Result<_, _>>()?;

        let (mut builder, proxy) = setup_test_util_builder(true)?;
        builder.add_environment_variables(test_env_cstr);
        let process = await!(builder.build())?.start()?;
        check_process_running(&process)?;

        let proc_env = await!(proxy.get_environment()).context("failed to get env from util")?;
        let proc_env_tuple: Vec<(&str, &str)> =
            proc_env.iter().map(|v| (&*v.key, &*v.value)).collect();
        assert_eq!(proc_env_tuple, test_env);

        mem::drop(proxy);
        await!(check_process_exited_ok(&process))?;
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn start_util_with_namespace_entries() -> Result<(), Error> {
        let mut randbuf = [0; 8];
        zx::cprng_draw(&mut randbuf)?;
        let test_content1 = format!("test content 1 {}", u64::from_le_bytes(randbuf));
        zx::cprng_draw(&mut randbuf)?;
        let test_content2 = format!("test content 2 {}", u64::from_le_bytes(randbuf));

        let test_content1_bytes = test_content1.clone().into_bytes();
        let (dir1_server, dir1_client) = zx::Channel::create()?;
        fasync::spawn(async move {
            let mut dir1 = pseudo_directory! {
                "test_file1" => read_only(|| Ok(test_content1_bytes.clone())),
            };
            dir1.open(
                fio::OPEN_RIGHT_READABLE,
                fio::MODE_TYPE_DIRECTORY,
                &mut iter::empty(),
                ServerEnd::new(dir1_server),
            );
            await!(dir1);
            panic!("Psuedo dir stopped serving!");
        });

        let test_content2_bytes = test_content2.clone().into_bytes();
        let (dir2_server, dir2_client) = zx::Channel::create()?;
        fasync::spawn(async move {
            let mut dir2 = pseudo_directory! {
                "test_file2" => read_only(|| Ok(test_content2_bytes.clone())),
            };
            dir2.open(
                fio::OPEN_RIGHT_READABLE,
                fio::MODE_TYPE_DIRECTORY,
                &mut iter::empty(),
                ServerEnd::new(dir2_server),
            );
            await!(dir2);
            panic!("Psuedo dir stopped serving!");
        });

        let (mut builder, proxy) = setup_test_util_builder(true)?;
        builder.add_namespace_entries(vec![
            NamespaceEntry { path: CString::new("/dir1")?, directory: ClientEnd::new(dir1_client) },
            NamespaceEntry { path: CString::new("/dir2")?, directory: ClientEnd::new(dir2_client) },
        ])?;
        let process = await!(builder.build())?.start()?;
        check_process_running(&process)?;

        let namespace_dump = await!(proxy.dump_namespace()).context("failed to dump namespace")?;
        assert_eq!(namespace_dump, "/dir1, /dir1/test_file1, /dir2, /dir2/test_file2");

        let dir1_contents =
            await!(proxy.read_file("/dir1/test_file1")).context("failed to read file via util")?;
        assert_eq!(dir1_contents, test_content1);
        let dir2_contents =
            await!(proxy.read_file("/dir2/test_file2")).context("failed to read file via util")?;
        assert_eq!(dir2_contents, test_content2);

        mem::drop(proxy);
        await!(check_process_exited_ok(&process))?;
        Ok(())
    }

    // Trying to start a dynamically linked process without providing a loader service should
    // fail. This verifies that nothing is automatically cloning a loader.
    #[fasync::run_singlethreaded(test)]
    async fn start_util_with_no_loader_fails() -> Result<(), Error> {
        let (builder, _) = setup_test_util_builder(false)?;
        let result = await!(builder.build());

        match result {
            Err(ProcessBuilderError::LoaderMissing()) => {}
            Err(err) => {
                panic!("Unexpected error type: {}", err);
            }
            Ok(_) => {
                panic!("Unexpectedly succeeded to build process without loader");
            }
        }
        Ok(())
    }

    // Checks that, for dynamically linked binaries, the lower half of the address space is
    // reserved for sanitizers.
    #[fasync::run_singlethreaded(test)]
    async fn verify_low_address_range_reserved() -> Result<(), Error> {
        let (builder, _) = setup_test_util_builder(true)?;
        let built = await!(builder.build())?;

        // This ends up being the same thing ReservationVmar does, but it's not reused here so that
        // this catches bugs or bad changes to ReservationVmar itself.
        let info = built.root_vmar.info()?;
        let lower_half_len = util::page_end((info.base + info.len) / 2) - info.base;
        built
            .root_vmar
            .allocate(0, lower_half_len, zx::VmarFlags::SPECIFIC)
            .context("Unable to allocate lower address range of new process")?;
        Ok(())
    }

    // Parses the given channel message as a processargs message and returns the HandleInfo's
    // contained in it.
    fn parse_handle_info_from_message(message: &zx::MessageBuf) -> Result<Vec<HandleInfo>, Error> {
        let bytes = message.bytes();
        let header = LayoutVerified::<&[u8], processargs::MessageHeader>::new_from_prefix(bytes)
            .ok_or(err_msg("Failed to parse processargs header"))?
            .0;

        let offset = header.handle_info_off as usize;
        let len = mem::size_of::<u32>() * message.n_handles();
        let info_bytes = &bytes[offset..offset + len];
        let raw_info = LayoutVerified::<&[u8], [u32]>::new_slice(info_bytes)
            .ok_or(err_msg("Failed to parse raw handle info"))?;

        Ok(raw_info.iter().map(|raw| HandleInfo::try_from(*raw)).collect::<Result<_, _>>()?)
    }

    const LINKER_MESSAGE_HANDLES: &[HandleType] = &[
        HandleType::ProcessSelf,
        HandleType::ThreadSelf,
        HandleType::RootVmar,
        HandleType::LdsvcLoader,
        HandleType::LoadedVmar,
        HandleType::ExecutableVmo,
    ];

    const MAIN_MESSAGE_HANDLES: &[HandleType] = &[
        HandleType::ProcessSelf,
        HandleType::ThreadSelf,
        HandleType::RootVmar,
        HandleType::VdsoVmo,
        HandleType::StackVmo,
    ];

    #[fasync::run_singlethreaded(test)]
    async fn correct_handles_present() -> Result<(), Error> {
        let mut builder = create_test_util_builder()?;
        builder.set_loader_service(clone_loader_service()?)?;
        let built = await!(builder.build())?;

        for correct in &[LINKER_MESSAGE_HANDLES, MAIN_MESSAGE_HANDLES] {
            let mut msg_buf = zx::MessageBuf::new();
            built.bootstrap.read(&mut msg_buf)?;
            let handle_info = parse_handle_info_from_message(&msg_buf)?;

            assert_eq!(handle_info.len(), correct.len());
            for correct_type in *correct {
                // Should only be one of each of these handles present.
                assert_eq!(
                    1,
                    handle_info.iter().filter(|info| &info.handle_type() == correct_type).count()
                );
            }
        }
        Ok(())
    }

    // Verify that [ProcessBuilder::add_handles()] rejects handle types that are added
    // automatically by the builder.
    #[fasync::run_singlethreaded(test)]
    async fn add_handles_rejects_automatic_handle_types() -> Result<(), Error> {
        // The VMO doesn't need to be valid since we're not calling build.
        let vmo = zx::Vmo::create(1)?;
        let job = fuchsia_runtime::job_default();
        let procname = CString::new("dummy_name")?;
        let mut builder = ProcessBuilder::new(&procname, &job, vmo)?;

        // There's some duplicates between these slices but just checking twice is easier than
        // deduping these.
        for handle_type in LINKER_MESSAGE_HANDLES.iter().chain(MAIN_MESSAGE_HANDLES) {
            if *handle_type == HandleType::LdsvcLoader {
                // Skip LdsvcLoader, which is required in the linker message but is not added
                // automatically. The user must supply it.
                continue;
            }

            // Another dummy VMO, just to have a valid handle.
            let dummy_vmo = zx::Vmo::create(1)?;
            let result = builder.add_handles(vec![StartupHandle {
                handle: dummy_vmo.into_handle(),
                info: HandleInfo::new(*handle_type, 0),
            }]);
            match result {
                Err(ProcessBuilderError::InvalidArg(_)) => {}
                Err(err) => {
                    panic!("Unexpected error type, should be invalid arg: {}", err);
                }
                Ok(_) => {
                    panic!("add_handle unexpectedly succeeded for type {:?}", handle_type);
                }
            }
        }
        Ok(())
    }

    // Verify that invalid handles are correctly rejected.
    #[fasync::run_singlethreaded(test)]
    async fn rejects_invalid_handles() -> Result<(), Error> {
        let invalid = || zx::Handle::invalid();
        let assert_invalid_arg = |result| match result {
            Err(ProcessBuilderError::BadHandle(_)) => {}
            Err(err) => {
                panic!("Unexpected error type, should be BadHandle: {}", err);
            }
            Ok(_) => {
                panic!("API unexpectedly accepted invalid handle");
            }
        };

        // The VMO doesn't need to be valid since we're not calling build with this.
        let vmo = zx::Vmo::create(1)?;
        let job = fuchsia_runtime::job_default();
        let procname = CString::new("dummy_name")?;

        assert_invalid_arg(ProcessBuilder::new(&procname, &invalid().into(), vmo).map(|_| ()));
        assert_invalid_arg(ProcessBuilder::new(&procname, &job, invalid().into()).map(|_| ()));

        let (mut builder, _) = setup_test_util_builder(true)?;

        assert_invalid_arg(builder.set_loader_service(invalid().into()));
        assert_invalid_arg(builder.add_handles(vec![StartupHandle {
            handle: invalid().into(),
            info: HandleInfo::new(HandleType::User0, 0),
        }]));
        assert_invalid_arg(builder.add_handles(vec![StartupHandle {
            handle: invalid().into(),
            info: HandleInfo::new(HandleType::User0, 0),
        }]));
        assert_invalid_arg(builder.add_namespace_entries(vec![NamespaceEntry {
            path: CString::new("/dir")?,
            directory: invalid().into(),
        }]));

        Ok(())
    }

    #[fasync::run_singlethreaded]
    #[test]
    async fn start_static_pie_binary() -> Result<(), Error> {
        const TEST_BIN: &'static str = "/pkg/bin/static_pie_test_util";
        let binpath = Path::new(TEST_BIN);
        let vmo = fdio::get_vmo_copy_from_file(&File::open(binpath)?)?.replace_as_executable()?;
        let job = fuchsia_runtime::job_default();

        let procname = CString::new(TEST_BIN.to_owned())?;
        let mut builder = ProcessBuilder::new(&procname, &job, vmo)?;

        // We pass the program a channel with handle type User0 which we send a message on and
        // expect it to echo back the message on the same channel.
        let (local, remote) = zx::Channel::create()?;
        builder.add_handles(vec![StartupHandle {
            handle: remote.into_handle(),
            info: HandleInfo::new(HandleType::User0, 0),
        }])?;

        let mut randbuf = [0; 8];
        zx::cprng_draw(&mut randbuf)?;
        let test_message = format!("test content 1 {}", u64::from_le_bytes(randbuf)).into_bytes();
        local.write(&test_message, &mut vec![])?;

        // Start process and wait for channel to have a message to read or be closed.
        await!(builder.build())?.start()?;
        let signals = await!(fasync::OnSignals::new(
            &local,
            zx::Signals::CHANNEL_READABLE | zx::Signals::CHANNEL_PEER_CLOSED
        ))?;
        assert!(signals.contains(zx::Signals::CHANNEL_READABLE));

        let mut echoed = zx::MessageBuf::new();
        local.read(&mut echoed)?;
        assert_eq!(echoed.bytes(), test_message.as_slice());
        assert_eq!(echoed.n_handles(), 0);

        Ok(())
    }
}
