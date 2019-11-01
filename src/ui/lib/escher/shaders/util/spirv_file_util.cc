// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/shaders/util/spirv_file_util.h"

namespace escher {
namespace {

// Given a path name for a variant shader and its args, generate a new hashed name for that
// shader's spirv code to be saved on disk.
// For example if the shader name was "main.vert" and the hash is "9731555" then the final
// hashed name will be "main_vert9731555.spirv".
std::string GenerateHashedSpirvName(const std::string& path, const ShaderVariantArgs& args) {
  uint64_t hash_value = args.hash().val;
  std::string result = path + std::to_string(hash_value);
  std::replace(result.begin(), result.end(), '.', '_');
  std::replace(result.begin(), result.end(), '/', '_');
  return result + ".spirv";
}
}  // namespace

namespace shader_util {
bool WriteSpirvToDisk(const std::vector<uint32_t>& spirv, const ShaderVariantArgs& args,
                      const std::string& base_path, const std::string& shader_name) {
  auto hash_name = GenerateHashedSpirvName(shader_name, args);
  auto full_path = base_path + hash_name;
  FILE* fp = fopen(full_path.c_str(), "wb");
  if (fp) {
    fwrite(spirv.data(), 1, spirv.size(), fp);
    fclose(fp);
    return true;
  } else {
    FXL_LOG(ERROR) << "Could not write file: " << full_path;
  }

  return false;
}

}  // namespace shader_util
}  // namespace escher
