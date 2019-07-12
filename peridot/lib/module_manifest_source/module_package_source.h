// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_MODULE_MANIFEST_SOURCE_MODULE_PACKAGE_SOURCE_H_
#define PERIDOT_LIB_MODULE_MANIFEST_SOURCE_MODULE_PACKAGE_SOURCE_H_

#include <fuchsia/maxwell/internal/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <src/lib/fxl/memory/weak_ptr.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "peridot/lib/module_manifest_source/module_manifest_source.h"

namespace modular {

// This source exposes a |ModuleManifestIndexer| service to this application's
// debug service. New manifests are published using that interfaces. When this
// source starts up, it gets its initial set of module packages to index by
// reading the module package index directory located under /system/data/.
class ModulePackageSource : public ModuleManifestSource,
                            ::fuchsia::maxwell::internal::ModulePackageIndexer {
 public:
  ModulePackageSource(sys::ComponentContext* context);
  ~ModulePackageSource() override;

  // |ModuleManifestSource|
  void Watch(async_dispatcher_t* dispatcher, IdleFn idle_fn, NewEntryFn new_fn,
             RemovedEntryFn removed_fn) override;

 private:
  // |ModulePackageIndexer|
  void IndexManifest(std::string package_name,
                     std::string module_manifest_path) override;

  const std::string dir_;
  NewEntryFn new_entry_fn_;
  fidl::BindingSet<::fuchsia::maxwell::internal::ModulePackageIndexer>
      indexer_bindings_;
  async_dispatcher_t* dispatcher_;

  fxl::WeakPtrFactory<ModulePackageSource> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModulePackageSource);
};

}  // namespace modular

#endif  // PERIDOT_LIB_MODULE_MANIFEST_SOURCE_MODULE_PACKAGE_SOURCE_H_
