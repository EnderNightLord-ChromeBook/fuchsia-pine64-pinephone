// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTICS_MANAGER_IMPL_H_
#define SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTICS_MANAGER_IMPL_H_
#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/vfs/cpp/pseudo_file.h>

#include "src/ui/a11y/bin/a11y_manager/semantics/semantic_tree_impl.h"

namespace a11y_manager {
class SemanticsManagerImpl : public fuchsia::accessibility::semantics::SemanticsManager {
 public:
  explicit SemanticsManagerImpl() = default;
  ~SemanticsManagerImpl() = default;

  void AddBinding(
      fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticsManager> request);

  void SetDebugDirectory(vfs::PseudoDir* debug_dir);

  // Provides the manager a way to query a node if it already knows
  // what view id and node id it wants to query for. This method returns
  // a copy of the queried node. It may return a nullptr if no node is found.
  fuchsia::accessibility::semantics::NodePtr GetAccessibilityNode(
      const fuchsia::ui::views::ViewRef& view_ref, const int32_t node_id);

 private:
  // |fuchsia::accessibility::semantics::SemanticsManager|:
  void RegisterView(
      fuchsia::ui::views::ViewRef view_ref,
      fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticActionListener> handle,
      fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree)
      override;

  fidl::BindingSet<fuchsia::accessibility::semantics::SemanticsManager> bindings_;

  fidl::BindingSet<fuchsia::accessibility::semantics::SemanticTree,
                   std::unique_ptr<SemanticTreeImpl>>
      semantic_tree_bindings_;

  vfs::PseudoDir* debug_dir_ = nullptr;
};
}  // namespace a11y_manager

#endif  // SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTICS_MANAGER_IMPL_H_
