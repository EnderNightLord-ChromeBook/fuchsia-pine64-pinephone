#include <vector>
#include <blobfs/compression/zstd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  size_t src_size = size;
  size_t target_size = size * 2;
  std::vector<uint8_t> target_buffer(target_size);

  blobfs::ZSTDDecompress(target_buffer.data(), &target_size, data, &src_size);
  return 0;
}
