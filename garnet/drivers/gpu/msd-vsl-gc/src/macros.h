// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_GPU_MSD_VSL_GC_SRC_MACROS_H_
#define GARNET_DRIVERS_GPU_MSD_VSL_GC_SRC_MACROS_H_

#include "magma_util/macros.h"

static inline bool fits_in_40_bits(uint64_t address) {
  return (address & 0xFFFFFF0000000000ull) == 0;
}

#endif  // GARNET_DRIVERS_GPU_MSD_VSL_GC_SRC_MACROS_H_
