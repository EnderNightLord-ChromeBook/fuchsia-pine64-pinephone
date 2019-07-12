// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/impl/file_index.h"

#include "gtest/gtest.h"
#include "src/ledger/bin/storage/impl/object_identifier_encoding.h"
#include "src/ledger/bin/storage/impl/storage_test_utils.h"
#include "src/ledger/bin/testing/test_with_environment.h"

namespace storage {
namespace {

using FileIndexSerializationTest = ledger::TestWithEnvironment;

TEST_F(FileIndexSerializationTest, CheckInvalid) {
  EXPECT_FALSE(FileIndexSerialization::CheckValidFileIndexSerialization(""));

  std::string ones(200, '\1');
  EXPECT_FALSE(FileIndexSerialization::CheckValidFileIndexSerialization(ones));
}

TEST_F(FileIndexSerializationTest, SerializationDeserialization) {
  const std::vector<FileIndexSerialization::ObjectIdentifierAndSize> elements = {
      {RandomObjectIdentifier(environment_.random()), 1},
      {RandomObjectIdentifier(environment_.random()), 2},
      {RandomObjectIdentifier(environment_.random()), 3},
      {RandomObjectIdentifier(environment_.random()), 4},
      {RandomObjectIdentifier(environment_.random()), 3},
      {RandomObjectIdentifier(environment_.random()), 2},
      {RandomObjectIdentifier(environment_.random()), 1},
  };

  constexpr size_t expected_total_size = 16;

  std::unique_ptr<DataSource::DataChunk> chunk;
  size_t total_size;
  FileIndexSerialization::BuildFileIndex(elements, &chunk, &total_size);

  EXPECT_EQ(expected_total_size, total_size);

  const FileIndex* file_index;
  Status status = FileIndexSerialization::ParseFileIndex(chunk->Get(), &file_index);
  ASSERT_EQ(Status::OK, status);

  EXPECT_EQ(expected_total_size, file_index->size());
  ASSERT_EQ(elements.size(), file_index->children()->size());
  const auto& children = *(file_index->children());
  for (size_t i = 0; i < elements.size(); ++i) {
    EXPECT_EQ(elements[i].size, children[i]->size());
    EXPECT_EQ(elements[i].identifier, ToObjectIdentifier(children[i]->object_identifier()));
  }
}

}  // namespace
}  // namespace storage
