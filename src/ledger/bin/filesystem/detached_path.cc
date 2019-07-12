// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/filesystem/detached_path.h"

#include <utility>

#include "src/lib/fxl/strings/concatenate.h"

namespace ledger {

DetachedPath::DetachedPath(int root_fd, std::string path)
    : root_fd_(root_fd), path_(std::move(path)) {}

DetachedPath::DetachedPath(std::string path) : root_fd_(AT_FDCWD), path_(std::move(path)) {}

DetachedPath::~DetachedPath() {}

DetachedPath::DetachedPath(const DetachedPath& other) = default;

DetachedPath::DetachedPath(DetachedPath&& other) noexcept = default;

DetachedPath& DetachedPath::operator=(const DetachedPath& other) = default;

DetachedPath& DetachedPath::operator=(DetachedPath&&) noexcept = default;

DetachedPath DetachedPath::SubPath(fxl::StringView path) const {
  return DetachedPath(root_fd_, fxl::Concatenate({path_, "/", path}));
}

DetachedPath DetachedPath::SubPath(std::initializer_list<fxl::StringView> components) const {
  std::string end_path = path_;
  for (const auto& component : components) {
    end_path.push_back('/');
    end_path.append(component.data(), component.size());
  }
  return DetachedPath(root_fd_, std::move(end_path));
}

fxl::UniqueFD DetachedPath::OpenFD(DetachedPath* detatched_path) const {
  fxl::UniqueFD fd(openat(root_fd_, path_.c_str(), O_RDONLY | O_DIRECTORY));
  if (fd.is_valid()) {
    *detatched_path = ledger::DetachedPath(fd.get());
  }
  return fd;
}

}  // namespace ledger
