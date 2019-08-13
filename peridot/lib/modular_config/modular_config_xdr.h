// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_MODULAR_CONFIG_MODULAR_CONFIG_XDR_H_
#define PERIDOT_LIB_MODULAR_CONFIG_MODULAR_CONFIG_XDR_H_

#include <fuchsia/modular/session/cpp/fidl.h>

#include "peridot/lib/fidl/json_xdr.h"

namespace modular {

// Serialization and deserialization of
// fuchsia::modular::session::BasemgrConfig to and from JSON
void XdrBasemgrConfig_v1(XdrContext* const xdr,
                         fuchsia::modular::session::BasemgrConfig* const data);

void XdrSessionmgrConfig_v1(XdrContext* const xdr,
                            fuchsia::modular::session::SessionmgrConfig* const data);

constexpr XdrFilterType<fuchsia::modular::session::BasemgrConfig> XdrBasemgrConfig[] = {
    XdrBasemgrConfig_v1,
    nullptr,
};

constexpr XdrFilterType<fuchsia::modular::session::SessionmgrConfig> XdrSessionmgrConfig[] = {
    XdrSessionmgrConfig_v1,
    nullptr,
};

}  // namespace modular

#endif  // PERIDOT_LIB_MODULAR_CONFIG_MODULAR_CONFIG_XDR_H_
