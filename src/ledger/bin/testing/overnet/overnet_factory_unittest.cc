// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/testing/overnet/overnet_factory.h"

#include <fuchsia/overnet/cpp/fidl.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/gtest/test_loop_fixture.h>

#include <memory>

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/fidl_helpers/message_relay.h"
#include "src/lib/fxl/macros.h"

namespace ledger {

namespace {

class OvernetFactoryTest : public gtest::TestLoopFixture {
 public:
  OvernetFactoryTest() {}
  ~OvernetFactoryTest() override {}

 protected:
  OvernetFactory factory_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(OvernetFactoryTest);
};

// Verifies that the host list is correct for one host.
TEST_F(OvernetFactoryTest, HostList_OneHost) {
  fuchsia::overnet::OvernetPtr overnet1;
  factory_.AddBinding(1u, overnet1.NewRequest());

  bool called = false;
  uint64_t version = 0;
  std::vector<fuchsia::overnet::Peer> host_list;
  overnet1->ListPeers(0u,
                      callback::Capture(callback::SetWhenCalled(&called), &version, &host_list));

  RunLoopUntilIdle();

  EXPECT_TRUE(called);
  EXPECT_NE(0u, version);
  ASSERT_GE(1u, host_list.size());
  EXPECT_EQ(1u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);

  called = false;
  overnet1->ListPeers(version,
                      callback::Capture(callback::SetWhenCalled(&called), &version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);
}

// Verifies that the host list is correct for two hosts.
TEST_F(OvernetFactoryTest, HostList_TwoHosts_Sequence) {
  fuchsia::overnet::OvernetPtr overnet1;
  factory_.AddBinding(1u, overnet1.NewRequest());

  bool called = false;
  uint64_t version = 0;
  std::vector<fuchsia::overnet::Peer> host_list;
  overnet1->ListPeers(0u,
                      callback::Capture(callback::SetWhenCalled(&called), &version, &host_list));

  RunLoopUntilIdle();
  EXPECT_TRUE(called);

  called = false;
  uint64_t new_version = version;
  overnet1->ListPeers(
      version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);

  fuchsia::overnet::OvernetPtr overnet2;
  factory_.AddBinding(2u, overnet2.NewRequest());

  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_NE(new_version, version);
  ASSERT_GE(2u, host_list.size());
  EXPECT_EQ(2u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
  EXPECT_EQ(2u, host_list.at(1).id.id);

  called = false;
  overnet2->ListPeers(
      0u, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  ASSERT_GE(2u, host_list.size());
  EXPECT_EQ(2u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
  EXPECT_EQ(2u, host_list.at(1).id.id);

  overnet2.Unbind();

  overnet1->ListPeers(
      new_version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));
  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  ASSERT_GE(1u, host_list.size());
  EXPECT_EQ(1u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
}

// Verifies that the host list is correct for two hosts when calls are chained,
// ie. when we have a pending call for a new host list waiting when a host
// connects or disconnects.
TEST_F(OvernetFactoryTest, HostList_TwoHosts_Chained) {
  fuchsia::overnet::OvernetPtr overnet1;
  factory_.AddBinding(1u, overnet1.NewRequest());

  bool called = false;
  uint64_t version = 0;
  std::vector<fuchsia::overnet::Peer> host_list;
  overnet1->ListPeers(0u,
                      callback::Capture(callback::SetWhenCalled(&called), &version, &host_list));

  RunLoopUntilIdle();
  EXPECT_TRUE(called);

  called = false;
  uint64_t new_version = version;
  overnet1->ListPeers(
      version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);

  fuchsia::overnet::OvernetPtr overnet2;
  factory_.AddBinding(2u, overnet2.NewRequest());

  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_NE(new_version, version);
  ASSERT_GE(2u, host_list.size());
  EXPECT_EQ(2u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
  EXPECT_EQ(2u, host_list.at(1).id.id);

  overnet1->ListPeers(
      new_version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);

  overnet2.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(called);

  ASSERT_GE(1u, host_list.size());
  EXPECT_EQ(1u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
}

TEST_F(OvernetFactoryTest, HostList_TwoHosts_Callback) {
  fuchsia::overnet::OvernetPtr overnet1;
  factory_.AddBinding(1u, overnet1.NewRequest());

  bool called = false;
  uint64_t version = 0;
  std::vector<fuchsia::overnet::Peer> host_list;
  overnet1->ListPeers(0u,
                      callback::Capture(callback::SetWhenCalled(&called), &version, &host_list));

  RunLoopUntilIdle();
  EXPECT_TRUE(called);

  called = false;
  uint64_t new_version;
  overnet1->ListPeers(
      version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);

  fuchsia::overnet::OvernetPtr overnet2;
  factory_.AddBinding(2u, overnet2.NewRequest());

  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_NE(new_version, version);
  ASSERT_GE(2u, host_list.size());
  EXPECT_EQ(2u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
  EXPECT_EQ(2u, host_list.at(1).id.id);

  bool called2;
  overnet1->ListPeers(
      new_version, callback::Capture(callback::SetWhenCalled(&called), &new_version, &host_list));
  overnet2->ListPeers(
      new_version, callback::Capture(callback::SetWhenCalled(&called2), &new_version, &host_list));

  RunLoopUntilIdle();
  EXPECT_FALSE(called);
  EXPECT_FALSE(called2);

  overnet2.Unbind();
  RunLoopUntilIdle();
  EXPECT_TRUE(called);
  EXPECT_FALSE(called2);

  ASSERT_GE(1u, host_list.size());
  EXPECT_EQ(1u, host_list.size());
  EXPECT_EQ(1u, host_list.at(0).id.id);
}

class OvernetServiceProvider : public fuchsia::overnet::ServiceProvider {
 public:
  OvernetServiceProvider(std::vector<std::unique_ptr<fidl_helpers::MessageRelay>>* relays)
      : relays_(relays) {}

  void ConnectToService(zx::channel channel) override {
    auto relay = std::make_unique<fidl_helpers::MessageRelay>();
    relay->SetChannel(std::move(channel));
    relays_->push_back(std::move(relay));
  }

 private:
  std::vector<std::unique_ptr<fidl_helpers::MessageRelay>>* const relays_;
};

// Tests that two "hosts" can talk to each other through the Overnet
TEST_F(OvernetFactoryTest, ServiceProvider) {
  // Sets up the first host (server).
  fuchsia::overnet::OvernetPtr overnet1;
  factory_.AddBinding(1u, overnet1.NewRequest());

  std::vector<std::unique_ptr<fidl_helpers::MessageRelay>> relays_host1;
  OvernetServiceProvider service_provider(&relays_host1);
  fidl::InterfaceHandle<fuchsia::overnet::ServiceProvider> handle;
  fidl::Binding binding(&service_provider, handle.NewRequest());
  overnet1->RegisterService("test_service", std::move(handle));

  RunLoopUntilIdle();

  // Sets up the second host (client).
  fuchsia::overnet::OvernetPtr overnet2;
  factory_.AddBinding(2u, overnet2.NewRequest());
  zx::channel local;
  zx::channel remote;
  zx_status_t status = zx::channel::create(0u, &local, &remote);

  FXL_CHECK(status == ZX_OK) << "zx::channel::create failed, status " << status;
  fuchsia::overnet::protocol::NodeId node_id;
  node_id.id = 1u;
  overnet2->ConnectToService(std::move(node_id), "test_service", std::move(remote));

  RunLoopUntilIdle();

  // Verifies that we have received the connection from host2 to host1.
  ASSERT_GE(1u, relays_host1.size());
  EXPECT_EQ(1u, relays_host1.size());

  // Sets up MessageRelays to abstract sending messages through channels.
  bool called_host1 = false;
  std::vector<uint8_t> message_host1;
  relays_host1[0]->SetMessageReceivedCallback(
      callback::Capture(callback::SetWhenCalled(&called_host1), &message_host1));

  fidl_helpers::MessageRelay relay2;
  relay2.SetChannel(std::move(local));
  bool called_host2 = false;
  std::vector<uint8_t> message_host2;
  relay2.SetMessageReceivedCallback(
      callback::Capture(callback::SetWhenCalled(&called_host2), &message_host2));

  // Sends a message from host2 to host1.
  relay2.SendMessage({0u, 1u});
  RunLoopUntilIdle();

  EXPECT_TRUE(called_host1);
  EXPECT_FALSE(called_host2);
  EXPECT_EQ(std::vector<uint8_t>({0u, 1u}), message_host1);

  // Sends a message from host1 to host2.
  called_host1 = false;
  relays_host1[0]->SendMessage({2u, 3u});
  RunLoopUntilIdle();

  EXPECT_FALSE(called_host1);
  EXPECT_TRUE(called_host2);
  EXPECT_EQ(std::vector<uint8_t>({2u, 3u}), message_host2);

  // Verifies that disconnection works.
  bool relay2_disconnected = false;
  relay2.SetChannelClosedCallback(callback::SetWhenCalled(&relay2_disconnected));
  relays_host1[0].reset();

  RunLoopUntilIdle();
  EXPECT_TRUE(relay2_disconnected);
}
}  // namespace

}  // namespace ledger
