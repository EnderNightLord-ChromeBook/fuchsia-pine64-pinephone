// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_FSHOST_FILESYSTEM_MOUNTER_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_FSHOST_FILESYSTEM_MOUNTER_H_

#include <lib/zx/channel.h>
#include <zircon/types.h>

#include <memory>

#include <fbl/unique_fd.h>
#include <fs-management/mount.h>

#include "fs-manager.h"
#include "metrics.h"

namespace devmgr {

// FilesystemMounter is a utility class which wraps the FsManager
// and helps clients mount filesystems within the fshost namespace.
class FilesystemMounter {
 public:
  FilesystemMounter(std::unique_ptr<FsManager> fshost, bool netboot, bool check_filesystems)
      : fshost_(std::move(fshost)), netboot_(netboot), check_filesystems_(check_filesystems) {}

  virtual ~FilesystemMounter() = default;

  void FuchsiaStart() const { fshost_->FuchsiaStart(); }

  zx_status_t InstallFs(const char* path, zx::channel h) {
    return fshost_->InstallFs(path, std::move(h));
  }

  bool Netbooting() const { return netboot_; }
  bool ShouldCheckFilesystems() const { return check_filesystems_; }

  // Attempts to mount a block device to "/data".
  // Fails if already mounted.
  zx_status_t MountData(zx::channel block_device_client, const mount_options_t& options);

  // Attempts to mount a block device to "/install".
  // Fails if already mounted.
  zx_status_t MountInstall(zx::channel block_device_client, const mount_options_t& options);

  // Attempts to mount a block device to "/blob".
  // Fails if already mounted.
  zx_status_t MountBlob(zx::channel block_device_client, const mount_options_t& options);

  // Attempts to mount pkgfs if all preconditions have been met:
  // - Pkgfs has not previously been mounted
  // - Blobfs has been mounted
  // - The data partition has been mounted
  void TryMountPkgfs();

  // Returns a pointer to the |FsHostMetrics| instance.
  FsHostMetrics* mutable_metrics() { return fshost_->mutable_metrics(); }

  void FlushMetrics() { fshost_->FlushMetrics(); }

  bool BlobMounted() const { return blob_mounted_; }
  bool DataMounted() const { return data_mounted_; }
  bool PkgfsMounted() const { return pkgfs_mounted_; }

 private:
  // Performs the mechanical action of mounting a filesystem, without
  // validating the type of filesystem being mounted.
  zx_status_t MountFilesystem(const char* mount_path, const char* binary,
                              const mount_options_t& options, zx::channel block_device_client);

  // Actually launches the filesystem process.
  //
  // Virtualized to enable testing.
  virtual zx_status_t LaunchFs(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids,
                               size_t len);

  std::unique_ptr<FsManager> fshost_;
  const bool netboot_ = false;
  const bool check_filesystems_ = false;
  bool data_mounted_ = false;
  bool install_mounted_ = false;
  bool blob_mounted_ = false;
  bool pkgfs_mounted_ = false;
};

}  // namespace devmgr

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_FSHOST_FILESYSTEM_MOUNTER_H_
