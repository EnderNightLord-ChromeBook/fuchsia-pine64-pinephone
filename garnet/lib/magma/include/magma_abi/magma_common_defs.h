// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_MAGMA_INCLUDE_MAGMA_ABI_MAGMA_COMMON_DEFS_H_
#define GARNET_LIB_MAGMA_INCLUDE_MAGMA_ABI_MAGMA_COMMON_DEFS_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// This is a list of vendor-neutral queries that can be passed to magma_query.
#define MAGMA_QUERY_DEVICE_ID 1
// TODO(MA-518): remove this
#define MAGMA_QUERY_IS_TEST_RESTART_SUPPORTED 2
#define MAGMA_QUERY_IS_TOTAL_TIME_SUPPORTED 3

// All vendor-specific queries IDs that can be passed to magma_query must be >=
// MAGMA_QUERY_VENDOR_PARAM_0.
#define MAGMA_QUERY_VENDOR_PARAM_0 10000

// This is a list of vendor-neutral queries that can be passed to
// magma_query_returns_buffer.
#define MAGMA_QUERY_TOTAL_TIME 500

// reserved ID to represent an invalid object
#define MAGMA_INVALID_OBJECT_ID 0ull

// possible values for magma_status_t
#define MAGMA_STATUS_OK (0)
#define MAGMA_STATUS_INTERNAL_ERROR (-1)
#define MAGMA_STATUS_INVALID_ARGS (-2)
#define MAGMA_STATUS_ACCESS_DENIED (-3)
#define MAGMA_STATUS_MEMORY_ERROR (-4)
#define MAGMA_STATUS_CONTEXT_KILLED (-5)
#define MAGMA_STATUS_CONNECTION_LOST (-6)
#define MAGMA_STATUS_TIMED_OUT (-7)
#define MAGMA_STATUS_UNIMPLEMENTED (-8)

// possible values for magma_cache_operation_t
#define MAGMA_CACHE_OPERATION_CLEAN 0
#define MAGMA_CACHE_OPERATION_CLEAN_INVALIDATE 1

// possible values for magma_cache_policy_t
#define MAGMA_CACHE_POLICY_CACHED 0
#define MAGMA_CACHE_POLICY_WRITE_COMBINING 1
#define MAGMA_CACHE_POLICY_UNCACHED 2

#define MAGMA_DUMP_TYPE_NORMAL (1 << 0)
// Dump current perf counters and disable them
#define MAGMA_DUMP_TYPE_PERF_COUNTERS (1 << 1)
// Start perf counter recording
#define MAGMA_DUMP_TYPE_PERF_COUNTER_ENABLE (1 << 2)

enum {
  MAGMA_FORMAT_R8G8B8A8 = 0,
  MAGMA_FORMAT_BGRA32 = 1,
  MAGMA_FORMAT_NV12 = 2,
};

// These must match the fuchsia.sysmem format modifier values.
enum {
  MAGMA_FORMAT_MODIFIER_LINEAR = 0x0000000000000000,

  MAGMA_FORMAT_MODIFIER_INTEL_X_TILED = 0x0100000000000001,
  MAGMA_FORMAT_MODIFIER_INTEL_Y_TILED = 0x0100000000000002,
  MAGMA_FORMAT_MODIFIER_INTEL_YF_TILED = 0x0100000000000003,

  MAGMA_FORMAT_MODIFIER_ARM_AFBC_16x16 = 0x0800000000000001,
  MAGMA_FORMAT_MODIFIER_ARM_AFBC_32x8 = 0x0800000000000002,
};

enum {
  MAGMA_COHERENCY_DOMAIN_CPU = 0,
  MAGMA_COHERENCY_DOMAIN_RAM = 1,
};

#define MAGMA_SYSMEM_FLAG_PROTECTED (1 << 0)
#define MAGMA_SYSMEM_FLAG_DISPLAY (1 << 1)

#define MAGMA_MAX_IMAGE_PLANES 4

typedef int32_t magma_status_t;

// Normal bool doesn't have to be a particular size.
typedef uint8_t magma_bool_t;

typedef uint32_t magma_cache_operation_t;

typedef uint32_t magma_cache_policy_t;

typedef uintptr_t magma_buffer_t;

typedef uintptr_t magma_semaphore_t;

typedef struct magma_connection {
  uint32_t magic_;
} * magma_connection_t;

typedef uintptr_t magma_sysmem_connection_t;

typedef uintptr_t magma_buffer_collection_t;

typedef uintptr_t magma_sysmem_buffer_constraints_t;

typedef uintptr_t magma_buffer_format_description_t;

// a buffer plus its associated relocations referenced by a command buffer
struct magma_system_exec_resource {
  uint64_t buffer_id;
  uint64_t offset;
  uint64_t length;
};

// A batch buffer to be executed plus the resources required to execute it
struct magma_system_command_buffer {
  uint32_t batch_buffer_resource_index;  // resource index of the batch buffer to execute
  uint32_t batch_start_offset;           // relative to the starting offset of the buffer
  uint32_t num_resources;
  uint32_t wait_semaphore_count;
  uint32_t signal_semaphore_count;
};

// TODO(MA-580): remove (deprecated)
struct magma_system_inline_command_buffer {
  void* data;
  uint64_t size;
  magma_semaphore_t* semaphores;
  uint32_t semaphore_count;
};

struct magma_inline_command_buffer {
  void* data;
  uint64_t size;
  uint64_t* semaphore_ids;
  uint32_t semaphore_count;
};

struct magma_total_time_query_result {
  uint64_t gpu_time_ns;        // GPU time in ns since driver start.
  uint64_t monotonic_time_ns;  // monotonic clock time measured at same time CPU time was.
};

// The top 16 bits are reserved for vendor-specific flags.
#define MAGMA_GPU_MAP_FLAG_VENDOR_SHIFT 16

enum MAGMA_GPU_MAP_FLAGS {
  MAGMA_GPU_MAP_FLAG_NONE = 0,
  MAGMA_GPU_MAP_FLAG_READ = (1 << 0),
  MAGMA_GPU_MAP_FLAG_WRITE = (1 << 1),
  MAGMA_GPU_MAP_FLAG_EXECUTE = (1 << 2),
  MAGMA_GPU_MAP_FLAG_GROWABLE = (1 << 3),

  MAGMA_GPU_MAP_FLAG_VENDOR_MASK = (0xffff << MAGMA_GPU_MAP_FLAG_VENDOR_SHIFT),
};

typedef struct magma_image_plane {
  uint32_t bytes_per_row;
  uint32_t byte_offset;
} magma_image_plane_t;

typedef struct {
  uint32_t image_format;
  magma_bool_t has_format_modifier;
  uint64_t format_modifier;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t bytes_per_row_divisor;
  uint32_t min_bytes_per_row;
} magma_image_format_constraints_t;

typedef struct {
  uint32_t count;
  uint32_t usage;
  magma_bool_t secure_permitted;
  magma_bool_t secure_required;
  magma_bool_t ram_domain_supported;
  magma_bool_t cpu_domain_supported;
  uint32_t min_size_bytes;
} magma_buffer_format_constraints_t;

#if defined(__cplusplus)
}
#endif

#endif  // GARNET_LIB_MAGMA_INCLUDE_MAGMA_ABI_MAGMA_COMMON_DEFS_H_
