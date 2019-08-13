// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::Error;
use fidl::encoding::Decodable;
use fidl_fuchsia_ledger_cloud::{self as cloud, Status};
use fuchsia_zircon::Vmo;
use std::convert::{TryFrom, TryInto};

use crate::state::{CloudError, Commit, CommitId, Fingerprint, ObjectId, PageId, Token};

impl From<Vec<u8>> for Fingerprint {
    fn from(bytes: Vec<u8>) -> Fingerprint {
        Fingerprint(bytes)
    }
}

impl From<Vec<u8>> for ObjectId {
    fn from(obj_id: Vec<u8>) -> ObjectId {
        ObjectId(obj_id)
    }
}

impl PageId {
    pub fn from(app_id: Vec<u8>, page_id: Vec<u8>) -> PageId {
        PageId(app_id, page_id)
    }
}

/// Reads bytes from a fidl buffer into `result`.
pub fn read_buffer(buffer: &fidl_fuchsia_mem::Buffer, output: &mut Vec<u8>) -> Result<(), Error> {
    let size = buffer.size;
    let vmo = &buffer.vmo;
    output.resize(size as usize, 0);
    vmo.read(output.as_mut_slice(), 0)?;
    Ok(())
}

/// Writes bytes to a fidl buffer.
pub fn write_buffer(data: &[u8]) -> Result<fidl_fuchsia_mem::Buffer, Error> {
    let size = data.len() as u64;
    let vmo = Vmo::create(size)?;
    vmo.write(data, 0)?;
    Ok(fidl_fuchsia_mem::Buffer { vmo, size })
}

impl TryFrom<Option<Box<fidl_fuchsia_ledger_cloud::PositionToken>>> for Token {
    type Error = ();
    fn try_from(
        val: Option<Box<fidl_fuchsia_ledger_cloud::PositionToken>>,
    ) -> Result<Self, Self::Error> {
        match val {
            None => Ok(Token(0)),
            Some(t) => match t.opaque_id.as_slice().try_into().map(usize::from_le_bytes) {
                Ok(v) => Ok(Token(v)),
                Err(_) => Err(()),
            },
        }
    }
}

impl Into<fidl_fuchsia_ledger_cloud::PositionToken> for Token {
    fn into(self: Token) -> fidl_fuchsia_ledger_cloud::PositionToken {
        fidl_fuchsia_ledger_cloud::PositionToken {
            opaque_id: Vec::from(&self.0.to_le_bytes() as &[u8]),
        }
    }
}

impl From<CloudError> for Status {
    fn from(e: CloudError) -> Self {
        match e {
            CloudError::ObjectNotFound(_) | CloudError::FingerprintNotFound(_) => Status::NotFound,
            CloudError::InvalidToken => Status::ArgumentError,
            CloudError::ParseError => Status::ParseError,
        }
    }
}

impl CloudError {
    /// Converts a Result<(), CloudError> to a Ledger status.
    pub fn status(res: Result<(), Self>) -> Status {
        match res {
            Ok(()) => Status::Ok,
            Err(s) => Status::from(s),
        }
    }
}

impl TryFrom<cloud::Commit> for Commit {
    type Error = CloudError;

    fn try_from(commit: cloud::Commit) -> Result<Commit, CloudError> {
        Ok(Commit {
            id: CommitId(commit.id.ok_or(CloudError::ParseError)?),
            data: commit.data.ok_or(CloudError::ParseError)?,
        })
    }
}

impl From<&Commit> for cloud::Commit {
    fn from(commit: &Commit) -> cloud::Commit {
        cloud::Commit {
            id: Some(commit.id.0.clone()),
            data: Some(commit.data.clone()),
            ..cloud::Commit::new_empty()
        }
    }
}

/// Converts from and to a buffer with serialized cloud::Commits.
impl Commit {
    pub fn serialize_vec(commits: Vec<&Commit>) -> fidl_fuchsia_mem::Buffer {
        let mut serialized =
            cloud::Commits { commits: commits.into_iter().map(cloud::Commit::from).collect() };
        fidl::encoding::with_tls_encoded(&mut serialized, |data, _handles| {
            write_buffer(data.as_slice())
        })
        .expect("Failed to write FIDL-encoded commit data to buffer")
    }

    pub fn deserialize_vec(buf: &fidl_fuchsia_mem::Buffer) -> Result<Vec<Commit>, CloudError> {
        fidl::encoding::with_tls_coding_bufs(|data, _handles| {
            read_buffer(buf, data).map_err(|_| CloudError::ParseError)?;
            let mut serialized_commits = cloud::Commits { commits: vec![] };
            fidl::encoding::Decoder::decode_into(&data, &mut [], &mut serialized_commits)
                .map_err(|_| CloudError::ParseError)?;
            serialized_commits.commits.into_iter().map(Self::try_from).collect()
        })
    }
}
