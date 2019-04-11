// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::io_packet::{IoPacket, IoPacketType},
    serde_derive::{Deserialize, Serialize},
    std::{io::Result, ops::Range, slice::Iter, sync::Arc, time::Instant},
};

/// The type of operations that can be performed using odu. Not all targets may
/// implement all the operations.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum OperationType {
    // Persist a buffer onto target that can be read at a later point in time.
    Write,

    // "Open" target for other IO operations. This is not implement at the
    // moment but is used to unit test some of the generate functionality.
    Open,

    //    Meta operations
    // Finish all outstanding operations and forward the command to next stage
    // of the pipeline before exiting gracefully.
    Exit,

    // Abort all outstanding operations and exit as soon as possible.
    Abort,
    //    Read,
    //    LSeek,
    Truncate,
    //    Close,
    //    FSync,
    //
    //    /// DirOps
    Create,
    //    Unlink,
    //    CreateDir,
    //    DeleteDir,
    //    ReadDir,
    //    OpenDir,
    //    Link, /// This is for hard links only. Symlinks are small files.
    //
    //    /// FsOps
    //    Mount,
    //    Unmount,
    //
}

/// IoPackets go through different stages in pipeline. These stages help track
// the IO and also are indicative of how loaded different parts of the app is.
#[derive(Serialize, Deserialize, Debug, Clone)]
pub enum PipelineStages {
    /// IoPacket is in generator stage
    Generate,

    /// IoPacket is in issuer stage
    Issue,

    /// IoPacket is in verifier stage
    Verify,
}

/// These functions makes better indexing and walking stages a bit better.
impl PipelineStages {
    pub const fn stage_count() -> usize {
        3 // number of entries in PipelineStages.
    }

    pub fn stage_number(&self) -> usize {
        match self {
            PipelineStages::Generate => 0,
            PipelineStages::Issue => 1,
            PipelineStages::Verify => 2,
        }
    }

    pub fn iterator() -> Iter<'static, PipelineStages> {
        static STAGES: [PipelineStages; PipelineStages::stage_count()] =
            [PipelineStages::Generate, PipelineStages::Issue, PipelineStages::Verify];
        STAGES.into_iter()
    }
}

pub type TargetType = Arc<Box<Target + Send + Sync>>;

/// Target trait. Each new target type should implement this trait. Currently
// only File blocking IO call are implemented. Some of these functions are no-ops
// as the work is still in progress.
pub trait Target {
    fn setup(&mut self, file_name: &String, range: Range<u64>) -> Result<()>;
    fn create_io_packet(
        &self,
        operation_type: OperationType,
        seq: u64,
        seed: u64,
        io_offset_range: Range<u64>,
        target: &TargetType, // &Arc<Box<Target + Send + Sync>>,
    ) -> IoPacketType;

    /// Returns target unique identifier.
    fn id(&self) -> u64;

    /// Returns a reference to a struct which contains all the valid operations
    /// for the instance of the target.
    fn supported_ops(&self) -> &TargetOps;

    /// issues an IO
    fn do_io(&self, io_packet: &mut IoPacket);

    /// Returns true if the issued IO is complete.
    fn is_complete(&self, io_packet: &IoPacket) -> bool;

    /// Returns true if verify needs an IO
    fn verify_needs_io(&self, io_packet: &IoPacket) -> bool;

    /// Generates parameters for verify IO packet.
    fn generate_verify_io(&self, io_packet: &mut IoPacket);

    /// Verifies "success" of an IO. Returns true if IO was successful.
    fn verify(&self, io_packet: &mut IoPacket, verify_packet: &IoPacket) -> bool;

    fn start_instant(&self) -> Instant;
}

#[derive(Clone)]
pub struct TargetOps {
    pub write: Option<OperationType>,
    pub truncate: Option<OperationType>,
    pub open: Option<OperationType>,
    pub create: Option<OperationType>,
    //    read: Option<OperationType>,
    //    lseek: Option<OperationType>,
    //    close: Option<OperationType>,
    //    fsync: Option<OperationType>,
    //
    //
    
    //    unlink: Option<OperationType>,
    //    createdir: Option<OperationType>,
    //    deletedir: Option<OperationType>,
    //    readdir: Option<OperationType>,
    //    opendir: Option<OperationType>,
    //    link: Option<OperationType>,
    //
    //    mount: Option<OperationType>,
    //    unmount: Option<OperationType>,
}
