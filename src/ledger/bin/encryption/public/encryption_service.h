// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_ENCRYPTION_PUBLIC_ENCRYPTION_SERVICE_H_
#define SRC_LEDGER_BIN_ENCRYPTION_PUBLIC_ENCRYPTION_SERVICE_H_

#include <lib/fit/function.h>

#include <functional>
#include <string>

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/storage/public/object.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/macros.h"

namespace encryption {

// Status of encryption operations.
enum class Status {
  OK,
  AUTH_ERROR,
  NETWORK_ERROR,
  INVALID_ARGUMENT,
  IO_ERROR,
  INTERNAL_ERROR,
};

// Returns whether the given |status| is a permanent error.
bool IsPermanentError(Status status);

// Handles all encryption for a page of the Ledger.
class EncryptionService {
 public:
  EncryptionService() {}
  virtual ~EncryptionService() {}

  // Construct the object identifier for the given digest, using the latest key
  // index and a default |deletion_scope_id|.
  // TODO(qsr): The user should have some control on the |deletion_scope_id| to
  // decide on the scope of deletion for objects.
  virtual storage::ObjectIdentifier MakeObjectIdentifier(storage::ObjectIdentifierFactory* factory,
                                                         storage::ObjectDigest digest) = 0;

  // Encrypts the given commit storage bytes for storing in the cloud.
  virtual void EncryptCommit(std::string commit_storage,
                             fit::function<void(Status, std::string)> callback) = 0;

  // Decrypts the given encrypted commit storage bytes from the cloud.
  virtual void DecryptCommit(convert::ExtendedStringView storage_bytes,
                             fit::function<void(Status, std::string)> callback) = 0;

  // Returns the obfuscated object name for the given identifier.
  //
  // This method is used to translate a local object identifier to the name that
  // is used to refer the object in the cloud provider.
  virtual void GetObjectName(storage::ObjectIdentifier object_identifier,
                             fit::function<void(Status, std::string)> callback) = 0;

  // Encrypts the given object.
  virtual void EncryptObject(storage::ObjectIdentifier object_identifier, fxl::StringView content,
                             fit::function<void(Status, std::string)> callback) = 0;

  // Decrypts the given object.
  virtual void DecryptObject(storage::ObjectIdentifier object_identifier,
                             std::string encrypted_data,
                             fit::function<void(Status, std::string)> callback) = 0;

  // Returns a permutation that can be applied to the window hash in the
  // chunking algorithm.
  virtual void GetChunkingPermutation(
      fit::function<void(Status, fit::function<uint64_t(uint64_t)>)> callback) = 0;

  // Returns an entry id that identifies an entry in a diff sent to the cloud.
  //
  // This version is used for non-merge commits.
  virtual std::string GetEntryId() = 0;

  // This version is used for merge commits to ensure different devices end up with the same entry
  // id for the same merge.
  virtual std::string GetEntryIdForMerge(fxl::StringView entry_name,
                                         storage::CommitId left_parent_id,
                                         storage::CommitId right_parent_id,
                                         fxl::StringView operation_list) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(EncryptionService);
};

}  // namespace encryption

#endif  // SRC_LEDGER_BIN_ENCRYPTION_PUBLIC_ENCRYPTION_SERVICE_H_
