// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_ENCRYPTION_FAKE_FAKE_ENCRYPTION_SERVICE_H_
#define SRC_LEDGER_BIN_ENCRYPTION_FAKE_FAKE_ENCRYPTION_SERVICE_H_

#include <lib/async/dispatcher.h>
#include <lib/fit/function.h>

#include <functional>
#include <string>

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/encryption/public/encryption_service.h"

namespace encryption {

// Creates an |ObjectIdentifier| from an |ObjectDigest|.
//
// This method is always constructing the indentifier with the same key index
// and deletion scope.
storage::ObjectIdentifier MakeDefaultObjectIdentifier(storage::ObjectDigest digest);

// Do a static permutation.
// This method applies a static permutation to a |chunk_window_hash|. It does
// not depend on any keys.
uint64_t DefaultPermutation(uint64_t chunk_window_hash);

class FakeEncryptionService : public EncryptionService {
 public:
  explicit FakeEncryptionService(async_dispatcher_t* dispatcher);
  ~FakeEncryptionService() override;

  // EncryptionService:
  storage::ObjectIdentifier MakeObjectIdentifier(storage::ObjectDigest digest) override;
  void EncryptCommit(std::string commit_storage,
                     fit::function<void(Status, std::string)> callback) override;
  void DecryptCommit(convert::ExtendedStringView storage_bytes,
                     fit::function<void(Status, std::string)> callback) override;
  void GetObjectName(storage::ObjectIdentifier object_identifier,
                     fit::function<void(Status, std::string)> callback) override;
  void EncryptObject(storage::ObjectIdentifier object_identifier, fxl::StringView content,
                     fit::function<void(Status, std::string)> callback) override;
  void DecryptObject(storage::ObjectIdentifier object_identifier, std::string encrypted_data,
                     fit::function<void(Status, std::string)> callback) override;
  void GetChunkingPermutation(
      fit::function<void(Status, fit::function<uint64_t(uint64_t)>)> callback) override;

  // Synchronously encrypts the given commit.
  std::string EncryptCommitSynchronous(convert::ExtendedStringView commit_storage);

  // Synchronously decrypts the given commit.
  std::string DecryptCommitSynchronous(convert::ExtendedStringView storage_bytes);

  // Synchronously gets the object name.
  std::string GetObjectNameSynchronous(storage::ObjectIdentifier object_identifier);

  // Synchronously encrypts the object.
  std::string EncryptObjectSynchronous(convert::ExtendedStringView object_content);

  // Synchronously decrypts the object.
  std::string DecryptObjectSynchronous(convert::ExtendedStringView encrypted_data);

 private:
  async_dispatcher_t* dispatcher_;
};

}  // namespace encryption

#endif  // SRC_LEDGER_BIN_ENCRYPTION_FAKE_FAKE_ENCRYPTION_SERVICE_H_
