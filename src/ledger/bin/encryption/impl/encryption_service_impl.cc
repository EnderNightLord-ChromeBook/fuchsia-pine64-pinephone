// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/encryption/impl/encryption_service_impl.h"

#include <flatbuffers/flatbuffers.h>
#include <lib/async/cpp/task.h>
#include <lib/callback/scoped_callback.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>

#include "src/ledger/bin/encryption/impl/encrypted_commit_generated.h"
#include "src/ledger/bin/encryption/primitives/encrypt.h"
#include "src/ledger/bin/encryption/primitives/kdf.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/lib/fxl/strings/concatenate.h"

namespace encryption {
namespace {

// The default encryption values. Only used until real encryption is
// implemented: LE-286
//
// Use max_int32 for key_index as it will never be used in practice as it is not
// expected that any user will change its key 2^32 times.
constexpr uint32_t kDefaultKeyIndex = std::numeric_limits<uint32_t>::max();
// Use max_int32 - 1 for default deletion scoped id. max_int32 has a special
// meaning in the specification and is used to have per object deletion scope.
constexpr uint32_t kDefaultDeletionScopeId = std::numeric_limits<uint32_t>::max() - 1;
// Special deletion scope id that produces a per-object deletion scope.
constexpr uint32_t kPerObjectDeletionScopedId = std::numeric_limits<uint32_t>::max();

// Size of keys. Key must have 128 bits of entropy. Randomly generated keys can
// be 128 bits long, but derived ones need to be twice as big because of the
// birthday paradox.
// Size of the randomly generated key.
constexpr size_t kRandomlyGeneratedKeySize = 16u;
// Size of the derived keys.
constexpr size_t kDerivedKeySize = 32u;

// Cache size values.
constexpr size_t kKeyIndexCacheSize = 10u;
constexpr size_t kReferenceKeysCacheSize = 10u;

// Checks whether the given |storage_bytes| are a valid serialization of an
// encrypted commit.
bool CheckValidSerialization(fxl::StringView storage_bytes) {
  flatbuffers::Verifier verifier(reinterpret_cast<const unsigned char*>(storage_bytes.data()),
                                 storage_bytes.size());

  return VerifyEncryptedCommitStorageBuffer(verifier);
}

}  // namespace

// Fake implementation of a key service for the Ledger.
//
// This implementation generate fake keys and will need to be replaced by a
// real component.
class EncryptionServiceImpl::KeyService {
 public:
  explicit KeyService(async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher), weak_factory_(this) {}

  // Retrieves the master key.
  void GetMasterKey(uint32_t key_index, fit::function<void(std::string)> callback) {
    async::PostTask(dispatcher_,
                    callback::MakeScoped(weak_factory_.GetWeakPtr(),
                                         [key_index, callback = std::move(callback)]() {
                                           std::string master_key(16u, 0);
                                           memcpy(&master_key[0], &key_index, sizeof(key_index));
                                           callback(std::move(master_key));
                                         }));
  }

  // Retrieves the reference key associated to the given namespace and reference
  // key. If the id is not yet associated with a reference key, generates a new
  // one and associates it with the id before returning.
  void GetReferenceKey(const std::string& namespace_id, const std::string& reference_key_id,
                       fit::function<void(const std::string&)> callback) {
    std::string result =
        HMAC256KDF(fxl::Concatenate({namespace_id, reference_key_id}), kRandomlyGeneratedKeySize);
    async::PostTask(dispatcher_, callback::MakeScoped(weak_factory_.GetWeakPtr(),
                                                      [result = std::move(result),
                                                       callback = std::move(callback)]() mutable {
                                                        callback(result);
                                                      }));
  }

 private:
  async_dispatcher_t* const dispatcher_;
  fxl::WeakPtrFactory<EncryptionServiceImpl::KeyService> weak_factory_;
};

EncryptionServiceImpl::EncryptionServiceImpl(ledger::Environment* environment,
                                             std::string namespace_id)
    : environment_(environment),
      namespace_id_(std::move(namespace_id)),
      key_service_(std::make_unique<KeyService>(environment_->dispatcher())),
      master_keys_(kKeyIndexCacheSize, Status::OK,
                   [this](auto k, auto c) { FetchMasterKey(std::move(k), std::move(c)); }),
      namespace_keys_(kKeyIndexCacheSize, Status::OK,
                      [this](auto k, auto c) { FetchNamespaceKey(std::move(k), std::move(c)); }),
      reference_keys_(kReferenceKeysCacheSize, Status::OK,
                      [this](auto k, auto c) { FetchReferenceKey(std::move(k), std::move(c)); }) {}

EncryptionServiceImpl::~EncryptionServiceImpl() {}

storage::ObjectIdentifier EncryptionServiceImpl::MakeObjectIdentifier(
    storage::ObjectDigest digest) {
  return {GetCurrentKeyIndex(), kDefaultDeletionScopeId, std::move(digest)};
}

void EncryptionServiceImpl::EncryptCommit(std::string commit_storage,
                                          fit::function<void(Status, std::string)> callback) {
  size_t key_index = GetCurrentKeyIndex();

  Encrypt(
      key_index, std::move(commit_storage),
      [key_index, callback = std::move(callback)](Status status, std::string encrypted_storage) {
        if (status != Status::OK) {
          callback(status, "");
          return;
        }

        flatbuffers::FlatBufferBuilder builder;

        auto storage = CreateEncryptedCommitStorage(
            builder, key_index, convert::ToFlatBufferVector(&builder, encrypted_storage));
        builder.Finish(storage);
        callback(Status::OK, std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                                         builder.GetSize()));
      });
}

void EncryptionServiceImpl::DecryptCommit(convert::ExtendedStringView storage_bytes,
                                          fit::function<void(Status, std::string)> callback) {
  if (!CheckValidSerialization(storage_bytes)) {
    FXL_LOG(WARNING) << "Received invalid data. Cannot decrypt commit.";
    callback(Status::INVALID_ARGUMENT, "");
    return;
  }

  const EncryptedCommitStorage* encrypted_commit_storage =
      GetEncryptedCommitStorage(storage_bytes.data());

  Decrypt(encrypted_commit_storage->key_index(),
          convert::ToString(encrypted_commit_storage->serialized_encrypted_commit_storage()),
          std::move(callback));
}

void EncryptionServiceImpl::GetObjectName(storage::ObjectIdentifier object_identifier,
                                          fit::function<void(Status, std::string)> callback) {
  GetReferenceKey(object_identifier, [object_identifier, callback = std::move(callback)](
                                         const std::string& reference_key) {
    callback(
        Status::OK,
        HMAC256KDF(fxl::Concatenate({reference_key, object_identifier.object_digest().Serialize()}),
                   kDerivedKeySize));
  });
}

void EncryptionServiceImpl::EncryptObject(storage::ObjectIdentifier object_identifier,
                                          fxl::StringView content,
                                          fit::function<void(Status, std::string)> callback) {
  Encrypt(object_identifier.key_index(), content.ToString(), std::move(callback));
}

void EncryptionServiceImpl::DecryptObject(storage::ObjectIdentifier object_identifier,
                                          std::string encrypted_data,
                                          fit::function<void(Status, std::string)> callback) {
  Decrypt(object_identifier.key_index(), std::move(encrypted_data), std::move(callback));
}

uint32_t EncryptionServiceImpl::GetCurrentKeyIndex() { return kDefaultKeyIndex; }

void EncryptionServiceImpl::GetReferenceKey(storage::ObjectIdentifier object_identifier,
                                            fit::function<void(const std::string&)> callback) {
  std::string deletion_scope_seed;
  if (object_identifier.deletion_scope_id() == kPerObjectDeletionScopedId) {
    deletion_scope_seed = object_identifier.object_digest().Serialize();
  } else {
    const uint32_t deletion_scope_id = object_identifier.deletion_scope_id();
    deletion_scope_seed =
        std::string(reinterpret_cast<const char*>(&deletion_scope_id), sizeof(deletion_scope_id));
  }
  DeletionScopeSeed seed = {object_identifier.key_index(), std::move(deletion_scope_seed)};
  reference_keys_.Get(seed, [callback = std::move(callback)](
                                Status status, const std::string& value) { callback(value); });
}

void EncryptionServiceImpl::Encrypt(size_t key_index, std::string data,
                                    fit::function<void(Status, std::string)> callback) {
  master_keys_.Get(key_index,
                   [environment = environment_, data = std::move(data),
                    callback = std::move(callback)](Status status, const std::string& key) {
                     if (status != Status::OK) {
                       callback(status, "");
                       return;
                     }
                     std::string encrypted_data;
                     if (!AES128GCMSIVEncrypt(environment->random(), key, data, &encrypted_data)) {
                       callback(Status::INTERNAL_ERROR, "");
                       return;
                     }
                     callback(Status::OK, std::move(encrypted_data));
                   });
}

void EncryptionServiceImpl::Decrypt(size_t key_index, std::string encrypted_data,
                                    fit::function<void(Status, std::string)> callback) {
  master_keys_.Get(key_index,
                   [encrypted_data = std::move(encrypted_data), callback = std::move(callback)](
                       Status status, const std::string& key) {
                     if (status != Status::OK) {
                       callback(status, "");
                       return;
                     }
                     std::string data;
                     if (!AES128GCMSIVDecrypt(key, encrypted_data, &data)) {
                       callback(Status::INTERNAL_ERROR, "");
                       return;
                     }
                     callback(Status::OK, std::move(data));
                   });
}

void EncryptionServiceImpl::FetchMasterKey(size_t key_index,
                                           fit::function<void(Status, std::string)> callback) {
  key_service_->GetMasterKey(key_index, [callback = std::move(callback)](std::string master_key) {
    callback(Status::OK, std::move(master_key));
  });
}

void EncryptionServiceImpl::FetchNamespaceKey(size_t key_index,
                                              fit::function<void(Status, std::string)> callback) {
  master_keys_.Get(key_index, [this, callback = std::move(callback)](
                                  Status status, const std::string& master_key) {
    if (status != Status::OK) {
      callback(status, "");
      return;
    }
    callback(Status::OK,
             HMAC256KDF(fxl::Concatenate({master_key, namespace_id_}), kDerivedKeySize));
  });
}

void EncryptionServiceImpl::FetchReferenceKey(DeletionScopeSeed deletion_scope_seed,
                                              fit::function<void(Status, std::string)> callback) {
  namespace_keys_.Get(
      deletion_scope_seed.first,
      [this, deletion_scope_seed = std::move(deletion_scope_seed), callback = std::move(callback)](
          Status status, const std::string& namespace_key) mutable {
        if (status != Status::OK) {
          callback(status, "");
          return;
        }
        key_service_->GetReferenceKey(
            namespace_id_,
            HMAC256KDF(fxl::Concatenate({namespace_key, deletion_scope_seed.second}),
                       kDerivedKeySize),
            [callback = std::move(callback)](std::string reference_key) {
              callback(Status::OK, std::move(reference_key));
            });
      });
}

void EncryptionServiceImpl::GetChunkingPermutation(
    fit::function<void(Status, fit::function<uint64_t(uint64_t)>)> callback) {
  master_keys_.Get(kDefaultKeyIndex, [this, callback = std::move(callback)](
                                         Status status, const std::string& master_key) {
    if (status != Status::OK) {
      callback(status, nullptr);
      return;
    }
    std::string derived_key = HMAC256KDF(fxl::Concatenate({master_key, namespace_id_}), 8u);
    uint64_t chunking_permutation_key = *reinterpret_cast<uint64_t*>(derived_key.data());
    auto chunking_permutation = [chunking_permutation_key](uint64_t chunk_window_hash) {
      return chunk_window_hash ^ chunking_permutation_key;
    };
    callback(Status::OK, std::move(chunking_permutation));
  });
}

}  // namespace encryption
