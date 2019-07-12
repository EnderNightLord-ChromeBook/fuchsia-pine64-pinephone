// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_APP_CLOUD_PROVIDER_IMPL_H_
#define SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_APP_CLOUD_PROVIDER_IMPL_H_

#include <fuchsia/ledger/cloud/cpp/fidl.h>
#include <fuchsia/ledger/cloud/firestore/cpp/fidl.h>
#include <lib/callback/managed_container.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>

#include "peridot/lib/rng/random.h"
#include "src/ledger/cloud_provider_firestore/bin/app/device_set_impl.h"
#include "src/ledger/cloud_provider_firestore/bin/app/page_cloud_impl.h"
#include "src/ledger/cloud_provider_firestore/bin/firestore/firestore_service.h"
#include "src/ledger/lib/firebase_auth/firebase_auth_impl.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace cloud_provider_firestore {

// Implementation of cloud_provider::CloudProvider.
//
// If the |on_empty| callback is set, it is called when the client connection is
// closed.
class CloudProviderImpl : public cloud_provider::CloudProvider {
 public:
  CloudProviderImpl(rng::Random* random, std::string user_id,
                    std::unique_ptr<firebase_auth::FirebaseAuth> firebase_auth,
                    std::unique_ptr<FirestoreService> firestore_service,
                    fidl::InterfaceRequest<cloud_provider::CloudProvider> request);
  ~CloudProviderImpl() override;

  void set_on_empty(fit::closure on_empty) { on_empty_ = std::move(on_empty); }

  // Shuts the class down and calls the on_empty callback, if set.
  //
  // It is only valid to delete the class after the on_empty callback is called.
  void ShutDownAndReportEmpty();

 private:
  void GetDeviceSet(fidl::InterfaceRequest<cloud_provider::DeviceSet> device_set,
                    GetDeviceSetCallback callback) override;

  void GetPageCloud(std::vector<uint8_t> app_id, std::vector<uint8_t> page_id,
                    fidl::InterfaceRequest<cloud_provider::PageCloud> page_cloud,
                    GetPageCloudCallback callback) override;

  // Makes a best-effort attempt to create a placeholder document at the given
  // location.
  //
  // Placeholder documents have a single field "exists: true" and ensure that
  // data under this path is visible when querying the parent collection. This
  // works around limitations of the web client API for purposes of the
  // development cloud dashboard, see LE-522.
  void CreatePlaceholderDocument(std::string parent_document_path, std::string collection_id,
                                 std::string document_id);

  void ScopedGetCredentials(fit::function<void(std::shared_ptr<grpc::CallCredentials>)> callback);

  rng::Random* const random_;
  const std::string user_id_;

  std::unique_ptr<CredentialsProvider> credentials_provider_;
  std::unique_ptr<FirestoreService> firestore_service_;
  fidl::Binding<cloud_provider::CloudProvider> binding_;
  fit::closure on_empty_;

  callback::AutoCleanableSet<DeviceSetImpl> device_sets_;
  callback::AutoCleanableSet<PageCloudImpl> page_clouds_;

  // Tracks pending requests to create placeholder documents.
  callback::ManagedContainer pending_placeholder_requests_;

  // Must be the last member.
  fxl::WeakPtrFactory<CloudProviderImpl> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CloudProviderImpl);
};

}  // namespace cloud_provider_firestore

#endif  // SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_APP_CLOUD_PROVIDER_IMPL_H_
