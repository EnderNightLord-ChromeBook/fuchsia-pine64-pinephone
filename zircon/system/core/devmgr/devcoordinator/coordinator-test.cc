// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "coordinator.h"

#include <ddk/binding.h>
#include <ddk/driver.h>
#include <fbl/algorithm.h>
#include <fbl/vector.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/coding.h>
#include <threads.h>
#include <zircon/fidl.h>
#include <zxtest/zxtest.h>

#include "../shared/fdio.h"
#include "devfs.h"
#include "devhost.h"

namespace devmgr {
zx::channel fs_clone(const char* path) { return zx::channel(); }
}  // namespace devmgr

namespace {

constexpr char kSystemDriverPath[] = "/boot/driver/platform-bus.so";
constexpr char kDriverPath[] = "/boot/driver/test/mock-device.so";

devmgr::CoordinatorConfig DefaultConfig(async_dispatcher_t* dispatcher) {
  devmgr::CoordinatorConfig config{};
  config.dispatcher = dispatcher;
  config.require_system = false;
  config.asan_drivers = false;
  zx::event::create(0, &config.fshost_event);
  return config;
}

TEST(CoordinatorTestCase, InitializeCoreDevices) {
  devmgr::Coordinator coordinator(DefaultConfig(nullptr));

  zx_status_t status = coordinator.InitializeCoreDevices(kSystemDriverPath);
  ASSERT_EQ(ZX_OK, status);
}

TEST(CoordinatorTestCase, DumpState) {
  devmgr::Coordinator coordinator(DefaultConfig(nullptr));

  zx_status_t status = coordinator.InitializeCoreDevices(kSystemDriverPath);
  ASSERT_EQ(ZX_OK, status);

  constexpr int32_t kBufSize = 256;
  char buf[kBufSize + 1] = {0};

  zx::vmo vmo;
  ASSERT_EQ(ZX_OK, zx::vmo::create(kBufSize, 0, &vmo));
  devmgr::VmoWriter writer(std::move(vmo));

  coordinator.DumpState(&writer);

  ASSERT_EQ(writer.written(), writer.available());
  ASSERT_LT(writer.written(), kBufSize);
  ASSERT_GT(writer.written(), 0);
  ASSERT_EQ(ZX_OK, writer.vmo().read(buf, 0, writer.written()));

  ASSERT_NE(nullptr, strstr(buf, "[root]"));
}

TEST(CoordinatorTestCase, LoadDriver) {
  bool found_driver = false;
  auto callback = [&found_driver](devmgr::Driver* drv, const char* version) {
    delete drv;
    found_driver = true;
  };
  devmgr::load_driver(kDriverPath, callback);
  ASSERT_TRUE(found_driver);
}

TEST(CoordinatorTestCase, BindDrivers) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  devmgr::Coordinator coordinator(DefaultConfig(loop.dispatcher()));

  zx_status_t status = coordinator.InitializeCoreDevices(kSystemDriverPath);
  ASSERT_EQ(ZX_OK, status);
  coordinator.set_running(true);

  devmgr::Driver* driver;
  auto callback = [&coordinator, &driver](devmgr::Driver* drv, const char* version) {
    driver = drv;
    return coordinator.DriverAdded(drv, version);
  };
  devmgr::load_driver(kDriverPath, callback);
  loop.RunUntilIdle();
  ASSERT_EQ(1, coordinator.drivers().size_slow());
  ASSERT_EQ(driver, &coordinator.drivers().front());
}

// Test binding drivers against the root/test/misc devices
TEST(CoordinatorTestCase, BindDriversForBuiltins) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  devmgr::Coordinator coordinator(DefaultConfig(loop.dispatcher()));

  zx_status_t status = coordinator.InitializeCoreDevices(kSystemDriverPath);
  ASSERT_EQ(ZX_OK, status);

  // AttemptBind function that asserts it has only been called once
  class CallOnce {
   public:
    explicit CallOnce(size_t line) : line_number_(line) {}
    CallOnce(const CallOnce&) = delete;
    CallOnce& operator=(const CallOnce&) = delete;

    CallOnce(CallOnce&& other) { *this = std::move(other); }
    CallOnce& operator=(CallOnce&& other) {
      if (this != &other) {
        line_number_ = other.line_number_;
        call_count_ = other.call_count_;
        // Ensure the dtor for the other one doesn't run
        other.call_count_ = 1;
      }
      return *this;
    }

    ~CallOnce() { EXPECT_EQ(1, call_count_, "Mismatch from line %zu\n", line_number_); }
    zx_status_t operator()(const devmgr::Driver* drv, const fbl::RefPtr<devmgr::Device>& dev) {
      ++call_count_;
      return ZX_OK;
    }

   private:
    size_t line_number_;
    size_t call_count_ = 0;
  };

  auto make_fake_driver = [](auto&& instructions) -> std::unique_ptr<devmgr::Driver> {
    size_t instruction_count = fbl::count_of(instructions);
    auto binding = std::make_unique<zx_bind_inst_t[]>(instruction_count);
    memcpy(binding.get(), instructions, instruction_count * sizeof(instructions[0]));
    auto drv = std::make_unique<devmgr::Driver>();
    drv->binding.reset(binding.release());
    drv->binding_size = static_cast<uint32_t>(instruction_count * sizeof(instructions[0]));
    return drv;
  };

  {
    zx_bind_inst_t test_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST_PARENT),
    };
    auto test_drv = make_fake_driver(test_drv_bind);
    ASSERT_OK(coordinator.BindDriver(test_drv.get(), CallOnce{__LINE__}));
  }

  {
    zx_bind_inst_t misc_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_MISC_PARENT),
    };
    auto misc_drv = make_fake_driver(misc_drv_bind);
    ASSERT_OK(coordinator.BindDriver(misc_drv.get(), CallOnce{__LINE__}));
  }

  {
    zx_bind_inst_t root_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_ROOT),
    };
    auto root_drv = make_fake_driver(root_drv_bind);
    ASSERT_OK(coordinator.BindDriver(root_drv.get(), CallOnce{__LINE__}));
  }

  {
    zx_bind_inst_t test_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST_PARENT),
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    };
    auto test_drv = make_fake_driver(test_drv_bind);
    ASSERT_OK(coordinator.BindDriver(test_drv.get(), CallOnce{__LINE__}));
  }

  {
    zx_bind_inst_t misc_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_MISC_PARENT),
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    };
    auto misc_drv = make_fake_driver(misc_drv_bind);
    ASSERT_OK(coordinator.BindDriver(misc_drv.get(), CallOnce{__LINE__}));
  }

  {
    zx_bind_inst_t root_drv_bind[] = {
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_ROOT),
        BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    };
    auto root_drv = make_fake_driver(root_drv_bind);
    ASSERT_OK(coordinator.BindDriver(root_drv.get(), CallOnce{__LINE__}));
  }
}

void InitializeCoordinator(devmgr::Coordinator* coordinator) {
  zx_status_t status = coordinator->InitializeCoreDevices(kSystemDriverPath);
  ASSERT_EQ(ZX_OK, status);

  // Load the component driver
  devmgr::load_driver(devmgr::kComponentDriverPath,
                      fit::bind_member(coordinator, &devmgr::Coordinator::DriverAddedInit));

  // Add the driver we're using as platform bus
  devmgr::load_driver(kSystemDriverPath,
                      fit::bind_member(coordinator, &devmgr::Coordinator::DriverAddedInit));

  // Initialize devfs.
  devmgr::devfs_init(coordinator->root_device(), coordinator->dispatcher());
  status = devmgr::devfs_publish(coordinator->root_device(), coordinator->test_device());
  status = devmgr::devfs_publish(coordinator->root_device(), coordinator->sys_device());
  ASSERT_EQ(ZX_OK, status);
  coordinator->set_running(true);
}

// Reads a BindDriver request from remote, checks that it is for the expected
// driver, and then sends a ZX_OK response.
void CheckBindDriverReceived(const zx::channel& remote, const char* expected_driver) {
  // Read the BindDriver request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(1, actual_handles);
  status = zx_handle_close(handles[0]);
  ASSERT_OK(status);

  // Validate the BindDriver request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerBindDriverOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerBindDriverRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DeviceControllerBindDriverRequest*>(bytes);
  ASSERT_EQ(req->driver_path.size, strlen(expected_driver));
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected_driver),
                  reinterpret_cast<const uint8_t*>(req->driver_path.data), req->driver_path.size,
                  "");

  // Write the BindDriver response.
  memset(bytes, 0, sizeof(bytes));
  auto resp = reinterpret_cast<fuchsia_device_manager_DeviceControllerBindDriverResponse*>(bytes);
  resp->hdr.ordinal = fuchsia_device_manager_DeviceControllerBindDriverOrdinal;
  resp->status = ZX_OK;
  status = fidl_encode(&fuchsia_device_manager_DeviceControllerBindDriverResponseTable, bytes,
                       sizeof(*resp), handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*resp), nullptr, 0);
  ASSERT_OK(status);
}

TEST(CoordinatorTestCase, BindDevices) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  devmgr::Coordinator coordinator(DefaultConfig(loop.dispatcher()));

  ASSERT_NO_FATAL_FAILURES(InitializeCoordinator(&coordinator));

  // Add the device.
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  ASSERT_EQ(ZX_OK, status);
  fbl::RefPtr<devmgr::Device> device;
  status = coordinator.AddDevice(coordinator.test_device(), std::move(local),
                                 nullptr /* props_data */, 0 /* props_count */, "mock-device",
                                 ZX_PROTOCOL_TEST, nullptr /* driver_path */, nullptr /* args */,
                                 false /* invisible */, zx::channel() /* client_remote */, &device);
  ASSERT_EQ(ZX_OK, status);
  ASSERT_EQ(1, coordinator.devices().size_slow());

  // Add the driver.
  devmgr::load_driver(kDriverPath,
                      fit::bind_member(&coordinator, &devmgr::Coordinator::DriverAdded));
  loop.RunUntilIdle();
  ASSERT_FALSE(coordinator.drivers().is_empty());

  // Bind the device to a fake devhost.
  fbl::RefPtr<devmgr::Device> dev = fbl::WrapRefPtr(&coordinator.devices().front());
  devmgr::Devhost host;
  host.AddRef();  // refcount starts at zero, so bump it up to keep us from being cleaned up
  dev->set_host(&host);
  status = coordinator.BindDevice(dev, kDriverPath, true /* new device */);
  ASSERT_EQ(ZX_OK, status);

  // Check the BindDriver request.
  ASSERT_NO_FATAL_FAILURES(CheckBindDriverReceived(remote, kDriverPath));
  loop.RunUntilIdle();

  // Reset the fake devhost connection.
  dev->set_host(nullptr);
  remote.reset();
  loop.RunUntilIdle();
}

// Reads a CreateDevice from remote, checks expectations, and sends a ZX_OK
// response.
void CheckCreateDeviceReceived(const zx::channel& remote, const char* expected_driver,
                               zx::channel* device_remote) {
  // Read the CreateDevice request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(3, actual_handles);
  *device_remote = zx::channel(handles[0]);
  status = zx_handle_close(handles[1]);
  ASSERT_OK(status);

  // Validate the CreateDevice request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DevhostControllerCreateDeviceOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DevhostControllerCreateDeviceRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DevhostControllerCreateDeviceRequest*>(bytes);
  ASSERT_EQ(req->driver_path.size, strlen(expected_driver));
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected_driver),
                  reinterpret_cast<const uint8_t*>(req->driver_path.data), req->driver_path.size,
                  "");
}

// Reads a Suspend request from remote, checks that it is for the expected
// flags, and then sends the given response.
void CheckSuspendReceived(const zx::channel& remote, uint32_t expected_flags,
                          zx_status_t return_status) {
  // Read the Suspend request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  // Validate the Suspend request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerSuspendOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerSuspendRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DeviceControllerSuspendRequest*>(bytes);
  ASSERT_EQ(req->flags, expected_flags);

  // Write the Suspend response.
  memset(bytes, 0, sizeof(bytes));
  auto resp = reinterpret_cast<fuchsia_device_manager_DeviceControllerSuspendResponse*>(bytes);
  resp->hdr.ordinal = fuchsia_device_manager_DeviceControllerSuspendOrdinal;
  resp->status = return_status;
  status = fidl_encode(&fuchsia_device_manager_DeviceControllerSuspendResponseTable, bytes,
                       sizeof(*resp), handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*resp), nullptr, 0);
  ASSERT_OK(status);
}

// Reads a CreateCompositeDevice from remote, checks expectations, and sends
// a ZX_OK response.
void CheckCreateCompositeDeviceReceived(const zx::channel& remote, const char* expected_name,
                                        size_t expected_components_count,
                                        zx::channel* composite_remote) {
  // Read the CreateCompositeDevice request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(1, actual_handles);
  composite_remote->reset(handles[0]);

  // Validate the CreateCompositeDevice request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DevhostControllerCreateCompositeDeviceOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DevhostControllerCreateCompositeDeviceRequestTable,
                       bytes, actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req =
      reinterpret_cast<fuchsia_device_manager_DevhostControllerCreateCompositeDeviceRequest*>(
          bytes);
  ASSERT_EQ(req->name.size, strlen(expected_name));
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected_name),
                  reinterpret_cast<const uint8_t*>(req->name.data), req->name.size, "");
  ASSERT_EQ(expected_components_count, req->components.count);

  // Write the CreateCompositeDevice response.
  memset(bytes, 0, sizeof(bytes));
  auto resp =
      reinterpret_cast<fuchsia_device_manager_DevhostControllerCreateCompositeDeviceResponse*>(
          bytes);
  resp->hdr.ordinal = fuchsia_device_manager_DevhostControllerCreateCompositeDeviceOrdinal;
  resp->status = ZX_OK;
  status =
      fidl_encode(&fuchsia_device_manager_DevhostControllerCreateCompositeDeviceResponseTable,
                  bytes, sizeof(*resp), handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*resp), nullptr, 0);
  ASSERT_OK(status);
}

// Helper for BindComposite for issuing an AddComposite for a composite with the
// given components.  It's assumed that these components are children of
// the platform_bus and have the given protocol_id
void BindCompositeDefineComposite(const fbl::RefPtr<devmgr::Device>& platform_bus,
                                  const uint32_t* protocol_ids, size_t component_count,
                                  const zx_device_prop_t* props, size_t props_count,
                                  const char* name, zx_status_t expected_status = ZX_OK) {
  auto components = std::make_unique<fuchsia_device_manager_DeviceComponent[]>(component_count);
  for (size_t i = 0; i < component_count; ++i) {
    // Define a union type to avoid violating the strict aliasing rule.
    union InstValue {
      zx_bind_inst_t inst;
      uint64_t value;
    };
    InstValue always = {.inst = BI_MATCH()};
    InstValue protocol = {.inst = BI_MATCH_IF(EQ, BIND_PROTOCOL, protocol_ids[i])};

    fuchsia_device_manager_DeviceComponent* component = &components[i];
    component->parts_count = 2;
    component->parts[0].match_program_count = 1;
    component->parts[0].match_program[0] = always.value;
    component->parts[1].match_program_count = 1;
    component->parts[1].match_program[0] = protocol.value;
  }
  devmgr::Coordinator* coordinator = platform_bus->coordinator;
  ASSERT_EQ(
      coordinator->AddCompositeDevice(platform_bus, name, props, props_count, components.get(),
                                      component_count, 0 /* coresident index */),
      expected_status);
}

struct DeviceState {
  // The representation in the coordinator of the device
  fbl::RefPtr<devmgr::Device> device;
  // The remote end of the channel that the coordinator is talking to
  zx::channel remote;
};

class MultipleDeviceTestCase : public zxtest::Test {
 public:
  ~MultipleDeviceTestCase() override = default;

  async::Loop* loop() { return &loop_; }
  devmgr::Coordinator* coordinator() { return &coordinator_; }

  devmgr::Devhost* devhost() { return &devhost_; }
  const zx::channel& devhost_remote() { return devhost_remote_; }

  const fbl::RefPtr<devmgr::Device>& platform_bus() const { return platform_bus_.device; }
  const zx::channel& platform_bus_remote() const { return platform_bus_.remote; }
  DeviceState* device(size_t index) const { return &devices_[index]; }

  void AddDevice(const fbl::RefPtr<devmgr::Device>& parent, const char* name, uint32_t protocol_id,
                 fbl::String driver, size_t* device_index);
  void RemoveDevice(size_t device_index);

  bool DeviceHasPendingMessages(size_t device_index);
  bool DeviceHasPendingMessages(const zx::channel& remote);

  void DoSuspend(uint32_t flags);
  void DoSuspend(uint32_t flags, fit::function<void(uint32_t)> suspend_cb);

 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURES(InitializeCoordinator(&coordinator_));

    // refcount starts at zero, so bump it up to keep us from being cleaned up
    devhost_.AddRef();
    {
      zx::channel local;
      zx_status_t status = zx::channel::create(0, &local, &devhost_remote_);
      ASSERT_EQ(ZX_OK, status);
      devhost_.set_hrpc(local.release());
    }

    // Set up the sys device proxy, inside of the devhost
    ASSERT_EQ(coordinator_.PrepareProxy(coordinator_.sys_device(), &devhost_), ZX_OK);
    loop_.RunUntilIdle();
    ASSERT_NO_FATAL_FAILURES(
        CheckCreateDeviceReceived(devhost_remote_, kSystemDriverPath, &sys_proxy_remote_));
    loop_.RunUntilIdle();

    // Create a child of the sys_device (an equivalent of the platform bus)
    {
      zx::channel local;
      zx_status_t status = zx::channel::create(0, &local, &platform_bus_.remote);
      ASSERT_EQ(ZX_OK, status);
      status = coordinator_.AddDevice(
          coordinator_.sys_device()->proxy(), std::move(local), nullptr /* props_data */,
          0 /* props_count */, "platform-bus", 0, nullptr /* driver_path */, nullptr /* args */,
          false /* invisible */, zx::channel() /* client_remote */, &platform_bus_.device);
      ASSERT_EQ(ZX_OK, status);
      loop_.RunUntilIdle();
    }
  }

  void TearDown() override {
    loop_.RunUntilIdle();
    // Remove the devices in the opposite order that we added them
    while (!devices_.is_empty()) {
      devices_.pop_back();
      loop_.RunUntilIdle();
    }
    platform_bus_.device.reset();
    loop_.RunUntilIdle();

    devhost_.devices().clear();
  }

  // The fake devhost that the platform bus is put into
  devmgr::Devhost devhost_;

  // The remote end of the channel that the coordinator uses to talk to the
  // devhost
  zx::channel devhost_remote_;

  // The remote end of the channel that the coordinator uses to talk to the
  // sys device proxy
  zx::channel sys_proxy_remote_;

  // The device object representing the platform bus driver (child of the
  // sys proxy)
  DeviceState platform_bus_;

  // These should be listed after devhost/sys_proxy as it needs to be
  // destroyed before them.
  async::Loop loop_{&kAsyncLoopConfigNoAttachToThread};
  devmgr::Coordinator coordinator_{DefaultConfig(loop_.dispatcher())};

  // A list of all devices that were added during this test, and their
  // channels.  These exist to keep them alive until the test is over.
  fbl::Vector<DeviceState> devices_;
};

void MultipleDeviceTestCase::AddDevice(const fbl::RefPtr<devmgr::Device>& parent, const char* name,
                                       uint32_t protocol_id, fbl::String driver, size_t* index) {
  DeviceState state;

  zx::channel local;
  zx_status_t status = zx::channel::create(0, &local, &state.remote);
  ASSERT_EQ(ZX_OK, status);
  status = coordinator_.AddDevice(
      parent, std::move(local), nullptr /* props_data */, 0 /* props_count */, name, protocol_id,
      driver.data() /* driver_path */, nullptr /* args */, false /* invisible */,
      zx::channel() /* client_remote */, &state.device);
  ASSERT_EQ(ZX_OK, status);
  loop_.RunUntilIdle();

  devices_.push_back(std::move(state));
  *index = devices_.size() - 1;
}

void MultipleDeviceTestCase::RemoveDevice(size_t device_index) {
  auto& state = devices_[device_index];
  ASSERT_OK(coordinator_.RemoveDevice(state.device, false));
  state.device.reset();
  state.remote.reset();
  loop_.RunUntilIdle();
}

bool MultipleDeviceTestCase::DeviceHasPendingMessages(const zx::channel& remote) {
  return remote.wait_one(ZX_CHANNEL_READABLE, zx::time(0), nullptr) == ZX_OK;
}
bool MultipleDeviceTestCase::DeviceHasPendingMessages(size_t device_index) {
  return DeviceHasPendingMessages(devices_[device_index].remote);
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags,
                                       fit::function<void(uint32_t flags)> suspend_cb) {
  const bool vfs_exit_expected = (flags != DEVICE_SUSPEND_FLAG_SUSPEND_RAM);
  if (vfs_exit_expected) {
    zx::unowned_event event(coordinator()->fshost_event());
    auto thrd_func = [](void* ctx) -> int {
      zx::unowned_event event(*static_cast<zx::unowned_event*>(ctx));
      if (event->wait_one(FSHOST_SIGNAL_EXIT, zx::time::infinite(), nullptr) != ZX_OK) {
        return false;
      }
      if (event->signal(0, FSHOST_SIGNAL_EXIT_DONE) != ZX_OK) {
        return false;
      }
      return true;
    };
    thrd_t fshost_thrd;
    ASSERT_EQ(thrd_create(&fshost_thrd, thrd_func, &event), thrd_success);

    suspend_cb(flags);
    loop()->RunUntilIdle();

    int thread_status;
    ASSERT_EQ(thrd_join(fshost_thrd, &thread_status), thrd_success);
    ASSERT_TRUE(thread_status);

    // Make sure that vfs_exit() happened.
    ASSERT_OK(
        coordinator()->fshost_event().wait_one(FSHOST_SIGNAL_EXIT_DONE, zx::time(0), nullptr));
  } else {
    suspend_cb(flags);
    loop()->RunUntilIdle();

    // Make sure that vfs_exit() didn't happen.
    ASSERT_EQ(coordinator()->fshost_event().wait_one(FSHOST_SIGNAL_EXIT | FSHOST_SIGNAL_EXIT_DONE,
                                                     zx::time(0), nullptr),
              ZX_ERR_TIMED_OUT);
  }
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags) {
  DoSuspend(flags, [this](uint32_t flags) { coordinator()->Suspend(flags); });
}

class SuspendTestCase : public MultipleDeviceTestCase {
 public:
  void SuspendTest(uint32_t flags);
};

TEST_F(SuspendTestCase, Poweroff) {
  ASSERT_NO_FATAL_FAILURES(SuspendTest(DEVICE_SUSPEND_FLAG_POWEROFF));
}

TEST_F(SuspendTestCase, Reboot) {
  ASSERT_NO_FATAL_FAILURES(SuspendTest(DEVICE_SUSPEND_FLAG_REBOOT));
}

TEST_F(SuspendTestCase, RebootWithFlags) {
  ASSERT_NO_FATAL_FAILURES(SuspendTest(DEVICE_SUSPEND_FLAG_REBOOT_BOOTLOADER));
}

TEST_F(SuspendTestCase, Mexec) { ASSERT_NO_FATAL_FAILURES(SuspendTest(DEVICE_SUSPEND_FLAG_MEXEC)); }

TEST_F(SuspendTestCase, SuspendToRam) {
  ASSERT_NO_FATAL_FAILURES(SuspendTest(DEVICE_SUSPEND_FLAG_SUSPEND_RAM));
}

// Verify the suspend order is correct
void SuspendTestCase::SuspendTest(uint32_t flags) {
  struct DeviceDesc {
    // Index into the device desc array below.  UINT32_MAX = platform_bus()
    const size_t parent_desc_index;
    const char* const name;
    // index for use with device()
    size_t index = 0;
    bool suspended = false;
  };
  DeviceDesc devices[] = {
      {UINT32_MAX, "root_child1"}, {UINT32_MAX, "root_child2"}, {0, "root_child1_1"},
      {0, "root_child1_2"},        {2, "root_child1_1_1"},      {1, "root_child2_1"},
  };
  for (auto& desc : devices) {
    fbl::RefPtr<devmgr::Device> parent;
    if (desc.parent_desc_index == UINT32_MAX) {
      parent = platform_bus();
    } else {
      size_t index = devices[desc.parent_desc_index].index;
      parent = device(index)->device;
    }
    ASSERT_NO_FATAL_FAILURES(AddDevice(parent, desc.name, 0 /* protocol id */, "", &desc.index));
  }

  ASSERT_NO_FATAL_FAILURES(DoSuspend(flags));

  size_t num_to_suspend = fbl::count_of(devices);
  while (num_to_suspend > 0) {
    // Check that platform bus is not suspended yet.
    ASSERT_FALSE(DeviceHasPendingMessages(platform_bus_remote()));

    bool made_progress = false;
    // Since the table of devices above is topologically sorted (i.e.
    // any child is below its parent), this loop should always be able
    // to catch a parent receiving a suspend message before its child.
    for (size_t i = 0; i < fbl::count_of(devices); ++i) {
      auto& desc = devices[i];
      if (desc.suspended) {
        continue;
      }

      if (!DeviceHasPendingMessages(desc.index)) {
        continue;
      }

      ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(device(desc.index)->remote, flags, ZX_OK));

      // Make sure all descendants of this device are already suspended.
      // We just need to check immediate children since this will
      // recursively enforce that property.
      for (auto& other_desc : devices) {
        if (other_desc.parent_desc_index == i) {
          ASSERT_TRUE(other_desc.suspended);
        }
      }

      desc.suspended = true;
      --num_to_suspend;
      made_progress = true;
    }

    // Make sure we're not stuck waiting
    ASSERT_TRUE(made_progress);
    loop()->RunUntilIdle();
  }

  ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(platform_bus_remote(), flags, ZX_OK));
}

class CompositeTestCase : public MultipleDeviceTestCase {
 public:
  ~CompositeTestCase() override = default;

  void CheckCompositeCreation(const char* composite_name, const size_t* device_indexes,
                              size_t device_indexes_count, size_t* component_indexes_out,
                              zx::channel* composite_remote_out);

 protected:
  void SetUp() override {
    MultipleDeviceTestCase::SetUp();
    ASSERT_NOT_NULL(coordinator_.component_driver());
  }
};

void CompositeTestCase::CheckCompositeCreation(const char* composite_name,
                                               const size_t* device_indexes,
                                               size_t device_indexes_count,
                                               size_t* component_indexes_out,
                                               zx::channel* composite_remote_out) {
  for (size_t i = 0; i < device_indexes_count; ++i) {
    auto device_state = device(device_indexes[i]);
    // Check that the components got bound
    fbl::String driver = coordinator()->component_driver()->libname;
    ASSERT_NO_FATAL_FAILURES(CheckBindDriverReceived(device_state->remote, driver.data()));
    loop()->RunUntilIdle();

    // Synthesize the AddDevice request the component driver would send
    char name[32];
    snprintf(name, sizeof(name), "component-device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(device_state->device, name, 0, driver, &component_indexes_out[i]));
  }
  // Make sure the composite comes up
  ASSERT_NO_FATAL_FAILURES(CheckCreateCompositeDeviceReceived(
      devhost_remote(), composite_name, device_indexes_count, composite_remote_out));
}

class CompositeAddOrderTestCase : public CompositeTestCase {
 public:
  enum class AddLocation {
    // Add the composite before any components
    BEFORE,
    // Add the composite after some components
    MIDDLE,
    // Add the composite after all components
    AFTER,
  };
  void ExecuteTest(AddLocation add);
};

void CompositeAddOrderTestCase::ExecuteTest(AddLocation add) {
  size_t device_indexes[3];
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
      ZX_PROTOCOL_ETHERNET,
  };
  static_assert(fbl::count_of(protocol_id) == fbl::count_of(device_indexes));

  const char* kCompositeDevName = "composite-dev";
  auto do_add = [&]() {
    ASSERT_NO_FATAL_FAILURES(
        BindCompositeDefineComposite(platform_bus(), protocol_id, fbl::count_of(protocol_id),
                                     nullptr /* props */, 0, kCompositeDevName));
  };

  if (add == AddLocation::BEFORE) {
    ASSERT_NO_FATAL_FAILURES(do_add());
  }

  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < fbl::count_of(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(platform_bus(), name, protocol_id[i], "", &device_indexes[i]));
    if (i == 0 && add == AddLocation::MIDDLE) {
      ASSERT_NO_FATAL_FAILURES(do_add());
    }
  }

  if (add == AddLocation::AFTER) {
    ASSERT_NO_FATAL_FAILURES(do_add());
  }

  zx::channel composite_remote;
  size_t component_device_indexes[fbl::count_of(device_indexes)];
  ASSERT_NO_FATAL_FAILURES(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                  fbl::count_of(device_indexes),
                                                  component_device_indexes, &composite_remote));
}

TEST_F(CompositeAddOrderTestCase, DefineBeforeDevices) {
  ASSERT_NO_FATAL_FAILURES(ExecuteTest(AddLocation::BEFORE));
}

TEST_F(CompositeAddOrderTestCase, DefineInbetweenDevices) {
  ASSERT_NO_FATAL_FAILURES(ExecuteTest(AddLocation::MIDDLE));
}

TEST_F(CompositeAddOrderTestCase, DefineAfterDevices) {
  ASSERT_NO_FATAL_FAILURES(ExecuteTest(AddLocation::AFTER));
}

TEST_F(CompositeTestCase, CantAddFromNonPlatformBus) {
  size_t index;
  ASSERT_NO_FATAL_FAILURES(AddDevice(platform_bus(), "test-device", 0, "", &index));
  auto device_state = device(index);

  uint32_t protocol_id[] = {ZX_PROTOCOL_I2C, ZX_PROTOCOL_GPIO};
  ASSERT_NO_FATAL_FAILURES(
      BindCompositeDefineComposite(device_state->device, protocol_id, fbl::count_of(protocol_id),
                                   nullptr /* props */, 0, "composite-dev", ZX_ERR_ACCESS_DENIED));
}

TEST_F(CompositeTestCase, ComponentUnbinds) {
  size_t device_indexes[2];
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  static_assert(fbl::count_of(protocol_id) == fbl::count_of(device_indexes));

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURES(BindCompositeDefineComposite(platform_bus(), protocol_id,
                                                        fbl::count_of(protocol_id),
                                                        nullptr /* props */, 0, kCompositeDevName));

  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < fbl::count_of(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(platform_bus(), name, protocol_id[i], "", &device_indexes[i]));
  }
  zx::channel composite_remote;
  size_t component_device_indexes[fbl::count_of(device_indexes)];
  ASSERT_NO_FATAL_FAILURES(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                  fbl::count_of(device_indexes),
                                                  component_device_indexes, &composite_remote));
  loop()->RunUntilIdle();

  {
    // Remove device the composite, device 0's component device, and device 0
    auto device1 = device(device_indexes[1])->device;
    auto composite = device1->component()->composite()->device();
    ASSERT_OK(coordinator()->RemoveDevice(composite, false));

    ASSERT_NO_FATAL_FAILURES(RemoveDevice(component_device_indexes[0]));
    ASSERT_NO_FATAL_FAILURES(RemoveDevice(device_indexes[0]));
  }

  // Add the device back and verify the composite gets created again
  ASSERT_NO_FATAL_FAILURES(
      AddDevice(platform_bus(), "device-0", protocol_id[0], "", &device_indexes[0]));
  {
    auto device_state = device(device_indexes[0]);
    // Wait for the components to get bound
    fbl::String driver = coordinator()->component_driver()->libname;
    ASSERT_NO_FATAL_FAILURES(CheckBindDriverReceived(device_state->remote, driver.data()));
    loop()->RunUntilIdle();

    // Synthesize the AddDevice request the component driver would send
    ASSERT_NO_FATAL_FAILURES(AddDevice(device_state->device, "component-device-0", 0, driver,
                                       &component_device_indexes[0]));
  }
  ASSERT_NO_FATAL_FAILURES(CheckCreateCompositeDeviceReceived(
      devhost_remote(), kCompositeDevName, fbl::count_of(device_indexes), &composite_remote));
}

TEST_F(CompositeTestCase, SuspendOrder) {
  size_t device_indexes[2];
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  static_assert(fbl::count_of(protocol_id) == fbl::count_of(device_indexes));

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURES(BindCompositeDefineComposite(platform_bus(), protocol_id,
                                                        fbl::count_of(protocol_id),
                                                        nullptr /* props */, 0, kCompositeDevName));
  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < fbl::count_of(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(platform_bus(), name, protocol_id[i], "", &device_indexes[i]));
  }

  zx::channel composite_remote;
  size_t component_device_indexes[fbl::count_of(device_indexes)];
  ASSERT_NO_FATAL_FAILURES(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                  fbl::count_of(device_indexes),
                                                  component_device_indexes, &composite_remote));

  const uint32_t suspend_flags = DEVICE_SUSPEND_FLAG_POWEROFF;
  ASSERT_NO_FATAL_FAILURES(DoSuspend(suspend_flags));

  // Make sure none of the components have received their suspend requests
  ASSERT_FALSE(DeviceHasPendingMessages(platform_bus_remote()));
  for (auto idx : device_indexes) {
    ASSERT_FALSE(DeviceHasPendingMessages(idx));
  }
  for (auto idx : component_device_indexes) {
    ASSERT_FALSE(DeviceHasPendingMessages(idx));
  }
  // The composite should have been the first to get one
  ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(composite_remote, suspend_flags, ZX_OK));
  loop()->RunUntilIdle();

  // Next, all of the internal component devices should have them, but none of the devices
  // themselves
  ASSERT_FALSE(DeviceHasPendingMessages(platform_bus_remote()));
  for (auto idx : device_indexes) {
    ASSERT_FALSE(DeviceHasPendingMessages(idx));
  }
  for (auto idx : component_device_indexes) {
    ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(device(idx)->remote, suspend_flags, ZX_OK));
  }
  loop()->RunUntilIdle();

  // Next, the devices should get them
  ASSERT_FALSE(DeviceHasPendingMessages(platform_bus_remote()));
  for (auto idx : device_indexes) {
    ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(device(idx)->remote, suspend_flags, ZX_OK));
  }
  loop()->RunUntilIdle();

  // Finally, the platform bus driver, which is the parent of all of the devices
  ASSERT_NO_FATAL_FAILURES(CheckSuspendReceived(platform_bus_remote(), suspend_flags, ZX_OK));
  loop()->RunUntilIdle();
}

// Make sure we receive devfs notifications when composite devices appear
TEST_F(CompositeTestCase, DevfsNotifications) {
  zx::channel watcher;
  {
    zx::channel remote;
    ASSERT_OK(zx::channel::create(0, &watcher, &remote));
    ASSERT_OK(devfs_watch(coordinator()->root_device()->self, std::move(remote),
                          fuchsia_io_WATCH_MASK_ADDED));
  }

  size_t device_indexes[2];
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  static_assert(fbl::count_of(protocol_id) == fbl::count_of(device_indexes));

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURES(BindCompositeDefineComposite(platform_bus(), protocol_id,
                                                        fbl::count_of(protocol_id),
                                                        nullptr /* props */, 0, kCompositeDevName));
  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < fbl::count_of(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(platform_bus(), name, protocol_id[i], "", &device_indexes[i]));
  }

  zx::channel composite_remote;
  size_t component_device_indexes[fbl::count_of(device_indexes)];
  ASSERT_NO_FATAL_FAILURES(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                  fbl::count_of(device_indexes),
                                                  component_device_indexes, &composite_remote));

  uint8_t msg[fuchsia_io_MAX_FILENAME + 2];
  uint32_t msg_len = 0;
  ASSERT_OK(watcher.read(0, msg, nullptr, sizeof(msg), 0, &msg_len, nullptr));
  ASSERT_EQ(msg_len, 2 + strlen(kCompositeDevName));
  ASSERT_EQ(msg[0], fuchsia_io_WATCH_EVENT_ADDED);
  ASSERT_EQ(msg[1], strlen(kCompositeDevName));
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(kCompositeDevName), msg + 2, msg[1]);
}

// Make sure the path returned by GetTopologicalPath is accurate
TEST_F(CompositeTestCase, Topology) {
  size_t device_indexes[2];
  uint32_t protocol_id[] = {
      ZX_PROTOCOL_GPIO,
      ZX_PROTOCOL_I2C,
  };
  static_assert(fbl::count_of(protocol_id) == fbl::count_of(device_indexes));

  const char* kCompositeDevName = "composite-dev";
  ASSERT_NO_FATAL_FAILURES(BindCompositeDefineComposite(platform_bus(), protocol_id,
                                                        fbl::count_of(protocol_id),
                                                        nullptr /* props */, 0, kCompositeDevName));
  // Add the devices to construct the composite out of.
  for (size_t i = 0; i < fbl::count_of(device_indexes); ++i) {
    char name[32];
    snprintf(name, sizeof(name), "device-%zu", i);
    ASSERT_NO_FATAL_FAILURES(
        AddDevice(platform_bus(), name, protocol_id[i], "", &device_indexes[i]));
  }

  zx::channel composite_remote;
  size_t component_device_indexes[fbl::count_of(device_indexes)];
  ASSERT_NO_FATAL_FAILURES(CheckCompositeCreation(kCompositeDevName, device_indexes,
                                                  fbl::count_of(device_indexes),
                                                  component_device_indexes, &composite_remote));

  devmgr::Devnode* dn = coordinator()->root_device()->self;
  fbl::RefPtr<devmgr::Device> composite_dev;
  ASSERT_OK(devmgr::devfs_walk(dn, "composite-dev", &composite_dev));

  char path_buf[PATH_MAX];
  ASSERT_OK(coordinator()->GetTopologicalPath(composite_dev, path_buf, sizeof(path_buf)));
  ASSERT_STR_EQ(path_buf, "/dev/composite-dev");
}

TEST_F(MultipleDeviceTestCase, SuspendFidlMexec) {
  ASSERT_OK(loop()->StartThread("DevCoordTestLoop"));

  async::Wait suspend_task_pbus(
      platform_bus_remote().get(), ZX_CHANNEL_READABLE,
      [this](async_dispatcher_t*, async::Wait*, zx_status_t, const zx_packet_signal_t*) {
        CheckSuspendReceived(platform_bus_remote(), DEVICE_SUSPEND_FLAG_MEXEC, ZX_OK);
      });
  ASSERT_OK(suspend_task_pbus.Begin(loop()->dispatcher()));

  async::Wait suspend_task_sys(
      sys_proxy_remote_.get(), ZX_CHANNEL_READABLE,
      [this](async_dispatcher_t*, async::Wait*, zx_status_t, const zx_packet_signal_t*) {
        CheckSuspendReceived(sys_proxy_remote_, DEVICE_SUSPEND_FLAG_MEXEC, ZX_OK);
      });
  ASSERT_OK(suspend_task_sys.Begin(loop()->dispatcher()));

  zx::channel services, services_remote;
  ASSERT_OK(zx::channel::create(0, &services, &services_remote));

  ASSERT_OK(coordinator()->BindOutgoingServices(std::move(services_remote)));

  zx::channel channel, channel_remote;
  ASSERT_OK(zx::channel::create(0, &channel, &channel_remote));

  const char* service = "svc/" fuchsia_device_manager_Administrator_Name;
  ASSERT_OK(fdio_service_connect_at(services.get(), service, channel_remote.release()));

  bool callback_executed = false;
  DoSuspend(DEVICE_SUSPEND_FLAG_MEXEC, [&](uint32_t flags) {
    zx_status_t call_status = ZX_OK;
    ASSERT_OK(fuchsia_device_manager_AdministratorSuspend(channel.get(), flags, &call_status));
    ASSERT_OK(call_status);
    callback_executed = true;
  });

  ASSERT_TRUE(callback_executed);
  ASSERT_FALSE(suspend_task_pbus.is_pending());
  ASSERT_FALSE(suspend_task_sys.is_pending());
}

}  // namespace

int main(int argc, char** argv) { return RUN_ALL_TESTS(argc, argv); }
