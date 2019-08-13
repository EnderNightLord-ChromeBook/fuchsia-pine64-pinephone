// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/ledger_client/page_client.h"

#include <memory>

#include "gtest/gtest.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/testing/test_with_ledger.h"
#include "src/lib/fxl/macros.h"

namespace modular {
namespace testing {
namespace {

// NOTE(mesch): Test cases here take about 300ms when running in CI.
// Occasionally they take much longer, presumably because of load on
// shared machines. With the default timeout, we see flakiness. Cf.
// FW-287.
constexpr zx::duration kTimeout = zx::sec(10);

class PageClientImpl : public PageClient {
 public:
  PageClientImpl(LedgerClient* ledger_client, LedgerPageId page_id, std::string prefix = "")
      : PageClient("PageClientImpl", ledger_client, std::move(page_id), std::move(prefix)) {}

  ~PageClientImpl() override = default;

  void OnPageChange(const std::string& key, const std::string& value) override {
    ++change_count_;

    values_[key] = value;

    FXL_LOG(INFO) << "OnPageChange \"" << prefix() << "\""
                  << " " << change_count_ << " " << key << " " << value;
  }

  void SetConflictResolver(fit::function<void(Conflict*)> conflict_resolver) {
    conflict_resolver_ = std::move(conflict_resolver);
  }

  void OnPageConflict(Conflict* const conflict) override {
    ++conflict_count_;

    FXL_LOG(INFO) << "OnPageConflict " << prefix() << " " << conflict_count_ << " "
                  << to_string(conflict->key) << " " << conflict->left << " " << conflict->right;

    conflict_resolver_(conflict);
  }

  int change_count() const { return change_count_; }
  int conflict_count() const { return conflict_count_; }

  bool has_value(const std::string& key) { return values_.count(key) > 0; }
  const std::string& value(const std::string& key) { return values_[key]; }

 private:
  std::map<std::string, std::string> values_;
  int change_count_{};
  int conflict_count_{};

  fit::function<void(Conflict*)> conflict_resolver_;
};

class PageClientTest : public TestWithLedger {
 protected:
  PageClientTest() {}

  ~PageClientTest() = default;

  void SetUp() override {
    TestWithLedger::SetUp();
    // We only handle one conflict resolution per test case for now.
    ledger_client()->add_watcher([this] { resolved_ = true; });
  }

  void TearDown() override {
    page_clients_.clear();
    TestWithLedger::TearDown();
  }

  PageClientImpl* CreatePageClient(const std::string& page_id, const std::string& prefix = "") {
    auto page_client =
        std::make_unique<PageClientImpl>(ledger_client(), MakePageId(page_id), prefix);
    auto* ptr = page_client.get();
    page_clients_.emplace_back(std::move(page_client));
    return ptr;
  }

  fuchsia::ledger::PagePtr CreatePagePtr(const std::string& page_id) {
    fuchsia::ledger::PagePtr page;
    ledger_client()->ledger()->GetPage(
        std::make_unique<fuchsia::ledger::PageId>(MakePageId(page_id)), page.NewRequest());
    return page;
  }

  bool resolved() const { return resolved_; }

 private:
  // Storage for page clients created with CreatePageClient(). All will use the
  // same page connection since they are created with the same LedgerClient.
  std::vector<std::unique_ptr<PageClientImpl>> page_clients_;

  // Set to true when LedgerClient sees a change. This happens to co-occur with
  // when conflict resolution is done.
  bool resolved_{};

  FXL_DISALLOW_COPY_AND_ASSIGN(PageClientTest);
};

// This test is flaky. https://fuchsia.atlassian.net/browse/MI4-797
TEST_F(PageClientTest, DISABLED_SimpleWriteObserve) {
  // Create a PageClient for a page, and write directly to it. We expect to see
  // the resulting change in the PageClient.
  auto client = CreatePageClient("page");

  client->page()->Put(to_array("key"), to_array("value"));

  RunLoopWithTimeoutOrUntil([&] { return client->value("key") == "value"; }, kTimeout);

  EXPECT_EQ(0, client->conflict_count());
  EXPECT_EQ("value", client->value("key"));
}

TEST_F(PageClientTest, PrefixWriteObserve) {
  // Put two values, one for each of two prefixes. The two PageClients, being
  // configured to only look for each of those two prefixes, respectively,
  // should only be notified of the relevant keys when the values change.
  auto client_a = CreatePageClient("page", "a/");
  auto client_b = CreatePageClient("page", "b/");

  auto page = CreatePagePtr("page");
  page->Put(to_array("a/key"), to_array("value"));
  page->Put(to_array("b/key"), to_array("value"));

  RunLoopWithTimeoutOrUntil(
      [&] { return client_a->value("a/key") == "value" && client_b->value("b/key") == "value"; },
      kTimeout);

  EXPECT_EQ(0, client_a->conflict_count());
  EXPECT_EQ(0, client_b->conflict_count());
  EXPECT_EQ("value", client_a->value("a/key"));
  EXPECT_FALSE(client_a->has_value("b/key"));
  EXPECT_EQ("value", client_b->value("b/key"));
  EXPECT_FALSE(client_b->has_value("a/key"));
}

TEST_F(PageClientTest, ConcurrentWrite) {
  // Put two different values using two different PagePtr connections. We
  // should still see both of them in a PageClient looking at the same page.
  auto client = CreatePageClient("page");

  auto page1 = CreatePagePtr("page");
  auto page2 = CreatePagePtr("page");
  page1->Put(to_array("key1"), to_array("value1"));
  page2->Put(to_array("key2"), to_array("value2"));

  RunLoopWithTimeoutOrUntil(
      [&] { return client->value("key1") == "value1" && client->value("key2") == "value2"; },
      kTimeout);

  EXPECT_EQ(0, client->conflict_count());
  EXPECT_EQ("value1", client->value("key1"));
  EXPECT_EQ("value2", client->value("key2"));
}

TEST_F(PageClientTest, ConflictWrite) {
  // Write to the same key on two different PagePtrs, and set our PageClient to
  // resolve conflict by setting yet a third value.
  auto client = CreatePageClient("page");
  client->SetConflictResolver([](PageClient::Conflict* const conflict) {
    conflict->resolution = PageClient::MERGE;
    conflict->merged = "value3";
  });

  auto page1 = client->page();
  auto page2 = CreatePagePtr("page");

  bool finished{};

  page2->StartTransaction();
  page2->Put(to_array("key"), to_array("value2"));
  page2->Sync([&] {
    page1->StartTransaction();
    page1->Put(to_array("key"), to_array("value1"));
    page1->Sync([&] {
      page2->Commit();
      page1->Commit();
      finished = true;
    });
  });

  RunLoopWithTimeoutOrUntil(
      [&] { return finished && resolved() && client->value("key") == "value3"; }, kTimeout);

  EXPECT_EQ(1, client->conflict_count());
  EXPECT_EQ("value3", client->value("key"));
}

TEST_F(PageClientTest, ConflictPrefixWrite) {
  // Same as above, but this time have two PageClients, each configured for a
  // different key prefix.  Show that the correct one is used for conflict
  // resolution, and the other is not consulted at all.
  auto client_a = CreatePageClient("page", "a/");
  auto client_b = CreatePageClient("page", "b/");
  client_a->SetConflictResolver([](PageClient::Conflict* const conflict) {
    conflict->resolution = PageClient::MERGE;
    conflict->merged = "value3";
  });

  auto page1 = client_a->page();
  auto page2 = CreatePagePtr("page");

  bool finished{};
  page2->StartTransaction();
  page2->Put(to_array("a/key"), to_array("value2"));
  page2->Sync([&] {
    page1->StartTransaction();
    page1->Put(to_array("a/key"), to_array("value1"));
    page1->Sync([&] {
      page2->Commit();
      page1->Commit();
      finished = true;
    });
  });

  RunLoopWithTimeoutOrUntil(
      [&] { return finished && resolved() && client_a->value("a/key") == "value3"; }, kTimeout);

  EXPECT_EQ(1, client_a->conflict_count());
  EXPECT_EQ(0, client_b->conflict_count());
  EXPECT_EQ("value3", client_a->value("a/key"));
  EXPECT_FALSE(client_b->has_value("a/key"));
}

TEST_F(PageClientTest, ConcurrentConflictWrite) {
  // Explicitly cause a conflict on one key, but not on other keys. We should
  // see the conflict resolve, but it should not affect the other keys at all.
  auto client = CreatePageClient("page");
  client->SetConflictResolver([](PageClient::Conflict* const conflict) {
    conflict->resolution = PageClient::MERGE;
    conflict->merged = "value3";
  });

  auto page1 = client->page();
  auto page2 = CreatePagePtr("page");

  bool finished{};
  page2->StartTransaction();
  page2->Put(to_array("key2"), to_array("value2"));
  page2->Put(to_array("key"), to_array("value2"));
  page2->Sync([&] {
    page1->StartTransaction();
    page1->Put(to_array("key1"), to_array("value1"));
    page1->Put(to_array("key"), to_array("value1"));
    page1->Sync([&] {
      page2->Commit();
      page1->Commit();
      finished = true;
    });
  });

  RunLoopWithTimeoutOrUntil(
      [&] {
        return finished && resolved() && client->value("key") == "value3" &&
               client->value("key1") == "value1" && client->value("key2") == "value2";
      },
      kTimeout);

  EXPECT_EQ(1, client->conflict_count());
  EXPECT_EQ("value1", client->value("key1"));
  EXPECT_EQ("value2", client->value("key2"));
  EXPECT_EQ("value3", client->value("key"));
}

}  // namespace
}  // namespace testing
}  // namespace modular
