// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/posix/socket/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/spawn.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/sync/completion.h>
#include <lib/zx/process.h>
#include <zircon/processargs.h>
#include <zxtest/zxtest.h>

namespace {

class Server final : public llcpp::fuchsia::posix::socket::Control::Interface {
 public:
  Server(zx_handle_t channel, zx::socket peer) : channel_(channel), peer_(std::move(peer)) {}

  void Clone(uint32_t flags, ::zx::channel object, CloneCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Close(CloseCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Describe(DescribeCompleter::Sync completer) override {
    llcpp::fuchsia::io::Socket socket;
    zx_status_t status = peer_.duplicate(ZX_RIGHT_SAME_RIGHTS, &socket.socket);
    if (status != ZX_OK) {
      return completer.Close(status);
    }
    llcpp::fuchsia::io::NodeInfo info;
    info.set_socket(std::move(socket));
    return completer.Reply(std::move(info));
  }

  void Sync(SyncCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetAttr(GetAttrCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void SetAttr(uint32_t flags, ::llcpp::fuchsia::io::NodeAttributes attributes,
               SetAttrCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Ioctl(uint32_t opcode, uint64_t max_out, fidl::VectorView<::zx::handle> handles,
             fidl::VectorView<uint8_t> in, IoctlCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Bind(fidl::VectorView<uint8_t> addr, BindCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Connect(fidl::VectorView<uint8_t> addr, ConnectCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Listen(int16_t backlog, ListenCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Accept(int16_t flags, AcceptCompleter::Sync completer) override {
    zx_status_t status = zx_object_signal_peer(channel_, 0, ZX_USER_SIGNAL_0);
    if (status != ZX_OK) {
      return completer.Close(status);
    }
    return completer.Close(sync_completion_wait(&accept_end_, ZX_TIME_INFINITE));
  }

  void GetSockName(GetSockNameCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetPeerName(GetPeerNameCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void SetSockOpt(int16_t level, int16_t optname, fidl::VectorView<uint8_t> optval,
                  SetSockOptCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetSockOpt(int16_t level, int16_t optname, GetSockOptCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void IoctlPOSIX(int16_t req, fidl::VectorView<uint8_t> in,
                  IoctlPOSIXCompleter::Sync completer) override {
    return completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  sync_completion_t accept_end_;

 private:
  zx_handle_t channel_;
  zx::socket peer_;
};

TEST(AtExit, ExitInAccept) {
  zx::channel client_channel, server_channel;
  ASSERT_OK(zx::channel::create(0, &client_channel, &server_channel));

  zx::socket client_socket, server_socket;
  ASSERT_OK(zx::socket::create(ZX_SOCKET_STREAM, &client_socket, &server_socket));

  // We're going to need the raw handle so we can signal on it and close it.
  zx_handle_t server_handle = server_channel.get();

  Server server(server_handle, std::move(server_socket));
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  ASSERT_OK(fidl::Bind(loop.dispatcher(), std::move(server_channel), &server));
  ASSERT_OK(loop.StartThread("fake-socket-server"));

  const char* root_dir = getenv("TEST_ROOT_DIR");
  if (root_dir == nullptr) {
    root_dir = "";
  }
  const std::string path = std::string(root_dir) + "/bin/accept-child";
  const char* argv[] = {path.c_str(), nullptr};
  const fdio_spawn_action_t actions[] = {
      {
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h =
              {
                  .id = PA_HND(PA_USER0, 0),
                  .handle = client_channel.release(),
              },
      },
  };
  zx::process process;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  EXPECT_OK(fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, argv[0], argv, nullptr,
                           sizeof(actions) / sizeof(fdio_spawn_action_t), actions,
                           process.reset_and_get_address(), err_msg));

  // Wait until the child has let us know that it is exiting.
  EXPECT_OK(zx_object_wait_one(server_handle, ZX_USER_SIGNAL_0, ZX_TIME_INFINITE, nullptr));
  // Close the channel to unblock the child's Close call.
  EXPECT_OK(zx_handle_close(server_handle));

  // Verify that the child didn't crash.
  EXPECT_OK(process.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), nullptr), "%s", err_msg);
  sync_completion_signal(&server.accept_end_);
  zx_info_process_t proc_info;
  ASSERT_OK(process.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr));
  EXPECT_EQ(proc_info.return_code, 0);
}

}  // namespace
