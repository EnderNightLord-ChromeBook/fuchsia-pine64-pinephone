// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! RFC 1071 "internet checksum" computation.
//!
//! This crate implements the "internet checksum" defined in [RFC 1071] and
//! updated in [RFC 1141] and [RFC 1624], which is used by many different
//! protocols' packet formats. The checksum operates by computing the 1s
//! complement of the 1s complement sum of successive 16-bit words of the input.
//!
//! SIMD acceleration is used on some platforms (currently, x86_64 with the avx2
//! extensions).
//!
//! # Benchmarks
//!
//! The following microbenchmarks were performed on a 2018 Google Pixelbook.
//! Each benchmark constructs a [`Checksum`] object, calls
//! [`Checksum::add_bytes`] with an input of the given number of bytes, and then
//! calls [`Checksum::checksum`] to finalize. Benchmarks were performed with
//! SIMD both enabled and disabled. Average values were calculated over 3
//! trials.
//!
//! Bytes | Time w/o SIMD | Rate w/o SIMD | Time w/ SIMD | Rate w/ SIMD | Ratio (time w / time w/o)
//! ----- | ------------- | ------------- | ------------ | ------------ | -------------------------
//!    31 |      3,657 ns |     8.48 MB/s |     3,692 ns |    8.40 MB/s |                      1.01
//!    32 |      3,735 ns |     8.57 MB/s |     3,767 ns |    8.50 MB/s |                      1.01
//!    64 |      7,092 ns |     9.02 MB/s |     6,580 ns |    9.73 MB/s |                      0.93
//!   128 |     13,790 ns |     9.28 MB/s |     7,428 ns |    17.2 MB/s |                      0.54
//!   256 |     27,169 ns |     9.42 MB/s |     9,224 ns |    27.8 MB/s |                      0.34
//!  1024 |    107,609 ns |     9.52 MB/s |    20,071 ns |    51.0 MB/s |                      0.19
//!
//! [RFC 1071]: https://tools.ietf.org/html/rfc1071
//! [RFC 1141]: https://tools.ietf.org/html/rfc1141
//! [RFC 1624]: https://tools.ietf.org/html/rfc1624

// TODO(joshlf): Right-justify the columns above

#![cfg_attr(feature = "benchmark", feature(test))]

#[cfg(all(test, feature = "benchmark"))]
extern crate test;

#[cfg(target_arch = "x86_64")]
use core::arch::x86_64;

use byteorder::{ByteOrder, NetworkEndian};

// TODO(joshlf):
// - Investigate optimizations proposed in RFC 1071 Section 2. The most
//   promising on modern hardware is probably (C) Parallel Summation, although
//   that needs to be balanced against (1) Deferred Carries. Benchmarks will
//   need to be performed to determine which is faster in practice, and under
//   what scenarios.

/// Compute the checksum of "bytes".
///
/// `checksum(bytes)` is shorthand for:
///
/// ```rust
/// # use internet_checksum::Checksum;
/// # let bytes = &[];
/// # let _ = {
/// let mut c = Checksum::new();
/// c.add_bytes(bytes);
/// c.checksum()
/// # };
/// ```
#[inline]
pub fn checksum(bytes: &[u8]) -> u16 {
    let mut c = Checksum::new();
    c.add_bytes(bytes);
    c.checksum()
}

/// Updates bytes in an existing checksum.
///
/// `update` updates a checksum to reflect that the already-checksummed bytes
/// `old` have been updated to contain the values in `new`. It implements the
/// algorithm described in Equation 3 in [RFC 1624]. The first byte must be at
/// an even number offset in the original input. If an odd number offset byte
/// needs to be updated, the caller should simply include the preceding byte as
/// well. If an odd number of bytes is given, it is assumed that these are the
/// last bytes of the input. If an odd number of bytes in the middle of the
/// input needs to be updated, the preceding or following byte of the input
/// should be added to make an even number of bytes.
///
/// # Panics
///
/// `update` panics if `old.len() != new.len()`.
///
/// [RFC 1624]: https://tools.ietf.org/html/rfc1624
#[inline]
pub fn update(checksum: u16, old: &[u8], new: &[u8]) -> u16 {
    assert_eq!(old.len(), new.len());

    // We compute on the sum, not the one's complement of the sum. checksum
    // is the one's complement of the sum, so we need to get back to the
    // sum. Thus, we negate checksum.
    let mut sum = u32::from(!checksum);

    // First, process as much as we can with SIMD.
    let (mut old, mut new) = Checksum::update_simd(&mut sum, old, new);

    // Continue with the normal algorithm to finish up whatever we couldn't
    // process with SIMD.
    while old.len() > 1 {
        let old_u16 = NetworkEndian::read_u16(old);
        let new_u16 = NetworkEndian::read_u16(new);
        // RFC 1624 Eqn. 3
        Checksum::add_u16(&mut sum, !old_u16);
        Checksum::add_u16(&mut sum, new_u16);
        old = &old[2..];
        new = &new[2..];
    }
    if old.len() == 1 {
        let old_u16 = NetworkEndian::read_u16(&[old[0], 0]);
        let new_u16 = NetworkEndian::read_u16(&[new[0], 0]);
        // RFC 1624 Eqn. 3
        Checksum::add_u16(&mut sum, !old_u16);
        Checksum::add_u16(&mut sum, new_u16);
    }
    !Checksum::normalize(sum)
}

/// RFC 1071 "internet checksum" computation.
///
/// `Checksum` implements the "internet checksum" defined in [RFC 1071] and
/// updated in [RFC 1141] and [RFC 1624], which is used by many different
/// protocols' packet formats. The checksum operates by computing the 1s
/// complement of the 1s complement sum of successive 16-bit words of the input.
///
/// [RFC 1071]: https://tools.ietf.org/html/rfc1071
/// [RFC 1141]: https://tools.ietf.org/html/rfc1141
/// [RFC 1624]: https://tools.ietf.org/html/rfc1624
#[derive(Default)]
pub struct Checksum {
    sum: u32,
    // Since odd-length inputs are treated specially, we store the trailing byte
    // for use in future calls to add_bytes(), and only treat it as a true
    // trailing byte in checksum().
    trailing_byte: Option<u8>,
}

impl Checksum {
    /// Minimum number of bytes in a buffer to run the SIMD algorithm.
    ///
    /// Running the algorithm with less than `MIN_BYTES_FOR_SIMD` bytes will
    /// cause the benefits of SIMD to be dwarfed by the overhead (performing
    /// worse than the normal/non-SIMD algorithm). This value was chosen after
    /// several benchmarks which showed that the algorithm performed worse than
    /// the normal/non-SIMD algorithm when the number of bytes was less than 64.
    #[cfg(target_arch = "x86_64")]
    const MIN_BYTES_FOR_SIMD: usize = 64;

    /// Initialize a new checksum.
    #[inline]
    pub const fn new() -> Self {
        Checksum { sum: 0, trailing_byte: None }
    }

    /// Add bytes to the checksum.
    ///
    /// If `bytes` does not contain an even number of bytes, a single zero byte
    /// will be added to the end before updating the checksum.
    ///
    /// Note that `add_bytes` has some fixed overhead regardless of the size of
    /// `bytes`. Additionally, SIMD optimizations are only available for inputs
    /// of a certain size. Where performance is a concern, prefer fewer calls to
    /// `add_bytes` with larger input over more calls with smaller input.
    #[inline]
    pub fn add_bytes(&mut self, mut bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }

        // if there's a trailing byte, consume it first
        if let Some(byte) = self.trailing_byte {
            Self::add_u16(&mut self.sum, NetworkEndian::read_u16(&[byte, bytes[0]]));
            bytes = &bytes[1..];
            self.trailing_byte = None;
        }

        // First, process as much as we can with SIMD.
        bytes = Self::add_bytes_simd(&mut self.sum, bytes);

        // Continue with the normal algorithm to finish up whatever we couldn't
        // process with SIMD.
        while bytes.len() > 1 {
            Self::add_u16(&mut self.sum, NetworkEndian::read_u16(bytes));
            bytes = &bytes[2..];
        }
        if bytes.len() == 1 {
            self.trailing_byte = Some(bytes[0]);
        }
    }

    /// Computes the checksum.
    ///
    /// `checksum` returns the checksum of all data added using `add_bytes` so
    /// far. Calling `checksum` does *not* reset the checksum. More bytes may be
    /// added after calling `checksum`, and they will be added to the checksum
    /// as expected.
    ///
    /// If an odd number of bytes have been added so far, the checksum will be
    /// computed as though a single 0 byte had been added at the end in order to
    /// even out the length of the input.
    #[inline]
    pub fn checksum(&self) -> u16 {
        let mut sum = self.sum;
        if let Some(byte) = self.trailing_byte {
            Self::add_u16(&mut sum, NetworkEndian::read_u16(&[byte, 0]));
        }
        !Self::normalize(sum)
    }

    /// Normalizes a 32-bit accumulator by mopping up the overflow until it fits
    /// in a `u16`.
    fn normalize(mut sum: u32) -> u16 {
        while (sum >> 16) != 0 {
            sum = (sum >> 16) + (sum & 0xFFFF);
        }
        sum as u16
    }

    /// Adds a new `u16` to a running sum, checking for overflow. If overflow is
    /// detected, the sum is first normalized back to a 16-bit representation
    /// and the addition is performed again.
    fn add_u16(sum: &mut u32, u: u16) {
        let new = if let Some(new) = sum.checked_add(u32::from(u)) {
            new
        } else {
            let tmp = *sum;
            *sum = u32::from(Self::normalize(tmp));
            // sum is now in the range [0, 2^16), so this can't overflow
            *sum + u32::from(u)
        };
        *sum = new;
    }

    /// Adds bytes to a running sum using architecture specific SIMD
    /// instructions.
    ///
    /// `add_bytes_simd` updates `sum` with the sum of `bytes` using
    /// architecture-specific SIMD instructions. It may not process all bytes,
    /// and whatever bytes are not processed will be returned. If no
    /// implementation exists for the target architecture and run-time CPU
    /// features, `add_bytes_simd` does nothing and simply returns `bytes`
    /// directly.
    #[inline(always)]
    fn add_bytes_simd<'a>(sum: &mut u32, bytes: &'a [u8]) -> &'a [u8] {
        #[cfg(target_arch = "x86_64")]
        {
            if is_x86_feature_detected!("avx2") && bytes.len() >= Self::MIN_BYTES_FOR_SIMD {
                return unsafe { Self::add_bytes_x86_64(sum, bytes) };
            }
        }

        // Suppress unused variable warning when we don't compile the preceding
        // block.
        #[cfg(not(target_arch = "x86_64"))]
        let _ = sum;

        bytes
    }

    /// Adds bytes to a running sum using x86_64's avx2 SIMD instructions.
    ///
    /// # Safety
    ///
    /// `add_bytes_x86_64` should never be called unless the run-time CPU
    /// features include 'avx2'. If `add_bytes_x86_64` is called and the
    /// run-time CPU features do not include 'avx2', it is considered undefined
    /// behaviour.
    #[cfg(target_arch = "x86_64")]
    #[target_feature(enable = "avx2")]
    unsafe fn add_bytes_x86_64<'a>(sum: &mut u32, mut bytes: &'a [u8]) -> &'a [u8] {
        // TODO(ghanan): Use safer alternatives to achieve SIMD algorithm once
        // they stabilize.

        let zeros: x86_64::__m256i = x86_64::_mm256_setzero_si256();
        let mut c0: x86_64::__m256i;
        let mut c1: x86_64::__m256i;
        let mut acc: x86_64::__m256i;
        let data: [u8; 32] = [0; 32];

        while bytes.len() >= 32 {
            let mut add_count: u32 = 0;

            // Reset accumulator.
            acc = zeros;

            // We can do (2^16 + 1) additions to the accumulator (`acc`) that
            // starts off at 0 without worrying about overflow. Since each
            // iteration of this loop does 2 additions to the accumulator,
            // `add_count` must be less than or equal to U16_MAX(= 2 ^ 16 - 1 =
            // 2 ^ 16 - 1 - 2) to guarantee no overflows during this loop
            // iteration. We know we can do 2^16 + 1 additions to the
            // accumulator because we are using 32bit integers which can hold a
            // max value of U32_MAX (2^32 - 1), and we are adding 16bit values
            // with a max value of U16_MAX (2^16 - 1). U32_MAX = (U16_MAX << 16
            // + U16_MAX) = U16_MAX * (2 ^ 16 + 1)
            while bytes.len() >= 32 && add_count <= u32::from(std::u16::MAX) {
                // Load 32 bytes from memory (16 16bit values to add to `sum`)
                //
                // `_mm256_lddqu_si256` does not require the memory address to
                // be aligned so remove the linter check for casting from a less
                // strictly-aligned pointer to a more strictly-aligned pointer.
                // https://doc.rust-lang.org/core/arch/x86_64/fn._mm256_lddqu_si256.html
                #[allow(clippy::cast_ptr_alignment)]
                {
                    c0 = x86_64::_mm256_lddqu_si256(bytes.as_ptr() as *const x86_64::__m256i);
                }

                // Create 32bit words with most significant 16 bits = 0, least
                // significant 16 bits set to a new 16 bit word to add to
                // checksum from bytes. Setting the most significant 16 bits to
                // 0 allows us to do 2^16 simd additions (2^20 16bit word
                // additions) without worrying about overflows.
                c1 = x86_64::_mm256_unpackhi_epi16(c0, zeros);
                c0 = x86_64::_mm256_unpacklo_epi16(c0, zeros);

                // Sum 'em up!
                // `acc` being treated as a vector of 8x 32bit words.
                acc = x86_64::_mm256_add_epi32(acc, c1);
                acc = x86_64::_mm256_add_epi32(acc, c0);

                // We did 2 additions to the accumulator in this iteration of
                // the loop.
                add_count += 2;

                bytes = &bytes[32..];
            }

            // Store the results of our accumlator of 8x 32bit words to our
            // temporary buffer `data` so that we can iterate over data 16 bits
            // at a time and add the values to `sum`. Since `acc` is a 256bit
            // value, it requires 32 bytes, provided by `data`.
            //
            // `_mm256_storeu_si256` does not require the memory address to be
            // aligned on any particular boundary so remove the linter check for
            // casting from a less strictly-aligned pointer to a more strictly-
            // aligned pointer.
            // https://doc.rust-lang.org/core/arch/x86_64/fn._mm256_storeu_si256.html
            #[allow(clippy::cast_ptr_alignment)]
            x86_64::_mm256_storeu_si256(data.as_ptr() as *mut x86_64::__m256i, acc);

            // Iterate over the accumulator data 2 bytes (16 bits) at a time,
            // and add it to `sum`.
            for x in (0..32).step_by(2) {
                Self::add_u16(sum, NetworkEndian::read_u16(&data[x..x + 2]));
            }
        }

        bytes
    }

    /// Updates bytes in an existing checksum using architecture-specific SIMD
    /// instructions.
    ///
    /// `update_simd` updates a checksum to reflect that the already-checksumed
    /// bytes `old_bytes` have been updated to contain the values in `new_bytes`
    /// using architecture-specific SIMD instructions. It may not process all
    /// the bytes, and whatever bytes are not processed will be returned. If no
    /// implementation exists for the target architecture and run-time CPU
    /// features, `update_simd` does nothing and simply returns `old_bytes` and
    /// `new_bytes' directly.
    #[inline(always)]
    fn update_simd<'a, 'b>(
        sum: &mut u32,
        old_bytes: &'a [u8],
        new_bytes: &'b [u8],
    ) -> (&'a [u8], &'b [u8]) {
        #[cfg(target_arch = "x86_64")]
        {
            if is_x86_feature_detected!("avx2") && old_bytes.len() >= Self::MIN_BYTES_FOR_SIMD {
                return unsafe { Self::update_x86_64(sum, old_bytes, new_bytes) };
            }
        }

        // Suppress unused variable warning when we don't compile the preceding
        // block.
        #[cfg(not(target_arch = "x86_64"))]
        let _ = sum;

        (old_bytes, new_bytes)
    }

    /// Updates bytes in an existing checksum using x86_64's avx2 instructions.
    ///
    /// # Safety
    ///
    /// `update_x86_64` should never be called unless the run-time CPU features
    /// include 'avx2'. If `update_x86_64` is called and the run-time CPU
    /// features do not include 'avx2', it is considered undefined behaviour.
    ///
    /// # Panics
    ///
    /// `update_x86_64` panics if `old_bytes.len() != new_bytes.len()`.
    #[cfg(target_arch = "x86_64")]
    unsafe fn update_x86_64<'a, 'b>(
        sum: &mut u32,
        old_bytes: &'a [u8],
        new_bytes: &'b [u8],
    ) -> (&'a [u8], &'b [u8]) {
        assert_eq!(new_bytes.len(), old_bytes.len());

        // Instead of gettings the 1s complement of each 16bit word before
        // adding it to sum, we can get the sum of just `old_bytes` to a
        // temporary variable `old_sum`. We can then add it as a normal 16bit
        // word to the current sum (`sum`) after normalizng it and getting the
        // 1s complement. This will 'remove' `old_bytes` from `sum`.
        let mut old_sum = 0;
        let old_bytes = Self::add_bytes_x86_64(&mut old_sum, old_bytes);
        Self::add_u16(sum, !Self::normalize(old_sum));

        // Add `new_bytes` to `sum` using SIMD as normal.
        let new_bytes = Self::add_bytes_x86_64(sum, new_bytes);

        // We should have the exact same number of bytes left over for both
        // `new_bytes` and `old_bytes`.
        assert_eq!(new_bytes.len(), old_bytes.len());

        (old_bytes, new_bytes)
    }
}

#[cfg(all(test, feature = "benchmark"))]
mod benchmarks {
    // Benchmark results for comparing checksum calculation with and without
    // SIMD implementation, running on Google's Pixelbook. Average values were
    // calculated over 3 trials.
    //
    // Number of | Average time  | Average time | Ratio
    //   bytes   | (ns) w/o SIMD | (ns) w/ SIMD | (w / w/o)
    // --------------------------------------------------
    //        31 |          3657 |         3692 |   1.01
    //        32 |          3735 |         3767 |   1.01
    //        64 |          7092 |         6580 |   0.93
    //       128 |         13790 |         7428 |   0.54
    //       256 |         27169 |         9224 |   0.34
    //      1024 |        107609 |        20071 |   0.19

    extern crate test;
    use super::*;

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 31 bytes.
    #[bench]
    fn bench_checksum_31(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 31]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 32 bytes.
    #[bench]
    fn bench_checksum_32(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 32]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 64 bytes.
    #[bench]
    fn bench_checksum_64(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 64]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 128 bytes.
    #[bench]
    fn bench_checksum_128(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 128]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 256 bytes.
    #[bench]
    fn bench_checksum_256(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 256]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }

    /// Benchmark time to calculate checksum with a single call to `add_bytes`
    /// with 1024 bytes.
    #[bench]
    fn bench_checksum_1024(b: &mut test::Bencher) {
        b.iter(|| {
            let buf = test::black_box([0xFF; 1024]);
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            test::black_box(c.checksum());
        });
    }
}

#[cfg(test)]
mod tests {
    use core::iter;

    use byteorder::NativeEndian;
    use rand::{Rng, SeedableRng};

    use rand_xorshift::XorShiftRng;

    use super::*;

    /// Create a new deterministic RNG from a seed.
    fn new_rng(mut seed: u64) -> XorShiftRng {
        if seed == 0 {
            // XorShiftRng can't take 0 seeds
            seed = 1;
        }
        let mut bytes = [0; 16];
        NativeEndian::write_u32(&mut bytes[0..4], seed as u32);
        NativeEndian::write_u32(&mut bytes[4..8], (seed >> 32) as u32);
        NativeEndian::write_u32(&mut bytes[8..12], seed as u32);
        NativeEndian::write_u32(&mut bytes[12..16], (seed >> 32) as u32);
        XorShiftRng::from_seed(bytes)
    }

    #[test]
    fn test_checksum() {
        for buf in IPV4_HEADERS {
            // compute the checksum as normal
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            assert_eq!(c.checksum(), 0);
            // compute the checksum one byte at a time to make sure our
            // trailing_byte logic works
            let mut c = Checksum::new();
            for byte in *buf {
                c.add_bytes(&[*byte]);
            }
            assert_eq!(c.checksum(), 0);

            // Make sure that it works even if we overflow u32. Performing this
            // loop 2 * 2^16 times is guaranteed to cause such an overflow
            // because 0xFFFF + 0xFFFF > 2^16, and we're effectively adding
            // (0xFFFF + 0xFFFF) 2^16 times. We verify the overflow as well by
            // making sure that, at least once, the sum gets smaller from one
            // loop iteration to the next.
            let mut c = Checksum::new();
            c.add_bytes(&[0xFF, 0xFF]);
            let mut prev_sum = c.sum;
            let mut overflowed = false;
            for _ in 0..((2 * (1 << 16)) - 1) {
                c.add_bytes(&[0xFF, 0xFF]);
                if c.sum < prev_sum {
                    overflowed = true;
                }
                prev_sum = c.sum;
            }
            assert!(overflowed);
            assert_eq!(c.checksum(), 0);
        }

        // Make sure that checksum works with add_bytes taking a buffer big
        // enough to test implementation with simd instructions and cause
        // overflow within the implementation.
        let mut c = Checksum::new();
        // 2 bytes/word * 8 words/additions * 2^16 additions/overflow
        // * 1 overflow + 79 extra bytes = 2^20 + 79
        let buf = vec![0xFF; (1 << 20) + 79];
        c.add_bytes(&buf);
        assert_eq!(c.checksum(), 0xFF);
    }

    #[test]
    fn test_checksum_simd_rand() {
        let mut rng = new_rng(70812476915813);

        // Test simd implementation with random values and buffer big enough to
        // cause an overflow within the implementation..
        // 2 bytes/word * 8 words/additions * 2^16 additions/overflow
        // * 1 overflow + 79 extra bytes
        // = 2^20 + 79
        const BUF_LEN: usize = (1 << 20) + 79;
        let buf: Vec<u8> = iter::repeat_with(|| rng.gen()).take(BUF_LEN).collect();

        let single_bytes = {
            // Add 1 byte at a time to make sure we do not enter implementation
            // with simd instructions
            let mut c = Checksum::new();
            for i in 0..BUF_LEN {
                c.add_bytes(&buf[i..=i]);
            }
            c.checksum()
        };
        let all_bytes = {
            // Calculate checksum with same buffer, but this time test the
            // implementation with simd instructions
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            c.checksum()
        };
        assert_eq!(single_bytes, all_bytes);
    }

    #[test]
    fn test_update() {
        for b in IPV4_HEADERS {
            let mut buf = Vec::new();
            buf.extend_from_slice(b);

            let mut c = Checksum::new();
            c.add_bytes(&buf);
            assert_eq!(c.checksum(), 0);

            // replace the destination IP with the loopback address
            let old = [buf[16], buf[17], buf[18], buf[19]];
            (&mut buf[16..20]).copy_from_slice(&[127, 0, 0, 1]);
            let updated = update(c.checksum(), &old, &[127, 0, 0, 1]);
            let from_scratch = {
                let mut c = Checksum::new();
                c.add_bytes(&buf);
                c.checksum()
            };
            assert_eq!(updated, from_scratch);
        }

        // Test update with bytes big enough to test simd implementation with
        // overflow.
        const BUF_LEN: usize = (1 << 20) + 79;
        let buf = vec![0xFF; BUF_LEN];
        let mut new_buf = buf.to_vec();
        let (begin, end) = (4, BUF_LEN);

        for i in begin..end {
            new_buf[i] = i as u8;
        }

        let updated = {
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            update(c.checksum(), &buf[begin..end], &new_buf[begin..end])
        };
        let from_scratch = {
            let mut c = Checksum::new();
            c.add_bytes(&new_buf);
            c.checksum()
        };
        assert_eq!(updated, from_scratch);
    }

    #[test]
    fn test_smoke_update() {
        let mut rng = new_rng(70_812_476_915_813);

        for _ in 0..2048 {
            // use an odd length so we test the odd length logic
            const BUF_LEN: usize = 31;
            let buf: [u8; BUF_LEN] = rng.gen();
            let mut c = Checksum::new();
            c.add_bytes(&buf);

            let (begin, end) = loop {
                let begin = rng.gen::<usize>() % BUF_LEN;
                let end = begin + (rng.gen::<usize>() % (BUF_LEN + 1 - begin));
                // update requires that begin is even and end is either even or
                // the end of the input
                if begin % 2 == 0 && (end % 2 == 0 || end == BUF_LEN) {
                    break (begin, end);
                }
            };

            let mut new_buf = buf;
            for i in begin..end {
                new_buf[i] = rng.gen();
            }
            let updated = update(c.checksum(), &buf[begin..end], &new_buf[begin..end]);
            let from_scratch = {
                let mut c = Checksum::new();
                c.add_bytes(&new_buf);
                c.checksum()
            };
            assert_eq!(updated, from_scratch);
        }
    }

    #[test]
    fn test_update_simd_rand() {
        let mut rng = new_rng(70812476915813);

        // Test updating with random values and update size big enough to test
        // simd implementation with overflow
        const MIN_BYTES: usize = 1 << 20;
        const BUF_LEN: usize = (1 << 21) + 79;
        let buf: Vec<u8> = iter::repeat_with(|| rng.gen()).take(BUF_LEN).collect();
        let orig_checksum = {
            let mut c = Checksum::new();
            c.add_bytes(&buf);
            c.checksum()
        };

        let (begin, end) = loop {
            let begin = rng.gen::<usize>() % ((BUF_LEN - MIN_BYTES) / 2);
            let end = begin
                + MIN_BYTES
                + (rng.gen::<usize>() % (((BUF_LEN - MIN_BYTES) / 2) + 1 - begin));
            // update requires that begin is even and end is either even or the
            // end of the input
            if begin % 2 == 0 && (end % 2 == 0 || end == BUF_LEN) {
                break (begin, end);
            }
        };

        let mut new_buf: Vec<u8> = buf.to_vec();

        for i in begin..end {
            new_buf[i] = rng.gen();
        }

        let from_update = update(orig_checksum, &buf[begin..end], &new_buf[begin..end]);
        let from_scratch = {
            let mut c = Checksum::new();
            c.add_bytes(&new_buf);
            c.checksum()
        };
        assert_eq!(from_scratch, from_update);
    }

    /// IPv4 headers.
    ///
    /// This data was obtained by capturing live network traffic.
    const IPV4_HEADERS: &[&[u8]] = &[
        &[
            0x45, 0x00, 0x00, 0x34, 0x00, 0x00, 0x40, 0x00, 0x40, 0x06, 0xae, 0xea, 0xc0, 0xa8,
            0x01, 0x0f, 0xc0, 0xb8, 0x09, 0x6a,
        ],
        &[
            0x45, 0x20, 0x00, 0x74, 0x5b, 0x6e, 0x40, 0x00, 0x37, 0x06, 0x5c, 0x1c, 0xc0, 0xb8,
            0x09, 0x6a, 0xc0, 0xa8, 0x01, 0x0f,
        ],
        &[
            0x45, 0x20, 0x02, 0x8f, 0x00, 0x00, 0x40, 0x00, 0x3b, 0x11, 0xc9, 0x3f, 0xac, 0xd9,
            0x05, 0x6e, 0xc0, 0xa8, 0x01, 0x0f,
        ],
    ];
}
