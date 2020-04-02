// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "algorithm.h"

#include <stdint.h>
#include <zircon/assert.h>

#include <blobfs/format.h>

namespace blobfs {

CompressionAlgorithm AlgorithmForInode(const Inode& inode) {
  if (inode.header.flags & kBlobFlagLZ4Compressed) {
    return CompressionAlgorithm::LZ4;
  } else if (inode.header.flags & kBlobFlagZSTDCompressed) {
    return CompressionAlgorithm::ZSTD;
  } else if (inode.header.flags & kBlobFlagZSTDSeekableCompressed) {
    return CompressionAlgorithm::ZSTD_SEEKABLE;
  } else if (inode.header.flags & kBlobFlagChunkCompressed) {
    return CompressionAlgorithm::CHUNKED;
  } else {
    return CompressionAlgorithm::UNCOMPRESSED;
  }
}

uint16_t CompressionInodeHeaderFlags(const CompressionAlgorithm& algorithm) {
  switch (algorithm) {
    case CompressionAlgorithm::LZ4:
      return kBlobFlagLZ4Compressed;
    case CompressionAlgorithm::ZSTD:
      return kBlobFlagZSTDCompressed;
    case CompressionAlgorithm::ZSTD_SEEKABLE:
      return kBlobFlagZSTDSeekableCompressed;
    case CompressionAlgorithm::CHUNKED:
      return kBlobFlagChunkCompressed;
    case CompressionAlgorithm::UNCOMPRESSED:
      return 0u;
    default:
      ZX_ASSERT(false);
      return kBlobFlagZSTDCompressed;
  }
}

}  // namespace blobfs
