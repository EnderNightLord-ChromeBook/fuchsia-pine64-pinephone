// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fit/function.h>
#include <lib/fsl/socket/strings.h>
#include <stdio.h>

#include <algorithm>

#include "gtest/gtest.h"
#include "src/ledger/bin/environment/environment.h"
#include "src/ledger/bin/storage/fake/fake_page_storage.h"
#include "src/ledger/bin/storage/impl/btree/builder.h"
#include "src/ledger/bin/storage/impl/btree/diff.h"
#include "src/ledger/bin/storage/impl/btree/iterator.h"
#include "src/ledger/bin/storage/impl/btree/tree_node.h"
#include "src/ledger/bin/storage/impl/storage_test_utils.h"
#include "src/ledger/bin/storage/public/constants.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/arraysize.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace storage {
namespace btree {
namespace {
// Pre-determined node level function.
uint8_t GetTestNodeLevel(convert::ExtendedStringView key) {
  if (key == "key03" || key == "key07" || key == "key30" || key == "key60" || key == "key89") {
    return 1;
  }

  if (key == "key50" || key == "key75") {
    return 2;
  }

  return 0;
}

constexpr NodeLevelCalculator kTestNodeLevelCalculator = {&GetTestNodeLevel};

class TrackGetObjectFakePageStorage : public fake::FakePageStorage {
 public:
  explicit TrackGetObjectFakePageStorage(ledger::Environment* environment, PageId id)
      : fake::FakePageStorage(environment, id) {}
  ~TrackGetObjectFakePageStorage() override {}

  void GetObject(ObjectIdentifier object_identifier, Location location,
                 fit::function<void(Status, std::unique_ptr<const Object>)> callback) override {
    object_requests.emplace(object_identifier, location);
    fake::FakePageStorage::GetObject(std::move(object_identifier), location, std::move(callback));
  }

  std::set<std::pair<ObjectIdentifier, Location>> object_requests;

 protected:
  ObjectDigest FakeDigest(fxl::StringView content) const override {
    // BTree code needs storage to return valid digests.
    return MakeObjectDigest(content.ToString());
  }
};

class BTreeUtilsTest : public StorageTest {
 public:
  BTreeUtilsTest() : fake_storage_(&environment_, "page_id") {}

  ~BTreeUtilsTest() override {}

 protected:
  PageStorage* GetStorage() override { return &fake_storage_; }

  ObjectIdentifier CreateTree(const std::vector<EntryChange>& entries) {
    ObjectIdentifier base_node_identifier;
    EXPECT_TRUE(GetEmptyNodeIdentifier(&base_node_identifier));

    ObjectIdentifier new_root_identifier;
    EXPECT_TRUE(CreateTreeFromChanges(base_node_identifier, entries, &new_root_identifier));
    return new_root_identifier;
  }

  std::set<ObjectIdentifier> GetTreeNodesList(ObjectIdentifier root_identifier) {
    std::set<ObjectIdentifier> identifiers;
    EXPECT_TRUE(RunInCoroutine([&](coroutine::CoroutineHandler* handler) {
      SynchronousStorage storage(&fake_storage_, handler);
      BTreeIterator it(&storage);
      ASSERT_EQ(it.Init({root_identifier, PageStorage::Location::Local()}), Status::OK);
      while (!it.Finished()) {
        identifiers.insert(it.GetIdentifier());
        ASSERT_EQ(it.Advance(), Status::OK);
      }
    }));
    return identifiers;
  }

  std::vector<Entry> GetEntriesList(ObjectIdentifier root_identifier) {
    std::vector<Entry> entries;
    auto on_next = [&entries](EntryAndNodeIdentifier entry) {
      entries.push_back(entry.entry);
      return true;
    };
    auto on_done = [this](Status status) {
      EXPECT_EQ(status, Status::OK);
      QuitLoop();
    };
    ForEachEntry(environment_.coroutine_service(), &fake_storage_,
                 {root_identifier, PageStorage::Location::Local()}, "", std::move(on_next),
                 std::move(on_done));
    RunLoopFor(kSufficientDelay);
    return entries;
  }

  TrackGetObjectFakePageStorage fake_storage_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(BTreeUtilsTest);
};

TEST_F(BTreeUtilsTest, GetNodeLevel) {
  size_t level_distribution[4];
  memset(level_distribution, 0, sizeof(level_distribution));

  for (size_t i = 0; i < 1000; ++i) {
    fxl::StringView key(reinterpret_cast<char*>(&i), sizeof(i));
    uint8_t node_level =
        std::min(arraysize(level_distribution) - 1,
                 static_cast<size_t>(GetDefaultNodeLevelCalculator()->GetNodeLevel(key)));
    level_distribution[node_level]++;
  }

  EXPECT_TRUE(std::is_sorted(level_distribution, level_distribution + arraysize(level_distribution),
                             [](int v1, int v2) { return v2 < v1; }));
  EXPECT_NE(0u, level_distribution[1]);
}

TEST_F(BTreeUtilsTest, ApplyChangesFromEmpty) {
  ObjectIdentifier root_identifier;
  ASSERT_TRUE(GetEmptyNodeIdentifier(&root_identifier));
  std::vector<EntryChange> changes;
  ASSERT_TRUE(CreateEntryChanges(3, &changes));

  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  // Expected layout (X is key "keyX"):
  // [00, 01, 02]
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status =
            ApplyChanges(handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
                         changes, &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_nodes.size(), 1u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), changes.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ(entries[i], changes[i].entry);
  }
}

TEST_F(BTreeUtilsTest, ApplyChangeSingleLevel1Entry) {
  ObjectIdentifier root_identifier;
  ASSERT_TRUE(GetEmptyNodeIdentifier(&root_identifier));
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({3}), &golden_entries));

  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  // Expected layout (XX is key "keyXX"):
  // [03]

  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, golden_entries,
                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_nodes.size(), 1u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size());
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    EXPECT_EQ(entries[i], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, ApplyChangesManyEntries) {
  ObjectIdentifier root_identifier;
  ASSERT_TRUE(GetEmptyNodeIdentifier(&root_identifier));
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));

  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10]
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, golden_entries,

                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_nodes.size(), 4u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size());
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    EXPECT_EQ(entries[i], golden_entries[i].entry);
  }

  Entry new_entry = {"key071", MakeObjectIdentifier("object_digest_071"), KeyPriority::EAGER,
                     EntryId()};
  std::vector<EntryChange> new_change{EntryChange{new_entry, false}};
  // Insert key "071" between keys "07" and "08".
  golden_entries.insert(golden_entries.begin() + 8, new_change[0]);

  new_nodes.clear();
  ObjectIdentifier new_root_identifier2;
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [071, 08, 09, 10]
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {new_root_identifier, PageStorage::Location::Local()},
            std::move(new_change), &new_root_identifier2, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(new_root_identifier, new_root_identifier2);
  // The root and the 3rd child have changed.
  EXPECT_EQ(new_nodes.size(), 2u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier2) != new_nodes.end());

  entries = GetEntriesList(new_root_identifier2);
  ASSERT_EQ(entries.size(), golden_entries.size());
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    EXPECT_EQ(entries[i], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, ApplyChangesBackToEmpty) {
  ObjectIdentifier root_identifier;
  ASSERT_TRUE(GetEmptyNodeIdentifier(&root_identifier));
  std::vector<EntryChange> changes;
  ASSERT_TRUE(CreateEntryChanges(3, &changes));

  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  // Expected layout (X is key "keyX"):
  // [00, 01, 02]
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status =
            ApplyChanges(handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
                         changes, &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_nodes.size(), 1u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  for (auto& change : changes) {
    change.deleted = true;
  }

  // Revert the changes
  ObjectIdentifier deleted_root_identifier;
  std::set<ObjectIdentifier> deleted_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status =
            ApplyChanges(handler, &fake_storage_,
                         {new_root_identifier, PageStorage::Location::Local()}, std::move(changes),
                         &deleted_root_identifier, &deleted_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(deleted_nodes.size(), 1u);
  EXPECT_TRUE(deleted_nodes.find(deleted_root_identifier) != deleted_nodes.end());
  EXPECT_EQ(deleted_root_identifier, root_identifier);
}

TEST_F(BTreeUtilsTest, UpdateValue) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Update entry.
  std::vector<Entry> entries_to_update{golden_entries[2].entry};
  std::vector<EntryChange> update_changes;
  for (size_t i = 0; i < entries_to_update.size(); ++i) {
    std::unique_ptr<const Object> object;
    ASSERT_TRUE(AddObject(fxl::StringPrintf("new_object%02" PRIuMAX, i), &object));
    entries_to_update[i].object_identifier = object->GetIdentifier();
    update_changes.push_back(EntryChange{entries_to_update[i], false});
  }

  // Expected layout is unchanged.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
            std::move(update_changes), &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // The root and the first child have changed.
  EXPECT_EQ(new_nodes.size(), 2u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size());
  size_t updated_index = 0;
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    if (updated_index < entries_to_update.size() &&
        golden_entries[i].entry.key == entries_to_update[updated_index].key) {
      EXPECT_EQ(entries[i], entries_to_update[updated_index]);
      // Skip the updated entries.
      updated_index++;
      continue;
    }
    EXPECT_EQ(entries[i], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, UpdateValueLevel1) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Update entry.
  std::vector<Entry> entries_to_update{golden_entries[3].entry};
  std::vector<EntryChange> update_changes;
  for (size_t i = 0; i < entries_to_update.size(); ++i) {
    std::unique_ptr<const Object> object;
    ASSERT_TRUE(AddObject(fxl::StringPrintf("new_object%02" PRIuMAX, i), &object));
    entries_to_update[i].object_identifier = object->GetIdentifier();
    update_changes.push_back(EntryChange{entries_to_update[i], false});
  }

  // Expected layout is unchanged.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
            std::move(update_changes), &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // Only the root has changed.
  EXPECT_EQ(new_nodes.size(), 1u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size());
  size_t updated_index = 0;
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    if (updated_index < entries_to_update.size() &&
        golden_entries[i].entry.key == entries_to_update[updated_index].key) {
      EXPECT_EQ(entries[i], entries_to_update[updated_index]);
      // Skip the updated entries.
      updated_index++;
      continue;
    }
    EXPECT_EQ(entries[i], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, UpdateValueSplitChange) {
  // Expected layout (XX is key "keyXX"):
  // [00, 04]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({
                                     0,
                                     4,
                                 }),
                                 &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Add level 1 entry.
  std::vector<EntryChange> update_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({3}), &update_changes));
  // Expected layout (XX is key "keyXX"):
  //    [03]
  //   /    \
  // [00]  [04]

  // Apply update.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, update_changes,
                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // The tree nodes are new.
  EXPECT_EQ(new_nodes.size(), 3u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size() + update_changes.size());
  size_t updated_index = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (updated_index < update_changes.size() &&
        entries[i] == update_changes[updated_index].entry) {
      // Skip the updated entries.
      updated_index++;
      continue;
    }
    ASSERT_GT(golden_entries.size(), i - updated_index);
    EXPECT_EQ(entries[i], golden_entries[i - updated_index].entry);
  }
}

TEST_F(BTreeUtilsTest, NoOpUpdateChange) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Apply all entries again.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
            std::move(golden_entries), &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_root_identifier, root_identifier);
  // The root and the first child have changed.
  EXPECT_EQ(new_nodes.size(), 0u);
}

TEST_F(BTreeUtilsTest, DeleteChanges) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Delete entries.
  std::vector<EntryChange> delete_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({2, 4}), &delete_changes, true));

  // Expected layout (XX is key "keyXX"):
  //            [03, 07]
  //         /     |        \
  // [00, 01]  [05, 06]    [08, 09, 10, 11]
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, delete_changes,
                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // The root and the first 2 children have changed.
  EXPECT_EQ(new_nodes.size(), 3u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size() - delete_changes.size());
  size_t deleted_index = 0;
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    if (deleted_index < delete_changes.size() &&
        golden_entries[i].entry.key == delete_changes[deleted_index].entry.key) {
      // Skip the deleted entries.
      deleted_index++;
      continue;
    }
    ASSERT_LT(i - deleted_index, entries.size());
    EXPECT_EQ(entries[i - deleted_index], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, DeleteLevel1Changes) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Delete entry.
  std::vector<EntryChange> delete_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({3}), &delete_changes, true));

  // Expected layout (XX is key "keyXX"):
  //                         [07]
  //                        /    \
  // [00, 01, 02, 04, 05, 06]    [08, 09, 10, 11]
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, delete_changes,
                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // The root and one child have changed.
  EXPECT_EQ(new_nodes.size(), 2u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size() - delete_changes.size());
  size_t deleted_index = 0;
  for (size_t i = 0; i < golden_entries.size(); ++i) {
    if (deleted_index < delete_changes.size() &&
        golden_entries[i].entry.key == delete_changes[deleted_index].entry.key) {
      // Skip the deleted entries.
      deleted_index++;
      continue;
    }
    ASSERT_LT(i - deleted_index, entries.size());
    EXPECT_EQ(entries[i - deleted_index], golden_entries[i].entry);
  }
}

TEST_F(BTreeUtilsTest, NoOpDeleteChange) {
  // Expected layout (XX is key "keyXX"):
  //                 [03, 07]
  //            /       |            \
  // [00, 01, 02]  [04, 05, 06] [08, 09, 10, 11]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(11, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Delete entry.
  std::vector<EntryChange> delete_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({12, 13, 14}), &delete_changes, true));

  // Apply deletion.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
            std::move(delete_changes), &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(new_root_identifier, root_identifier);
  // The root and the first child have changed.
  EXPECT_EQ(new_nodes.size(), 0u);
}

TEST_F(BTreeUtilsTest, SplitMergeUpdate) {
  // Expected layout (XX is key "keyXX"):
  //        [50]
  //     /        \
  // [03]          [60, 89]
  //            /      |     \
  //        [55] [65, 74, 76] [99]
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({3, 50, 55, 60, 65, 74, 76, 89, 99}),
                                 &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Add level 2 entry.
  std::vector<EntryChange> update_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({75}), &update_changes));
  // Expected layout (XX is key "keyXX"):
  //          [50, 75]
  //     /       |       \
  // [03]     [60]         [89]
  //         /    \       /   \
  //      [55] [65, 74] [76] [99]

  // Apply update.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {root_identifier, PageStorage::Location::Local()}, update_changes,
                              &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  // The tree nodes are new.
  EXPECT_EQ(new_nodes.size(), 5u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());

  std::vector<Entry> entries = GetEntriesList(new_root_identifier);
  ASSERT_EQ(entries.size(), golden_entries.size() + update_changes.size());
  size_t updated_index = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (updated_index < update_changes.size() &&
        entries[i] == update_changes[updated_index].entry) {
      // Skip the updated entries.
      updated_index++;
      continue;
    }
    ASSERT_LT(i - updated_index, golden_entries.size());
    EXPECT_EQ(entries[i], golden_entries[i - updated_index].entry);
  }

  // Remove the new entry.
  std::vector<EntryChange> delete_changes;
  ASSERT_TRUE(CreateEntryChanges(std::vector<size_t>({75}), &delete_changes, true));

  ObjectIdentifier final_node_identifier;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(handler, &fake_storage_,
                              {new_root_identifier, PageStorage::Location::Local()},
                              std::move(delete_changes), &final_node_identifier, &new_nodes,
                              &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(final_node_identifier, root_identifier);
}

TEST_F(BTreeUtilsTest, DeleteAll) {
  // Create an initial tree.
  std::vector<size_t> values({0, 1, 2, 3, 4, 5, 6, 7});
  std::vector<EntryChange> golden_entries;
  ASSERT_TRUE(CreateEntryChanges(values, &golden_entries));
  ObjectIdentifier root_identifier = CreateTree(golden_entries);

  // Delete everything.
  std::vector<EntryChange> delete_changes;
  ASSERT_TRUE(CreateEntryChanges(values, &delete_changes, true));
  // Apply update.
  Status status;
  ObjectIdentifier new_root_identifier;
  std::set<ObjectIdentifier> new_nodes;
  ASSERT_TRUE(RunInCoroutine(
      [&](coroutine::CoroutineHandler* handler) {
        status = ApplyChanges(
            handler, &fake_storage_, {root_identifier, PageStorage::Location::Local()},
            std::move(delete_changes), &new_root_identifier, &new_nodes, &kTestNodeLevelCalculator);
      },
      kSufficientDelay));
  ASSERT_EQ(status, Status::OK);
  EXPECT_NE(root_identifier, new_root_identifier);
  EXPECT_TRUE(new_root_identifier.object_digest().IsValid());
  // The empty node is new.
  EXPECT_EQ(new_nodes.size(), 1u);
  EXPECT_TRUE(new_nodes.find(new_root_identifier) != new_nodes.end());
}

TEST_F(BTreeUtilsTest, GetObjectIdentifiersFromEmpty) {
  ObjectIdentifier root_identifier;
  ASSERT_TRUE(GetEmptyNodeIdentifier(&root_identifier));
  bool called;
  Status status;
  std::set<ObjectIdentifier> object_identifiers;
  GetObjectIdentifiers(
      environment_.coroutine_service(), &fake_storage_,
      {root_identifier, PageStorage::Location::Local()},
      callback::Capture(callback::SetWhenCalled(&called), &status, &object_identifiers));
  RunLoopFor(kSufficientDelay);
  EXPECT_TRUE(called);
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(object_identifiers.size(), 1u);
  EXPECT_TRUE(object_identifiers.find(root_identifier) != object_identifiers.end());
}

TEST_F(BTreeUtilsTest, GetObjectOneNodeTree) {
  std::vector<EntryChange> entries;
  ASSERT_TRUE(CreateEntryChanges(4, &entries));
  ObjectIdentifier root_identifier = CreateTree(entries);

  bool called;
  Status status;
  std::set<ObjectIdentifier> object_identifiers;
  GetObjectIdentifiers(
      environment_.coroutine_service(), &fake_storage_,
      {root_identifier, PageStorage::Location::Local()},
      callback::Capture(callback::SetWhenCalled(&called), &status, &object_identifiers));
  RunLoopFor(kSufficientDelay);
  EXPECT_TRUE(called);
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(object_identifiers.size(), 6u);
  EXPECT_TRUE(object_identifiers.find(root_identifier) != object_identifiers.end());
  for (const EntryChange& e : entries) {
    EXPECT_TRUE(object_identifiers.find(e.entry.object_identifier) != object_identifiers.end());
  }
}

TEST_F(BTreeUtilsTest, GetObjectIdentifiersBigTree) {
  std::vector<EntryChange> entries;
  ASSERT_TRUE(CreateEntryChanges(99, &entries));
  ObjectIdentifier root_identifier = CreateTree(entries);

  bool called;
  Status status;
  std::set<ObjectIdentifier> object_identifiers;
  GetObjectIdentifiers(
      environment_.coroutine_service(), &fake_storage_,
      {root_identifier, PageStorage::Location::Local()},
      callback::Capture(callback::SetWhenCalled(&called), &status, &object_identifiers));
  RunLoopFor(kSufficientDelay);
  EXPECT_TRUE(called);
  ASSERT_EQ(status, Status::OK);
  EXPECT_EQ(object_identifiers.size(), 99u + 12);
  EXPECT_TRUE(object_identifiers.find(root_identifier) != object_identifiers.end());
  for (EntryChange& e : entries) {
    EXPECT_TRUE(object_identifiers.find(e.entry.object_identifier) != object_identifiers.end());
  }
}

TEST_F(BTreeUtilsTest, GetObjectsFromSync) {
  CommitId commit_id = "commit0";
  std::vector<EntryChange> entries;
  ASSERT_TRUE(CreateEntryChanges(5, &entries));
  entries[3].entry.priority = KeyPriority::LAZY;
  ObjectIdentifier root_identifier = CreateTree(entries);

  // List the identifiers of the values.
  std::set<ObjectIdentifier> values;
  for (auto& entry : entries) {
    values.insert(entry.entry.object_identifier);
  }
  EXPECT_EQ(values.size(), 5u);

  fake_storage_.object_requests.clear();
  bool called;
  Status status;
  // Expected layout (XX is key "keyXX"):
  //          [03]
  //       /        \
  // [00, 01, 02]  [04]
  GetObjectsFromSync(environment_.coroutine_service(), &fake_storage_,
                     {root_identifier, PageStorage::Location::TreeNodeFromNetwork(commit_id)},
                     callback::Capture(callback::SetWhenCalled(&called), &status));
  RunLoopFor(kSufficientDelay);
  EXPECT_TRUE(called);
  ASSERT_EQ(status, Status::OK);

  std::vector<std::pair<ObjectIdentifier, PageStorage::Location>> object_requests;
  std::copy(fake_storage_.object_requests.begin(), fake_storage_.object_requests.end(),
            std::back_inserter(object_requests));
  // There are 8 objects: 3 nodes and 4 eager values and 1 lazy. Except from
  // the lazy object, all others should have been requested.
  EXPECT_EQ(object_requests.size(), 3 + 4u);

  std::set<ObjectIdentifier> object_identifiers;
  GetObjectIdentifiers(
      environment_.coroutine_service(), &fake_storage_,
      {root_identifier, PageStorage::Location::Local()},
      callback::Capture(callback::SetWhenCalled(&called), &status, &object_identifiers));
  RunLoopFor(kSufficientDelay);
  EXPECT_TRUE(called);
  ASSERT_EQ(status, Status::OK);
  ASSERT_EQ(object_identifiers.size(), 3 + 5u);
  for (auto& [identifier, location] : object_requests) {
    // entries[3] contains the lazy value.
    EXPECT_TRUE(identifier != entries[3].entry.object_identifier);
    if (values.find(identifier) != values.end()) {
      EXPECT_TRUE(location.is_value_from_network());
    } else {
      ASSERT_TRUE(location.is_tree_node_from_network());
      EXPECT_EQ(location.in_commit(), commit_id);
    }
    EXPECT_TRUE(object_identifiers.find(identifier) != object_identifiers.end());
  }
}

TEST_F(BTreeUtilsTest, ForEachEmptyTree) {
  std::vector<EntryChange> entries = {};
  ObjectIdentifier root_identifier = CreateTree(entries);
  auto on_next = [](EntryAndNodeIdentifier e) {
    // Fail: There are no elements in the tree.
    EXPECT_TRUE(false);
    return false;
  };
  auto on_done = [this](Status status) {
    EXPECT_EQ(status, Status::OK);
    QuitLoop();
  };
  ForEachEntry(environment_.coroutine_service(), &fake_storage_,
               {root_identifier, PageStorage::Location::Local()}, "", std::move(on_next),
               std::move(on_done));
  RunLoopFor(kSufficientDelay);
}

TEST_F(BTreeUtilsTest, ForEachAllEntries) {
  // Create a tree from entries with keys from 00-99.
  std::vector<EntryChange> entries;
  ASSERT_TRUE(CreateEntryChanges(100, &entries));
  ObjectIdentifier root_identifier = CreateTree(entries);

  int current_key = 0;
  auto on_next = [&current_key](EntryAndNodeIdentifier e) {
    EXPECT_EQ(e.entry.key, fxl::StringPrintf("key%02d", current_key));
    current_key++;
    return true;
  };
  auto on_done = [this](Status status) {
    EXPECT_EQ(status, Status::OK);
    QuitLoop();
  };
  ForEachEntry(environment_.coroutine_service(), &fake_storage_,
               {root_identifier, PageStorage::Location::Local()}, "", on_next, on_done);
  RunLoopFor(kSufficientDelay);
}

TEST_F(BTreeUtilsTest, ForEachEntryPrefix) {
  // Create a tree from entries with keys from 00-99.
  std::vector<EntryChange> entries;
  ASSERT_TRUE(CreateEntryChanges(100, &entries));
  ObjectIdentifier root_identifier = CreateTree(entries);

  // Find all entries with "key3" prefix in the key.
  std::string prefix = "key3";
  int current_key = 30;
  auto on_next = [&current_key, &prefix](EntryAndNodeIdentifier e) {
    if (e.entry.key.substr(0, prefix.length()) != prefix) {
      return false;
    }
    EXPECT_EQ(e.entry.key, fxl::StringPrintf("key%02d", current_key++));
    return true;
  };
  auto on_done = [this, &current_key](Status status) {
    EXPECT_EQ(status, Status::OK);
    EXPECT_EQ(current_key, 40);
    QuitLoop();
  };
  ForEachEntry(environment_.coroutine_service(), &fake_storage_,
               {root_identifier, PageStorage::Location::Local()}, prefix, on_next, on_done);
  RunLoopFor(kSufficientDelay);
}

}  // namespace
}  // namespace btree
}  // namespace storage
