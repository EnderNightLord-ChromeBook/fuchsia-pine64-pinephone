// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/register.h"

#include <inttypes.h>

#include "src/developer/debug/zxdb/client/session.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

// RegisterSet -----------------------------------------------------------------

RegisterSet::RegisterSet() = default;

RegisterSet::RegisterSet(debug_ipc::Arch arch, std::vector<debug_ipc::RegisterCategory> categories)
    : arch_(arch) {
  for (auto& category : categories) {
    std::vector<Register> registers;
    registers.reserve(category.registers.size());
    for (auto& ipc_reg : category.registers) {
      registers.emplace_back(Register(std::move(ipc_reg)));
    }

    category_map_[category.type] = std::move(registers);
  }
}

RegisterSet::RegisterSet(const RegisterSet&) = default;
RegisterSet::RegisterSet(RegisterSet&&) = default;
RegisterSet& RegisterSet::operator=(const RegisterSet&) = default;
RegisterSet& RegisterSet::operator=(RegisterSet&&) = default;

RegisterSet::~RegisterSet() = default;

const Register* RegisterSet::operator[](debug_ipc::RegisterID id) const {
  if (id == debug_ipc::RegisterID::kUnknown)
    return nullptr;

  // If this becomes to costly, switch to a cache RegisterID <--> Register map.
  const Register* found_reg = nullptr;
  for (const auto& kv : category_map_) {
    for (const auto& reg : kv.second) {
      if (reg.id() == id) {
        found_reg = &reg;
        break;
      }
    }
  }
  return found_reg;
}

// Register --------------------------------------------------------------------

namespace {

template <typename UintType>
inline UintType ReadRegisterData(const Register& reg) {
  FXL_DCHECK(reg.size() == sizeof(UintType));
  return *reinterpret_cast<const UintType*>(reg.begin());
}

}  // namespace

Register::Register(debug_ipc::Register reg) : reg_(std::move(reg)) {}

Register::Register(debug_ipc::RegisterID id, uint64_t value) : reg_(id, value) {}

uint64_t Register::GetValue() const {
  switch (size()) {
    case 1:
      return ReadRegisterData<uint8_t>(*this);
    case 2:
      return ReadRegisterData<uint16_t>(*this);
    case 4:
      return ReadRegisterData<uint32_t>(*this);
    case 8:
      return ReadRegisterData<uint64_t>(*this);
    default:
      FXL_NOTREACHED() << fxl::StringPrintf("Invalid size for %s: %lu", __PRETTY_FUNCTION__,
                                            size());
      return 0;
  }
}

}  // namespace zxdb
