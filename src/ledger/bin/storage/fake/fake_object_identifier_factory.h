// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_FAKE_FAKE_OBJECT_IDENTIFIER_FACTORY_H_
#define SRC_LEDGER_BIN_STORAGE_FAKE_FAKE_OBJECT_IDENTIFIER_FACTORY_H_

#include <map>
#include <memory>

#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace storage {
namespace fake {

// A fake class to create and track object identifiers, which leaks memory but is good enough for
// tests.
class FakeObjectIdentifierFactory : public ObjectIdentifierFactory {
 public:
  class TokenImpl;

  FakeObjectIdentifierFactory();
  ~FakeObjectIdentifierFactory();

  FakeObjectIdentifierFactory(const FakeObjectIdentifierFactory&) = delete;
  FakeObjectIdentifierFactory& operator=(const FakeObjectIdentifierFactory&) = delete;

  // Returns whether there are any live ObjectIdentifier for |digest|.
  bool IsLive(const ObjectDigest& digest) const;

  // ObjectIdentifierFactory:
  ObjectIdentifier MakeObjectIdentifier(uint32_t key_index, uint32_t deletion_scope_id,
                                        ObjectDigest object_digest) override;

 private:
  // Token for each digest. Entries are never cleaned up, the count stays to at least 1 because the
  // map retains a reference.
  std::map<ObjectDigest, std::shared_ptr<ObjectIdentifier::Token>> tokens_;

  fxl::WeakPtrFactory<FakeObjectIdentifierFactory> weak_factory_;
};

}  // namespace fake
}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_FAKE_FAKE_OBJECT_IDENTIFIER_FACTORY_H_
