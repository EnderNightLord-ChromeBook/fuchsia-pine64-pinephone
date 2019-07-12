// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Handles negotiating buffer sets with the codec server and sysmem.

use crate::{buffer_collection_constraints::*, Result};
use failure::{self, Fail, ResultExt};
use fidl::endpoints::{create_endpoints, ClientEnd};
use fidl_fuchsia_media::*;
use fidl_fuchsia_sysmem::*;
use fidl_table_validation::{ValidFidlTable, Validate};
use fuchsia_component::client;
use fuchsia_zircon as zx;
use std::{
    convert::TryFrom,
    fmt,
    iter::{IntoIterator, StepBy},
    ops::RangeFrom,
};

#[derive(Debug)]
pub enum Error {
    ReclaimClientTokenChannel,
    ServerOmittedBufferVmo,
    PacketReferencesInvalidBuffer,
    VmoReadFail(zx::Status),
}

impl fmt::Display for Error {
    fn fmt(&self, w: &mut fmt::Formatter) -> fmt::Result {
        fmt::Debug::fmt(&self, w)
    }
}

impl Fail for Error {}

#[allow(unused)]
#[derive(ValidFidlTable, Copy, Clone, Debug, PartialEq)]
#[fidl_table_src(StreamBufferSettings)]
pub struct ValidStreamBufferSettings {
    buffer_lifetime_ordinal: u64,
    buffer_constraints_version_ordinal: u64,
    packet_count_for_server: u32,
    packet_count_for_client: u32,
    per_packet_buffer_bytes: u32,
    #[fidl_field_type(default = false)]
    single_buffer_mode: bool,
}

#[allow(unused)]
#[derive(ValidFidlTable)]
#[fidl_table_src(StreamBufferConstraints)]
#[fidl_table_validator(StreamBufferConstraintsValidator)]
pub struct ValidStreamBufferConstraints {
    buffer_constraints_version_ordinal: u64,
    default_settings: ValidStreamBufferSettings,
    per_packet_buffer_bytes_min: u32,
    per_packet_buffer_bytes_recommended: u32,
    per_packet_buffer_bytes_max: u32,
    packet_count_for_server_min: u32,
    packet_count_for_server_recommended: u32,
    packet_count_for_server_recommended_max: u32,
    packet_count_for_server_max: u32,
    packet_count_for_client_min: u32,
    packet_count_for_client_max: u32,
    single_buffer_mode_allowed: bool,
    #[fidl_field_type(default = false)]
    is_physically_contiguous_required: bool,
    #[fidl_field_type(optional)]
    very_temp_kludge_bti_handle: Option<zx::Handle>,
}

#[derive(ValidFidlTable)]
#[fidl_table_src(StreamOutputConstraints)]
pub struct ValidStreamOutputConstraints {
    pub stream_lifetime_ordinal: u64,
    pub buffer_constraints_action_required: bool,
    pub buffer_constraints: ValidStreamBufferConstraints,
}

pub struct StreamBufferConstraintsValidator;

#[derive(Debug)]
pub enum StreamBufferConstraintsError {
    VersionOrdinalZero,
    SingleBufferMode,
    ConstraintsNoBtiHandleForPhysicalBuffers,
}

impl Validate<ValidStreamBufferConstraints> for StreamBufferConstraintsValidator {
    type Error = StreamBufferConstraintsError;
    fn validate(candidate: &ValidStreamBufferConstraints) -> std::result::Result<(), Self::Error> {
        if candidate.buffer_constraints_version_ordinal == 0 {
            // An ordinal of 0 in StreamBufferConstraints is not allowed.
            return Err(StreamBufferConstraintsError::VersionOrdinalZero);
        }

        if candidate.default_settings.single_buffer_mode {
            // StreamBufferConstraints should never suggest single buffer mode.
            return Err(StreamBufferConstraintsError::SingleBufferMode);
        }

        if candidate.is_physically_contiguous_required
            && candidate
                .very_temp_kludge_bti_handle
                .as_ref()
                .map(|h| h.is_invalid())
                .unwrap_or(true)
        {
            // The bti handle must be provided if the buffers need to be physically contiguous.
            return Err(StreamBufferConstraintsError::ConstraintsNoBtiHandleForPhysicalBuffers);
        }

        Ok(())
    }
}

/// The pattern to use when advancing ordinals.
#[derive(Debug, Clone, Copy)]
pub enum OrdinalPattern {
    /// Odd ordinal pattern starts at 1 and moves in increments of 2: [1,3,5..]
    Odd,
    /// All ordinal pattern starts at 1 and moves in increments of 1: [1,2,3..]
    All,
}

impl IntoIterator for OrdinalPattern {
    type Item = u64;
    type IntoIter = StepBy<RangeFrom<Self::Item>>;
    fn into_iter(self) -> Self::IntoIter {
        let (start, step) = match self {
            OrdinalPattern::Odd => (1, 2),
            OrdinalPattern::All => (1, 1),
        };
        (start..).step_by(step)
    }
}

pub fn get_ordinal(pattern: &mut <OrdinalPattern as IntoIterator>::IntoIter) -> u64 {
    pattern.next().expect("Getting next item in infinite pattern")
}

pub enum BufferSetType {
    Input,
    Output,
}

pub struct BufferSetFactory;

impl BufferSetFactory {
    pub async fn buffer_set(
        buffer_lifetime_ordinal: u64,
        constraints: ValidStreamBufferConstraints,
        codec: &mut StreamProcessorProxy,
        buffer_set_type: BufferSetType,
        buffer_collection_constraints: Option<BufferCollectionConstraints>,
    ) -> Result<BufferSet> {
        let (collection_client, settings) = await!(Self::settings(
            buffer_lifetime_ordinal,
            constraints,
            buffer_collection_constraints
        ))?;

        vlog!(2, "Got settings; waiting for buffers.");

        match buffer_set_type {
            BufferSetType::Input => codec
                .set_input_buffer_partial_settings(settings)
                .context("Sending input partial settings to codec")?,
            BufferSetType::Output => codec
                .set_output_buffer_partial_settings(settings)
                .context("Sending output partial settings to codec")?,
        };

        let (status, collection_info) = await!(collection_client.wait_for_buffers_allocated())
            .context("Waiting for buffers")?;
        vlog!(2, "Sysmem responded: {:?}", status);
        let collection_info = zx::Status::ok(status).map(|_| collection_info)?;

        if let BufferSetType::Output = buffer_set_type {
            vlog!(2, "Completing settings for output.");
            codec.complete_output_buffer_partial_settings(buffer_lifetime_ordinal)?;
        }

        //collection_client.close()?;

        vlog!(
            2,
            "Got {} buffers of size {:?}",
            collection_info.buffer_count,
            collection_info.settings.buffer_settings.size_bytes
        );
        vlog!(3, "Buffer collection is: {:#?}", collection_info.settings);
        for (i, buffer) in collection_info.buffers.iter().enumerate() {
            // We enumerate beyond collection_info.buffer_count just for debugging
            // purposes at this log level.
            vlog!(3, "Buffer {} is : {:#?}", i, buffer);
        }

        Ok(BufferSet::try_from(BufferSetSpec {
            proxy: collection_client,
            buffer_lifetime_ordinal,
            collection_info,
        })?)
    }

    async fn settings(
        buffer_lifetime_ordinal: u64,
        constraints: ValidStreamBufferConstraints,
        buffer_collection_constraints: Option<BufferCollectionConstraints>,
    ) -> Result<(BufferCollectionProxy, StreamBufferPartialSettings)> {
        let (client_token, client_token_request) =
            create_endpoints::<BufferCollectionTokenMarker>()?;
        let (codec_token, codec_token_request) = create_endpoints::<BufferCollectionTokenMarker>()?;
        let client_token = client_token.into_proxy()?;

        let sysmem_client =
            client::connect_to_service::<AllocatorMarker>().context("Connecting to sysmem")?;

        sysmem_client
            .allocate_shared_collection(client_token_request)
            .context("Allocating shared collection")?;
        client_token.duplicate(std::u32::MAX, codec_token_request)?;

        let (collection_client, collection_request) = create_endpoints::<BufferCollectionMarker>()?;
        sysmem_client.bind_shared_collection(
            ClientEnd::new(
                client_token
                    .into_channel()
                    .map_err(|_| Error::ReclaimClientTokenChannel)?
                    .into_zx_channel(),
            ),
            collection_request,
        )?;
        let collection_client = collection_client.into_proxy()?;
        await!(collection_client.sync()).context("Syncing codec_token_request with sysmem")?;

        let mut collection_constraints =
            buffer_collection_constraints.unwrap_or(BUFFER_COLLECTION_CONSTRAINTS_DEFAULT);
        assert_eq!(
            collection_constraints.min_buffer_count_for_camping, 0,
            "Codecs assert that buffer_count == packet count, so we can't change this yet."
        );
        collection_constraints.min_buffer_count_for_camping =
            constraints.default_settings.packet_count_for_client;

        vlog!(3, "Our buffer collection constraints are: {:#?}", collection_constraints);

        // By design we must say true even if all our fields are left at
        // default, or sysmem will not give us buffer handles.
        let has_constraints = true;
        collection_client
            .set_constraints(has_constraints, &mut collection_constraints)
            .context("Sending buffer constraints to sysmem")?;

        Ok((
            collection_client,
            StreamBufferPartialSettings {
                buffer_lifetime_ordinal: Some(buffer_lifetime_ordinal),
                buffer_constraints_version_ordinal: Some(
                    constraints.buffer_constraints_version_ordinal,
                ),
                single_buffer_mode: Some(constraints.default_settings.single_buffer_mode),
                packet_count_for_server: Some(constraints.default_settings.packet_count_for_server),
                packet_count_for_client: Some(constraints.default_settings.packet_count_for_client),
                sysmem_token: Some(codec_token),
            },
        ))
    }
}

#[derive(ValidFidlTable, Clone, Copy, Debug, PartialEq)]
#[fidl_table_src(PacketHeader)]
pub struct ValidPacketHeader {
    pub buffer_lifetime_ordinal: u64,
    pub packet_index: u32,
}

#[derive(ValidFidlTable, Clone, Copy, Debug, PartialEq)]
#[fidl_table_src(Packet)]
pub struct ValidPacket {
    pub header: ValidPacketHeader,
    pub buffer_index: u32,
    pub stream_lifetime_ordinal: u64,
    pub start_offset: u32,
    pub valid_length_bytes: u32,
    #[fidl_field_type(optional)]
    pub timestamp_ish: Option<u64>,
    #[fidl_field_type(default = false)]
    pub start_access_unit: bool,
    #[fidl_field_type(default = false)]
    pub known_end_access_unit: bool,
}

struct BufferSetSpec {
    proxy: BufferCollectionProxy,
    buffer_lifetime_ordinal: u64,
    collection_info: BufferCollectionInfo2,
}

#[derive(Debug, PartialEq)]
pub struct Buffer {
    pub data: zx::Vmo,
    pub start: u64,
    pub size: u64,
}

#[derive(Debug)]
pub struct BufferSet {
    pub proxy: BufferCollectionProxy,
    pub buffers: Vec<Buffer>,
    pub buffer_lifetime_ordinal: u64,
    pub buffer_size: usize,
}

impl TryFrom<BufferSetSpec> for BufferSet {
    type Error = failure::Error;
    fn try_from(mut src: BufferSetSpec) -> std::result::Result<Self, Self::Error> {
        let mut buffers = vec![];
        for (i, buffer) in src.collection_info.buffers
            [0..(src.collection_info.buffer_count as usize)]
            .iter_mut()
            .enumerate()
        {
            buffers.push(Buffer {
                data: buffer.vmo.take().ok_or(Error::ServerOmittedBufferVmo).context(format!(
                    "Trying to ingest {}th buffer of {}: {:#?}",
                    i, src.collection_info.buffer_count, buffer
                ))?,
                start: buffer.vmo_usable_start,
                size: src.collection_info.settings.buffer_settings.size_bytes as u64,
            });
        }

        Ok(Self {
            proxy: src.proxy,
            buffers,
            buffer_lifetime_ordinal: src.buffer_lifetime_ordinal,
            buffer_size: src.collection_info.settings.buffer_settings.size_bytes as usize,
        })
    }
}

impl BufferSet {
    pub fn read_packet(&self, packet: &ValidPacket) -> Result<Vec<u8>> {
        let buffer = self
            .buffers
            .get(packet.buffer_index as usize)
            .ok_or(Error::PacketReferencesInvalidBuffer)?;
        let mut dest = vec![0; packet.valid_length_bytes as usize];
        buffer.data.read(&mut dest, packet.start_offset as u64).map_err(Error::VmoReadFail)?;
        Ok(dest)
    }
}
