// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FXL_FILES_UNIQUE_FD_H_
#define LIB_FXL_FILES_UNIQUE_FD_H_

#include <fbl/unique_fd.h>

#include "src/lib/fxl/memory/unique_object.h"

namespace fxl {

using UniqueFD = fbl::unique_fd;

}  // namespace fxl

#endif  // LIB_FXL_FILES_UNIQUE_FD_H_
