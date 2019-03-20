// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::utils;
use failure::{bail, Error};

extern crate toml;

#[allow(dead_code)]
#[derive(Deserialize, Debug)]
pub struct SkuTable {
    pub version: String,
    pub sku: Vec<SkuInfo>,
}

#[derive(Deserialize, Debug)]
pub struct SkuInfo {
    pub name: String,
    pub wlan_country_code: String,     // ISO alpha-2
    pub eligible_country: Vec<String>, // ISO alpha-2
}

pub fn read_sku() -> Result<String, Error> {
    const FILENAME: &str = "./data/sku.txt";
    let contents = match utils::load_file(FILENAME) {
        Err(e) => {
            bail!("{} in reading {}", e, FILENAME);
        }
        Ok(c) => c,
    };
    Ok(contents.trim().to_string().to_lowercase())
}

pub fn get_sku_info(sku_name: String) -> Result<SkuInfo, Error> {
    const FILENAME: &str = "./data/sku_countries.toml";
    let contents = utils::load_file(FILENAME)?;

    let sku_table: SkuTable = toml::from_str(contents.as_str())?;
    for elem in sku_table.sku {
        if sku_name == elem.name {
            return Ok(elem);
        }
    }

    bail!("SKU name '{}' was not found from the file {}", sku_name, FILENAME)
}
