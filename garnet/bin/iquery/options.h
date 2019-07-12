// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_IQUERY_OPTIONS_H_
#define GARNET_BIN_IQUERY_OPTIONS_H_

#include <lib/inspect_deprecated/query/formatter.h>
#include <src/lib/fxl/command_line.h>

#include <memory>

namespace iquery {

class Formatter;

class Options {
 public:
  enum class Mode {
    UNSET,
    CAT,
    FIND,
    HEALTH,
    LS,
  };
  enum class FormatterType {
    UNSET,
    JSON,
    TEXT,
  };

  // Directory to change to before executing commands.
  std::string chdir;

  // The mode of operation.
  Options::Mode mode = Options::Mode::UNSET;

  // Path formatting mode.
  inspect_deprecated::Formatter::PathFormat path_format =
      inspect_deprecated::Formatter::PathFormat::NONE;

  // Create a health search, which looks for specific nodes within the inspect
  // hierarchy.
  bool health = false;

  // If true, sort all children, metrics, and properties within each object.
  bool sort = false;

  // If true, override all other options and report all hub data with full
  // paths. This mode can still be affected by --format.
  bool report = false;

  // List of paths specified on the command line.
  std::vector<std::string> paths;

  // The type of formatter to use.
  FormatterType formatter_type;

  // Instance of the formatter.
  std::unique_ptr<inspect_deprecated::Formatter> formatter;

  // Create |Options| by parsing the given command line.
  Options(const fxl::CommandLine& command_line);

  // Returns true if the command line was parsed correctly.
  bool Valid() const { return valid_; }

  int depth() const { return depth_; }

  // Print out usage string to stdout.
  void Usage(const std::string& argv0);

 private:
  bool SetMode(const fxl::CommandLine& command_line, Mode m);
  void Invalid(const std::string& argv0, std::string reason);

  // How far within the hierarchy iquery will search.
  int depth_ = 0;

  bool valid_ = false;
};

}  // namespace iquery

#endif  // GARNET_BIN_IQUERY_OPTIONS_H_
