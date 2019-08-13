// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <stdint.h>
#include <vector>

#include <fbl/condition_variable.h>
#include <fbl/function.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <fbl/vector.h>
#include <zircon/types.h>

#include <io-scheduler/queue.h>
#include <io-scheduler/scheduler-client.h>
#include <io-scheduler/stream.h>
#include <io-scheduler/stream-op.h>
#include <io-scheduler/worker.h>

namespace ioscheduler {

// Reordering rules for the scheduler.
// Allow reordering of Read class operations with respect to each other.
constexpr uint32_t kOptionReorderReads = (1u << 0);

// Allow reordering of Write class operations with respect to each other.
constexpr uint32_t kOptionReorderWrites = (1u << 1);

// Allow reordering of Read class operations ahead of Write class operations.
constexpr uint32_t kOptionReorderReadsAheadOfWrites = (1u << 2);

// Allow reordering of Write class operations ahead of Read class operations.
constexpr uint32_t kOptionReorderWritesAheadOfReads = (1u << 3);

// Disallow any reordering.
constexpr uint32_t kOptionStrictlyOrdered = 0;

// Allow all reordering options.
constexpr uint32_t kOptionFullyOutOfOrder =
    (kOptionReorderReads | kOptionReorderWrites | kOptionReorderReadsAheadOfWrites |
     kOptionReorderWritesAheadOfReads);

// Maximum priority for a stream.
constexpr uint32_t kMaxPriority = 31;

// Suggested default priority for a stream.
constexpr uint32_t kDefaultPriority = 8;

class Scheduler {
 public:
  Scheduler() : queue_(this) {}
  ~Scheduler();
  DISALLOW_COPY_ASSIGN_AND_MOVE(Scheduler);

  // Client API - synchronous calls.
  // -------------------------------

  // Initialize a Scheduler object to usable state. Initialize must be called on
  // a newly created Scheduler object or Scheduler that has been shut down
  // before it can be used.
  // The Scheduler holds a pointer to |client| until Shutdown() has returned. It does not
  // manage the lifetime of this pointer and does not free it.
  zx_status_t Init(SchedulerClient* client, uint32_t options) __TA_EXCLUDES(lock_);

  // Open a new stream with the requested ID and priority. It is safe to invoke
  // this function from a Scheduler callback context, except from Fatal().
  // |id| may not be that of a currently open stream.
  // |priority| must be in the inclusive range 0 to kMaxPriority.
  // Returns:
  // ZX_OK on success.
  // ZX_ERR_ALREADY_EXISTS if stream with same |id| is already open.
  // ZX_ERR_INVALID_ARGS if |priority| is out of range.
  // Other error status for internal errors.
  zx_status_t StreamOpen(uint32_t id, uint32_t priority) __TA_EXCLUDES(lock_);

  // Close an open stream. All ops in the stream will be issued before the stream
  // is closed. New incoming ops to the closed stream will be released with
  // an error.
  zx_status_t StreamClose(uint32_t id) __TA_EXCLUDES(lock_);

  // Begin scheduler service. This creates the worker threads that will invoke
  // the callbacks in SchedulerCallbacks.
  zx_status_t Serve() __TA_EXCLUDES(lock_);

  // End scheduler service. This function blocks until all outstanding ops in
  // all streams are completed and closes all streams. Shutdown should not be invoked from a
  // callback function. To reuse the scheduler, call Init() again.
  void Shutdown() __TA_EXCLUDES(lock_);

  // Client API - asynchronous calls.
  // --------------------------------

  // Asynchronous completion. When an issued operation has completed
  // asynchronously, this function should be called. The status of the operation
  // should be set in |sop|’s result field. This function is non-blocking and
  // safe to call from an interrupt handler context.
  void AsyncComplete(StreamOp* sop) __TA_EXCLUDES(lock_);

  // API invoked by worker threads.
  // --------------------------------
  SchedulerClient* client() { return client_; }

  // Insert a list of ops into the scheduler queue.
  //
  // Ownership:
  //    Ops are exclusively retained by the Scheduler if they were successfully enqueued. Ops that
  // encounter enqueueing errors will be added to |out_list| for caller to release.
  //
  // |in_list| and |out_list| may point to the same buffer.
  zx_status_t Enqueue(UniqueOp* in_list, size_t in_count, UniqueOp* out_list, size_t* out_actual)
      __TA_EXCLUDES(lock_);

  // Remove an op from the scheduler queue.
  //
  // Ownership:
  //    If successful, ownership of the op is transferred to the caller.
  //
  // If no ops are available:
  //      returns ZX_ERR_CANCELED if shutdown has started.
  //      returns ZX_ERR_SHOULD_WAIT if |wait| is false.
  //      otherwise returns ZX_ERR_SHOULD_WAIT.
  zx_status_t Dequeue(UniqueOp* op_out, bool wait) __TA_EXCLUDES(lock_);

  // Returns true if shutdown has begun and workers should exit.
  bool ShutdownInitiated() __TA_EXCLUDES(lock_);

  // API invokded by streams.
  // --------------------------------
  // Mark a stream as having more ops to be issued. The stream is added to the issue queue.
  void SetActive(StreamRef stream) { queue_.SetActive(std::move(stream)); }

  // Mark a stream as empty and closed. Releases all references to the stream held by the
  // scheduler.
  void StreamRelease(uint32_t id) __TA_EXCLUDES(lock_);

 private:
  using StreamRefIdMap = Stream::WAVLTreeSortById;

  // Find an open stream by ID.
  zx_status_t FindStream(uint32_t id, StreamRef* out) __TA_EXCLUDES(lock_);

  SchedulerClient* client_ = nullptr;  // Client-supplied callback interface.
  uint32_t options_ = 0;               // Ordering options.

  // Priority queue of streams that contain ops ready to be issued.
  Queue queue_;

  fbl::Mutex lock_;
  // Set when shutdown has been called and workers should exit.
  bool shutdown_initiated_ __TA_GUARDED(lock_) = true;

  // Map of id to stream ref.
  StreamRefIdMap open_map_ __TA_GUARDED(lock_);
  StreamRefIdMap closed_map_ __TA_GUARDED(lock_);

  fbl::Vector<fbl::unique_ptr<Worker>> workers_;
};

}  // namespace ioscheduler
