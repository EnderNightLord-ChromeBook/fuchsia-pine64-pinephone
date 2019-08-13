// Copyright 2019  The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/system-metrics/memory_stats_fetcher_impl.h"

#include <fcntl.h>
#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/boot/c/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/resource.h>
#include <trace/event.h>
#include <zircon/status.h>

#include "lib/syslog/cpp/logger.h"

namespace cobalt {

MemoryStatsFetcherImpl::MemoryStatsFetcherImpl() { InitializeRootResourceHandle(); }

bool MemoryStatsFetcherImpl::FetchMemoryStats(zx_info_kmem_stats_t* mem_stats) {
  TRACE_DURATION("system_metrics", "MemoryStatsFetcherImpl::FetchMemoryStats");
  if (root_resource_handle_ == ZX_HANDLE_INVALID) {
    FX_LOGS(ERROR) << "MemoryStatsFetcherImpl: No root resource"
                   << "present. Reconnecting...";
    InitializeRootResourceHandle();
    return false;
  }
  zx_status_t err = zx_object_get_info(root_resource_handle_, ZX_INFO_KMEM_STATS, mem_stats,
                                       sizeof(*mem_stats), NULL, NULL);
  if (err != ZX_OK) {
    FX_LOGS(ERROR) << "MemoryStatsFetcherImpl: Fetching "
                   << "ZX_INFO_KMEM_STATS through syscall returns " << zx_status_get_string(err);
    return false;
  }
  return true;
}

// TODO(CF-691) When Component Stats (CS) supports memory metrics,
// switch to Component Stats / iquery, by creating a new class with the
// interface MemoryStatsFetcher.
void MemoryStatsFetcherImpl::InitializeRootResourceHandle() {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    return;
  }
  static const char kRootResourceSvc[] = "/svc/fuchsia.boot.RootResource";
  status = fdio_service_connect(kRootResourceSvc, remote.release());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Cobalt SystemMetricsDaemon: Error getting root_resource_handle_. "
                   << "Cannot open fuchsia.boot.RootResource: " << zx_status_get_string(status);
    return;
  }
  zx_status_t fidl_status = fuchsia_boot_RootResourceGet(local.get(), &root_resource_handle_);
  if (fidl_status != ZX_OK) {
    FX_LOGS(ERROR) << "Cobalt SystemMetricsDaemon: Error getting root_resource_handle_. "
                   << zx_status_get_string(fidl_status);
    return;
  } else if (root_resource_handle_ == ZX_HANDLE_INVALID) {
    FX_LOGS(ERROR) << "Cobalt SystemMetricsDaemon: Failed to get root_resource_handle_.";
    return;
  }
}

}  // namespace cobalt
