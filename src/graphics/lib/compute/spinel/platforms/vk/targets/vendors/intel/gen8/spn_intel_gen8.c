// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
//
//

#include "spn_config.h"
#include "spn_target.h"

//
//
//

#include "spn_vk_target.h"

//
//
//

#include <vulkan/vulkan_core.h>

//
// clang-format off
//

#define SPN_VK_EXTENT_PDRW         (SPN_VK_ALLOC_PERM_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
#define SPN_VK_EXTENT_TDRW         (SPN_VK_ALLOC_TEMP_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
#define SPN_VK_EXTENT_PHW1G_TDR1S  (SPN_VK_ALLOC_PERM_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
#define SPN_VK_EXTENT_PHW1G_TDRNS  (SPN_VK_ALLOC_PERM_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
#define SPN_VK_EXTENT_PHWN_PDRN    (SPN_VK_ALLOC_PERM_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) // bad
#define SPN_VK_EXTENT_IMAGE        0

//
//
//

static struct spn_vk_target const target =
{
  .config = {
    .allocator = {
      .host = {
        .perm = {
          .alignment        = 16,      // 16 byte alignment
        },
        .temp = {
          .subbufs          = 128,     // 128 subbufs
          .size             = 8 << 20, // 8 MBytes
          .alignment        = 16       // 16 byte alignment
        }
      },
      .device = {
        .temp = {
          .subbufs          = 256,      // 256 subbufs
          .size             = 32 << 20, // 32 MBytes
        }
      }
    },

    .subgroup_size_log2     = SPN_DEVICE_SUBGROUP_SIZE_LOG2,

    .tile = {
      .width_log2           = SPN_TILE_WIDTH_LOG2,
      .height_log2          = SPN_TILE_HEIGHT_LOG2
    },

    .block_pool = {
      .block_dwords_log2    = SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2,
      .subblock_dwords_log2 = SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2,
      .ids_per_workgroup    = SPN_KERNEL_BLOCK_POOL_INIT_BP_IDS_PER_WORKGROUP
    },

    .path_builder = {
      .size = {
        .dispatches         = 32,
        .ring               = 16384,
        .eager              = 4096
      }
    },

    .raster_builder = {
      .vk = {
        .rings = {
          .h                = 0,   // FIXME -- replace with extent type
          .d                = 0
        }
      },
      .size = {
        .dispatches         = 32,
        .ring               = 8192,
        .eager              = 1024,
        .cohort             = SPN_KERNEL_RASTERS_ALLOC_METAS_SIZE, // FIXME -- change name
        .cmds               = 1 << 18,
        .ttrks              = 1 << 20
      },
      .fills_scan = {
        .rows               = SPN_KERNEL_FILLS_SCAN_ROWS
      }
    },

    .styling = {
      .vk = {
        .h                  = 0,   // FIXME -- replace with extent type
        .d                  = 0
      }
    },

    .composition = {
      .vk = {
        .rings = {
          .h                = 0,   // FIXME -- replace with extent type
          .d                = 0
        }
      },
      .size = {
        .dispatches         = 32,
        .ring               = 8192,
        .eager              = 1024,
        .cmds               = 1 << 18,
        .ttcks              = 1 << 20,
        .rasters            = 1 << 17
      }
    },

    //
    // capture target-specific number of sets and extent sizes
    //
#define SPN_DS_WAG_COUNT  255

    .ds = {
      .block_pool = {
        .sets = SPN_DS_WAG_COUNT
      },
      .paths_copy = {
        .sets = SPN_DS_WAG_COUNT
      },
      .rasterize = {
        .sets = SPN_DS_WAG_COUNT
      },
      .ttrks = {
        .sets = SPN_DS_WAG_COUNT
      },
      .raster_ids = {
        .sets = SPN_DS_WAG_COUNT
      },
      .ttcks = {
        .sets = SPN_DS_WAG_COUNT
      },
      .place = {
        .sets = SPN_DS_WAG_COUNT
      },
      .styling = {
        .sets = SPN_DS_WAG_COUNT
      },
      .surface = {
        .sets = SPN_DS_WAG_COUNT
      }
    },

    //
    // capture target-specific extent types
    //
#undef  SPN_VK_DESC_TYPE_STORAGE_BUFFER
#define SPN_VK_DESC_TYPE_STORAGE_BUFFER(_ds_id,_d_idx,_d_ext,_d_id) ._d_id = _d_ext,

#undef  SPN_VK_DESC_TYPE_STORAGE_IMAGE
#define SPN_VK_DESC_TYPE_STORAGE_IMAGE(_ds_id,_d_idx,_d_ext,_d_id)  ._d_id = _d_ext,

#undef  SPN_VK_DS_EXPAND_X
#define SPN_VK_DS_EXPAND_X(_ds_idx,_ds_id,_ds)  \
    .ds_extents._ds_id.props = {                \
      _ds                                       \
    },

    SPN_VK_DS_EXPAND()

    //
    // capture target-specific pipeline push constant sizes
    //
    .p = {
      .push_sizes = {
#include "spn_target_push.inl"
      }
    }
  },

  //
  // FILL IN REST OF SPINEL CONFIG STRUCT FROM OPENCL
  //
  .modules = {
#include "spn_modules.inl"

#ifdef SPN_DUMP
    0
#endif
  }
};

//
// clang-format on
//

struct spn_vk_target const * const SPN_TARGET_NAME = &target;

#include "spn_target_modules_dump.inl"

//
//
//
