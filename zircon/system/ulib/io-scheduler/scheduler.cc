// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <io-scheduler/io-scheduler.h>

#include <fbl/auto_lock.h>

#include <stdio.h>

namespace ioscheduler {

zx_status_t Scheduler::Init(SchedulerClient* client, uint32_t options) {
  client_ = client;
  options_ = options;
  fbl::AutoLock lock(&lock_);
  shutdown_initiated_ = false;
  return ZX_OK;
}

void Scheduler::Shutdown() {
  if (client_ == nullptr) {
    return;  // Not initialized or already shut down.
  }

  // Wake threads blocking on incoming ops.
  // Threads will complete outstanding work and exit.
  client_->CancelAcquire();
  {
    fbl::AutoLock lock(&lock_);
    shutdown_initiated_ = true;

    // Close all streams.
    for (auto& stream : open_map_) {
      stream.Close();
    }
  }

  // Wake all workers blocking on the queue. They will observe shutdown_initiated_ and exit.
  queue_.SignalAvailable();

  // Block until all worker threads exit.
  workers_.reset();

  {
    fbl::AutoLock lock(&lock_);
    // Delete any existing stream in the case where no worker threads were launched.
    open_map_.clear();
    closed_map_.clear();
  }

  client_ = nullptr;
}

zx_status_t Scheduler::StreamOpen(uint32_t id, uint32_t priority) {
  if (priority > kMaxPriority) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&lock_);
  auto iter = open_map_.find(id);
  if (iter.IsValid()) {
    return ZX_ERR_ALREADY_EXISTS;
  }

  fbl::AllocChecker ac;
  StreamRef stream = fbl::AdoptRef(new (&ac) Stream(id, priority, this));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  open_map_.insert(std::move(stream));
  return ZX_OK;
}

zx_status_t Scheduler::StreamClose(uint32_t id) {
  fbl::AutoLock lock(&lock_);
  StreamRef stream = open_map_.erase(id);
  if (stream == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (stream->Close() == ZX_OK) {
    // Stream has no more ops. No more ops can be added since it is now closed.
    // Stream will be deleted when all references are released.
    return ZX_OK;
  }
  // Stream is closed but still active. No more ops can be added.
  // Retain a reference to it in the closed map. Stream will call SetIdle() when it is ready
  // for deletion.
  closed_map_.insert(std::move(stream));
  return ZX_OK;
}

zx_status_t Scheduler::Serve() {
  ZX_DEBUG_ASSERT(client_ != nullptr);

  // Create a single thread for now.
  const uint32_t num_workers = 1;

  for (uint32_t i = 0; i < num_workers; i++) {
    fbl::unique_ptr<Worker> worker;
    zx_status_t status = Worker::Create(this, i, &worker);
    if (status != ZX_OK) {
      fprintf(stderr, "Scheduler: Failed to create worker thread\n");
      Shutdown();
      return status;
    }
    workers_.push_back(std::move(worker));
  }
  return ZX_OK;
}

void Scheduler::AsyncComplete(StreamOp* sop) {
  ZX_DEBUG_ASSERT(false);  // Not yet implemented.
}

Scheduler::~Scheduler() { Shutdown(); }

// Enqueue - file a list of ops into their respective streams and schedule those streams.
zx_status_t Scheduler::Enqueue(UniqueOp* in_list, size_t in_count, UniqueOp* out_list,
                               size_t* out_actual) {
  size_t out_num = 0;
  for (size_t i = 0; i < in_count; i++) {
    UniqueOp op = std::move(in_list[i]);

    // Initialize op fields modified by scheduler.
    op->set_result(ZX_OK);

    StreamRef stream;
    if (FindStream(op->stream_id(), &stream) != ZX_OK) {
      op->set_result(ZX_ERR_INVALID_ARGS);
      out_list[out_num++] = std::move(op);
      continue;
    }
    zx_status_t status = stream->Insert(std::move(op), &out_list[out_num]);
    if (status != ZX_OK) {
      // Stream is closed, cannot add ops. Op has been added to out_list[out_num]
      out_num++;
      continue;
    }
  }
  *out_actual = out_num;
  return ZX_OK;
}

zx_status_t Scheduler::Dequeue(UniqueOp* op_out, bool wait) {
  StreamRef stream;
  zx_status_t status = queue_.GetNextStream(wait, &stream);
  if (status != ZX_OK) {
    return status;
  }
  stream->GetNext(op_out);
  return ZX_OK;
}

bool Scheduler::ShutdownInitiated() {
  fbl::AutoLock lock(&lock_);
  return shutdown_initiated_;
}

zx_status_t Scheduler::FindStream(uint32_t id, StreamRef* out) {
  fbl::AutoLock lock(&lock_);
  auto iter = open_map_.find(id);
  if (!iter.IsValid()) {
    return ZX_ERR_NOT_FOUND;
  }
  *out = StreamRef(iter.CopyPointer());
  return ZX_OK;
}

void Scheduler::StreamRelease(uint32_t id) {
  fbl::AutoLock lock(&lock_);
  // Stream is already closed and should be in the closed map, pending release.
  ZX_DEBUG_ASSERT(!closed_map_.is_empty());
  closed_map_.erase(id);
}

}  // namespace ioscheduler
