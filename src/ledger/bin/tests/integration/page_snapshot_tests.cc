// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/callback/capture.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/vmo/sized_vmo.h>
#include <lib/fsl/vmo/strings.h>
#include <zircon/errors.h>

#include <utility>
#include <vector>

#include "fuchsia/ledger/cpp/fidl.h"
#include "garnet/public/lib/callback/capture.h"
#include "garnet/public/lib/callback/waiter.h"
#include "gtest/gtest.h"
#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/app/constants.h"
#include "src/ledger/bin/app/fidl/serialization_size.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/testing/ledger_matcher.h"
#include "src/ledger/bin/tests/integration/integration_test.h"
#include "src/ledger/bin/tests/integration/test_utils.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace ledger {
namespace {

class PageSnapshotIntegrationTest : public IntegrationTest {
 public:
  PageSnapshotIntegrationTest() {}
  ~PageSnapshotIntegrationTest() override {}

  // Returns a snapshot of |page|, checking success.
  PageSnapshotPtr PageGetSnapshot(
      PagePtr* page, fidl::VectorPtr<uint8_t> prefix = fidl::VectorPtr<uint8_t>::New(0)) {
    PageSnapshotPtr snapshot;
    (*page)->GetSnapshot(snapshot.NewRequest(), std::move(prefix), nullptr);
    return snapshot;
  }

  // Returns all keys from |snapshot|, starting at |start|. If |num_queries| is
  // not null, stores the number of calls to GetKeys.
  std::vector<std::vector<uint8_t>> SnapshotGetKeys(
      PageSnapshotPtr* snapshot, std::vector<uint8_t> start = std::vector<uint8_t>(),
      int* num_queries = nullptr) {
    std::vector<std::vector<uint8_t>> result;
    std::unique_ptr<Token> token;
    if (num_queries) {
      *num_queries = 0;
    }
    do {
      std::vector<std::vector<uint8_t>> keys;
      auto waiter = NewWaiter();
      (*snapshot)->GetKeys(start, std::move(token),
                           callback::Capture(waiter->GetCallback(), &keys, &token));
      if (!waiter->RunUntilCalled()) {
        ADD_FAILURE() << "|GetKeys| failed to call back.";
        return {};
      }
      if (num_queries) {
        (*num_queries)++;
      }
      for (auto& key : keys) {
        result.push_back(std::move(key));
      }
    } while (token);
    return result;
  }

  std::string SnapshotFetchPartial(PageSnapshotPtr* snapshot, std::vector<uint8_t> key,
                                   int64_t offset, int64_t max_size) {
    fuchsia::ledger::PageSnapshot_FetchPartial_Result result;
    auto waiter = NewWaiter();
    (*snapshot)->FetchPartial(std::move(key), offset, max_size,
                              callback::Capture(waiter->GetCallback(), &result));
    if (!waiter->RunUntilCalled()) {
      ADD_FAILURE() << "|FetchPartial| failed to call back.";
      return {};
    }
    EXPECT_TRUE(result.is_response());
    std::string result_as_string;
    EXPECT_TRUE(fsl::StringFromVmo(result.response().buffer, &result_as_string));
    return result_as_string;
  }

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageSnapshotIntegrationTest);
};

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGet) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"));

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result result;
  auto waiter = NewWaiter();
  snapshot->Get(convert::ToArray("name"), callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_THAT(result, MatchesString("Alice"));

  // Attempt to get an entry that is not in the page.
  waiter = NewWaiter();
  snapshot->Get(convert::ToArray("favorite book"),
                callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  // People don't read much these days.
  EXPECT_THAT(result, MatchesError(fuchsia::ledger::Error::KEY_NOT_FOUND));
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetPipeline) {
  auto instance = NewLedgerAppInstance();
  std::string expected_value = "Alice";
  expected_value.resize(100);

  PagePtr page = instance->GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray(expected_value));

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result result;
  auto waiter = NewWaiter();
  snapshot->Get(convert::ToArray("name"), callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_THAT(result, MatchesString(expected_value));
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotPutOrder) {
  auto instance = NewLedgerAppInstance();
  std::string value1 = "Alice";
  value1.resize(100);
  std::string value2;

  // Put the 2 values without waiting for the callbacks.
  PagePtr page = instance->GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray(value1));
  page->Put(convert::ToArray("name"), convert::ToArray(value2));

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result result;
  auto waiter = NewWaiter();
  snapshot->Get(convert::ToArray("name"), callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_THAT(result, MatchesString(value2));
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotFetchPartial) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"));

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  EXPECT_EQ("Alice", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 0, -1));
  EXPECT_EQ("e", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 4, -1));
  EXPECT_EQ("", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 5, -1));
  EXPECT_EQ("", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 6, -1));
  EXPECT_EQ("i", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 2, 1));
  EXPECT_EQ("", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), 2, 0));

  // Negative offsets.
  EXPECT_EQ("Alice", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), -5, -1));
  EXPECT_EQ("e", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), -1, -1));
  EXPECT_EQ("", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), -5, 0));
  EXPECT_EQ("i", SnapshotFetchPartial(&snapshot, convert::ToArray("name"), -3, 1));

  // Attempt to get an entry that is not in the page.
  fuchsia::ledger::PageSnapshot_FetchPartial_Result result;
  auto waiter = NewWaiter();
  snapshot->FetchPartial(convert::ToArray("favorite book"), 0, -1,
                         callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  // People don't read much these days.
  EXPECT_THAT(result, MatchesError(fuchsia::ledger::Error::KEY_NOT_FOUND));
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetKeys) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetKeys()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  std::vector<std::vector<uint8_t>> result = SnapshotGetKeys(&snapshot);
  EXPECT_EQ(0u, result.size());

  // Add entries and grab a new snapshot.
  const size_t N = 4;
  std::vector<uint8_t> keys[N] = {
      RandomArray(GetRandom(), 20, {0, 0, 0}),
      RandomArray(GetRandom(), 20, {0, 0, 1}),
      RandomArray(GetRandom(), 20, {0, 1, 0}),
      RandomArray(GetRandom(), 20, {0, 1, 1}),
  };
  for (auto& key : keys) {
    page->Put(key, RandomArray(GetRandom(), 50));
  }
  snapshot = PageGetSnapshot(&page);

  // Get all keys.
  result = SnapshotGetKeys(&snapshot);
  EXPECT_EQ(N, result.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], result.at(i));
  }

  // Get keys matching the prefix "0".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0}));
  result = SnapshotGetKeys(&snapshot);
  EXPECT_EQ(N, result.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], result.at(i));
  }

  // Get keys matching the prefix "00".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0, 0}));
  result = SnapshotGetKeys(&snapshot);
  ASSERT_EQ(2u, result.size());
  for (size_t i = 0; i < 2u; ++i) {
    EXPECT_EQ(keys[i], result.at(i));
  }

  // Get keys matching the prefix "010".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0, 1, 0}));
  result = SnapshotGetKeys(&snapshot);
  ASSERT_EQ(1u, result.size());
  EXPECT_EQ(keys[2], result.at(0));

  // Get keys matching the prefix "5".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{5}));
  result = SnapshotGetKeys(&snapshot);
  EXPECT_EQ(0u, result.size());

  // Get keys matching the prefix "0" and starting with the key "010".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0}));
  result = SnapshotGetKeys(&snapshot, std::vector<uint8_t>(std::vector<uint8_t>{0, 1, 0}));
  EXPECT_EQ(2u, result.size());
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetKeysMultiPart) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetKeys()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  int num_queries;
  std::vector<std::vector<uint8_t>> result =
      SnapshotGetKeys(&snapshot, std::vector<uint8_t>(), &num_queries);
  EXPECT_EQ(0u, result.size());
  EXPECT_EQ(1, num_queries);

  // Add entries and grab a new snapshot.
  // Add enough keys so they don't all fit in memory and we will have to have
  // multiple queries.
  const size_t key_size = kMaxKeySize;
  const size_t N = fidl_serialization::kMaxInlineDataSize / key_size + 1;
  std::vector<uint8_t> keys[N];
  for (size_t i = 0; i < N; ++i) {
    // Generate keys so that they are in increasing order to match the order
    // of results from GetKeys().
    keys[i] = RandomArray(GetRandom(), key_size,
                          {static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i & 0xFF)});
  }

  for (auto& key : keys) {
    page->Put(key, RandomArray(GetRandom(), 10));
  }
  snapshot = PageGetSnapshot(&page);

  // Get all keys.
  result = SnapshotGetKeys(&snapshot, fidl::VectorPtr<uint8_t>::New(0), &num_queries);
  EXPECT_TRUE(num_queries > 1);
  ASSERT_EQ(N, result.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], result.at(i));
  }
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetEntries) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetEntries()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  auto entries = SnapshotGetEntries(this, &snapshot);
  EXPECT_EQ(0u, entries.size());

  // Add entries and grab a new snapshot.
  const size_t N = 4;
  std::vector<uint8_t> keys[N] = {
      RandomArray(GetRandom(), 20, {0, 0, 0}),
      RandomArray(GetRandom(), 20, {0, 0, 1}),
      RandomArray(GetRandom(), 20, {0, 1, 0}),
      RandomArray(GetRandom(), 20, {0, 1, 1}),
  };
  std::vector<uint8_t> values[N] = {
      RandomArray(GetRandom(), 50),
      RandomArray(GetRandom(), 50),
      RandomArray(GetRandom(), 50),
      RandomArray(GetRandom(), 50),
  };
  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i], values[i]);
  }
  snapshot = PageGetSnapshot(&page);

  // Get all entries.
  entries = SnapshotGetEntries(this, &snapshot);
  EXPECT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], entries.at(i).key);
    EXPECT_EQ(values[i], ToArray(entries.at(i).value));
  }

  // Get entries matching the prefix "0".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0}));
  entries = SnapshotGetEntries(this, &snapshot);
  EXPECT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], entries.at(i).key);
    EXPECT_EQ(values[i], ToArray(entries.at(i).value));
  }

  // Get entries matching the prefix "00".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0, 0}));
  entries = SnapshotGetEntries(this, &snapshot);
  ASSERT_EQ(2u, entries.size());
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(keys[i], entries.at(i).key);
    EXPECT_EQ(values[i], ToArray(entries.at(i).value));
  }

  // Get keys matching the prefix "010".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{0, 1, 0}));
  entries = SnapshotGetEntries(this, &snapshot);
  ASSERT_EQ(1u, entries.size());
  EXPECT_EQ(keys[2], entries.at(0).key);
  EXPECT_EQ(values[2], ToArray(entries.at(0).value));

  // Get keys matching the prefix "5".
  snapshot = PageGetSnapshot(&page, fidl::VectorPtr<uint8_t>(std::vector<uint8_t>{5}));

  entries = SnapshotGetEntries(this, &snapshot);
  EXPECT_EQ(0u, entries.size());
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetEntriesMultiPartSize) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetEntries()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  int num_queries;
  auto entries =
      SnapshotGetEntries(this, &snapshot, fidl::VectorPtr<uint8_t>::New(0), &num_queries);
  EXPECT_EQ(0u, entries.size());
  EXPECT_EQ(1, num_queries);

  // Add entries and grab a new snapshot.
  // Add enough keys so they don't all fit in memory and we will have to have
  // multiple queries.
  const size_t value_size = 100;
  const size_t key_size = kMaxKeySize;
  const size_t N = fidl_serialization::kMaxInlineDataSize / (key_size + value_size) + 1;
  std::vector<uint8_t> keys[N];
  std::vector<uint8_t> values[N];
  for (size_t i = 0; i < N; ++i) {
    // Generate keys so that they are in increasing order to match the order
    // of results from GetEntries().
    keys[i] = RandomArray(GetRandom(), key_size,
                          {static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i & 0xFF)});
    values[i] = RandomArray(GetRandom(), value_size);
  }

  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i], values[i]);
  }
  snapshot = PageGetSnapshot(&page);

  // Get all entries.
  entries = SnapshotGetEntries(this, &snapshot, fidl::VectorPtr<uint8_t>::New(0), &num_queries);
  EXPECT_TRUE(num_queries > 1);
  ASSERT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], entries[i].key);
    EXPECT_EQ(values[i], ToArray(entries[i].value));
  }
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGetEntriesMultiPartHandles) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  // Grab a snapshot before adding any entries and verify that GetEntries()
  // returns empty results.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  int num_queries;
  auto entries =
      SnapshotGetEntries(this, &snapshot, fidl::VectorPtr<uint8_t>::New(0), &num_queries);
  EXPECT_EQ(0u, entries.size());
  EXPECT_EQ(1, num_queries);

  // Add entries and grab a new snapshot.
  const size_t N = 100;
  std::vector<uint8_t> keys[N];
  std::vector<uint8_t> values[N];
  for (size_t i = 0; i < N; ++i) {
    // Generate keys so that they are in increasing order to match the order
    // of results from GetEntries().
    keys[i] = RandomArray(GetRandom(), 20,
                          {static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i & 0xFF)});
    values[i] = RandomArray(GetRandom(), 100);
  }

  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i], values[i]);
  }
  snapshot = PageGetSnapshot(&page);

  // Get all entries.
  entries = SnapshotGetEntries(this, &snapshot, fidl::VectorPtr<uint8_t>::New(0), &num_queries);
  EXPECT_TRUE(num_queries > 1);
  ASSERT_EQ(N, entries.size());
  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(keys[i], entries[i].key);
    EXPECT_EQ(values[i], ToArray(entries[i].value));
  }
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotGettersReturnSortedEntries) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();

  const size_t N = 4;
  std::vector<uint8_t> keys[N] = {
      RandomArray(GetRandom(), 20, {2}),
      RandomArray(GetRandom(), 20, {5}),
      RandomArray(GetRandom(), 20, {3}),
      RandomArray(GetRandom(), 20, {0}),
  };
  std::vector<uint8_t> values[N] = {
      RandomArray(GetRandom(), 20),
      RandomArray(GetRandom(), 20),
      RandomArray(GetRandom(), 20),
      RandomArray(GetRandom(), 20),
  };
  for (size_t i = 0; i < N; ++i) {
    page->Put(keys[i], values[i]);
  }

  // Get a snapshot.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);

  // Verify that GetKeys() results are sorted.
  std::vector<std::vector<uint8_t>> result = SnapshotGetKeys(&snapshot);
  EXPECT_EQ(keys[3], result.at(0));
  EXPECT_EQ(keys[0], result.at(1));
  EXPECT_EQ(keys[2], result.at(2));
  EXPECT_EQ(keys[1], result.at(3));

  // Verify that GetEntries() results are sorted.
  auto entries = SnapshotGetEntries(this, &snapshot);
  EXPECT_EQ(keys[3], entries[0].key);
  EXPECT_EQ(values[3], ToArray(entries[0].value));
  EXPECT_EQ(keys[0], entries[1].key);
  EXPECT_EQ(values[0], ToArray(entries[1].value));
  EXPECT_EQ(keys[2], entries[2].key);
  EXPECT_EQ(values[2], ToArray(entries[2].value));
  EXPECT_EQ(keys[1], entries[3].key);
  EXPECT_EQ(values[1], ToArray(entries[3].value));
}

TEST_P(PageSnapshotIntegrationTest, PageCreateReferenceFromSocketWrongSize) {
  auto instance = NewLedgerAppInstance();
  const std::string big_data(1'000'000, 'a');

  PagePtr page = instance->GetTestPage();

  fuchsia::ledger::Page_CreateReferenceFromSocket_Result result;
  auto waiter = NewWaiter();
  page->CreateReferenceFromSocket(123, StreamDataToSocket(big_data),
                                  callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_TRUE(result.is_err());
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, result.err());
}

TEST_P(PageSnapshotIntegrationTest, PageCreatePutLargeReferenceFromSocket) {
  auto instance = NewLedgerAppInstance();
  const std::string big_data(1'000'000, 'a');

  PagePtr page = instance->GetTestPage();

  // Stream the data into the reference.
  fuchsia::ledger::Page_CreateReferenceFromSocket_Result create_result;
  auto waiter = NewWaiter();
  page->CreateReferenceFromSocket(big_data.size(), StreamDataToSocket(big_data),
                                  callback::Capture(waiter->GetCallback(), &create_result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_TRUE(create_result.is_response());

  // Set the reference under a key.
  page->PutReference(convert::ToArray("big data"), std::move(create_result.response().reference),
                     Priority::EAGER);

  // Get a snapshot and read the value.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result get_result;
  waiter = NewWaiter();
  snapshot->Get(convert::ToArray("big data"),
                callback::Capture(waiter->GetCallback(), &get_result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_THAT(get_result, MatchesString(big_data));
}

TEST_P(PageSnapshotIntegrationTest, PageCreatePutLargeReferenceFromVmo) {
  auto instance = NewLedgerAppInstance();
  const std::string big_data(1'000'000, 'a');
  fsl::SizedVmo vmo;
  ASSERT_TRUE(fsl::VmoFromString(big_data, &vmo));

  PagePtr page = instance->GetTestPage();

  // Stream the data into the reference.
  fuchsia::ledger::Page_CreateReferenceFromBuffer_Result create_result;
  auto waiter = NewWaiter();
  page->CreateReferenceFromBuffer(std::move(vmo).ToTransport(),
                                  callback::Capture(waiter->GetCallback(), &create_result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_TRUE(create_result.is_response());

  // Set the reference under a key.
  page->PutReference(convert::ToArray("big data"), std::move(create_result.response().reference),
                     Priority::EAGER);

  // Get a snapshot and read the value.
  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result get_result;
  waiter = NewWaiter();
  snapshot->Get(convert::ToArray("big data"),
                callback::Capture(waiter->GetCallback(), &get_result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_THAT(get_result, MatchesString(big_data));
}

TEST_P(PageSnapshotIntegrationTest, PageSnapshotClosePageGet) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();
  page->Put(convert::ToArray("name"), convert::ToArray("Alice"));

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);

  // Close the channel. PageSnapshotPtr should remain valid.
  page.Unbind();

  fuchsia::ledger::PageSnapshot_Get_Result result;
  auto waiter = NewWaiter();
  snapshot->Get(convert::ToArray("name"), callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_THAT(result, MatchesString("Alice"));

  // Attempt to get an entry that is not in the page.
  waiter = NewWaiter();
  snapshot->Get(convert::ToArray("favorite book"),
                callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  // People don't read much these days.
  EXPECT_THAT(result, MatchesError(fuchsia::ledger::Error::KEY_NOT_FOUND));
}

TEST_P(PageSnapshotIntegrationTest, PageGetById) {
  auto instance = NewLedgerAppInstance();
  PagePtr page = instance->GetTestPage();
  PageId test_page_id;
  auto waiter = NewWaiter();
  page->GetId(callback::Capture(waiter->GetCallback(), &test_page_id));
  ASSERT_TRUE(waiter->RunUntilCalled());

  page->Put(convert::ToArray("name"), convert::ToArray("Alice"));
  // Waiting to sync, otherwise the snapshot requested in the rest of the test
  // might be bound before |Put| has terminated.
  waiter = NewWaiter();
  page->Sync(waiter->GetCallback());
  ASSERT_TRUE(waiter->RunUntilCalled());

  page.Unbind();

  page = instance->GetPage(fidl::MakeOptional(test_page_id));
  PageId page_id;
  waiter = NewWaiter();
  page->GetId(callback::Capture(waiter->GetCallback(), &page_id));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_EQ(test_page_id.id, page_id.id);

  PageSnapshotPtr snapshot = PageGetSnapshot(&page);
  fuchsia::ledger::PageSnapshot_Get_Result result;
  waiter = NewWaiter();
  snapshot->Get(convert::ToArray("name"), callback::Capture(waiter->GetCallback(), &result));
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_THAT(result, MatchesString("Alice"));
}

INSTANTIATE_TEST_SUITE_P(PageSnapshotIntegrationTest, PageSnapshotIntegrationTest,
                         ::testing::ValuesIn(GetLedgerAppInstanceFactoryBuilders()));

}  // namespace
}  // namespace ledger
