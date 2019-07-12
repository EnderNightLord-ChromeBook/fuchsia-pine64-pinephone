// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_INDEX_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_INDEX_H_

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/index_node.h"
#include "src/lib/fxl/macros.h"

namespace llvm {

class DWARFCompileUnit;
class DWARFContext;
class DWARFDie;
class DWARFUnit;

namespace object {
class ObjectFile;
}  // namespace object

}  // namespace llvm

namespace zxdb {

// Holds the index of symbols for a given module.
//
// Since this is per-module, looking up a symbol for a given process (the common case) requires
// going through the index for each module loaded in that process.
class Index {
 public:
  Index();
  ~Index();

  // This function takes an object file rather than a context so it can create its own context, and
  // then discard the context when it's done. Since most debugging information is not needed after
  // indexing, this saves a lot of memory.
  void CreateIndex(llvm::object::ObjectFile* object_file);

  const IndexNode& root() const { return root_; }
  IndexNode& root() { return root_; }

  size_t files_indexed() const { return file_name_index_.size(); }

  // Returns how many symbols are indexed. This iterates through everything so can be slow.
  size_t CountSymbolsIndexed() const;

  // Takes a fully-qualified name with namespaces and classes and template parameters and returns
  // the list of symbols which match exactly.
  const std::vector<IndexNode::DieRef>& FindExact(const Identifier& input) const;

  // Takes a fully-qualified name with namespaces and classes and returns a pair of iterators.
  //
  // The first iterator points to the first node that has the input as a prefix.
  //
  // The second returned iterator points to the last node IN THE CONTAINER. This does not indicate
  // the last node with the prefix. Many callers won't need all of the matches and doing it this way
  // avoids a second lookup.
  //
  // Non-last input nodes must match exactly with "std::string::operator==". For example, the input:
  //   { "std", "vector<" }
  // Would look in the "std" node and would return an iterator to the "vector<Aardvark>" node inside
  // it and the end of the "std" mode. Nodes are sorted by "std::string::operator<".
  //
  // If there are no matches both iterators will be the same (found == end).
  //
  // If the caller wants to find all matching prefixes, it can advance the iterator as long as the
  // last input component is a prefix if the current iterator key and less than the end.
  std::pair<IndexNode::ConstIterator, IndexNode::ConstIterator> FindPrefix(
      const Identifier& input) const;

  // Looks up the name in the file index and returns the set of matches. The name is matched from
  // the right side with a left boundary of either a slash or the beginning of the full path. This
  // may match more than one file name, and the caller is left to decide which one(s) it wants.
  std::vector<std::string> FindFileMatches(std::string_view name) const;

  // Same as FindFileMatches but does a prefix search. This only matches the file name component
  // (not directory paths).
  //
  // In the future it would be nice to match directories if there was a "/".
  std::vector<std::string> FindFilePrefixes(const std::string& prefix) const;

  // Looks up the given exact file path and returns all compile units it appears in. The file must
  // be an exact match (normally it's one of the results from FindFileMatches).
  //
  // The contents of the vector are indices into the compilation unit array. (see
  // llvm::DWARFContext::getCompileUnitAtIndex).
  const std::vector<unsigned>* FindFileUnitIndices(const std::string& name) const;

  // See main_functions_ below.
  const std::vector<IndexNode::DieRef>& main_functions() const { return main_functions_; }
  std::vector<IndexNode::DieRef>& main_functions() { return main_functions_; }

  // Dumps the file index to the stream for debugging.
  void DumpFileIndex(std::ostream& out) const;

 private:
  void IndexCompileUnit(llvm::DWARFContext* context, llvm::DWARFUnit* unit, unsigned unit_index);

  void IndexCompileUnitSourceFiles(llvm::DWARFContext* context, llvm::DWARFUnit* unit,
                                   unsigned unit_index);

  // Populates the file_name_index_ given a now-unchanging files_ map.
  void IndexFileNames();

  IndexNode root_;

  // Maps full path names to compile units that reference them. This must not be mutated once the
  // file_name_index_ is built.
  //
  // The contents of the vector are indices into the compilation unit array. (see
  // llvm::DWARFContext::getCompileUnitAtIndex). These are "unsigned" type because that's what LLVM
  // uses for these indices.
  //
  // This is a map, not a multimap, because some files will appear in many compilation units. I
  // suspect it's better to avoid duplicating the names (like a multimap would) and eating the cost
  // of indirect heap allocations for vectors in the single-item case.
  using FileIndex = std::map<std::string, std::vector<unsigned>>;
  FileIndex files_;

  // Maps the last file name component (the part following the last slash) to the set of entries in
  // the files_ index that have that name.
  //
  // This is a multimap because the name parts will generally be unique so we should get few
  // duplicates. The cost of using a vector for most items containing one element becomes higher in
  // that case.
  using FileNameIndex = std::multimap<std::string_view, FileIndex::const_iterator>;
  FileNameIndex file_name_index_;

  // All references to functions in this module found annotated with the DW_AT_main_subprogram
  // attribute. Normally there will be 0 (not all compiler annotate this) or 1.
  std::vector<IndexNode::DieRef> main_functions_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Index);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_INDEX_H_
