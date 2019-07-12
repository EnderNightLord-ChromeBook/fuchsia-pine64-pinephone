// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{block::Block, block_type::BlockType, constants, utils, Inspector},
    failure::{self, bail, format_err, Error},
    fuchsia_zircon::Vmo,
    std::convert::TryFrom,
};

/// Enables to scan all the blocks in a given buffer.
pub struct Snapshot {
    /// The buffer read from an Inspect VMO.
    buffer: Vec<u8>,
}

/// A scanned block.
pub type ScannedBlock<'a> = Block<&'a [u8]>;

const SNAPSHOT_TRIES: u64 = 1024;

impl Snapshot {
    /// Returns an iterator that returns all the Blocks in the buffer.
    pub fn scan(&self) -> BlockIterator {
        return BlockIterator::from(self.buffer.as_ref());
    }

    /// Gets the block at the given |index|.
    pub fn get_block(&self, index: u32) -> Option<ScannedBlock> {
        if utils::offset_for_index(index) < self.buffer.len() {
            Some(Block::new(&self.buffer, index))
        } else {
            None
        }
    }

    fn try_from_with_callback<F>(vmo: &Vmo, mut first_read_callback: F) -> Result<Snapshot, Error>
    where
        F: FnMut() -> (),
    {
        let mut header_bytes: [u8; 16] = [0; 16];

        for _ in 0..SNAPSHOT_TRIES {
            vmo.read(&mut header_bytes, 0)?;

            let generation = header_generation_count(&header_bytes[..]);

            // Used for testing
            first_read_callback();

            match generation {
                None => {}
                Some(gen) => {
                    let size = vmo.get_size()?;
                    let mut buffer = vec![0u8; size as usize];
                    vmo.read(&mut buffer[..], 0)?;
                    if BlockIterator::from(&buffer[..16])
                        .find(|block| {
                            block.block_type() == BlockType::Header
                                && block.header_magic().unwrap() == constants::HEADER_MAGIC_NUMBER
                                && block.header_version().unwrap()
                                    == constants::HEADER_VERSION_NUMBER
                                && block.header_generation_count().unwrap() == gen
                        })
                        .is_some()
                    {
                        return Ok(Snapshot { buffer: buffer });
                    }
                }
            }
        }
        bail!("Failed to read snapshot from vmo");
    }

    // Used for snapshot tests.
    #[cfg(test)]
    pub fn build(bytes: &[u8]) -> Self {
        Snapshot { buffer: bytes.to_vec() }
    }
}

/// Reads the given 16 bytes as an Inspect Block Header and returns the
/// generation count if the header is valid: correct magic number, version number
/// and nobody is writing to it.
fn header_generation_count(bytes: &[u8]) -> Option<u64> {
    if bytes.len() < 16 {
        None
    } else {
        BlockIterator::from(&bytes[..])
            .find(|block| {
                block.block_type() == BlockType::Header
                    && block.header_magic().unwrap() == constants::HEADER_MAGIC_NUMBER
                    && block.header_version().unwrap() == constants::HEADER_VERSION_NUMBER
                    && !block.header_is_locked().unwrap()
            })
            .and_then(|block| block.header_generation_count().ok())
    }
}

/// Construct a snapshot from a byte array.
impl TryFrom<&[u8]> for Snapshot {
    type Error = failure::Error;

    fn try_from(bytes: &[u8]) -> Result<Self, Self::Error> {
        if header_generation_count(&bytes[..16]).is_some() {
            Ok(Snapshot { buffer: bytes.to_vec() })
        } else {
            bail!("expected block with at least a header");
        }
    }
}

/// Construct a snapshot from a byte vector.
impl TryFrom<Vec<u8>> for Snapshot {
    type Error = failure::Error;

    fn try_from(bytes: Vec<u8>) -> Result<Self, Self::Error> {
        if header_generation_count(&bytes[..16]).is_some() {
            Ok(Snapshot { buffer: bytes })
        } else {
            bail!("expected block with at least a header");
        }
    }
}
/// Construct a snapshot from a VMO.
impl TryFrom<&Vmo> for Snapshot {
    type Error = failure::Error;

    fn try_from(vmo: &Vmo) -> Result<Self, Self::Error> {
        Snapshot::try_from_with_callback(vmo, || {})
    }
}

impl TryFrom<&Inspector> for Snapshot {
    type Error = failure::Error;

    fn try_from(inspector: &Inspector) -> Result<Self, Self::Error> {
        inspector
            .vmo
            .as_ref()
            .ok_or(format_err!("Cannot read from no-op Inspector"))
            .and_then(|vmo| Snapshot::try_from(vmo))
    }
}

/// Iterates over a byte array containing Inspect API blocks and returns the
/// blocks in order.
pub struct BlockIterator<'h> {
    /// Current offset at which the iterator is reading.
    offset: usize,

    /// The bytes being read.
    container: &'h [u8],
}

impl<'a> From<&'a [u8]> for BlockIterator<'a> {
    fn from(container: &'a [u8]) -> Self {
        BlockIterator { offset: 0, container: container }
    }
}

impl<'h> Iterator for BlockIterator<'h> {
    type Item = Block<&'h [u8]>;

    fn next(&mut self) -> Option<Block<&'h [u8]>> {
        if self.offset >= self.container.len() {
            return None;
        }
        let index = utils::index_for_offset(self.offset);
        let block = Block::new(self.container.clone(), index);
        if self.container.len() - self.offset < utils::order_to_size(block.order()) {
            return None;
        }
        self.offset += utils::order_to_size(block.order());
        Some(block)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mapped_vmo::Mapping;
    use std::sync::Arc;

    #[test]
    fn scan() -> Result<(), Error> {
        let (mapping, vmo) = Mapping::allocate(4096)?;
        let mapping_ref = Arc::new(mapping);
        let mut header = Block::new_free(mapping_ref.clone(), 0, 0, 0)?;
        header.become_reserved()?;
        header.become_header()?;

        let b = Block::new_free(mapping_ref.clone(), 1, 2, 0)?;
        b.become_reserved()?;
        b.become_extent(5)?;
        let b = Block::new_free(mapping_ref.clone(), 5, 0, 0)?;
        b.become_reserved()?;
        b.become_int_value(1, 2, 3)?;

        let snapshot = Snapshot::try_from(&vmo)?;

        // Scan blocks
        let blocks = snapshot.scan().collect::<Vec<ScannedBlock>>();

        assert_eq!(blocks[0].block_type(), BlockType::Header);
        assert_eq!(blocks[0].index(), 0);
        assert_eq!(blocks[0].order(), 0);
        assert_eq!(blocks[0].header_magic().unwrap(), constants::HEADER_MAGIC_NUMBER);
        assert_eq!(blocks[0].header_version().unwrap(), constants::HEADER_VERSION_NUMBER);

        assert_eq!(blocks[1].block_type(), BlockType::Extent);
        assert_eq!(blocks[1].index(), 1);
        assert_eq!(blocks[1].order(), 2);
        assert_eq!(blocks[1].next_extent().unwrap(), 5);

        assert_eq!(blocks[2].block_type(), BlockType::IntValue);
        assert_eq!(blocks[2].index(), 5);
        assert_eq!(blocks[2].order(), 0);
        assert_eq!(blocks[2].name_index().unwrap(), 2);
        assert_eq!(blocks[2].parent_index().unwrap(), 3);
        assert_eq!(blocks[2].int_value().unwrap(), 1);
        assert!(blocks[6..].iter().all(|b| b.block_type() == BlockType::Free));

        // Verify get_block
        assert_eq!(snapshot.get_block(0).unwrap().block_type(), BlockType::Header);
        assert_eq!(snapshot.get_block(1).unwrap().block_type(), BlockType::Extent);
        assert_eq!(snapshot.get_block(5).unwrap().block_type(), BlockType::IntValue);
        assert_eq!(snapshot.get_block(6).unwrap().block_type(), BlockType::Free);
        assert!(snapshot.get_block(4096).is_none());

        Ok(())
    }

    #[test]
    fn invalid_pending_write() -> Result<(), Error> {
        let (mapping, vmo) = Mapping::allocate(4096)?;
        let mapping_ref = Arc::new(mapping);
        let mut header = Block::new_free(mapping_ref.clone(), 0, 0, 0)?;
        header.become_reserved()?;
        header.become_header()?;
        header.lock_header()?;
        assert!(Snapshot::try_from(&vmo).is_err());
        Ok(())
    }

    #[test]
    fn invalid_magic_number() -> Result<(), Error> {
        let (mapping, vmo) = Mapping::allocate(4096)?;
        let mapping_ref = Arc::new(mapping);
        let mut header = Block::new_free(mapping_ref.clone(), 0, 0, 0)?;
        header.become_reserved()?;
        header.become_header()?;
        header.set_header_magic(3)?;
        assert!(Snapshot::try_from(&vmo).is_err());
        Ok(())
    }

    #[test]
    fn invalid_generation_count() -> Result<(), Error> {
        let (mapping, vmo) = Mapping::allocate(4096)?;
        let mapping_ref = Arc::new(mapping);
        let mut header = Block::new_free(mapping_ref.clone(), 0, 0, 0)?;
        header.become_reserved()?;
        header.become_header()?;
        assert!(Snapshot::try_from_with_callback(&vmo, || {
            header.lock_header().unwrap();
            header.unlock_header().unwrap();
        })
        .is_err());
        Ok(())
    }
}
