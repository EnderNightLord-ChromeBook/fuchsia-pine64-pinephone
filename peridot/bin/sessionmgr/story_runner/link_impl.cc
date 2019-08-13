// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/sessionmgr/story_runner/link_impl.h"

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/entity/cpp/json.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fostr/fidl/fuchsia/modular/formatting.h>
#include <lib/fsl/vmo/strings.h>
#include <src/lib/fxl/logging.h>

#include <functional>

#include "peridot/lib/rapidjson/rapidjson.h"

using fuchsia::modular::LinkWatcherPtr;

namespace modular {

namespace {

// Applies a JSON mutation operation using |apply_fn|. Its parameters are
// references because we treat |apply_fn| as part of ApplyOp's body.
void ApplyOp(fidl::StringPtr* value_str, const std::vector<std::string>& path,
             fit::function<void(CrtJsonDoc& doc, CrtJsonPointer& pointer)> apply_fn) {
  CrtJsonDoc value;
  if (value_str->has_value()) {
    value.Parse(value_str->value());
  }
  auto pointer = CreatePointer(value, path);
  apply_fn(value, pointer);
  *value_str = JsonValueToString(value);
}

bool ApplySetOp(fidl::StringPtr* value_str, const fidl::VectorPtr<std::string>& path,
                const fidl::StringPtr& new_value_at_path_str) {
  CrtJsonDoc new_value_at_path;
  new_value_at_path.Parse(new_value_at_path_str.value_or(""));
  if (new_value_at_path.HasParseError()) {
    return false;
  }

  auto apply_fn = [&new_value_at_path](CrtJsonDoc& doc, CrtJsonPointer& p) {
    p.Set(doc, std::move(new_value_at_path));
  };
  ApplyOp(value_str, path.value_or({}), apply_fn);
  return true;
}

void ApplyEraseOp(fidl::StringPtr* value_str, const std::vector<std::string>& path) {
  auto apply_fn = [](CrtJsonDoc& doc, CrtJsonPointer& p) { p.Erase(doc); };
  ApplyOp(value_str, path, apply_fn);
}

fuchsia::mem::Buffer StringToVmo(const std::string& string) {
  fsl::SizedVmo vmo;
  FXL_CHECK(fsl::VmoFromString(string, &vmo));
  return std::move(vmo).ToTransport();
}

}  // namespace

LinkImpl::LinkImpl(StoryStorage* const story_storage, LinkPath link_path)
    : story_storage_(story_storage),
      link_path_(std::move(link_path)),
      weak_factory_(this),
      link_watcher_auto_cancel_(nullptr) {
  FXL_DCHECK(story_storage != nullptr);

  // When |link_watcher_auto_cancel_| goes out of scope, |story_storage_| will
  // stop calling OnLinkValueChanged.
  link_watcher_auto_cancel_ = story_storage_->WatchLink(
      link_path_, [this](const fidl::StringPtr& value, const void* context) {
        OnLinkValueChanged(value, context);
      });
}

LinkImpl::~LinkImpl() = default;

void LinkImpl::Get(fidl::VectorPtr<std::string> path, GetCallback callback) {
  // TODO: Need error reporting. MI4-1082
  story_storage_->GetLinkValue(link_path_)
      ->WeakMap(GetWeakPtr(),
                [this /* for link_path_ */, path = std::move(path)](StoryStorage::Status status,
                                                                    fidl::StringPtr value) {
                  if (status != StoryStorage::Status::OK) {
                    FXL_LOG(ERROR)
                        << "Getting link " << link_path_ << " failed: " << static_cast<int>(status);

                    return std::string("null");
                  }

                  else if (!path || path->empty()) {
                    // Common case requires no parsing of the JSON.
                    return *value;
                  } else {
                    // Extract just the |path| portion of the value.
                    CrtJsonDoc json;
                    json.Parse(value.value_or(""));
                    if (json.HasParseError()) {
                      return std::string("null");
                    }
                    auto& value_at_path =
                        CreatePointer(json, *path).GetWithDefault(json, CrtJsonValue());
                    return JsonValueToString(value_at_path);
                  }
                })
      ->WeakThen(GetWeakPtr(), [callback = std::move(callback)](std::string json) {
        fsl::SizedVmo vmo;
        FXL_CHECK(fsl::VmoFromString(json, &vmo));
        auto vmo_ptr = std::make_unique<fuchsia::mem::Buffer>(std::move(vmo).ToTransport());
        callback(std::move(vmo_ptr));
      });
}

void LinkImpl::Set(fidl::VectorPtr<std::string> path, fuchsia::mem::Buffer json) {
  std::string json_string;
  FXL_CHECK(fsl::StringFromVmo(json, &json_string));
  Set(std::move(path), json_string);
}

void LinkImpl::Set(fidl::VectorPtr<std::string> path, const std::string& json) {
  // TODO: Need error reporting. MI4-1082
  story_storage_
      ->UpdateLinkValue(
          link_path_,
          [this /* for link_path_ */, path = std::move(path),
           json = std::move(json)](fidl::StringPtr* value) {
            if (!ApplySetOp(value, path, json)) {
              FXL_LOG(ERROR) << "LinkImpl.Set failed for link " << link_path_ << " with json "
                             << json;
            }
          },
          this /* context */)
      ->Then([](StoryStorage::Status status) {
        // TODO: Error reporting. MI4-1082
      });
}

void LinkImpl::Erase(std::vector<std::string> path) {
  // TODO: Need error reporting. MI4-1082
  story_storage_
      ->UpdateLinkValue(
          link_path_,
          [path = std::move(path)](fidl::StringPtr* value) { ApplyEraseOp(value, path); },
          this /* context */)
      ->Then([](StoryStorage::Status status) {
        // TODO: Error reporting. MI4-1082
      });
}

void LinkImpl::GetEntity(GetEntityCallback callback) {
  // TODO: Need error reporting. MI4-1082

  story_storage_->GetLinkValue(link_path_)
      ->WeakThen(GetWeakPtr(), [this, callback = std::move(callback)](StoryStorage::Status status,
                                                                      fidl::StringPtr value) {
        if (status != StoryStorage::Status::OK) {
          FXL_LOG(ERROR) << "Getting link " << link_path_
                         << " failed: " << static_cast<int>(status);
          callback(nullptr);
          return;
        }
        // Convert the contents to an Entity reference, if possible.
        std::string ref;
        if (!EntityReferenceFromJson(value.value_or(""), &ref)) {
          FXL_LOG(ERROR) << "Link value for " << link_path_ << " is not an entity reference.";
          callback(nullptr);
          return;
        }

        callback(ref);
      });
}

void LinkImpl::SetEntity(fidl::StringPtr entity_reference) {
  // SetEntity() is just a variation on Set(), so delegate to Set().
  Set(nullptr, EntityReferenceToJson(entity_reference.value_or("")));
}

void LinkImpl::Sync(SyncCallback callback) {
  story_storage_->Sync()->WeakThen(GetWeakPtr(), std::move(callback));
}

void LinkImpl::OnLinkValueChanged(const fidl::StringPtr& value, const void* context) {
  // If context == this, the change came from us. Otherwise, it either came
  // from a different LinkImpl (in which case context != nullptr), or a
  // different StoryStorage altogether (even on a different device).
  if (context != this) {
    for (auto& dst : normal_watchers_.ptrs()) {
      (*dst)->Notify(StringToVmo(value.value_or("")));
    }
  }

  // No matter what, everyone in |everything_watchers_| sees everything.
  for (auto& dst : everything_watchers_.ptrs()) {
    (*dst)->Notify(StringToVmo(value.value_or("")));
  }
}

void LinkImpl::Watch(fidl::InterfaceHandle<LinkWatcher> watcher) {
  // Move |watcher| into the callback for Get(): we are guaranteed
  // that no other operation will run on |story_storage_| until our callback
  // is complete, which means the next mutation that happens will be sent to
  // |watcher|.
  Get(nullptr,
      [this, watcher = std::move(watcher)](std::unique_ptr<fuchsia::mem::Buffer> value) mutable {
        auto ptr = watcher.Bind();
        ptr->Notify(std::move(*value));
        normal_watchers_.AddInterfacePtr(std::move(ptr));
      });
}

void LinkImpl::WatchAll(fidl::InterfaceHandle<fuchsia::modular::LinkWatcher> watcher) {
  Get(nullptr,
      [this, watcher = std::move(watcher)](std::unique_ptr<fuchsia::mem::Buffer> value) mutable {
        auto ptr = watcher.Bind();
        ptr->Notify(std::move(*value));
        everything_watchers_.AddInterfacePtr(std::move(ptr));
      });
}

fxl::WeakPtr<LinkImpl> LinkImpl::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

}  // namespace modular
