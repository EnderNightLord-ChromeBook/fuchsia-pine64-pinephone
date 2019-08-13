// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/agents/clipboard/clipboard_storage.h"

#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>

namespace modular {
namespace {

std::vector<uint8_t> ToArray(const std::string& str) {
  std::vector<uint8_t> array(str.size());
  memcpy(array.data(), str.data(), str.size());
  return array;
}

std::string ToString(fuchsia::mem::Buffer value) {
  fsl::SizedVmo vmo;
  std::string parsed_string;
  if (!fsl::SizedVmo::FromTransport(std::move(value), &vmo)) {
    FXL_LOG(ERROR) << "Could not decode clipboard value.";
    return "";
  }
  if (!fsl::StringFromVmo(vmo, &parsed_string)) {
    FXL_LOG(ERROR) << "fuchsia::modular::Clipboard vmo could not be decoded to string.";
    return "";
  }
  return parsed_string;
}

// The Ledger key that is used to store the current value.
constexpr char kCurrentValueKey[] = "current_value";

}  // namespace

class ClipboardStorage::PushCall : public Operation<> {
 public:
  PushCall(ClipboardStorage* const impl, const fidl::StringPtr& text)
      : Operation("ClipboardStorage::PushCall", [] {}), impl_(impl), text_(text) {}

 private:
  void Run() override {
    FlowToken flow{this};
    impl_->page()->Put(ToArray(kCurrentValueKey), ToArray(text_.value_or("")));
  }

  ClipboardStorage* const impl_;  // not owned
  const fidl::StringPtr text_;
};

class ClipboardStorage::PeekCall : public Operation<fidl::StringPtr> {
 public:
  PeekCall(ClipboardStorage* const impl, fit::function<void(fidl::StringPtr)> result)
      : Operation("ClipboardStorage::PeekCall", std::move(result)), impl_(impl) {
    // No error checking: Absent ledger value yields "", not
    // null. TODO(mesch): Once we support types, distinction of
    // null may make sense.
    text_ = "";
  }

 private:
  void Run() override {
    FlowToken flow{this, &text_};
    impl_->page()->GetSnapshot(snapshot_.NewRequest(), {}, nullptr);
    snapshot_->Get(ToArray(kCurrentValueKey),
                   [this, flow](fuchsia::ledger::PageSnapshot_Get_Result result) {
                     if (result.is_response()) {
                       text_ = ToString(std::move(result.response().buffer));
                     }
                   });
  }

  ClipboardStorage* const impl_;  // not owned
  fuchsia::ledger::PageSnapshotPtr snapshot_;
  fidl::StringPtr text_;
};

ClipboardStorage::ClipboardStorage(LedgerClient* ledger_client, LedgerPageId page_id)
    : PageClient("ClipboardStorage", ledger_client, std::move(page_id)) {}

ClipboardStorage::~ClipboardStorage() = default;

void ClipboardStorage::Push(const fidl::StringPtr& text) {
  operation_queue_.Add(std::make_unique<PushCall>(this, text));
}

void ClipboardStorage::Peek(fit::function<void(fidl::StringPtr)> callback) {
  operation_queue_.Add(std::make_unique<PeekCall>(this, std::move(callback)));
}

}  // namespace modular
