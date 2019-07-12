// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_TESTING_PAGE_DATA_GENERATOR_H_
#define SRC_LEDGER_BIN_TESTING_PAGE_DATA_GENERATOR_H_

#include <lib/fit/function.h>

#include <vector>

#include "peridot/lib/rng/random.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/public/status.h"
#include "src/ledger/bin/testing/data_generator.h"
#include "src/lib/fxl/strings/string_view.h"

namespace ledger {

// Helper class for filling a ledger page with random data.
class PageDataGenerator {
 public:
  // Strategy on how to put values: inline or as references.
  // INLINE: Put entry inline (as a FIDL array).
  // REFERENCE: Put entry as reference.
  enum class ReferenceStrategy {
    INLINE,
    REFERENCE,
  };

  PageDataGenerator(rng::Random* random);

  // Put an entry (|key|, |value|) to the given page |page|, inline or as
  // reference depending on |ref_strategy| and with priority specified by
  // |priority|.
  void PutEntry(PagePtr* page, std::vector<uint8_t> key, std::vector<uint8_t> value,
                ReferenceStrategy ref_strategy, Priority priority,
                fit::function<void(Status)> callback);

  // Fill the page |page| with entries with keys |keys| and random values of
  // size |value_size|, performing at maximum
  // |transaction_size| Put operations per commit.
  void Populate(PagePtr* page, std::vector<std::vector<uint8_t>> keys, size_t value_size,
                size_t transaction_size, ReferenceStrategy ref_strategy, Priority priority,
                fit::function<void(Status)> /*callback*/);

 private:
  // Run PutEntry |transaction_size| times on provided keys |keys| with random
  // values of size |value_size|. in transaction starting with key
  // number |curent_key_index|. After commiting a transaction, run a next one
  // recursively. Call |callback| with Status::OK once all keys have been put,
  // or with a first encountered status that is different from Status::OK.
  void PutInTransaction(PagePtr* page, std::vector<std::vector<uint8_t>> keys,
                        size_t current_key_index, size_t value_size, size_t transaction_size,
                        ReferenceStrategy ref_strategy, Priority priority,
                        fit::function<void(Status)> callback);

  // Run PutEntry on all the provided keys in |keys| with random value of size
  // |value_size|.
  void PutMultipleEntries(PagePtr* page, std::vector<std::vector<uint8_t>> keys, size_t value_size,
                          ReferenceStrategy ref_strategy, Priority priority,
                          fit::function<void(Status)> /*callback*/);

  DataGenerator generator_;
};

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_TESTING_PAGE_DATA_GENERATOR_H_
