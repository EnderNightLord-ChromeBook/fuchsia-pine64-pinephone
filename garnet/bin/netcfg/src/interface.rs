// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{self, ResultExt},
    fidl_fuchsia_hardware_ethernet_ext::MacAddress,
    serde_derive::{Deserialize, Serialize},
    std::{fs, io, path},
};

#[derive(PartialEq, Eq, Serialize, Deserialize, Debug)]
enum PersistentIdentifier {
    MacAddress(MacAddress),
    TopologicalPath(String),
}

#[derive(Serialize, Deserialize, Debug)]
struct Config {
    names: Vec<(PersistentIdentifier, String)>,
}

impl Config {
    fn load<R: io::Read>(reader: R) -> Result<Self, failure::Error> {
        serde_json::from_reader(reader).map_err(Into::into)
    }

    fn generate_identifier(
        &self,
        topological_path: String,
        mac_address: MacAddress,
    ) -> PersistentIdentifier {
        if topological_path.contains("/pci/") {
            if topological_path.contains("/usb/") {
                PersistentIdentifier::MacAddress(mac_address)
            } else {
                PersistentIdentifier::TopologicalPath(topological_path)
            }
        } else {
            if topological_path.contains("/platform/") {
                PersistentIdentifier::TopologicalPath(topological_path)
            } else {
                PersistentIdentifier::MacAddress(mac_address)
            }
        }
    }

    fn lookup_by_identifier(&self, persistent_id: &PersistentIdentifier) -> Option<usize> {
        self.names.iter().enumerate().find_map(
            |(i, (key, _value))| {
                if key == persistent_id {
                    Some(i)
                } else {
                    None
                }
            },
        )
    }

    // We use MAC addresses to identify USB devices; USB devices are those devices whose
    // topological path contains "/usb/". We use topological paths to identify on-board
    // devices; on-board devices are those devices whose topological path does not
    // contain "/usb". Topological paths of
    // both device types are expected to
    // contain "/pci"; devices whose topological path does not contain "/pci/" are
    // identified by their MAC address.
    //
    // At the time of writing, typical topological paths appear similar to:
    //
    // PCI:
    // "/dev/sys/pci/02:00.0/intel-ethernet/ethernet"
    //
    // USB:
    // "/dev/sys/pci/00:14.0/xhci/usb/007/ifc-000/<snip>/wlan/wlan-ethernet/ethernet"
    // 00:14:0 following "/pci/" represents BDF (Bus Device Function)
    //
    // SDIO
    // "/dev/sys/platform/05:00:6/aml-sd-emmc/sdio/broadcom-wlanphy/wlanphy"
    // 05:00:6 following "platform" represents
    // vid(vendor id):pid(product id):did(device id) and are defined in each board file
    //
    // Ethernet Jack for VIM2
    // "/dev/sys/platform/04:02:7/aml-ethernet/Designware MAC/ethernet"
    // Though it is not a sdio device, it has the vid:pid:did info following "/platform/",
    // it's handled the same way as a sdio device.
    fn generate_name_from_mac(
        &self,
        octets: &[u8; 6],
        wlan: bool,
    ) -> Result<String, failure::Error> {
        let prefix = match wlan {
            true => "wlanx",
            false => "ethx",
        };
        let last_byte = octets[octets.len() - 1];
        for i in 0u8..255u8 {
            let candidate = ((last_byte as u16 + i as u16) % 256 as u16) as u8;
            if self.names.iter().any(|(_key, name)| {
                name.starts_with(prefix)
                    && u8::from_str_radix(&name[prefix.len()..], 16) == Ok(candidate)
            }) {
                continue; // if the candidate is used, try next one
            } else {
                return Ok(format!("{}{:x}", prefix, candidate));
            }
        }
        Err(failure::format_err!(
            "could not find unique name for mac={}, wlan={}",
            MacAddress { octets: *octets },
            wlan
        ))
    }

    fn generate_name_from_topological_path(
        &self,
        topological_path: &str,
        wlan: bool,
    ) -> Result<String, failure::Error> {
        let (prefix, pat) = if topological_path.contains("/pci/") {
            (if wlan { "wlanp" } else { "ethp" }, "/pci/")
        } else {
            (if wlan { "wlans" } else { "eths" }, "/platform/")
        };

        let index = topological_path.find(pat).ok_or(failure::format_err!(
            "unexpected topological path {}: {} is not found",
            topological_path,
            pat
        ))?;
        let topological_path = &topological_path[index + pat.len()..];
        let index = topological_path.find('/').ok_or(failure::format_err!(
            "unexpected topological path suffix {}: '/' is not found after {}",
            topological_path,
            pat
        ))?;
        let mut name = String::from(prefix);
        for digit in topological_path[..index]
            .trim_end_matches(|c: char| !c.is_digit(16) || c == '0')
            .chars()
            .filter(|c| c.is_digit(16))
        {
            name.push(digit);
        }
        Ok(name)
    }

    fn generate_name(
        &self,
        persistent_id: &PersistentIdentifier,
        wlan: bool,
    ) -> Result<String, failure::Error> {
        match persistent_id {
            PersistentIdentifier::MacAddress(mac_addr) => {
                self.generate_name_from_mac(&mac_addr.octets, wlan)
            }
            PersistentIdentifier::TopologicalPath(ref topological_path) => {
                self.generate_name_from_topological_path(&topological_path, wlan)
            }
        }
    }
}

#[derive(Debug)]
pub struct FileBackedConfig<'a> {
    path: &'a path::Path,
    config: Config,
}

impl<'a> FileBackedConfig<'a> {
    pub fn load<P: AsRef<path::Path>>(path: &'a P) -> Result<Self, failure::Error> {
        let path = path.as_ref();
        let config = match fs::File::open(path) {
            Ok(file) => Config::load(file)
                .with_context(|_| format!("could not deserialize config file {}", path.display())),
            Err(error) => {
                if error.kind() == io::ErrorKind::NotFound {
                    Ok(Config { names: vec![] })
                } else {
                    Err(error)
                        .with_context(|_| format!("could not open config file {}", path.display()))
                }
            }
        }?;
        Ok(Self { path, config })
    }

    pub fn store(&self) -> Result<(), failure::Error> {
        let Self { path, config } = self;
        let temp_file_path = match path.file_name() {
            None => Err(failure::format_err!("unexpected non-file path {}", path.display())),
            Some(file_name) => {
                let mut file_name = file_name.to_os_string();
                file_name.push(".tmp");
                Ok(path.with_file_name(file_name))
            }
        }?;
        {
            let temp_file = fs::File::create(&temp_file_path).with_context(|_| {
                format!("could not create temporary file {}", temp_file_path.display())
            })?;
            serde_json::to_writer_pretty(temp_file, &config).with_context(|_| {
                format!(
                    "could not serialize config into temporary file {}",
                    temp_file_path.display()
                )
            })?;
        }

        fs::rename(&temp_file_path, path).with_context(|_| {
            format!(
                "could not rename temporary file {} to {}",
                temp_file_path.display(),
                path.display()
            )
        })?;
        Ok(())
    }

    pub fn get_stable_name(
        &mut self,
        topological_path: String,
        mac_address: MacAddress,
        wlan: bool,
    ) -> Result<&str, failure::Error> {
        let persistent_id = self.config.generate_identifier(topological_path, mac_address);

        let index = if let Some(index) = self.config.lookup_by_identifier(&persistent_id) {
            index
        } else {
            let name = self.config.generate_name(&persistent_id, wlan)?;
            self.config.names.push((persistent_id, name));
            self.store()?;
            self.config.names.len() - 1
        };
        let (_key, value) = &self.config.names[index];
        Ok(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[derive(Clone)]
    struct TestCase {
        topological_path: String,
        mac: [u8; 6],
        wlan: bool,
        want_name: &'static str,
    }

    #[test]
    fn test_generate_name() {
        let test_cases = vec![
            // usb interfaces
            TestCase {
                topological_path: String::from(
                    "@/dev/sys/pci/00:14.0/xhci/usb/004/004/ifc-000/ax88179/ethernet",
                ),
                mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
                wlan: true,
                want_name: "wlanx1",
            },
            TestCase {
                topological_path: String::from(
                    "@/dev/sys/pci/00:15.0/xhci/usb/004/004/ifc-000/ax88179/ethernet",
                ),
                mac: [0x02, 0x02, 0x02, 0x02, 0x02, 0x02],
                wlan: false,
                want_name: "ethx2",
            },
            // pci intefaces
            TestCase {
                topological_path: String::from("@/dev/sys/pci/00:14.0/ethernet"),
                mac: [0x03, 0x03, 0x03, 0x03, 0x03, 0x03],
                wlan: true,
                want_name: "wlanp0014",
            },
            TestCase {
                topological_path: String::from("@/dev/sys/pci/00:15.0/ethernet"),
                mac: [0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
                wlan: false,
                want_name: "ethp0015",
            },
            // platform interfaces (ethernet jack and sdio devices)
            TestCase {
                topological_path: String::from(
                    "@/dev/sys/platform/05:00:6/aml-sd-emmc/sdio/broadcom-wlanphy/wlanphy",
                ),
                mac: [0x05, 0x05, 0x05, 0x05, 0x05, 0x05],
                wlan: true,
                want_name: "wlans05006",
            },
            TestCase {
                topological_path: String::from(
                    "@/dev/sys/platform/04:02:7/aml-ethernet/Designware MAC/ethernet",
                ),
                mac: [0x07, 0x07, 0x07, 0x07, 0x07, 0x07],
                wlan: false,
                want_name: "eths04027",
            },
            // unknown interfaces
            TestCase {
                topological_path: String::from("@/dev/sys/unknown"),
                mac: [0x08, 0x08, 0x08, 0x08, 0x08, 0x08],
                wlan: true,
                want_name: "wlanx8",
            },
            TestCase {
                topological_path: String::from("unknown"),
                mac: [0x09, 0x09, 0x09, 0x09, 0x09, 0x09],
                wlan: true,
                want_name: "wlanx9",
            },
        ];
        let config = Config { names: vec![] };
        for test in test_cases.into_iter() {
            let persistent_id =
                config.generate_identifier(test.topological_path, MacAddress { octets: test.mac });
            let name = config
                .generate_name(&persistent_id, test.wlan)
                .expect("failed to generate the name");
            assert_eq!(name, test.want_name);
        }
    }

    #[test]
    fn test_get_stable_name() {
        let test1 = TestCase {
            topological_path: String::from("@/dev/sys/pci/00:14.0/ethernet"),
            mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
            wlan: true,
            want_name: "wlanp0014",
        };
        let mut test2 = test1.clone();
        test2.mac[0] ^= 0xff;
        let test_cases = vec![test1, test2];

        let temp_dir = tempfile::tempdir_in("/data").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");

        // query an existing interface with the same topo path and a different mac address
        for (i, test) in test_cases.into_iter().enumerate() {
            let mut interface_config =
                FileBackedConfig::load(&path).expect("failed to load the interface config");
            assert_eq!(interface_config.config.names.len(), i);

            let name = interface_config
                .get_stable_name(test.topological_path, MacAddress { octets: test.mac }, test.wlan)
                .expect("failed to get the interface name");
            assert_eq!(name, test.want_name);
            assert_eq!(interface_config.config.names.len(), 1);
        }
    }

    #[test]
    fn test_get_usb_255() {
        let topo_usb =
            String::from("@/dev/sys/pci/00:14.0/xhci/usb/004/004/ifc-000/ax88179/ethernet");

        // test cases for 256 usb interfaces
        let mut config = Config { names: vec![] };
        for n in 0u8..255u8 {
            let octets = [n, 0x01, 0x01, 0x01, 0x01, 00];

            let persistent_id = config.generate_identifier(topo_usb.clone(), MacAddress { octets });

            if let Some(index) = config.lookup_by_identifier(&persistent_id) {
                assert_eq!(config.names[index].1, format!("{}{:x}", "wlanx", n));
            } else {
                let name = config
                    .generate_name(&persistent_id, true)
                    .expect("failed to generate the name");
                assert_eq!(name, format!("{}{:x}", "wlanx", n));
                config.names.push((persistent_id, name));
            }
        }
        let octets = [0x00, 0x00, 0x01, 0x01, 0x01, 00];
        let persistent_id = config.generate_identifier(topo_usb, MacAddress { octets });
        assert!(config.generate_name(&persistent_id, true).is_err());
    }

    #[test]
    fn test_load_malformed_file() {
        let temp_dir = tempfile::tempdir_in("/data").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");
        {
            let mut file = fs::File::create(&path).expect("failed to open file for writing");
            // Write invalid JSON and close the file
            file.write(b"{").expect("failed to write broken json into file");
        }
        assert_eq!(
            FileBackedConfig::load(&path)
                .unwrap_err()
                .find_root_cause()
                .downcast_ref::<serde_json::error::Error>()
                .unwrap()
                .classify(),
            serde_json::error::Category::Eof
        );
    }

    #[test]
    fn test_store_nonexistant_path() {
        let interface_config = FileBackedConfig::load(&"not/a/real/path")
            .expect("failed to load the interface config");
        assert_eq!(
            interface_config
                .store()
                .unwrap_err()
                .find_root_cause()
                .downcast_ref::<io::Error>()
                .unwrap()
                .kind(),
            io::ErrorKind::NotFound
        );
    }
}
