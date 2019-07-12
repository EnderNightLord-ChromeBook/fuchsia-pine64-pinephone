// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/input_location_parser.h"

#include <inttypes.h>

#include <algorithm>

#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/target.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/string_util.h"
#include "src/developer/debug/zxdb/expr/expr_parser.h"
#include "src/developer/debug/zxdb/expr/find_name.h"
#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/index.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/module_symbols.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"
#include "src/developer/debug/zxdb/symbols/target_symbols.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

Err ParseInputLocation(const Frame* frame, const std::string& input, InputLocation* location) {
  if (input.empty())
    return Err("Passed empty location.");

  // Check for one colon. Two colons is a C++ member function.
  size_t colon = input.find(':');
  if (colon != std::string::npos && colon < input.size() - 1 && input[colon + 1] != ':') {
    // <file>:<line> format.
    std::string file = input.substr(0, colon);

    uint64_t line = 0;
    Err err = StringToUint64(input.substr(colon + 1), &line);
    if (err.has_error())
      return err;

    location->type = InputLocation::Type::kLine;
    location->line = FileLine(std::move(file), static_cast<int>(line));
    return Err();
  }

  // Check for memory addresses.
  bool is_address = false;
  size_t address_begin = 0;  // Index of address number when is_address.
  if (input[0] == '*') {
    // *<address> format
    is_address = true;
    address_begin = 1;  // Skip "*".
  } else if (CheckHexPrefix(input)) {
    // Hex numbers are addresses.
    is_address = true;
    address_begin = 0;
  }
  if (is_address) {
    std::string addr_str = input.substr(address_begin);
    Err err = StringToUint64(addr_str, &location->address);
    if (err.has_error())
      return err;

    location->type = InputLocation::Type::kAddress;
    return Err();
  }

  uint64_t line = 0;
  Err err = StringToUint64(input, &line);
  if (!err.has_error()) {
    // A number, assume line number and use the file name from the frame.
    if (!frame) {
      return Err(
          "There is no current frame to get a file name, you'll have to "
          "specify an explicit frame or file name.");
    }
    const Location& frame_location = frame->GetLocation();
    if (frame_location.file_line().file().empty()) {
      return Err(
          "The current frame doesn't have a file name to use, you'll "
          "have to specify a file.");
    }
    location->type = InputLocation::Type::kLine;
    location->line = FileLine(frame_location.file_line().file(), static_cast<int>(line));
    return Err();
  }

  // Anything else, assume its an identifier.
  Identifier ident;
  err = ExprParser::ParseIdentifier(input, &location->symbol);
  if (err.has_error())
    return err;

  location->type = InputLocation::Type::kSymbol;
  return Err();
}

Err ResolveInputLocations(const ProcessSymbols* process_symbols,
                          const InputLocation& input_location, bool symbolize,
                          std::vector<Location>* locations) {
  ResolveOptions options;
  options.symbolize = symbolize;
  *locations = process_symbols->ResolveInputLocation(input_location, options);

  if (locations->empty()) {
    return Err("Nothing matching this %s was found.",
               InputLocation::TypeToString(input_location.type));
  }
  return Err();
}

Err ResolveInputLocations(const ProcessSymbols* process_symbols, const Frame* optional_frame,
                          const std::string& input, bool symbolize,
                          std::vector<Location>* locations) {
  InputLocation input_location;
  Err err = ParseInputLocation(optional_frame, input, &input_location);
  if (err.has_error())
    return err;
  return ResolveInputLocations(process_symbols, input_location, symbolize, locations);
}

// This implementation isn't great, it doesn't always show the best disambiguations for the given
// input.
//
// Also it misses a file name edge case: If there is one file whose full path in the symbols is a
// right-side subset of another (say "foo/bar.cc" and "something/foo/bar.cc"), then "foo/bar.cc" is
// the most unique name of the first file. But if the user types that, they'll get both matches and
// this function will report an ambiguous location.
//
// Instead, if the input is a file name and there is only one result where the file name matches
// exactly, we should pick it.
Err ResolveUniqueInputLocation(const ProcessSymbols* process_symbols,
                               const InputLocation& input_location, bool symbolize,
                               Location* location) {
  std::vector<Location> locations;
  Err err = ResolveInputLocations(process_symbols, input_location, symbolize, &locations);
  if (err.has_error())
    return err;

  FXL_DCHECK(!locations.empty());  // Non-empty on success should be guaranteed.

  if (locations.size() == 1u) {
    // Success, got a unique location.
    *location = locations[0];
    return Err();
  }

  // When there is more than one, generate an error that lists the possibilities for disambiguation.
  std::string err_str = "This resolves to more than one location. Could be:\n";
  constexpr size_t kMaxSuggestions = 10u;

  if (!symbolize) {
    // The original call did not request symbolization which will produce very
    // non-helpful suggestions. We're not concerned about performance in this error case so re-query
    // to get the full symbols.
    locations.clear();
    ResolveInputLocations(process_symbols, input_location, true, &locations);
  }

  for (size_t i = 0; i < locations.size() && i < kMaxSuggestions; i++) {
    // Always show the full path (omit TargetSymbols) since we're doing disambiguation and the
    // problem could have been two files with the same name but different paths.
    err_str += fxl::StringPrintf(" %s ", GetBullet().c_str());
    if (locations[i].file_line().is_valid()) {
      err_str += DescribeFileLine(nullptr, locations[i].file_line());
      err_str += fxl::StringPrintf(" = 0x%" PRIx64, locations[i].address());
    } else {
      err_str += FormatLocation(nullptr, locations[i], true, false).AsString();
    }
    err_str += "\n";
  }
  if (locations.size() > kMaxSuggestions) {
    err_str += fxl::StringPrintf("...%zu more omitted...\n", locations.size() - kMaxSuggestions);
  }
  return Err(err_str);
}

Err ResolveUniqueInputLocation(const ProcessSymbols* process_symbols, const Frame* optional_frame,
                               const std::string& input, bool symbolize, Location* location) {
  InputLocation input_location;
  Err err = ParseInputLocation(optional_frame, input, &input_location);
  if (err.has_error())
    return err;
  return ResolveUniqueInputLocation(process_symbols, input_location, symbolize, location);
}

void CompleteInputLocation(const Command& command, const std::string& prefix,
                           std::vector<std::string>* completions) {
  if (!command.target())
    return;

  // Number of items of each category that can be added to the completions.
  constexpr size_t kMaxFileNames = 32;
  constexpr size_t kMaxNamespaces = 8;
  constexpr size_t kMaxClasses = 32;
  constexpr size_t kMaxFunctions = 32;

  // Extract the current code block if possible. This will be used to find local variables and to
  // prioritize symbols from the current module.
  const CodeBlock* code_block = nullptr;
  SymbolContext symbol_context = SymbolContext::ForRelativeAddresses();
  if (const Frame* frame = command.frame()) {
    const Location& location = frame->GetLocation();
    if (const CodeBlock* fn_block = location.symbol().Get()->AsCodeBlock()) {
      symbol_context = location.symbol_context();
      code_block = fn_block->GetMostSpecificChild(symbol_context, location.address());
    }
  }

  // TODO(brettw) prioritize the current module when it's known (when there is a current frame with
  // symbol information). Factor prioritization code from find_name.cc
  for (const ModuleSymbols* mod : command.target()->GetSymbols()->GetModuleSymbols()) {
    const Index& index = mod->GetIndex();
    auto files = index.FindFilePrefixes(prefix);

    // Files get colons at the end for the user to type a line number next.
    for (auto& file : files)
      file.push_back(':');

    completions->insert(completions->end(), files.begin(), files.end());
  }

  std::sort(completions->begin(), completions->end());
  if (completions->size() > kMaxFileNames)
    completions->resize(kMaxFileNames);

  // Now search for functions matching the given input.
  FindNameOptions options(FindNameOptions::kNoKinds);
  options.how = FindNameOptions::kPrefix;

  ParsedIdentifier prefix_identifier;
  Err err = ExprParser::ParseIdentifier(prefix, &prefix_identifier);
  if (err.has_error())
    return;  // Can't match identifier names.

  // When there's a live process there is more context to find stuff.
  std::unique_ptr<FindNameContext> find_context;
  if (Process* process = command.target()->GetProcess()) {
    find_context =
        std::make_unique<FindNameContext>(process->GetSymbols(), symbol_context, code_block);
  } else {
    find_context = std::make_unique<FindNameContext>(command.target()->GetSymbols());
  }

  // First start with namespaces.
  options.find_namespaces = true;
  options.max_results = kMaxNamespaces;
  std::vector<FoundName> found_names;
  FindName(*find_context, options, prefix_identifier, &found_names);
  for (const FoundName& found : found_names)
    completions->push_back(found.GetName() + "::");
  options.find_namespaces = false;

  // Follow with types. Only do structure and class types since we're really looking for function
  // names. In the future it might be nice to check if there are any member functions in the types
  // before adding them.
  options.find_types = true;
  options.max_results = kMaxClasses;
  found_names.clear();
  FindName(*find_context, options, prefix_identifier, &found_names);
  for (const FoundName& found : found_names) {
    FXL_DCHECK(found.kind() == zxdb::FoundName::kType);
    if (const Collection* collection = found.type()->AsCollection())
      completions->push_back(found.GetName() + "::");
  }
  options.find_types = false;

  // Finish with functions.
  options.find_functions = true;
  options.max_results = kMaxFunctions;
  found_names.clear();
  FindName(*find_context, options, prefix_identifier, &found_names);
  for (const FoundName& found : found_names)
    completions->push_back(found.function()->GetFullName());
  options.find_functions = false;
}

}  // namespace zxdb
