// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file represents the interface used by the allocator to interact with
// the underlying storage medium.

#pragma once

#include <fbl/function.h>
#include <fbl/macros.h>
#include <fs/block-txn.h>
#include <minfs/superblock.h>

#ifdef __Fuchsia__
#include <block-client/cpp/block-device.h>
#endif

#include "metadata.h"

namespace minfs {

// Types of data to use with read and write transactions.
#ifdef __Fuchsia__
using ReadData = vmoid_t;
using WriteData = zx_handle_t;
#else
using ReadData = const void*;
using WriteData = const void*;
#endif

using GrowMapCallback = fbl::Function<zx_status_t(size_t pool_size, size_t* old_pool_size)>;

// Interface for an Allocator's underlying storage.
class AllocatorStorage {
 public:
  AllocatorStorage() = default;
  AllocatorStorage(const AllocatorStorage&) = delete;
  AllocatorStorage& operator=(const AllocatorStorage&) = delete;
  virtual ~AllocatorStorage() {}

#ifdef __Fuchsia__
  virtual zx_status_t AttachVmo(const zx::vmo& vmo, fuchsia_hardware_block_VmoID* vmoid) = 0;
#endif

  // Loads data from disk into |data| using |txn|.
  virtual void Load(fs::ReadTxn* txn, ReadData data) = 0;

  // Extend the on-disk extent containing map_.
  virtual zx_status_t Extend(WriteTxn* txn, WriteData data, GrowMapCallback grow_map) = 0;

  // Returns the number of unallocated elements.
  virtual uint32_t PoolAvailable() const = 0;

  // Returns the total number of elements.
  virtual uint32_t PoolTotal() const = 0;

  // The number of blocks necessary to store |PoolTotal()| elements.
  uint32_t PoolBlocks() const;

  // Persists the map at range |index| - |index + count|.
  virtual void PersistRange(WriteTxn* txn, WriteData data, size_t index, size_t count) = 0;

  // Marks |count| elements allocated and persists the latest data.
  virtual void PersistAllocate(WriteTxn* txn, size_t count) = 0;

  // Marks |count| elements released and persists the latest data.
  virtual void PersistRelease(WriteTxn* txn, size_t count) = 0;
};

// A type of storage which represents a persistent disk.
class PersistentStorage : public AllocatorStorage {
 public:
  // Callback invoked after the data portion of the allocator grows.
  using GrowHandler = fbl::Function<zx_status_t(uint32_t pool_size)>;

  PersistentStorage() = delete;
  PersistentStorage(const PersistentStorage&) = delete;
  PersistentStorage& operator=(const PersistentStorage&) = delete;

#ifdef __Fuchsia__
  // |grow_cb| is an optional callback to increase the size of the allocator.
  PersistentStorage(block_client::BlockDevice* device, SuperblockManager* sb, size_t unit_size,
                    GrowHandler grow_cb, AllocatorMetadata metadata);
#else
  // |grow_cb| is an optional callback to increase the size of the allocator.
  PersistentStorage(SuperblockManager* sb, size_t unit_size, GrowHandler grow_cb,
                    AllocatorMetadata metadata);
#endif
  ~PersistentStorage() {}

#ifdef __Fuchsia__
  zx_status_t AttachVmo(const zx::vmo& vmo, fuchsia_hardware_block_VmoID* vmoid);
#endif

  void Load(fs::ReadTxn* txn, ReadData data);

  zx_status_t Extend(WriteTxn* txn, WriteData data, GrowMapCallback grow_map) final;

  uint32_t PoolAvailable() const final { return metadata_.PoolAvailable(); }

  uint32_t PoolTotal() const final { return metadata_.PoolTotal(); }

  void PersistRange(WriteTxn* txn, WriteData data, size_t index, size_t count) final;

  void PersistAllocate(WriteTxn* txn, size_t count) final;

  void PersistRelease(WriteTxn* txn, size_t count) final;

 private:
  // Returns the number of blocks necessary to store a pool containing |size| bits.
  static blk_t BitmapBlocksForSize(size_t size);

#ifdef __Fuchsia__
  block_client::BlockDevice* device_;
  size_t unit_size_;
#endif
  SuperblockManager* sb_;
  GrowHandler grow_cb_;
  AllocatorMetadata metadata_;
};

}  // namespace minfs
