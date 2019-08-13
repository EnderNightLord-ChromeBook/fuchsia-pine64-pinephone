// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/fd.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/zx/event.h>

#include <vector>

#include <gtest/gtest.h>

#include "src/ui/a11y/bin/a11y_manager/semantics/semantics_manager_impl.h"
#include "src/ui/a11y/bin/a11y_manager/util.h"
#include "src/ui/a11y/tests/integration/semantic_tree_parser.h"
#include "src/ui/a11y/tests/mocks/mock_semantic_provider.h"

namespace accessibility_test {
using fuchsia::accessibility::semantics::Attributes;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::NodePtr;
using fuchsia::accessibility::semantics::Role;
using fuchsia::accessibility::semantics::SemanticsManager;

const std::string kSemanticTreeSingle = "Node_id: 0, Label:Node-0\n";
const std::string kSemanticTreeOdd =
    "Node_id: 0, Label:Node-0\n"
    "    Node_id: 1, Label:Node-1\n"
    "        Node_id: 3, Label:Node-3\n"
    "        Node_id: 4, Label:Node-4\n"
    "    Node_id: 2, Label:Node-2\n"
    "        Node_id: 5, Label:Node-5\n"
    "        Node_id: 6, Label:Node-6\n";
const std::string kSemanticTreeEven =
    "Node_id: 0, Label:Node-0\n"
    "    Node_id: 1, Label:Node-1\n"
    "        Node_id: 3, Label:Node-3\n"
    "            Node_id: 7, Label:Node-7\n"
    "        Node_id: 4, Label:Node-4\n"
    "    Node_id: 2, Label:Node-2\n"
    "        Node_id: 5, Label:Node-5\n"
    "        Node_id: 6, Label:Node-6\n";
const int kMaxLogBufferSize = 1024;
const int kDeleteNodeId = 2;

const std::string kSemanticTreeSingleNodePath = "/pkg/data/semantic_tree_single_node.json";
const std::string kSemanticTreeOddNodesPath = "/pkg/data/semantic_tree_odd_nodes.json";
const std::string kSemanticTreeEvenNodesPath = "/pkg/data/semantic_tree_even_nodes.json";
const std::string kCyclicSemanticTreePath = "/pkg/data/cyclic_semantic_tree.json";
const std::string kDeletedSemanticSubtreePath = "/pkg/data/deleted_subtree_even_nodes.json";

// Unit tests for src/ui/a11y/bin/a11y_manager/semantics_manager_impl.h and
// semantic_tree_impl.h
class SemanticsManagerTest : public gtest::RealLoopFixture {
 public:
  void SetUp() override {
    RealLoopFixture::SetUp();
    syslog::InitLogger();

    zx::eventpair a, b;
    zx::eventpair::create(0u, &a, &b);
    view_ref_ = fuchsia::ui::views::ViewRef({
        .reference = std::move(a),
    });

    semantics_manager_impl_.SetDebugDirectory(context_provider_.context()->outgoing()->debug_dir());

    context_provider_.service_directory_provider()->AddService<SemanticsManager>(
        [this](fidl::InterfaceRequest<SemanticsManager> request) {
          semantics_manager_impl_.AddBinding(std::move(request));
        });
    RunLoopUntilIdle();
  }

  Node CreateTestNode(uint32_t node_id, std::string label);
  void InitializeActionListener(std::string file_path,
                                accessibility_test::MockSemanticProvider *provider);
  int OpenAsFD(vfs::internal::Node *node, async_dispatcher_t *dispatcher);
  char *ReadFile(vfs::internal::Node *node, int length, char *buffer);

  fuchsia::ui::views::ViewRef view_ref_;
  a11y_manager::SemanticsManagerImpl semantics_manager_impl_;
  sys::testing::ComponentContextProvider context_provider_;
  SemanticTreeParser semantic_tree_parser_;
};

// Create a test node with only a node id and a label.
Node SemanticsManagerTest::CreateTestNode(uint32_t node_id, std::string label) {
  Node node = Node();
  node.set_node_id(node_id);
  node.set_child_ids({});
  node.set_role(Role::UNKNOWN);
  node.set_attributes(Attributes());
  node.mutable_attributes()->set_label(std::move(label));
  fuchsia::ui::gfx::BoundingBox box;
  node.set_location(std::move(box));
  fuchsia::ui::gfx::mat4 transform;
  node.set_transform(std::move(transform));
  return node;
}

void SemanticsManagerTest::InitializeActionListener(
    std::string file_path, accessibility_test::MockSemanticProvider *provider) {
  // Create Node List for the current semantic provider.
  std::vector<Node> nodes;
  ASSERT_TRUE(semantic_tree_parser_.ParseSemanticTree(file_path, &nodes));

  // Add nodes list to the current semantic providers list.
  provider->UpdateSemanticNodes(std::move(nodes));
  RunLoopUntilIdle();

  // Commit the nodes.
  provider->Commit();
  RunLoopUntilIdle();
}

int SemanticsManagerTest::OpenAsFD(vfs::internal::Node *node, async_dispatcher_t *dispatcher) {
  zx::channel local, remote;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &local, &remote));
  EXPECT_EQ(ZX_OK, node->Serve(fuchsia::io::OPEN_RIGHT_READABLE, std::move(remote), dispatcher));
  int fd = -1;
  EXPECT_EQ(ZX_OK, fdio_fd_create(local.release(), &fd));
  return fd;
}

char *SemanticsManagerTest::ReadFile(vfs::internal::Node *node, int length, char *buffer) {
  EXPECT_LE(length, kMaxLogBufferSize);
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  loop.StartThread("ReadingDebugFile");

  int fd = OpenAsFD(node, loop.dispatcher());
  EXPECT_LE(0, fd);

  memset(buffer, 0, kMaxLogBufferSize);
  EXPECT_EQ(length, pread(fd, buffer, length, 0));
  return buffer;
}

// Basic test to check that a node update without commit will not result in
// any change to semantic tree.
TEST_F(SemanticsManagerTest, NodeUpdateWithoutCommit) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Check that the node is not present in the tree.
  EXPECT_EQ(nullptr, semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0));
}

// Basic test to check that a node update with commit will result in
// node being changed in the tree.
TEST_F(SemanticsManagerTest, NodeUpdateWithCommit) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check that the committed node is present in the semantic tree.
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_NE(returned_node, nullptr);
  EXPECT_EQ(node.node_id(), returned_node->node_id());
  EXPECT_STREQ(node.attributes().label().data(), returned_node->attributes().label().data());
}

// Basic test to check that a node delete without commit should result in
// node not being deleted in the tree.
TEST_F(SemanticsManagerTest, NodeDeleteWithoutCommit) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Call Delete Node.
  std::vector<uint32_t> delete_nodes;
  delete_nodes.push_back(node.node_id());
  semantic_provider.DeleteSemanticNodes(std::move(delete_nodes));
  RunLoopUntilIdle();

  // Node should still be present.
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_NE(returned_node, nullptr);
  EXPECT_EQ(node.node_id(), returned_node->node_id());
  EXPECT_STREQ(node.attributes().label().data(), returned_node->attributes().label().data());
}

// Basic test to check that a node delete with commit should result in
// node being deleted in the tree.
TEST_F(SemanticsManagerTest, NodeDeleteWithCommit) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Call Delete Node with commit.
  std::vector<uint32_t> delete_nodes;
  delete_nodes.push_back(node.node_id());
  semantic_provider.DeleteSemanticNodes(std::move(delete_nodes));
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check that the node is not present in the tree.
  EXPECT_EQ(nullptr, semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0));
}

// Commit() should ensure that there are no cycles in the tree after
// Update/Delete has been applied. If they are present, the tree should be
// deleted.
TEST_F(SemanticsManagerTest, DetectCycleInCommit) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Create Semantic Tree;
  std::vector<Node> nodes_list;
  ASSERT_TRUE(semantic_tree_parser_.ParseSemanticTree(kCyclicSemanticTreePath, &nodes_list));

  std::vector<Node> nodes_list_copy;
  nodes_list_copy = fidl::Clone(nodes_list);

  // Call update on the newly created semantic tree with cycle.
  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(nodes_list));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check that nodes are not present in the semantic tree.
  for (const Node &node : nodes_list_copy) {
    // Check that the node is not present in the tree.
    EXPECT_EQ(nullptr, semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy,
                                                                    node.node_id()));
  }
}

// Commit() should ensure that there are no dangling subtrees i.e.
// trees without parents. Which means if a node is deleted then the
// entire tree should be deleted.
TEST_F(SemanticsManagerTest, DetectDanglingSubtrees) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Create Semantic Tree;
  std::vector<Node> nodes_list;
  ASSERT_TRUE(semantic_tree_parser_.ParseSemanticTree(kSemanticTreeEvenNodesPath, &nodes_list));

  // Call update on the newly created semantic tree with cycle.
  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(nodes_list));
  RunLoopUntilIdle();

  // Delete a node.
  std::vector<uint32_t> delete_nodes;
  delete_nodes.push_back(kDeleteNodeId);
  semantic_provider.DeleteSemanticNodes(std::move(delete_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check root node is present.
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_NE(returned_node, nullptr);

  // Check subtree rooted at delete_node_id doesn't exist.
  std::vector<Node> subtree_list;
  ASSERT_TRUE(semantic_tree_parser_.ParseSemanticTree(kDeletedSemanticSubtreePath, &subtree_list));
  for (const Node &node : subtree_list) {
    // Check that the node is not present in the tree.
    EXPECT_EQ(nullptr, semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy,
                                                                    node.node_id()));
  }
}

// Update()/Delete(): These operations should happen in the order in
// which these request came.
// For example: Update 1, data 1
//              delete 1
//              update 1, data 2
// should result in Update 1 , data2 and NOT Empty Tree.
TEST_F(SemanticsManagerTest, InOrderUpdatesAndDelete) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Update Node 0 to Label-A
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label-A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));

  // Delete Node 0.
  std::vector<uint32_t> delete_nodes;
  int delete_node_id = 0;
  delete_nodes.push_back(delete_node_id);
  semantic_provider.DeleteSemanticNodes(std::move(delete_nodes));

  // Update Node 0 to Label-B
  std::vector<Node> update_nodes2;
  Node node2 = CreateTestNode(0, "Label-B");
  Node clone_node2;
  node2.Clone(&clone_node2);
  update_nodes2.push_back(std::move(clone_node2));
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes2));

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check Node 0 is present and has Label-B.
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_NE(returned_node, nullptr);
  EXPECT_EQ(node2.node_id(), returned_node->node_id());
  EXPECT_STREQ(node2.attributes().label().data(), returned_node->attributes().label().data());
}

// Test for LogSemanticTree() to make sure correct logs are generated,
// when number of nodes in the tree are odd.
TEST_F(SemanticsManagerTest, LogSemanticTree_OddNumberOfNodes) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  InitializeActionListener(kSemanticTreeOddNodesPath, &semantic_provider);
  vfs::PseudoDir *debug_dir = context_provider_.context()->outgoing()->debug_dir();
  vfs::internal::Node *node;
  EXPECT_EQ(ZX_OK, debug_dir->Lookup(
                       std::to_string(a11y_manager::GetKoid(view_ref_connection_copy)), &node));

  char buffer[kMaxLogBufferSize];
  ReadFile(node, kSemanticTreeOdd.size(), buffer);
  EXPECT_EQ(kSemanticTreeOdd, buffer);
}

// Test for LogSemanticTree() to make sure correct logs are generated,
// when number of nodes in the tree are even.
TEST_F(SemanticsManagerTest, LogSemanticTree_EvenNumberOfNodes) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  InitializeActionListener(kSemanticTreeEvenNodesPath, &semantic_provider);
  vfs::PseudoDir *debug_dir = context_provider_.context()->outgoing()->debug_dir();
  vfs::internal::Node *node;
  EXPECT_EQ(ZX_OK, debug_dir->Lookup(
                       std::to_string(a11y_manager::GetKoid(view_ref_connection_copy)), &node));

  char buffer[kMaxLogBufferSize];
  ReadFile(node, kSemanticTreeEven.size(), buffer);
  EXPECT_EQ(kSemanticTreeEven, buffer);
}

// Test for LogSemanticTree() to make sure correct logs are generated,
// when there is just a single node in the tree for a particular view.
TEST_F(SemanticsManagerTest, LogSemanticTree_SingleNode) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  InitializeActionListener(kSemanticTreeSingleNodePath, &semantic_provider);
  vfs::PseudoDir *debug_dir = context_provider_.context()->outgoing()->debug_dir();
  vfs::internal::Node *node;
  EXPECT_EQ(ZX_OK, debug_dir->Lookup(
                       std::to_string(a11y_manager::GetKoid(view_ref_connection_copy)), &node));

  char buffer[kMaxLogBufferSize];
  ReadFile(node, kSemanticTreeSingle.size(), buffer);
  EXPECT_EQ(kSemanticTreeSingle, buffer);
}

// Basic test to check that Semantic Tree is deleted when Semantics Manager is
// disabled.
TEST_F(SemanticsManagerTest, SemanticsManagerDisabled) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection, view_ref_connection_copy;
  fidl::Clone(view_ref_, &view_ref_connection);
  fidl::Clone(view_ref_, &view_ref_connection_copy);

  // Enable Semantics Manager.
  // Note: Enable has no effect on the behavior of semantics manager.
  semantics_manager_impl_.SetSemanticsManagerEnabled(true);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check that the committed node is present in the semantic tree.
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_NE(returned_node, nullptr);
  EXPECT_EQ(node.node_id(), returned_node->node_id());
  EXPECT_STREQ(node.attributes().label().data(), returned_node->attributes().label().data());

  // Disable Semantics Manager.
  // This should delete all the registered semantic tree so far.
  semantics_manager_impl_.SetSemanticsManagerEnabled(false);

  // Check that previously committed node is not present in the semantic tree.
  returned_node = semantics_manager_impl_.GetAccessibilityNode(view_ref_connection_copy, 0);
  EXPECT_EQ(returned_node, nullptr);
}

// Test for PerformHitTesting() to make sure correct node_id is passed from the
// semantic provider to semantics manager.
TEST_F(SemanticsManagerTest, PerformHitTesting_Pass) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection;
  fidl::Clone(view_ref_, &view_ref_connection);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  InitializeActionListener(kSemanticTreeOddNodesPath, &semantic_provider);

  // Set HitTest result in action listener.
  uint32_t expected_result = 5;
  semantic_provider.SetHitTestResult(expected_result);

  zx_koid_t koid = a11y_manager::GetKoid(view_ref_);
  ::fuchsia::math::PointF local_point;
  bool done = false;
  semantics_manager_impl_.PerformHitTesting(
      koid, local_point, [expected_result, &done](::fuchsia::accessibility::semantics::Hit hit) {
        EXPECT_EQ(expected_result, hit.node_id());
        EXPECT_EQ(1ul, hit.path_from_root().size());
        EXPECT_EQ(expected_result, hit.path_from_root()[0]);
        done = true;
      });
  RunLoopUntil([&done] { return done; });
}

// Basic test to make sure nodes can be searched using node id and Koid of
// ViewRef of that semantic tree.
TEST_F(SemanticsManagerTest, GetAccessibilityNodeByKoid) {
  // Create ViewRef.
  fuchsia::ui::views::ViewRef view_ref_connection;
  fidl::Clone(view_ref_, &view_ref_connection);

  // Create ActionListener.
  accessibility_test::MockSemanticProvider semantic_provider(context_provider_.context(),
                                                             std::move(view_ref_connection));
  // We make sure the Semantic Action Listener has finished connecting to the
  // root.
  RunLoopUntilIdle();

  // Creating test node to update.
  std::vector<Node> update_nodes;
  Node node = CreateTestNode(0, "Label A");
  Node clone_node;
  node.Clone(&clone_node);
  update_nodes.push_back(std::move(clone_node));

  // Update the node created above.
  semantic_provider.UpdateSemanticNodes(std::move(update_nodes));
  RunLoopUntilIdle();

  // Commit nodes.
  semantic_provider.Commit();
  RunLoopUntilIdle();

  // Check that the committed node is present in the semantic tree.
  zx_koid_t koid = a11y_manager::GetKoid(view_ref_);
  NodePtr returned_node = semantics_manager_impl_.GetAccessibilityNodeByKoid(koid, 0);
  EXPECT_NE(returned_node, nullptr);
  EXPECT_EQ(node.node_id(), returned_node->node_id());
  EXPECT_STREQ(node.attributes().label().data(), returned_node->attributes().label().data());
}

}  // namespace accessibility_test
