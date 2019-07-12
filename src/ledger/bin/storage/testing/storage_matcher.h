// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_TESTING_STORAGE_MATCHER_H_
#define SRC_LEDGER_BIN_STORAGE_TESTING_STORAGE_MATCHER_H_

#include <gmock/gmock.h>

#include <tuple>

#include "src/ledger/bin/storage/public/types.h"

namespace storage {

// Matcher that matches an ObjectIdentifier against a matcher for its digest.
// Its key_index and deletion_scoped_id are ignored.
testing::Matcher<ObjectIdentifier> MatchesDigest(testing::Matcher<std::string> matcher);
testing::Matcher<ObjectIdentifier> MatchesDigest(testing::Matcher<ObjectDigest> matcher);

// Matcher that matches a Ledger entry against a pair of matchers on the entry's
// key and object_identifier. The entry's priority is not considered in this
// Matcher.
testing::Matcher<Entry> MatchesEntry(
    std::pair<testing::Matcher<std::string>, testing::Matcher<ObjectIdentifier>> matcher);

// Matcher that matches a Ledger entry against a tuple of matchers on the
// entry's key, object_identifier and priority.
testing::Matcher<Entry> MatchesEntry(
    std::tuple<testing::Matcher<std::string>, testing::Matcher<ObjectIdentifier>,
               testing::Matcher<KeyPriority>>
        matcher);
}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_TESTING_STORAGE_MATCHER_H_
