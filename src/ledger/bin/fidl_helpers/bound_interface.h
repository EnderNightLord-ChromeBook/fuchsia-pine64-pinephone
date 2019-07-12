// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_H_
#define SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_H_

#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>

#include "src/lib/fxl/macros.h"

namespace ledger {
namespace fidl_helpers {
template <class Interface, class Impl, class Binding = fidl::Binding<Interface>>
class BoundInterface {
 public:
  template <class... Args>
  explicit BoundInterface(fidl::InterfaceRequest<Interface> request, Args&&... args)
      : impl_(std::forward<Args>(args)...), binding_(&impl_, std::move(request)) {}

  template <class... Args>
  explicit BoundInterface(Args&&... args) : impl_(std::forward<Args>(args)...), binding_(&impl_) {}

  void Bind(fidl::InterfaceRequest<Interface> request) { binding_.Bind(std::move(request)); }

  void set_on_empty(fit::closure on_empty_callback) {
    binding_.set_error_handler(
        [this, on_empty_callback = std::move(on_empty_callback)](zx_status_t status) {
          binding_.Unbind();
          if (on_empty_callback)
            on_empty_callback();
        });
  }

  bool is_bound() { return binding_.is_bound(); }

  Impl* impl() { return &impl_; }

 private:
  Impl impl_;
  Binding binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(BoundInterface);
};
}  // namespace fidl_helpers
}  // namespace ledger

#endif  // SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_H_
