// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use cm_json::Error;
use serde_json::{json, Value};
use std::fs;
use std::io::{Read, Write};
use std::path::PathBuf;

/// read in the provided list of json files, merge them, and pretty-print the merged result to
/// stdout if output is None or to the provided path if output is Some. JSON objects are merged
/// recursively, and if two blobs set the same key an error is returned. JSON arrays are appended
/// together, with duplicate items being removed.
pub fn merge(files: Vec<PathBuf>, output: Option<PathBuf>) -> Result<(), Error> {
    if files.is_empty() {
        return Err(Error::invalid_args(format!("no files provided")));
    }
    let mut res = json!({});
    for filename in files {
        let mut buffer = String::new();
        fs::File::open(&filename)?.read_to_string(&mut buffer)?;

        let v: Value = serde_json::from_str(&buffer)
            .map_err(|e| Error::parse(format!("Couldn't read input as JSON: {}", e)))?;

        merge_json(&mut res, &v)
            .map_err(|e| Error::parse(format!("Multiple manifests set the same key: {}", e)))?;
    }
    if let Some(output_path) = output {
        fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(output_path)?
            .write_all(format!("{:#}", res).as_bytes())?;
    } else {
        println!("{:#}", res);
    }
    Ok(())
}

fn merge_json(mut res: &mut Value, from: &Value) -> Result<(), String> {
    match (&mut res, &from) {
        (Value::Object(res_map), Value::Object(from_map)) => {
            for (k, v) in from_map {
                if !res_map.contains_key(k) {
                    res_map.insert(k.clone(), v.clone());
                } else {
                    merge_json(&mut res_map[k], v).map_err(|e| {
                        if e == "" {
                            format!("{}", k)
                        } else {
                            format!("{}.{}", k, e)
                        }
                    })?;
                }
            }
        }
        (Value::Array(res_arr), Value::Array(from_arr)) => {
            for item in from_arr {
                if !res_arr.contains(&item) {
                    res_arr.push(item.clone())
                }
            }
        }
        _ => return Err(format!("")),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::fs::File;
    use std::io::Write;
    use tempfile::TempDir;

    #[test]
    fn test_merge_json() {
        let tests = vec![
            // Valid merges
            (vec![json!({}), json!({})], Some(json!({}))),
            (vec![json!({"foo": 1}), json!({})], Some(json!({"foo": 1}))),
            (vec![json!({}), json!({"foo": 1})], Some(json!({"foo": 1}))),
            (vec![json!({"foo": 1}), json!({"bar": 2})], Some(json!({"foo": 1, "bar": 2}))),
            (vec![json!({"foo": [1]}), json!({"bar": [2]})], Some(json!({"foo": [1], "bar": [2]}))),
            (
                vec![json!({"foo": {"bar": 1}}), json!({"foo": {"baz": 2}})],
                Some(json!({"foo": {"bar": 1, "baz": 2}})),
            ),
            (vec![json!({"foo": [1]}), json!({"foo": [2]})], Some(json!({"foo": [1,2]}))),
            (vec![json!({"foo": [1]}), json!({"foo": [1]})], Some(json!({"foo": [1]}))),
            (
                vec![json!({"foo": [{"bar": 1}]}), json!({"foo": [{"bar": 1}]})],
                Some(json!({"foo": [{"bar": 1}]})),
            ),
            (
                vec![json!({"foo": [{"bar": 1}]}), json!({"foo": [{"bar": 2}]})],
                Some(json!({"foo": [{"bar": 1},{"bar": 2}]})),
            ),
            // merges that should fail
            (vec![json!({"foo": 1}), json!({"foo": 1})], None),
            (vec![json!({"foo": 1}), json!({"foo": 2})], None),
            (vec![json!({"foo": {"bar": 1}}), json!({"foo": 2})], None),
            (vec![json!({"foo": [1]}), json!({"foo": 1})], None),
            (vec![json!({"foo": [1]}), json!({"foo": {"bar": 1}})], None),
        ];

        for (vec_to_merge, expected_results) in tests {
            let tmp_dir = TempDir::new().unwrap();

            let mut counter = 0;
            let mut filenames = vec![];
            for json_val in vec_to_merge {
                let tmp_file_path = tmp_dir.path().join(format!("{}.json", counter));
                counter += 1;
                File::create(&tmp_file_path)
                    .unwrap()
                    .write_all(format!("{}", json_val).as_bytes())
                    .unwrap();
                filenames.push(tmp_file_path);
            }

            let output_file_path = tmp_dir.path().join("output.json");

            let result = merge(filenames, Some(output_file_path.clone()));

            if result.is_ok() != expected_results.is_some() {
                println!("{:?}", result);
            }
            assert_eq!(result.is_ok(), expected_results.is_some());

            if let Some(expected_json) = expected_results {
                let mut buffer = String::new();
                File::open(&output_file_path).unwrap().read_to_string(&mut buffer).unwrap();
                assert_eq!(buffer, format!("{:#}", expected_json));
            }
        }
    }

    #[test]
    fn test_merge_invalid_json_fails() {
        let tmp_dir = TempDir::new().unwrap();

        let input = vec![
            (tmp_dir.path().join("1.json"), "{\"foo\": 1}"),
            (tmp_dir.path().join("1.json"), "{\"foo\": 1,}"),
        ];
        let mut filenames = vec![];
        for (fname, contents) in &input {
            File::create(fname).unwrap().write_all(contents.as_bytes()).unwrap();
            filenames.push(fname.clone());
        }

        let result = merge(filenames, None);
        assert!(result.is_err());
    }
}
