// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTIC_TREE_IMPL_H_
#define SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTIC_TREE_IMPL_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/math/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/vfs/cpp/pseudo_file.h>

#include <unordered_map>
#include <unordered_set>

#include <src/lib/fxl/macros.h>

#include "garnet/lib/ui/gfx/util/unwrap.h"
#include "src/ui/a11y/bin/a11y_manager/util.h"

namespace a11y_manager {

class SemanticTreeImpl : public fuchsia::accessibility::semantics::SemanticTree {
 public:
  explicit SemanticTreeImpl(
      fuchsia::ui::views::ViewRef view_ref,
      fuchsia::accessibility::semantics::SemanticActionListenerPtr client_action_listener,
      vfs::PseudoDir* debug_dir)
      : view_ref_(std::move(view_ref)),
        client_action_listener_(std::move(client_action_listener)),
        debug_dir_(debug_dir) {
    InitializeDebugEntry(debug_dir_);
  }

  ~SemanticTreeImpl() override = default;

  // Provides a way to query a node with node id. This method returns
  // a copy of the queried node. It may return a nullptr if no node is found.
  fuchsia::accessibility::semantics::NodePtr GetAccessibilityNode(uint32_t node_id);

  // Asks the semantics provider to perform an accessibility action on the
  // node with node id in the front-end.
  void OnAccessibilityActionRequested(uint32_t node_id,
                                      fuchsia::accessibility::semantics::Action action,
                                      fuchsia::accessibility::semantics::SemanticActionListener::
                                          OnAccessibilityActionRequestedCallback callback);

  // Compares a view with the current view of the semantic tree, based on KOID.
  bool IsSameView(const fuchsia::ui::views::ViewRef& view_ref);

  // Compares given koid with the koid of the current view's viewref.
  bool IsSameKoid(const zx_koid_t koid);

  // Calls HitTest() function for the current semantic tree with given local
  // point.
  void PerformHitTesting(
      ::fuchsia::math::PointF local_point,
      fuchsia::accessibility::semantics::SemanticActionListener::HitTestCallback callback);

 private:
  // Representation of single semantic tree update/delete transaction.
  struct SemanticTreeTransaction {
    uint32_t node_id;
    bool delete_node;
    fuchsia::accessibility::semantics::Node node;
  };

  // Semantic Tree for a particular view. Each client is responsible for
  // maintaining the state of their tree. Nodes can be added, updated or
  // deleted. Because the size of an update may exceed FIDL transfer limits,
  // clients are responsible for breaking up changes into multiple update
  // and delete calls that conform to these limits. The commit function must
  // always be called at the end of a full update push to signal the end of
  // an update.
  // |fuchsia::accessibility::semantics::SemanticsTree|
  void Commit() override;

  // |fuchsia::accessibility::semantics::SemanticsTree|
  void UpdateSemanticNodes(std::vector<fuchsia::accessibility::semantics::Node> nodes) override;

  // |fuchsia::accessibility::semantics::SemanticsTree|
  void DeleteSemanticNodes(std::vector<uint32_t> node_ids) override;

  // Create semantic tree logs in a human readable form.
  std::string LogSemanticTree();

  // Helper function to traverse semantic tree with a root node, and for
  // creating string with tree information.
  void LogSemanticTreeHelper(fuchsia::accessibility::semantics::NodePtr root_node,
                             int current_level, std::string* tree_log);

  // Detect directed and undirected cycles in the tree rooted at "node".
  bool IsCyclic(fuchsia::accessibility::semantics::NodePtr node,
                std::unordered_set<uint32_t>* visited);

  // Helper function to delete subtree rooted at node_id.
  void DeleteSubtree(uint32_t node_id);

  // Helper function to delete pointer from parent node to given node.
  void DeletePointerFromParent(uint32_t node_id);

  // Internal helper function to check if a point is within a bounding box.
  bool BoxContainsPoint(const fuchsia::ui::gfx::BoundingBox& box, const escher::vec2& point) const;

  // Function to create per view Log files under debug directory for debugging
  // semantic tree.
  void InitializeDebugEntry(vfs::PseudoDir* debug_dir);

  // List of committed, cached nodes for each front-end. We represent semantics
  // tree as a map of local node ids to the actual node objects. All query
  // operations should use the node information from these trees.
  std::unordered_map<uint32_t /*node_id*/, fuchsia::accessibility::semantics::Node> nodes_;

  // List of pending semantic tree transactions.
  std::vector<SemanticTreeTransaction> pending_transactions_;

  fuchsia::ui::views::ViewRef view_ref_;
  fuchsia::accessibility::semantics::SemanticActionListenerPtr client_action_listener_;
  vfs::PseudoDir* const debug_dir_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SemanticTreeImpl);
};
}  // namespace a11y_manager

#endif  // SRC_UI_A11Y_BIN_A11Y_MANAGER_SEMANTICS_SEMANTIC_TREE_IMPL_H_
