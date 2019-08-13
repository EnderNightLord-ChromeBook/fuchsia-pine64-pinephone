// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "system_objects_directory.h"

#include <fs/pseudo-file.h>
#include <src/lib/fxl/strings/string_printf.h>

#include <algorithm>

#include "debug_info_retriever.h"

using fxl::StringPrintf;

namespace {
struct ThreadInfo {
  zx_koid_t koid;
  fbl::String name;
  zx::thread thread;
};

static constexpr size_t kMaxThreads = 2048;

const char* obj_type_get_name(zx_obj_type_t type) {
  switch (type) {
    case ZX_OBJ_TYPE_NONE:
      return "none";
    case ZX_OBJ_TYPE_PROCESS:
      return "process";
    case ZX_OBJ_TYPE_THREAD:
      return "thread";
    case ZX_OBJ_TYPE_VMO:
      return "vmo";
    case ZX_OBJ_TYPE_CHANNEL:
      return "channel";
    case ZX_OBJ_TYPE_EVENT:
      return "event";
    case ZX_OBJ_TYPE_PORT:
      return "port";
    case ZX_OBJ_TYPE_INTERRUPT:
      return "interrupt";
    case ZX_OBJ_TYPE_PCI_DEVICE:
      return "pci_device";
    case ZX_OBJ_TYPE_LOG:
      return "log";
    case ZX_OBJ_TYPE_SOCKET:
      return "socket";
    case ZX_OBJ_TYPE_RESOURCE:
      return "resource";
    case ZX_OBJ_TYPE_EVENTPAIR:
      return "eventpair";
    case ZX_OBJ_TYPE_JOB:
      return "job";
    case ZX_OBJ_TYPE_VMAR:
      return "vmar";
    case ZX_OBJ_TYPE_FIFO:
      return "fifo";
    case ZX_OBJ_TYPE_GUEST:
      return "guest";
    case ZX_OBJ_TYPE_VCPU:
      return "vcpu";
    case ZX_OBJ_TYPE_TIMER:
      return "timer";
    case ZX_OBJ_TYPE_IOMMU:
      return "iommu";
    case ZX_OBJ_TYPE_BTI:
      return "bti";
    case ZX_OBJ_TYPE_PROFILE:
      return "profile";
    default:
      return "unknown";
  }
}

zx_status_t GetProcessHandleStats(const zx::process* process,
                                  zx_info_process_handle_stats_t* process_handle_stats) {
  zx_status_t status = process->get_info(ZX_INFO_PROCESS_HANDLE_STATS, process_handle_stats,
                                         sizeof(zx_info_process_handle_stats), nullptr, nullptr);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "zx_object_get_info failed, status: " << status;
    return status;
  }

  return ZX_OK;
}

zx_status_t GetTaskStats(const zx::process* process, zx_info_task_stats_t* task_stats) {
  zx_status_t status = process->get_info(ZX_INFO_TASK_STATS, task_stats,
                                         sizeof(zx_info_task_stats_t), nullptr, nullptr);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "zx_object_get_info failed, status: " << status;
    return status;
  }

  return ZX_OK;
}

void GetThreads(const zx::process* process, fbl::Vector<ThreadInfo>* out) {
  zx_koid_t thread_ids[kMaxThreads];
  size_t num_ids;
  if (process->get_info(ZX_INFO_PROCESS_THREADS, thread_ids, sizeof(zx_koid_t) * kMaxThreads,
                        &num_ids, nullptr) != ZX_OK) {
    return;
  }

  for (size_t i = 0; i < num_ids; i++) {
    zx::thread t;
    char name[ZX_MAX_NAME_LEN];
    if (process->get_child(thread_ids[i], ZX_RIGHT_SAME_RIGHTS, &t) != ZX_OK) {
      return;
    }
    if (t.get_property(ZX_PROP_NAME, &name, ZX_MAX_NAME_LEN) != ZX_OK) {
      return;
    }
    t.get_property(ZX_PROP_NAME, &name, ZX_MAX_NAME_LEN);
    out->push_back({thread_ids[i], name, std::move(t)});
  }
}

zx_status_t GetThreadStats(zx_handle_t thread, zx_info_thread_stats_t* thread_stats) {
  zx_status_t status = zx_object_get_info(thread, ZX_INFO_THREAD_STATS, thread_stats,
                                          sizeof(zx_info_thread_stats_t), nullptr, nullptr);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "zx_object_get_info failed, status: " << status << " thread: " << thread;
    return status;
  }

  return ZX_OK;
}

}  // namespace

namespace component {

SystemObjectsDirectory::SystemObjectsDirectory(zx::process process)
    : ExposedObject("system_objects"),
      process_(std::make_shared<zx::process>(std::move(process))),
      threads_(std::make_unique<ThreadsDirectory>(process_)),
      memory_(std::make_unique<MemoryDirectory>(process_)) {
  std::weak_ptr<zx::process> weak_process = process_;
  add_child(threads_.get());
  add_child(memory_.get());
  object_dir().set_children_callback([weak_process](component::Object::ObjectVector* out_children) {
    auto process = weak_process.lock();
    if (!process) {
      return;
    }
    zx_info_process_handle_stats_t process_handle_stats;
    if (GetProcessHandleStats(process.get(), &process_handle_stats) != ZX_OK)
      return;
    auto handle_count_dir = component::ObjectDir::Make("handle_count");

    for (zx_obj_type_t obj_type = ZX_OBJ_TYPE_NONE; obj_type < ZX_OBJ_TYPE_UPPER_BOUND;
         ++obj_type) {
      handle_count_dir.set_metric(
          obj_type_get_name(obj_type),
          component::UIntMetric(process_handle_stats.handle_count[obj_type]));
    }
    out_children->push_back(handle_count_dir.object());
  });
}

SystemObjectsDirectory::ThreadsDirectory::ThreadsDirectory(std::shared_ptr<zx::process> process)
    : ExposedObject("threads"), process_(process) {
  std::weak_ptr<zx::process> weak_process = process_;
  auto all_dir = component::ObjectDir::Make("all_thread_stacks");
  all_dir.set_prop("stacks", [weak_process]() -> std::string {
    auto process = weak_process.lock();
    if (!process) {
      return "Error: Process closed";
    }
    return StringPrintf("\n%s", DebugInfoRetriever::GetInfo(process.get()).data());
  });

  object_dir().set_child(all_dir.object());
  object_dir().set_children_callback([weak_process](component::Object::ObjectVector* out_children) {
    auto process = weak_process.lock();
    if (!process) {
      return;
    }
    fbl::Vector<ThreadInfo> threads;
    threads.reserve(kMaxThreads);
    GetThreads(process.get(), &threads);

    for (const auto& thread : threads) {
      auto koid_string = StringPrintf("%lu", thread.koid);
      auto thread_obj = component::ObjectDir::Make(koid_string);
      thread_obj.set_prop("koid", koid_string);
      thread_obj.set_prop("name", thread.name.data());
      zx_handle_t handle = thread.thread.get();
      zx_info_thread_stats_t thread_stats;
      thread_obj.set_metric(
          "total_runtime",
          UIntMetric(GetThreadStats(handle, &thread_stats) == ZX_OK ? thread_stats.total_runtime
                                                                    : 0u));
      out_children->push_back(thread_obj.object());

      auto koid = thread.koid;
      auto stack_obj = component::ObjectDir::Make("stack");
      stack_obj.set_prop("dump", [weak_process, koid]() -> std::string {
        auto process = weak_process.lock();
        if (!process) {
          return "Error: Process stopped";
        }

        zx_koid_t koids[] = {koid};
        return StringPrintf("\n%s", DebugInfoRetriever::GetInfo(process.get(), koids, 1).data());
      });
      thread_obj.set_child(stack_obj.object());
    }
  });
}

SystemObjectsDirectory::MemoryDirectory::MemoryDirectory(std::shared_ptr<zx::process> process)
    : ExposedObject("memory"), process_(process) {
  std::weak_ptr<zx::process> weak_process = process_;
  object_dir().set_metric("mapped_bytes",
                          component::CallbackMetric([weak_process](component::Metric* out) {
                            auto process = weak_process.lock();
                            if (!process) {
                              return;
                            }
                            zx_info_task_stats_t task_stats;
                            if (GetTaskStats(process.get(), &task_stats) != ZX_OK)
                              return;
                            out->SetUInt(task_stats.mem_mapped_bytes);
                          }));

  object_dir().set_metric("private_bytes",
                          component::CallbackMetric([weak_process](component::Metric* out) {
                            auto process = weak_process.lock();
                            if (!process) {
                              return;
                            }
                            zx_info_task_stats_t task_stats;
                            if (GetTaskStats(process.get(), &task_stats) != ZX_OK)
                              return;
                            out->SetUInt(task_stats.mem_private_bytes);
                          }));

  object_dir().set_metric("shared_bytes",
                          component::CallbackMetric([weak_process](component::Metric* out) {
                            auto process = weak_process.lock();
                            if (!process) {
                              return;
                            }
                            zx_info_task_stats_t task_stats;
                            if (GetTaskStats(process.get(), &task_stats) != ZX_OK)
                              return;
                            out->SetUInt(task_stats.mem_shared_bytes);
                          }));

  object_dir().set_metric("scaled_shared_bytes",
                          component::CallbackMetric([weak_process](component::Metric* out) {
                            auto process = weak_process.lock();
                            if (!process) {
                              return;
                            }
                            zx_info_task_stats_t task_stats;
                            if (GetTaskStats(process.get(), &task_stats) != ZX_OK)
                              return;
                            out->SetUInt(task_stats.mem_scaled_shared_bytes);
                          }));
}

}  // namespace component
