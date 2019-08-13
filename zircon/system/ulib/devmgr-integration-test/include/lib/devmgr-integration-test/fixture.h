// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <lib/devmgr-launcher/launch.h>
#include <lib/zx/job.h>
#include <lib/zx/time.h>

namespace devmgr_integration_test {

class IsolatedDevmgr {
 public:
  IsolatedDevmgr() = default;
  ~IsolatedDevmgr();

  IsolatedDevmgr(const IsolatedDevmgr&) = delete;
  IsolatedDevmgr& operator=(const IsolatedDevmgr&) = delete;

  IsolatedDevmgr(IsolatedDevmgr&& other)
      : job_(std::move(other.job_)), devfs_root_(std::move(other.devfs_root_)) {}

  IsolatedDevmgr& operator=(IsolatedDevmgr&& other) {
    job_ = std::move(other.job_);
    devfs_root_ = std::move(other.devfs_root_);
    return *this;
  }

  // Path to the test sysdev driver
  static inline constexpr char kSysdevDriver[] = "/boot/driver/test/sysdev.so";

  // Get an args structure pre-populated with the test sysdev driver, the
  // test control driver, and the test driver directory.
  static devmgr_launcher::Args DefaultArgs();

  // Launch a new isolated devmgr.  The instance will be destroyed when
  // |*out|'s dtor runs.
  static zx_status_t Create(devmgr_launcher::Args args, IsolatedDevmgr* out);

  // Get a fd to the root of the isolate devmgr's devfs.  This fd
  // may be used with openat() and fdio_watch_directory().
  const fbl::unique_fd& devfs_root() const { return devfs_root_; }

  // Borrow the handle to the job containing the isolated devmgr.  This may be
  // used for things like binding to an exception port.
  const zx::job& containing_job() const { return job_; }

  void reset() { *this = IsolatedDevmgr(); }

 private:
  // Job that contains the devmgr environment
  zx::job job_;

  // FD to the root of devmgr's devfs
  fbl::unique_fd devfs_root_;
};

// Wait for |file| to appear in |dir|, and open it when it does.
zx_status_t WaitForFile(const fbl::unique_fd& dir, const char* file, fbl::unique_fd* out);

// Waits for the relative |path| starting in |dir| to appear, and opens it.
zx_status_t RecursiveWaitForFile(const fbl::unique_fd& dir, const char* path, fbl::unique_fd* out);

// DirWatcher can be used to detect when a file has been removed from the filesystem.
//
// Example usage:
//
//   std::unique_ptr<DirWatcher> watcher;
//   zx_status_t status = DirWatcher::Create(dir_fd, &watcher);
//   ...
//   // Trigger removal of file here.
//   ...
//   status = watcher->WaitForRemoval(filename, deadline);
class DirWatcher {
 public:
  // |dir_fd| is the directory to watch.
  static zx_status_t Create(fbl::unique_fd dir_fd, std::unique_ptr<DirWatcher>* out_dir_watcher);

  // Users should call Create instead. This is public for make_unique.
  explicit DirWatcher(zx::channel client) : client_(std::move(client)) {}

  // Returns ZX_OK if |filename| is removed from the directory before the given timeout elapses.
  zx_status_t WaitForRemoval(const fbl::String& filename, zx::duration timeout);

 private:
  zx::channel client_;
};

}  // namespace devmgr_integration_test
