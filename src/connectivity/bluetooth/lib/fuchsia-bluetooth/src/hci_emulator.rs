// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        constants::{EMULATOR_DEVICE_DIR, EMULATOR_DRIVER_PATH, HOST_DEVICE_DIR},
        device_watcher::{DeviceFile, DeviceWatcher, WatchFilter},
        util::open_rdwr,
    },
    failure::{bail, format_err, Error},
    fidl_fuchsia_bluetooth_test::{EmulatorSettings, HciEmulatorProxy},
    fidl_fuchsia_device::ControllerSynchronousProxy,
    fidl_fuchsia_device_test::{
        DeviceSynchronousProxy, RootDeviceSynchronousProxy, CONTROL_DEVICE, MAX_DEVICE_NAME_LEN,
    },
    fidl_fuchsia_hardware_bluetooth::EmulatorProxy,
    fuchsia_async as fasync,
    fuchsia_syslog::fx_log_err,
    fuchsia_zircon as zx,
    std::{fs::File, path::PathBuf},
};

fn watch_timeout() -> zx::Duration {
    zx::Duration::from_seconds(10)
}

/// Represents a bt-hci device emulator. Instances of this type can be used manage the
/// bt-hci-emulator driver within the test device hierarchy. The associated driver instance gets
/// unbound and all bt-hci and bt-emulator device instances destroyed when a FakeHciDevice goes out
/// of scope.
pub struct Emulator {
    dev: TestDevice,
    emulator: HciEmulatorProxy,
}

impl Emulator {
    /// Returns the default settings.
    // TODO(armansito): Consider defining a library type for EmulatorSettings.
    pub fn default_settings() -> EmulatorSettings {
        EmulatorSettings {
            address: None,
            hci_config: None,
            extended_advertising: None,
            acl_buffer_settings: None,
            le_acl_buffer_settings: None,
        }
    }

    /// Publish a new bt-emulator device and return a handle to it. No corresponding bt-hci device
    /// will be published; to do so it must be explicitly configured and created with a call to
    /// `publish()`
    pub async fn create(name: &str) -> Result<Emulator, Error> {
        let dev = TestDevice::create(name)?;
        let emulator = await!(dev.bind())?;
        Ok(Emulator { dev: dev, emulator: emulator })
    }

    /// Publish a bt-emulator and a bt-hci device using the default emulator settings.
    pub async fn create_and_publish(name: &str) -> Result<Emulator, Error> {
        let fake_dev = await!(Emulator::create(name))?;
        await!(fake_dev.publish(Self::default_settings()))?;
        Ok(fake_dev)
    }

    /// Sends a publish message to the emulator. This is a convenience method that internally
    /// handles the FIDL binding error.
    pub async fn publish(&self, settings: EmulatorSettings) -> Result<(), Error> {
        let result = await!(self.emulator().publish(settings))?;
        result.map_err(|e| format_err!("failed to publish bt-hci device: {:#?}", e))
    }

    /// Sends a publish message emulator and returns a Future that resolves when a bt-host device is
    /// published. Note that this requires the bt-host driver to be installed. On success, returns a
    /// `DeviceFile` that represents the bt-host device.
    pub async fn publish_and_wait_for_host(
        &self,
        settings: EmulatorSettings,
    ) -> Result<DeviceFile, Error> {
        let mut watcher = DeviceWatcher::new(HOST_DEVICE_DIR, watch_timeout())?;
        let _ = await!(self.publish(settings))?;
        let topo = PathBuf::from(fdio::device_get_topo_path(self.file())?);
        await!(watcher.watch_new(&topo, WatchFilter::AddedOrExisting))
    }

    /// Returns a reference to the underlying file.
    pub fn file(&self) -> &File {
        &self.dev.0
    }

    /// Returns a reference to the fuchsia.bluetooth.test.HciEmulator protocol proxy.
    pub fn emulator(&self) -> &HciEmulatorProxy {
        &self.emulator
    }
}

// Represents the test device. Destroys the underlying device when it goes out of scope.
struct TestDevice(File);

impl TestDevice {
    // Creates a new device as a child of the root test device. This device will act as the parent
    // of our fake HCI device. If successful, `name` will act as the final fragment of the device
    // path, for example "/dev/test/test/{name}".
    fn create(name: &str) -> Result<TestDevice, Error> {
        if name.len() > (MAX_DEVICE_NAME_LEN as usize) {
            bail!(
                "Device name '{}' too long (must be {} or fewer chars)",
                name,
                MAX_DEVICE_NAME_LEN
            );
        }

        // Connect to the test control device and obtain a channel to the RootDevice capability.
        let control_dev = open_rdwr(CONTROL_DEVICE)?;
        let mut root_device = RootDeviceSynchronousProxy::new(fdio::clone_channel(&control_dev)?);

        // Create a device with the requested name.
        let (status, path) = root_device.create_device(name, zx::Time::INFINITE)?;
        zx::Status::ok(status)?;
        let path = path.ok_or(format_err!("RootDevice.CreateDevice returned null path"))?;

        // Open the device that was just created.
        Ok(TestDevice(open_rdwr(&path)?))
    }

    // Send the test device a destroy message which will unbind the driver.
    fn destroy(&mut self) -> Result<(), Error> {
        let channel = fdio::clone_channel(&self.0)?;
        let mut device = DeviceSynchronousProxy::new(channel);
        Ok(device.destroy()?)
    }

    // Bind the bt-hci-emulator driver and obtain the HciEmulator protocol channel.
    async fn bind(&self) -> Result<HciEmulatorProxy, Error> {
        let channel = fdio::clone_channel(&self.0)?;
        let mut controller = ControllerSynchronousProxy::new(channel);

        // Create a watcher for the emulator device before binding the driver so that we can watch
        // for addition events.
        let status = controller.bind(EMULATOR_DRIVER_PATH, zx::Time::INFINITE)?;
        zx::Status::ok(status)?;

        // Wait until a bt-emulator device gets published under our test device.
        let topo_path = PathBuf::from(fdio::device_get_topo_path(&self.0)?);
        let mut watcher = DeviceWatcher::new(EMULATOR_DEVICE_DIR, watch_timeout())?;
        let emulator_dev = await!(watcher.watch_new(&topo_path, WatchFilter::AddedOrExisting))?;

        // Connect to the bt-emulator device.
        let channel = fdio::clone_channel(emulator_dev.file())?;
        let emulator = EmulatorProxy::new(fasync::Channel::from_channel(channel)?);

        // Open a HciEmulator protocol channel.
        let (proxy, remote) = zx::Channel::create()?;
        emulator.open(remote)?;
        Ok(HciEmulatorProxy::new(fasync::Channel::from_channel(proxy)?))
    }
}

impl Drop for TestDevice {
    fn drop(&mut self) {
        if let Err(e) = self.destroy() {
            fx_log_err!("error while destroying test device: {:?}", e);
        }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, crate::constants::HCI_DEVICE_DIR, fidl_fuchsia_bluetooth_test::EmulatorError};

    fn default_settings() -> EmulatorSettings {
        EmulatorSettings {
            address: None,
            hci_config: None,
            extended_advertising: None,
            acl_buffer_settings: None,
            le_acl_buffer_settings: None,
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_publish_lifecycle() {
        // We use these watchers to verify the addition and removal of these devices as tied to the
        // lifetime of the Emulator instance we create below.
        let mut hci_watcher: DeviceWatcher;
        let mut emul_watcher: DeviceWatcher;
        let mut hci_dev: DeviceFile;
        let mut emul_dev: DeviceFile;

        {
            let fake_dev =
                await!(Emulator::create("publish-test-0")).expect("Failed to construct Emulator");
            let topo_path = fdio::device_get_topo_path(&fake_dev.dev.0)
                .expect("Failed to obtain topological path for Emulator");
            let topo_path = PathBuf::from(topo_path);

            // A bt-emulator device should already exist by now.
            emul_watcher = DeviceWatcher::new(EMULATOR_DEVICE_DIR, watch_timeout())
                .expect("Failed to create bt-emulator device watcher");
            emul_dev = await!(emul_watcher.watch_existing(&topo_path))
                .expect("Expected bt-emulator device to have been published");

            // Send a publish message to the device. This call should succeed and result in a new
            // bt-hci device. (Note: it is important for `hci_watcher` to get constructed here since
            // our expectation is based on the `ADD_FILE` event).
            hci_watcher = DeviceWatcher::new(HCI_DEVICE_DIR, watch_timeout())
                .expect("Failed to create bt-hci device watcher");
            let _ = await!(fake_dev.publish(default_settings()))
                .expect("Failed to send Publish message to emulator device");
            hci_dev = await!(hci_watcher.watch_new(&topo_path, WatchFilter::AddedOnly))
                .expect("Expected a new bt-hci device");

            // Once a device is published, it should not be possible to publish again while the
            // HciEmulator channel is open.
            let result = await!(fake_dev.emulator().publish(default_settings()))
                .expect("Failed to send second Publish message to emulator device");
            assert_eq!(Err(EmulatorError::HciAlreadyPublished), result);
        }

        // Both devices should be destroyed when `fake_dev` gets dropped.
        let _ = await!(hci_watcher.watch_removed(hci_dev.path()))
            .expect("Expected bt-hci device to get removed");
        let _ = await!(emul_watcher.watch_removed(emul_dev.path()))
            .expect("Expected bt-emulator device to get removed");
    }
}
