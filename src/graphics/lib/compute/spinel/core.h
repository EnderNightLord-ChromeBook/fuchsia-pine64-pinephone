// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_SPINEL_CORE_H_
#define SRC_GRAPHICS_LIB_COMPUTE_SPINEL_CORE_H_

//
// clang-format off
//

#define SPN_EMPTY

//
// MAXIMUM SUBGROUP SIZE
//
// This is used to properly align GLSL buffers so the variable-sized
// arrays are aligned on an architectural memory transaction boundary.
//

#define SPN_SUBGROUP_ALIGN_LIMIT                256

//
// DEVICE SUBGROUP SIZE
//

#define SPN_DEVICE_SUBGROUP_SIZE                (1<<SPN_DEVICE_SUBGROUP_SIZE_LOG2)

//
// TILE SIZE
//
// Width is a power-of-2 of height
//
// FIXME(allanmac) -- rename SPN_TILE_* to SPN_DEVICE_TILE_*
//

#define SPN_TILE_WIDTH                          (1<<SPN_TILE_WIDTH_LOG2)
#define SPN_TILE_HEIGHT                         (1<<SPN_TILE_HEIGHT_LOG2)

#define SPN_TILE_HEIGHT_MASK                    (SPN_TILE_HEIGHT - 1)

//
// TAGGED BLOCK ID
//
//   0     5                    31
//   | TAG |       BLOCK ID      |
//   |     | SUBBLOCK |   BLOCK  |
//   +-----+----------+----------+
//   |  5  |    SUB   | 27 - SUB |
//
// There are 27 bits of subblocks and 5 bits of tag.
//
// The block pool vends block ids.
//
// There are (2^S) subblocks in a block.
//

#define SPN_TAGGED_BLOCK_ID_BITS_ID             27 // this size is cast in stone
#define SPN_TAGGED_BLOCK_ID_BITS_TAG            5  // which leaves 5 bits of tag

#define SPN_TAGGED_BLOCK_ID_INVALID             SPN_UINT_MAX
#define SPN_TAGGED_BLOCK_ID_MASK_TAG            SPN_BITS_TO_MASK(SPN_TAGGED_BLOCK_ID_BITS_TAG)

#define SPN_TAGGED_BLOCK_ID_GET_TAG(tbid_)      ((tbid_) & SPN_TAGGED_BLOCK_ID_MASK_TAG)
#define SPN_TAGGED_BLOCK_ID_GET_ID(tbid_)       SPN_BITFIELD_EXTRACT(tbid_,SPN_TAGGED_BLOCK_ID_BITS_TAG,SPN_TAGGED_BLOCK_ID_BITS_ID)

#define SPN_BLOCK_ID_MAX                        SPN_BITS_TO_MASK(SPN_TAGGED_BLOCK_ID_BITS_ID)
#define SPN_BLOCK_ID_INVALID                    SPN_UINT_MAX

#define SPN_BLOCK_ID_TAG_PATH_LINE              0  // 0 -- 4  segments
#define SPN_BLOCK_ID_TAG_PATH_QUAD              1  // 1 -- 6  segments
#define SPN_BLOCK_ID_TAG_PATH_CUBIC             2  // 2 -- 8  segments
#define SPN_BLOCK_ID_TAG_PATH_RAT_QUAD          3  // 3 -- 7  segments : 6 + w0
#define SPN_BLOCK_ID_TAG_PATH_RAT_CUBIC         4  // 4 -- 10 segments : 8 + w0 + w1
// ...
// tags 5-29 are available
// ...
#define SPN_BLOCK_ID_TAG_PATH_COUNT             5  // how many path types?  can share same value with PATH_NEXT
#define SPN_BLOCK_ID_TAG_PATH_NEXT              (SPN_TAGGED_BLOCK_ID_MASK_TAG - 1) // 30 : 0x1E
#define SPN_BLOCK_ID_TAG_INVALID                SPN_TAGGED_BLOCK_ID_MASK_TAG       // 31 : 0x1F

//
// BLOCK POOL
//

#define SPN_BLOCK_POOL_BLOCK_DWORDS             (1<<SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2)
#define SPN_BLOCK_POOL_SUBBLOCK_DWORDS          (1<<SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2)

#define SPN_BLOCK_POOL_BLOCK_DWORDS_MASK        SPN_BITS_TO_MASK(SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2)
#define SPN_BLOCK_POOL_SUBBLOCK_DWORDS_MASK     SPN_BITS_TO_MASK(SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2)

#define SPN_BLOCK_POOL_SUBBLOCKS_PER_BLOCK_LOG2 (SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2 - SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2)
#define SPN_BLOCK_POOL_SUBBLOCKS_PER_BLOCK      (1<<SPN_BLOCK_POOL_SUBBLOCKS_PER_BLOCK_LOG2)
#define SPN_BLOCK_POOL_SUBBLOCKS_PER_BLOCK_MASK SPN_BITS_TO_MASK(SPN_BLOCK_POOL_SUBBLOCKS_PER_BLOCK_LOG2)

#define SPN_BLOCK_POOL_SUBGROUPS_PER_BLOCK_LOG2 (SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2 - SPN_DEVICE_SUBGROUP_SIZE_LOG2)
#define SPN_BLOCK_POOL_SUBGROUPS_PER_BLOCK      (1<<SPN_BLOCK_POOL_SUBGROUPS_PER_BLOCK_LOG2)
#define SPN_BLOCK_POOL_SUBGROUPS_PER_BLOCK_MASK SPN_BITS_TO_MASK(SPN_BLOCK_POOL_SUBGROUPS_PER_BLOCK_LOG2)

#define SPN_BLOCK_POOL_SUBBLOCKS_PER_SUBGROUP   (SPN_DEVICE_SUBGROUP_SIZE / SPN_BLOCK_POOL_SUBBLOCK_DWORDS)

#define SPN_BLOCK_POOL_BLOCK_QWORDS_LOG2        (SPN_BLOCK_POOL_BLOCK_DWORDS_LOG2-1)
#define SPN_BLOCK_POOL_BLOCK_QWORDS             (1<<SPN_BLOCK_POOL_BLOCK_QWORDS_LOG2)
#define SPN_BLOCK_POOL_BLOCK_QWORDS_MASK        SPN_BITS_TO_MASK(SPN_BLOCK_POOL_BLOCK_QWORDS_LOG2)

#define SPN_BLOCK_POOL_SUBBLOCK_QWORDS_LOG2     (SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2-1)
#define SPN_BLOCK_POOL_SUBBLOCK_QWORDS          (1<<SPN_BLOCK_POOL_SUBBLOCK_QWORDS_LOG2)

#define SPN_BLOCK_POOL_SUBBLOCK_OWORDS_LOG2     (SPN_BLOCK_POOL_SUBBLOCK_DWORDS_LOG2-2)
#define SPN_BLOCK_POOL_SUBBLOCK_OWORDS          (1<<SPN_BLOCK_POOL_SUBBLOCK_OWORDS_LOG2)

#define SPN_BLOCK_POOL_ATOMICS_READS            0 // FIXME(allanmac): start using these throughout shaders
#define SPN_BLOCK_POOL_ATOMICS_WRITES           1

//
// PATH HEAD
//
//
//   struct spn_path_header
//   {
//     struct {
//       uint32_t handle; // host handle
//       uint32_t blocks; // total number of blocks in path object
//       uint32_t nodes;  // number of path node blocks -- does not include head
//       uint32_t na;     // unused
//     } count;           // uvec4
//
//     uvec4      prims;  // packed counts: lines, quads, cubics, rat-quads, rat-cubics
//
//     struct {
//       float    x0;
//       float    y0;
//       float    x1;
//       float    y1;
//     } bounds;
//   };
//

#define SPN_PATH_HEAD_DWORDS                    12
#define SPN_PATH_HEAD_QWORDS                    (SPN_PATH_HEAD_DWORDS / 2)
#define SPN_PATH_HEAD_DWORDS_POW2_RU            16

#define SPN_PATH_HEAD_OFFSET_HANDLE             0
#define SPN_PATH_HEAD_OFFSET_BLOCKS             1
#define SPN_PATH_HEAD_OFFSET_NODES              2
#define SPN_PATH_HEAD_OFFSET_PRIMS              4

//
// lines:      26
// quads:      26
// cubics:     26
// rat_quads:  25
// rat_cubics: 25
//

#define SPN_PATH_PRIMS_GET_LINES(p_)       (SPN_BITFIELD_EXTRACT(p_[0], 0,26))                                             // 26
#define SPN_PATH_PRIMS_GET_QUADS(p_)       (SPN_BITFIELD_EXTRACT(p_[0],26, 6) | (SPN_BITFIELD_EXTRACT(p_[1],0,20) <<  6))  // 26
#define SPN_PATH_PRIMS_GET_CUBICS(p_)      (SPN_BITFIELD_EXTRACT(p_[1],20,12) | (SPN_BITFIELD_EXTRACT(p_[2],0,14) << 12))  // 26
#define SPN_PATH_PRIMS_GET_RAT_QUADS(p_)   (SPN_BITFIELD_EXTRACT(p_[2],14,18) | (SPN_BITFIELD_EXTRACT(p_[3],0, 7) << 18))  // 25
#define SPN_PATH_PRIMS_GET_RAT_CUBICS(p_)  (SPN_BITFIELD_EXTRACT(p_[3], 7,25))                                             // 25

#define SPN_PATH_PRIMS_INIT_UNSAFE(ll_,qq_,cc_,rq_,rc_) \
  {                                                     \
    (ll_      ) | (qq_ << 26),                          \
    (qq_ >>  6) | (cc_ << 20),                          \
    (cc_ >> 12) | (rq_ << 14),                          \
    (rq_ >> 18) | (rc_ <<  7)                           \
  }

#define SPN_PATH_PRIMS_INIT(ll_,qq_,cc_,rq_,rc_)        \
  SPN_PATH_PRIMS_INIT_UNSAFE(ll_,qq_,cc_,rq_,rc_)

//
// PATH HEAD COMPILE-TIME PREDICATES
//

#define SPN_PATH_HEAD_ELEM_GTE(sgsz_,x_,i_)     \
  ((x_) >= (i_) * sgsz_)

#define SPN_PATH_HEAD_ELEM_IN_RANGE(sgsz_,x_,i_)        \
  (SPN_PATH_HEAD_ELEM_GTE(sgsz_,x_,i_) &&               \
   !SPN_PATH_HEAD_ELEM_GTE(sgsz_,x_,(i_)+1))

#define SPN_PATH_HEAD_ENTIRELY_HEADER(sgsz_,i_)                 \
  SPN_PATH_HEAD_ELEM_GTE(sgsz_,SPN_PATH_HEAD_DWORDS,(i_)+1)

#define SPN_PATH_HEAD_PARTIALLY_HEADER(sgsz_,i_)                \
  SPN_PATH_HEAD_ELEM_IN_RANGE(sgsz_,SPN_PATH_HEAD_DWORDS,i_)

#define SPN_PATH_HEAD_IS_HEADER(sgsz_,i_)                       \
  (gl_SubgroupInvocationID + i_ * sgsz_ < SPN_PATH_HEAD_DWORDS)

//
// FILL COMMANDS
//
//
// Fill and rasterize cmds only differ in their first word semantics.
//
// The rasterize command points to a 32-bit nodeword so will need to
// be more than 32-bits if we want to access more than 16GB of blocks.
//
// FIXME(allanmac): drop support for more than 16GB
//
// For GLSL we will use a uvec4 laid out as follows:
//
//  union {
//
//    uvec4 u32v4;
//
//    struct spn_cmd_fill {
//      uint32_t path_h;          // host id
//      uint32_t na         : 16; // unused
//      uint32_t cohort     : 16; // cohort is limited to 13 bits
//      uint32_t transform;       // index of transform
//      uint32_t clip;            // index of clip
//    } fill;
//
//    struct spn_cmd_rast {
//      uint32_t node_id;         // device block id
//      uint32_t node_dword : 16; // block dword offset
//      uint32_t cohort     : 16; // cohort is limited to 13 bits
//      uint32_t transform;       // index of transform
//      uint32_t clip;            // index of clip
//    } rasterize;
//
//  };
//
// NOTE: we can pack the transform and clip indices down to a more
// practical 16 bits in case we want to add additional rasterization
// command indices or flags.
//

#define SPN_CMD_FILL_GET_PATH_H(c_)                c_[0]
#define SPN_CMD_FILL_GET_COHORT(c_)                SPN_BITFIELD_EXTRACT(c_[1],16,16)
#define SPN_CMD_FILL_GET_TRANSFORM(c_)             c_[2]
#define SPN_CMD_FILL_GET_CLIP(c_)                  c_[3]

#define SPN_CMD_RASTERIZE_GET_COHORT(c_)           SPN_CMD_FILL_GET_COHORT(c_)
#define SPN_CMD_RASTERIZE_GET_TRANSFORM(c_)        SPN_CMD_FILL_GET_TRANSFORM(c_)
#define SPN_CMD_RASTERIZE_GET_CLIP(c_)             SPN_CMD_FILL_GET_CLIP(c_)

#define SPN_CMD_RASTERIZE_GET_NODE_ID(c_)          c_[0]
#define SPN_CMD_RASTERIZE_GET_NODE_DWORD(c_)       SPN_BITFIELD_EXTRACT(c_[1],0,16)

#define SPN_CMD_RASTERIZE_SET_NODE_ID(c_,n_id_)    c_[0] = n_id_
#define SPN_CMD_RASTERIZE_SET_NODE_DWORD(c_,n_lo_) c_[1] = SPN_BITFIELD_INSERT(c_[1],n_lo_,0,16)

//
// Spinel supports a projective transformation matrix with the
// requirement that w2 is implicitly 1.0.
//
//   A---------B----+
//   | sx  shx | tx |
//   | shy sy  | ty |
//   C---------D----+
//   | w0  w1  | 1  |
//   +---------+----+
//
// The transformation matrix can be initialized with the array:
//
//   { sx shx shy sy tx ty w0 w1 }
//
// struct spn_transform
// {
//   SPN_TYPE_MAT2X2 a; //  { { sx shx } {shy sy } } -- rotate
//   SPN_TYPE_VEC2   b; //  { tx ty }                -- translate
//   SPN_TYPE_VEC2   c; //  { w0 w1 }                -- project
// };
//
// struct spn_transform_lo
// {
//   SPN_TYPE_MAT2X2 a; //  { { sx shx } {shy sy } } -- rotate
// };
//
// struct spn_transform_hi
// {
//   SPN_TYPE_VEC2   b; //  { tx ty }                -- translate
//   SPN_TYPE_VEC2   c; //  { w0 w1 }                -- project
// };
//
//
// Note that the raster builder is storing the transform as two
// float[4] quads.
//
// The rasterization shaders then load these vec4 quads as mat2
// matrices.
//

#define SPN_TRANSFORM_LO_INDEX_SX                 0
#define SPN_TRANSFORM_LO_INDEX_SHX                1
#define SPN_TRANSFORM_LO_INDEX_SHY                2
#define SPN_TRANSFORM_LO_INDEX_SY                 3

#define SPN_TRANSFORM_HI_INDEX_TX                 0
#define SPN_TRANSFORM_HI_INDEX_TY                 1
#define SPN_TRANSFORM_HI_INDEX_W0                 2
#define SPN_TRANSFORM_HI_INDEX_W1                 3

//
// PATHS COPY COMMANDS
//
// The PATH COPY command is simply a 32-bit tagged block id with a
// host-controlled rolling counter stuffed into the id field.
//

#define SPN_PATHS_COPY_CMD_TYPE_SEGS              0
#define SPN_PATHS_COPY_CMD_TYPE_NODE              1
#define SPN_PATHS_COPY_CMD_TYPE_HEAD              2

#define SPN_PATHS_COPY_CMD_GET_TYPE(cmd)          SPN_TAGGED_BLOCK_ID_GET_TAG(cmd)

//
// RASTER HEAD
//
// The raster header and nodes use a strided layout so that the block is
// split in two with the low dword of the 64-bit keys stored in hte
// first half of the block and the high dword in the second half.
//
// Note: a simple 32-bit .pkidx implies a 16 GB limit to the block pool.
//
// Note: we could interpret the 32-bit .pkidx as the low bits indexing
// the dwords in the low half of the block and the high bits indexing
// qwords.  This will index a 32 GB block pool.
//
//   raster head block
//   {
//     struct spn_raster_header.lo
//     {
//       uint32_t blocks;  // # of blocks -- head+node+skb+pkb
//       uint32_t nodes;   // # of nodes  -- not including header
//       uint32_t pkidx;   // block pool dword of first ttpk.lo
//       uint32_t ttpks;   // # of ttpks
//       uint32_t ttsks;   // # of ttsks
//
//       ... TTXK.lo ...
//     };
//
//     struct spn_raster_header.hi
//     {
//       int32_t  x0;      // axis-aligned bounding box
//       int32_t  x1;      // axis-aligned bounding box
//       int32_t  y0;      // axis-aligned bounding box
//       int32_t  y1;      // axis-aligned bounding box
//       uint32_t na0;     // reserved
//
//       ... TTXK.hi ...
//     };
//   }
//
// Usage:
//
//   - RASTERS_RECLAIM: this shader only needs to load the low dwords of
//     each block because only the block and node counts and the TTXB id
//     of each key are required.
//
//   - RASTERS_PREFIX: this shader needs to vector load the values
//     calculated by RASTERS_ALLOC and write them back to the block.
//
//   - PLACE_TT*K: these shaders need to efficiently load the raster
//     header.
//

#define SPN_RASTER_NODE_QWORDS                   SPN_BLOCK_POOL_BLOCK_QWORDS

#define SPN_RASTER_HEAD_DWORDS                   10
#define SPN_RASTER_HEAD_QWORDS                   (SPN_RASTER_HEAD_DWORDS / 2)

#define SPN_RASTER_HEAD_LO_OFFSET_BLOCKS         0
#define SPN_RASTER_HEAD_LO_OFFSET_NODES          1
#define SPN_RASTER_HEAD_LO_OFFSET_PKIDX          2
#define SPN_RASTER_HEAD_LO_OFFSET_TTPKS          3
#define SPN_RASTER_HEAD_LO_OFFSET_TTSKS          4

#define SPN_RASTER_HEAD_HI_OFFSET_X0             0
#define SPN_RASTER_HEAD_HI_OFFSET_X1             1
#define SPN_RASTER_HEAD_HI_OFFSET_Y0             2
#define SPN_RASTER_HEAD_HI_OFFSET_Y1             3
#define SPN_RASTER_HEAD_HI_OFFSET_NA0            4

//
// RASTER HEAD COMPILE-TIME PREDICATES
//

#define SPN_RASTER_HEAD_ELEM_GTE(sgsz_,x_,i_)      \
  ((x_) >= (i_) * sgsz_)

#define SPN_RASTER_HEAD_ELEM_IN_RANGE(sgsz_,x_,i_) \
  (SPN_RASTER_HEAD_ELEM_GTE(sgsz_,x_,i_) &&        \
   !SPN_RASTER_HEAD_ELEM_GTE(sgsz_,x_,(i_)+1))

#define SPN_RASTER_HEAD_ENTIRELY_HEADER(sgsz_,i_)                 \
  SPN_RASTER_HEAD_ELEM_GTE(sgsz_,SPN_RASTER_HEAD_QWORDS,(i_)+1)

#define SPN_RASTER_HEAD_PARTIALLY_HEADER(sgsz_,i_)                \
  SPN_RASTER_HEAD_ELEM_IN_RANGE(sgsz_,SPN_RASTER_HEAD_QWORDS,i_)

#define SPN_RASTER_HEAD_IS_HEADER(sgsz_,i_)                       \
  (gl_SubgroupInvocationID + i_ * sgsz_ < SPN_RASTER_HEAD_QWORDS)

//
// Hard requirements:
//
//   - A TTXB "block pool" extent that is at least 1GB.
//
//   - A virtual surface of at least 8K x 8K
//
//   - A physical surface of __don't really care__ because it's
//     advantageous to tile the physical surface since it's likely
//     to shrink the post-place TTCK sorting step.
//
//
//      EXTENT                 TTXB BITS
//     SIZE (MB) +------------------------------------+
//               |  22    23    24    25    26    27  |
//          +----+------------------------------------+
//          |  8 |  128   256   512  1024  2048  4096 |
//    TTXB  | 16 |  256   512  1024  2048  4096  8192 |
//   DWORDS | 32 |  512  1024  2048  4096  8192 16384 |
//          | 64 | 1024  2048  4096  8192 16384 32768 |
//          +----+------------------------------------+
//
//
//         SURF                        X/Y BITS
//         TILE  +------------------------------------------------------+
//               |   5     6     7     8     9    10    11    12    13  |
//          +----+------------------------------------------------------+
//          |  3 |  256   512  1024  2048  4096  8192 16384 32768 65536 |
//     TILE |  4 |  512  1024  2048  4096  8192 16384 32768 65536  128K |
//     SIDE |  5 | 1024  2048  4096  8192 16384 32768 65536  128K  256K |
//     BITS |  6 | 2048  4096  8192 16384 32768 65536  128K  256K  512K |
//          |  7 | 4096  8192 16384 32768 65536  128K  256K  512K 1024K |
//          +----+------------------------------------------------------+
//      TILES^2  | 1024  4096 16384 65536  256K    1M    4M   16M   64M |
//               +------------------------------------------------------+
//
// The following values should be pretty future-proof across all GPUs:
//
//   - The minimum addressable subblock size is 16 dwords (64 bytes)
//     to ensure there is enough space for a path or raster header and
//     its payload.
//
//   - Blocks are power-of-2 multiples of subblocks. Larger blocks can
//     reduce allocation activity (fewer atomic adds).
//
//   - 27 bits of TTXB_ID space implies a max of 4GB-32GB of
//     rasterized paths depending on the size of the TTXB block.
//     This could enable interesting use cases.
//
//   - A virtual rasterization surface that's from +/-16K to +/-128K
//     depending on the size of the TTXB block.
//
//   - Keys that (optionally) only require a 32-bit high word
//     comparison.
//
//   - Support for a minimum of 256K layers. This can be practically
//     raised to 1m or 2m layers.
//
//
// TTRK (32-BIT COMPARE)
//
//  0                                               63
//  | TTSB ID | N/A |   X  |   Y  | RASTER COHORT ID |
//  +---------+-----+------+------+------------------+
//  |    27   |  5  |  12  |  12  |        8         |
//
//
// TTRK (64-bit COMPARE) (DEFAULT)
//
//  0                                         63
//  | TTSB ID |   X  |   Y  | RASTER COHORT ID |
//  +---------+------+------+------------------+
//  |    27   |  12  |  12  |        13        |
//
//
// TTSK v1 ( DEFAULT )
//
//  0                            63
//  | TTSB ID |   SPAN  |  X |  Y |
//  +---------+---------+----+----+
//  |    27   | 13 [-1] | 12 | 12 |
//
//
// TTPK v2 ( DEFAULT )
//
//  0                                  63
//  | TTPB ID |      SPAN     |  X |  Y |
//  +---------+---------------+----+----+
//  |    27   | 13 [+1,+4095] | 12 | 12 |
//
//
// TTCK (32-BIT COMPARE) v2
//
//  0                                                           63
//  | PAYLOAD/TTSB/TTPB ID | PREFIX | ESCAPE | LAYER |  X  |  Y  |
//  +----------------------+--------+--------+-------+-----+-----+
//  |          30          |    1   |    1   |   15  |  9  |  8  |
//
//
// TTCK (64-BIT COMPARE) -- achieves 4K x 4K with an 8x16 tile ( DEFAULT )
//
//  0                                                           63
//  | PAYLOAD/TTSB/TTPB ID | PREFIX | ESCAPE | LAYER |  X  |  Y  |
//  +----------------------+--------+--------+-------+-----+-----+
//  |          27          |    1   |    1   |   18  |  9  |  8  |
//

//
// TTRK (64-bit COMPARE) (DEFAULT)
//
//  0                                         63
//  | TTSB ID |   X  |   Y  | RASTER COHORT ID |
//  +---------+------+------+------------------+
//  |    27   |  12  |  12  |        13        |
//
//  0                                                63
//  | TTSB ID | X_LO | X_HI |   Y  | RASTER COHORT ID |
//  +---------+------+------+------+------------------+
//  |    27   |  5   |  7   |  12  |        13        |
//

#define SPN_TTRK_LO_BITS_TTSB_ID                 SPN_TAGGED_BLOCK_ID_BITS_ID
#define SPN_TTRK_LO_HI_BITS_X                    12
#define SPN_TTRK_LO_BITS_X                       5  // straddles a
#define SPN_TTRK_HI_BITS_X                       7  // word boundary
#define SPN_TTRK_HI_BITS_Y                       12
#define SPN_TTRK_HI_BITS_COHORT                  13

#define SPN_TTRK_LO_HI_BITS_YX                   (SPN_TTRK_LO_HI_BITS_X + SPN_TTRK_HI_BITS_Y)
#define SPN_TTRK_HI_BITS_YX                      (SPN_TTRK_HI_BITS_Y + SPN_TTRK_HI_BITS_X)

#define SPN_TTRK_LO_OFFSET_X                     SPN_TTRK_LO_BITS_TTSB_ID
#define SPN_TTRK_HI_OFFSET_Y                     SPN_TTRK_HI_BITS_X             // 7
#define SPN_TTRK_HI_OFFSET_COHORT                (32 - SPN_TTRK_HI_BITS_COHORT) // 19

#define SPN_TTRK_LO_MASK_TTSB_ID                 SPN_BITS_TO_MASK(SPN_TTRK_LO_BITS_TTSB_ID)
#define SPN_TTRK_HI_MASK_Y                       SPN_BITS_TO_MASK_AT(SPN_TTRK_HI_OFFSET_Y,SPN_TTRK_HI_BITS_Y)

#define SPN_TTRK_GET_X(t_)                       SPN_GLSL_EXTRACT_UVEC2_UINT(t_,SPN_TTRK_LO_OFFSET_X,SPN_TTRK_LO_HI_BITS_X)
#define SPN_TTRK_GET_YX(t_)                      SPN_GLSL_EXTRACT_UVEC2_UINT(t_,SPN_TTRK_LO_OFFSET_X,SPN_TTRK_LO_HI_BITS_YX)

//
// TTSK v1 ( DEFAULT )
//
//  0                            63
//  | TTSB ID |   SPAN  |  X |  Y |
//  +---------+---------+----+----+
//  |    27   | 13 [-1] | 12 | 12 |
//
//
// TTPK v2 ( DEFAULT )
//
//  0                                  63
//  | TTPB ID |      SPAN     |  X |  Y |
//  +---------+---------------+----+----+
//  |    27   | 13 [+1,+4095] | 12 | 12 |
//
//
// A TTSK.SPAN is always -1
//
// A TTPK.SPAN has a range of [+1,+4095].
//
// A TTXK.SPAN of 0 indicates either:
//
//   - an invalid key
//   - a TTXK key pointing to all TTS_INVALID values
//   - a TTPK key pointing to all zero values
//
// In all cases, this key can be skipped during rendering.
//
// TTXK.Y and TTXK.X are signed but stored as biased unsigned.
//
// An invalid TTXK has a span of zero and a TTPB ID of all 1's.
//

#define SPN_TTXK_LO_BITS_TTXB_ID                 SPN_TAGGED_BLOCK_ID_BITS_ID
#define SPN_TTXK_LO_HI_BITS_SPAN                 13
#define SPN_TTXK_LO_BITS_SPAN                    5  // straddles a
#define SPN_TTXK_HI_BITS_SPAN                    8  // word boundary
#define SPN_TTXK_HI_BITS_X                       SPN_TTRK_LO_HI_BITS_X
#define SPN_TTXK_HI_BITS_Y                       SPN_TTRK_HI_BITS_Y
#define SPN_TTXK_HI_BITS_YX                      (SPN_TTXK_HI_BITS_Y + SPN_TTXK_HI_BITS_X)

#define SPN_TTXK_LO_OFFSET_SPAN                  SPN_TTXK_LO_BITS_TTXB_ID
#define SPN_TTXK_HI_OFFSET_X                     (32 - SPN_TTXK_HI_BITS_YX) // 8
#define SPN_TTXK_HI_OFFSET_Y                     (32 - SPN_TTXK_HI_BITS_Y)  // 20
#define SPN_TTXK_HI_OFFSET_YX                    (32 - SPN_TTXK_HI_BITS_YX) // 8

#define SPN_TTXK_INVALID                         uvec2(0,0) // a span of zero is invalid

#define SPN_TTXK_LO_MASK_TTXB_ID                 SPN_BITS_TO_MASK(SPN_TTXK_LO_BITS_TTXB_ID)
#define SPN_TTXK_HI_MASK_X                       SPN_BITS_TO_MASK_AT(SPN_TTXK_HI_OFFSET_X,SPN_TTXK_HI_BITS_X)
#define SPN_TTXK_HI_MASK_Y                       SPN_BITS_TO_MASK_AT(SPN_TTXK_HI_OFFSET_Y,SPN_TTXK_HI_BITS_Y)
#define SPN_TTXK_HI_MASK_YX                      SPN_BITS_TO_MASK_AT(SPN_TTXK_HI_OFFSET_Y,SPN_TTXK_HI_BITS_YX)

#define SPN_TTXK_HI_ONE_X                        (1u << SPN_TTXK_HI_OFFSET_X)

#define SPN_TTXK_LO_GET_TTXB_ID(t_lo_)           SPN_BITFIELD_EXTRACT(t_lo_,0,SPN_TTXK_LO_BITS_TTXB_ID)
#define SPN_TTXK_HI_GET_YX(t_hi_)                SPN_BITFIELD_EXTRACT(t_hi_,SPN_TTXK_HI_OFFSET_YX,SPN_TTXK_HI_BITS_YX)

#define SPN_TTXK_GET_TTXB_ID(t_)                 SPN_TTXK_LO_GET_TTXB_ID(t_[0])
#define SPN_TTXK_GET_SPAN(t_)                    SPN_GLSL_EXTRACT_UVEC2_INT(t_,SPN_TTXK_LO_OFFSET_SPAN,SPN_TTXK_LO_HI_BITS_SPAN)
#define SPN_TTXK_GET_X(t_)                       SPN_BITFIELD_EXTRACT(t_[1],SPN_TTXK_HI_OFFSET_X,SPN_TTXK_HI_BITS_X)
#define SPN_TTXK_GET_Y(t_)                       SPN_BITFIELD_EXTRACT(t_[1],SPN_TTXK_HI_OFFSET_Y,SPN_TTXK_HI_BITS_Y)
#define SPN_TTXK_GET_YX(t_)                      SPN_TTXK_HI_GET_YX(t_[1])

#define SPN_TTXK_SET_TTXB_ID(t_,i_)              t_[0] = SPN_BITFIELD_INSERT(t_[0],i_,0,SPN_TTXK_LO_BITS_TTXB_ID)
#define SPN_TTXK_SET_SPAN(t_,s_)                 SPN_GLSL_INSERT_UVEC2_UINT(t_,s_,SPN_TTXK_LO_OFFSET_SPAN,SPN_TTXK_LO_HI_BITS_SPAN)
#define SPN_TTXK_SET_YX(t_,i_)                   t_[1] = SPN_BITFIELD_INSERT(t_[1],i_,SPN_TTXK_HI_OFFSET_YX,SPN_TTXK_HI_BITS_YX)

//
// YX
//
//  0        32
//  |  X |  Y |
//  +----+----+
//  | 12 | 22 |
//
// A few shaders work probe the YX value.
//
// The max value of X is 4095.
//

#define SPN_YX_GET_Y(yx_)                        SPN_BITFIELD_EXTRACT(yx_,SPN_TTXK_HI_BITS_X,32-SPN_TTXK_HI_BITS_X)
#define SPN_YX_X_MASK                            SPN_BITS_TO_MASK(SPN_TTXK_HI_BITS_X)

//
// PLACE
//

struct spn_cmd_place
{
  SPN_TYPE_UINT raster_h;
  SPN_TYPE_UINT layer_id;
  SPN_TYPE_INT  txty[2];
};

//
// TTCK (64-BIT COMPARE)
//
//  0                                                           63
//  | PAYLOAD/TTSB/TTPB ID | PREFIX | ESCAPE | LAYER |  X  |  Y  |
//  +----------------------+--------+--------+-------+-----+-----+
//  |          27          |    1   |    1   |   18  |  9  |  8  |
//
//  0                                                  32                     63
//  | PAYLOAD/TTSB/TTPB ID | PREFIX | ESCAPE | LAYER_LO | LAYER_HI |  X  |  Y  |
//  +----------------------+--------+--------+----------+----------+-----+-----+
//  |          27          |    1   |    1   |     3    |    15    |  9  |  8  |
//
//
// TTCK.Y and TTCK.X are unsigned
//
//  +-----------+-------------+
//  | TILE SIZE | MAX SURFACE |
//  +-----------+-------------+
//  |   16x16   |   8K x 4K   | NVIDIA, AMD
//  |    8x8    |   4K x 2K   | INTEL GEN, Mali G52+
//  |    4x4    |   2K x 1K   | Mali G31, SwiftShader
//  +-----------+-------------+
//

#define SPN_TTCK_LO_BITS_TTXB_ID                 SPN_TAGGED_BLOCK_ID_BITS_ID
#define SPN_TTCK_LO_BITS_PREFIX                  1
#define SPN_TTCK_LO_BITS_ESCAPE                  1

#define SPN_TTCK_LO_HI_BITS_LAYER                18
#define SPN_TTCK_LO_BITS_LAYER                   3
#define SPN_TTCK_HI_BITS_LAYER                   15

#define SPN_TTCK_HI_BITS_X                       9
#define SPN_TTCK_HI_BITS_Y                       8
#define SPN_TTCK_HI_BITS_YX                      (SPN_TTCK_HI_BITS_Y + SPN_TTCK_HI_BITS_X)

#define SPN_TTCK_LO_OFFSET_PREFIX                SPN_TTCK_LO_BITS_TTXB_ID
#define SPN_TTCK_LO_OFFSET_ESCAPE                (SPN_TTCK_LO_OFFSET_PREFIX + SPN_TTCK_LO_BITS_PREFIX)
#define SPN_TTCK_LO_OFFSET_LAYER                 (SPN_TTCK_LO_OFFSET_ESCAPE + SPN_TTCK_LO_BITS_ESCAPE)

#define SPN_TTCK_HI_OFFSET_X                     (32 - SPN_TTCK_HI_BITS_YX)
#define SPN_TTCK_HI_OFFSET_Y                     (32 - SPN_TTCK_HI_BITS_Y)
#define SPN_TTCK_HI_OFFSET_YX                    (32 - SPN_TTCK_HI_BITS_YX)

#define SPN_TTCK_LO_MASK_TTXB_ID                 SPN_BITS_TO_MASK(SPN_TTCK_LO_BITS_TTXB_ID)
#define SPN_TTCK_LO_MASK_PREFIX                  SPN_BITS_TO_MASK_AT(SPN_TTCK_LO_OFFSET_PREFIX,SPN_TTCK_LO_BITS_PREFIX)
#define SPN_TTCK_LO_MASK_ESCAPE                  SPN_BITS_TO_MASK_AT(SPN_TTCK_LO_OFFSET_ESCAPE,SPN_TTCK_LO_BITS_ESCAPE)
#define SPN_TTCK_LO_MASK_LAYER                   SPN_BITS_TO_MASK_AT(SPN_TTCK_LO_OFFSET_LAYER,SPN_TTCK_LO_BITS_LAYER)

#define SPN_TTCK_HI_MASK_LAYER                   SPN_BITS_TO_MASK(SPN_TTCK_HI_BITS_LAYER)
#define SPN_TTCK_HI_MASK_YX                      SPN_BITS_TO_MASK_AT(SPN_TTCK_HI_OFFSET_YX,SPN_TTCK_HI_BITS_YX)

#define SPN_TTCK_GET_TTXB_ID(t_)                 ( t_[0] & SPN_TTCK_LO_MASK_TTXB_ID)
#define SPN_TTCK_LO_GET_TTXB_ID(t_lo_)           ( t_lo_ & SPN_TTCK_LO_MASK_TTXB_ID)

#define SPN_TTCK_IS_PREFIX(t_)                   ((t_[0] & SPN_TTCK_LO_MASK_PREFIX) != 0)
#define SPN_TTCK_LO_IS_PREFIX(t_lo_)             ((t_lo_ & SPN_TTCK_LO_MASK_PREFIX) != 0)

#define SPN_TTCK_IS_ESCAPE(t_)                   ((t_[0] & SPN_TTCK_LO_MASK_ESCAPE) != 0)

#define SPN_TTCK_GET_LAYER(t_)                   SPN_GLSL_EXTRACT_UVEC2_UINT(t_,SPN_TTCK_LO_OFFSET_LAYER,SPN_TTCK_LO_HI_BITS_LAYER)
#define SPN_TTCK_SET_LAYER(t_,l_)                SPN_GLSL_INSERT_UVEC2_UINT(t_,l_,SPN_TTCK_LO_OFFSET_LAYER,SPN_TTCK_LO_HI_BITS_LAYER)

#define SPN_TTCK_GET_Y(t_)                       SPN_BITFIELD_EXTRACT(t_[1],SPN_TTCK_HI_OFFSET_Y,SPN_TTCK_HI_BITS_Y)
#define SPN_TTCK_GET_X(t_)                       SPN_BITFIELD_EXTRACT(t_[1],SPN_TTCK_HI_OFFSET_X,SPN_TTCK_HI_BITS_X)

#define SPN_TTCK_ADD_X(t_,d_)                    (t_[1] += ((d_) << SPN_TTCK_HI_OFFSET_X))

#define SPN_TTCK_LAYER_MAX                       SPN_BITS_TO_MASK(SPN_TTCK_LO_HI_BITS_LAYER)

//
// TILE TRACE SUBPIXEL v2 (DEFAULT)
//
// TTS:
//
//  0                  31
//  | TX | DX | TY | DY |
//  +----+----+----+----+
//  | 10 |  7 |  9 |  6 |
//
//
// A a subpixel-resolution line segment within a 32x16 (WxH) tile is
// encoded in a 32-bit dword.
//
// Subpixel resoluion is 5 bits.
//
// We're using this representation across all target
// architectures.
//
// A tile X is encoded as:
//
//   TX : 10 : unsigned min(x0,x1) tile subpixel coordinate with a range
//             of [0,1023].
//
//   DX :  7 : signed subpixel delta x1-x0. The range of the delta is
//             [-32,32] including 0.  Note that with 7 signed bits the
//             range of the bitfield is [-64,63].  An "invalid" TTS
//             relies on DX being infeasible value.
//
// A tile Y is encoded as:
//
//   TY :  9 : unsigned min(y0,y1) tile subpixel coordinate with a range
//             of [0,511].
//
//   DY :  6 : signed subpixel delta y1-y0. The range of delta is
//             [-32,32] but horizontal lines are not encoded so [1,32]
//             is mapped to [0,31]. The resulting range [-32,31] fits in
//             6 bits.
//
// Note: There are assumptions in the shaders that the X and Y subpixel
// resolutions are the same.  Despite this, let's keep the X and Y
// definitions separated.
//

#define SPN_TTS_BITS_TX                          10
#define SPN_TTS_BITS_DX                          7
#define SPN_TTS_BITS_TY                          9
#define SPN_TTS_BITS_DY                          6

//
//
//

#define SPN_TTS_SUBPIXEL_X_LOG2                  5
#define SPN_TTS_SUBPIXEL_Y_LOG2                  5

#define SPN_TTS_SUBPIXEL_X_SIZE                  (1 << SPN_TTS_SUBPIXEL_X_LOG2)
#define SPN_TTS_SUBPIXEL_Y_SIZE                  (1 << SPN_TTS_SUBPIXEL_Y_LOG2)

#define SPN_TTS_PIXEL_X_LOG2                     (SPN_TTS_BITS_TX - SPN_TTS_SUBPIXEL_X_LOG2)
#define SPN_TTS_PIXEL_Y_LOG2                     (SPN_TTS_BITS_TY - SPN_TTS_SUBPIXEL_Y_LOG2)

#define SPN_TTS_SUBPIXEL_X_RESL                  float(SPN_TTS_SUBPIXEL_X_SIZE)
#define SPN_TTS_SUBPIXEL_Y_RESL                  float(SPN_TTS_SUBPIXEL_Y_SIZE)

#define SPN_TTS_SUBPIXEL_X_SCALE_UP              SPN_TTS_SUBPIXEL_X_RESL
#define SPN_TTS_SUBPIXEL_Y_SCALE_UP              SPN_TTS_SUBPIXEL_Y_RESL

#define SPN_TTS_SUBPIXEL_X_SCALE_DOWN            (1.0f / SPN_TTS_SUBPIXEL_X_RESL)
#define SPN_TTS_SUBPIXEL_Y_SCALE_DOWN            (1.0f / SPN_TTS_SUBPIXEL_Y_RESL)

//
// TTXK.X and .Y are biased and unsigned
//

#define SPN_DEVICE_TILE_SUBPIXEL_X_BITS_LOG2     (SPN_TILE_WIDTH_LOG2  + SPN_TTS_SUBPIXEL_X_LOG2)
#define SPN_DEVICE_TILE_SUBPIXEL_Y_BITS_LOG2     (SPN_TILE_HEIGHT_LOG2 + SPN_TTS_SUBPIXEL_Y_LOG2)

#define SPN_DEVICE_TILE_SUBPIXEL_X_SIZE          (1 << SPN_DEVICE_TILE_SUBPIXEL_X_BITS_LOG2)
#define SPN_DEVICE_TILE_SUBPIXEL_Y_SIZE          (1 << SPN_DEVICE_TILE_SUBPIXEL_Y_BITS_LOG2)

#define SPN_DEVICE_TTXK_X_BIAS                   (1 << (SPN_TTXK_HI_BITS_X + SPN_DEVICE_TILE_SUBPIXEL_X_BITS_LOG2 - 1))
#define SPN_DEVICE_TTXK_Y_BIAS                   (1 << (SPN_TTXK_HI_BITS_Y + SPN_DEVICE_TILE_SUBPIXEL_Y_BITS_LOG2 - 1))

#define SPN_DEVICE_TTXK_TILE_X_BIAS              (1 << (SPN_TTXK_HI_BITS_X - 1))
#define SPN_DEVICE_TTXK_TILE_Y_BIAS              (1 << (SPN_TTXK_HI_BITS_Y - 1))

#define SPN_DEVICE_TTXK_XY_BIAS                  ivec2(SPN_DEVICE_TTXK_X_BIAS, SPN_DEVICE_TTXK_Y_BIAS)

//
//
//

#define SPN_TTS_OFFSET_TX                        0
#define SPN_TTS_OFFSET_DX                        (SPN_TTS_OFFSET_TX + SPN_TTS_BITS_TX)
#define SPN_TTS_OFFSET_TY                        (SPN_TTS_OFFSET_DX + SPN_TTS_BITS_DX)
#define SPN_TTS_OFFSET_DY                        (SPN_TTS_OFFSET_TY + SPN_TTS_BITS_TY)

#define SPN_TTS_OFFSET_TX_PIXEL                  (SPN_TTS_OFFSET_TX + SPN_TTS_SUBPIXEL_X_LOG2)
#define SPN_TTS_OFFSET_TY_PIXEL                  (SPN_TTS_OFFSET_TY + SPN_TTS_SUBPIXEL_Y_LOG2)

#define SPN_TTS_MASK_TX                          SPN_BITS_TO_MASK(SPN_TTS_BITS_TX)
#define SPN_TTS_MASK_DX                          SPN_BITS_TO_MASK_AT(SPN_TTS_BITS_DX,SPN_TTS_OFFSET_DX)
#define SPN_TTS_MASK_TY                          SPN_BITS_TO_MASK_AT(SPN_TTS_BITS_TY,SPN_TTS_OFFSET_TY)

#define SPN_TTS_GET_DX(tts_)                     SPN_BITFIELD_EXTRACT(int(tts_),SPN_TTS_OFFSET_DX,SPN_TTS_BITS_DX)
#define SPN_TTS_GET_DY(tts_)                     SPN_BITFIELD_EXTRACT(int(tts_),SPN_TTS_OFFSET_DY,SPN_TTS_BITS_DY)

#define SPN_TTS_GET_TX_SUBPIXEL(tts_)            SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TX,SPN_TTS_SUBPIXEL_X_LOG2)
#define SPN_TTS_GET_TY_SUBPIXEL(tts_)            SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TY,SPN_TTS_SUBPIXEL_Y_LOG2)

#define SPN_TTS_GET_TX_PIXEL(tts_)               SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TX_PIXEL,SPN_TTS_PIXEL_X_LOG2)
#define SPN_TTS_GET_TY_PIXEL(tts_)               SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TY_PIXEL,SPN_TTS_PIXEL_Y_LOG2)

#define SPN_TTS_GET_TX(tts_)                     SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TX,SPN_TTS_BITS_TX)
#define SPN_TTS_GET_TY(tts_)                     SPN_BITFIELD_EXTRACT(uint(tts_),SPN_TTS_OFFSET_TY,SPN_TTS_BITS_TY)

//
//
//

#define SPN_TTS_INVALID                          (63<<SPN_TTS_OFFSET_DX)

//
// Note that 2048.0 can be represented exactly with fp16... fortuitous!
//

#define SPN_TTS_FILL_MAX_AREA                    (2 * SPN_TTS_SUBPIXEL_X_SIZE * SPN_TTS_SUBPIXEL_Y_SIZE)
#define SPN_TTS_FILL_MAX_AREA_2                  (2 * SPN_TTS_FILL_MAX_AREA)
#define SPN_TTS_FILL_EVEN_ODD_MASK               (SPN_TTS_FILL_MAX_AREA_2 - 1)
#define SPN_TTS_FILL_MAX_AREA_RCP_F32            (1.0f / SPN_TTS_FILL_MAX_AREA)

//
// RASTER COHORT METADATA
//
// MAXIMUM RASTER COHORT META TABLE SIZE IS DETERMINED BY COHORT BITFIELD
//

#define SPN_RASTER_COHORT_METAS_SIZE_LOG2          SPN_TTRK_HI_BITS_COHORT
#define SPN_RASTER_COHORT_METAS_SIZE               (1 << SPN_RASTER_COHORT_METAS_SIZE_LOG2)

#define SPN_RASTER_COHORT_META_ALLOC_OFFSET_RKOFF  0
#define SPN_RASTER_COHORT_META_ALLOC_OFFSET_READS  1
#define SPN_RASTER_COHORT_META_ALLOC_OFFSET_PKNODE 2
#define SPN_RASTER_COHORT_META_ALLOC_OFFSET_NA     3

struct spn_rc_meta
{
  // alloc[0] - rkoff  : offset of first ttrk in cohort
  // alloc[1] - reads  : block pool read counter of first block
  // alloc[2] - pknode : index of head/node block for first ttpk // <----- FIXME(allanmac): don't need this anymore
  SPN_TYPE_UVEC4 alloc [SPN_RASTER_COHORT_METAS_SIZE]; // doesn't need to be zeroed
  SPN_TYPE_UINT  blocks[SPN_RASTER_COHORT_METAS_SIZE]; // number of blocks
  SPN_TYPE_UINT  ttpks [SPN_RASTER_COHORT_METAS_SIZE]; // number of TTPK keys
  SPN_TYPE_UINT  ttrks [SPN_RASTER_COHORT_METAS_SIZE]; // number of TTRK keys

  //
  // FIXME(allanmac): the signed bounding box will be added to the meta
  // using the atomic signed min/max trick.
  //
};

//
// STYLING STRUCTS
//
//
// LAYER
//
//   |     LAYER     |
//   +---------------+
//   | cmds | parent |
//   +------+--------+
//   0      1        2
//
// GROUP
//
//   |                 GROUP                  |
//   +--------------+---------+---------------+
//   |    parents   |  range  |     cmds      |
//   | depth | base | lo | hi | enter | leave |
//   +-------+------+----+----+-------+-------+
//   0       1      2    3    4       5       6
//
//
// It's simpler to define the group as a uvec2[3]:
//
//   struct spn_group_node
//   {
//     spn_group_parents parents; // path of parent groups leading back to root
//     spn_group_range   range;   // range of layers enclosed by this group
//     spn_group_cmds    cmds;    // enter/leave command indices
//   };
//
// The RENDER kernel lays out the current layer node, group node and
// flags in either registers or shared memory:
//
// LGF -- layer / group / flags
//                                                               optional
//   | current layer |          current group           |       |       |       |
//   +---------------+------------+-------+-------------+.......+.......+.......f....
//   |     layer     |   parents  | range |    cmds     | layer | group | flags | ...
//   |  cmds parent  | depth base | lo hi | enter leave |  id   |  id   |       |
//   +------+--------+------+-----+---+---+------+------+.......+-......+.......+....
//   0      1        2      3     4   5   6      7      8       9       10      11
//

struct spn_layer_node
{
  SPN_TYPE_UINT cmds;   // starting index of sequence of command dwords
  SPN_TYPE_UINT parent; // index of parent group
};

struct spn_group_parents
{
  SPN_TYPE_UINT depth;
  SPN_TYPE_UINT base;
};

struct spn_group_range
{ // inclusive layer range [lo,hi]
  SPN_TYPE_UINT lo; // first layer
  SPN_TYPE_UINT hi; // last  layer
};

struct spn_group_cmds
{
  SPN_TYPE_UINT enter; // starting index of sequence of command dwords
  SPN_TYPE_UINT leave; // starting index of sequence of command dwords
};

//
//
//

#define SPN_STYLING_LAYER_OFFSET_CMDS                  0
#define SPN_STYLING_LAYER_OFFSET_PARENT                1
#define SPN_STYLING_LAYER_COUNT_DWORDS                 2

#define SPN_STYLING_GROUP_OFFSET_PARENTS_DEPTH         0
#define SPN_STYLING_GROUP_OFFSET_PARENTS_BASE          1
#define SPN_STYLING_GROUP_OFFSET_RANGE_LO              2
#define SPN_STYLING_GROUP_OFFSET_RANGE_HI              3
#define SPN_STYLING_GROUP_OFFSET_CMDS_ENTER            4
#define SPN_STYLING_GROUP_OFFSET_CMDS_LEAVE            5
#define SPN_STYLING_GROUP_COUNT_DWORDS                 6

//
//
//

#define SPN_STYLING_CMDS_BITS_COUNT                    3
#define SPN_STYLING_CMDS_BITS_BASE                     (32-SPN_STYLING_CMDS_BITS_COUNT)

#define SPN_STYLING_CMDS_OFFSET_BASE                   0
#define SPN_STYLING_CMDS_OFFSET_COUNT                  SPN_STYLING_CMDS_BITS_BASE

#define SPN_STYLING_CMDS_MAX_BASE                      (1<<SPN_STYLING_CMDS_BITS_BASE)
#define SPN_STYLING_CMDS_MAX_COUNT                     (1<<SPN_STYLING_CMDS_BITS_COUNT)

#define SPN_STYLING_CMDS_GET_COUNT(c_)                 SPN_BITFIELD_EXTRACT(c_,                                 \
                                                                            SPN_STYLING_CMDS_OFFSET_COUNT,      \
                                                                            SPN_STYLING_CMDS_BITS_COUNT)

#define SPN_STYLING_CMDS_GET_BASE(c_)                  SPN_BITFIELD_EXTRACT(c_,                                 \
                                                                            SPN_STYLING_CMDS_OFFSET_BASE,       \
                                                                            SPN_STYLING_CMDS_BITS_BASE)

//
//
//

#define SPN_STYLING_OPCODE_NOOP                        0

#define SPN_STYLING_OPCODE_COVER_NONZERO               1
#define SPN_STYLING_OPCODE_COVER_EVENODD               2
#define SPN_STYLING_OPCODE_COVER_ACCUMULATE            3
#define SPN_STYLING_OPCODE_COVER_MASK                  4

#define SPN_STYLING_OPCODE_COVER_WIP_ZERO              5
#define SPN_STYLING_OPCODE_COVER_ACC_ZERO              6
#define SPN_STYLING_OPCODE_COVER_MASK_ZERO             7
#define SPN_STYLING_OPCODE_COVER_MASK_ONE              8
#define SPN_STYLING_OPCODE_COVER_MASK_INVERT           9

#define SPN_STYLING_OPCODE_COLOR_FILL_SOLID            10
#define SPN_STYLING_OPCODE_COLOR_FILL_GRADIENT_LINEAR  11

#define SPN_STYLING_OPCODE_COLOR_WIP_ZERO              12
#define SPN_STYLING_OPCODE_COLOR_ACC_ZERO              13

#define SPN_STYLING_OPCODE_BLEND_OVER                  14
#define SPN_STYLING_OPCODE_BLEND_PLUS                  15
#define SPN_STYLING_OPCODE_BLEND_MULTIPLY              16
#define SPN_STYLING_OPCODE_BLEND_KNOCKOUT              17

#define SPN_STYLING_OPCODE_COVER_WIP_MOVE_TO_MASK      18
#define SPN_STYLING_OPCODE_COVER_ACC_MOVE_TO_MASK      19

#define SPN_STYLING_OPCODE_COLOR_ACC_OVER_BACKGROUND   20
#define SPN_STYLING_OPCODE_COLOR_ACC_STORE_TO_SURFACE  21
#define SPN_STYLING_OPCODE_COLOR_ACC_TEST_OPACITY      22

#define SPN_STYLING_OPCODE_COLOR_ILL_ZERO              23
#define SPN_STYLING_OPCODE_COLOR_ILL_COPY_ACC          24
#define SPN_STYLING_OPCODE_COLOR_ACC_MULTIPLY_ILL      25

#define SPN_STYLING_OPCODE_COUNT                       26

//
//
//

#if 0

union spn_gradient_vector
{
  skc_float4               f32v4;

  struct {
    skc_float              dx;
    skc_float              p0;
    skc_float              dy;
    skc_float              denom;
  };

  union skc_gradient_slope slopes[4];
};

#endif

//
// FIXME -- will eventually need to know if this gradient is
// perspective transformed and if so additional values will need to be
// encoded
//
// VERSION 1
// =============================================================
//
// LINEAR GRADIENT HEADER FOR N STOPS
//
// +----------+----------+------------+----------+-------------+
// |  HEADER  |   INFO   |    LUTS    |  FLOORS  |    COLORS   |
// +----------+----------+------------+----------+-------------+
// |  uintv4  | u32v2[1] | f32v2[N-1] | f32[N-2] | ushort2[4N] |
// +----------+----------+------------+----------+-------------+
//
//   COLOR PAIR            WORD EXPANSION            TOTAL
// +------------+---------------------------------+--------+-------------------------+
// |  ushort2   |  4 + 2 + 2*(N-1) + N - 2 + 4*N  | 7N + 2 | = 7(N-1+1)+2 = 7(N-1)+9 |
// +------------+---------------------------------+--------+-------------------------+
//
// COLOR LAYOUT:
//
//   R[0]R[1], R[1]R[2], ... R[N-1]R[N-1]
//   G[0]G[1], G[1]G[2], ... G[N-1]G[N-1]
//   B[0]B[1], B[1]B[2], ... B[N-1]B[N-1]
//   A[0]A[1], A[1]A[2], ... A[N-1]A[N-1]
//
//
// MINIMUM DWORDS:  N=2 --> 16
//
//
// VERSION 2
// =============================================================
//
// LINEAR GRADIENT DESCRIPTOR FOR N STOPS
//
//                           +--------------- REMOVE ME LATER
//                           v
// +--------+------+-------+---+----------+-----------+
// | VECTOR | TYPE | COUNT | N |  SLOPES  |   COLORS  |
// +--------+------+-------+---+----------+-----------+
// |  f32v4 |   1  |   1   | 1 | f32[N-1] | f16v2[4N] |
// +--------+------+-------+---+----------+-----------+
//
//   COLOR PAIR           WORD EXPANSION            TOTAL
// +------------+--------------------------------+--------+
// |   f16v2    |  4 + 1 + 1 + 1 + [N-1] + [4*N] | 5N + 6 |
// +------------+--------------------------------+--------+
//
// COLOR LAYOUT:
//
//   R[0]R[1], R[1]R[2], ... R[N-1]R[N-1] <-------------------------- FIXME -- USE HERB'S SINGLE FMA REPRESENTATION
//   G[0]G[1], G[1]G[2], ... G[N-1]G[N-1] <-------------------------- FIXME -- USE HERB'S SINGLE FMA REPRESENTATION
//   B[0]B[1], B[1]B[2], ... B[N-1]B[N-1] <-------------------------- FIXME -- USE HERB'S SINGLE FMA REPRESENTATION
//   A[0]A[1], A[1]A[2], ... A[N-1]A[N-1] <-------------------------- FIXME -- USE HERB'S SINGLE FMA REPRESENTATION
//
//
// MINIMUM DWORDS:  N=2 --> 16
//
//
// VERSION 3+
// =============================================================
//
// FIXME -- will probably want to try using the sampler/texture
// hardware to interpolate colors.
//
// This will require that the colors are laid out in sampler-friendly
// order:
//
//    RGBA[0]RGBA[1], RGBA[1]RGBA[2], ..., RGBA[N-1]RGBA[N-1]
//
//

#if 0
#define SPN_GRADIENT_HEADER_DWORDS_LUTS_OFFSET       4
#define SPN_GRADIENT_HEADER_DWORDS_TOTAL(n_minus_1)  (7 * (n_minus_1) + 9)
#define SPN_GRADIENT_HEADER_DWORDS_MIN               SPN_GRADIENT_HEADER_DWORDS_TOTAL(1)
#define SPN_GRADIENT_CMD_DWORDS_V1(n)                (1 + SPN_GRADIENT_HEADER_DWORDS_TOTAL(n-1))
#endif

#define SPN_GRADIENT_CMD_DWORDS_V1(n)                (7 * (n) + 2)
#define SPN_GRADIENT_CMD_DWORDS_V2(n)                (5 * (n) + 6)
#define SPN_GRADIENT_CMD_DWORDS_V2_ADJUST(v1,v2)     (SPN_GRADIENT_CMD_DWORDS_V1(v1) - ((v2) + 6))

//
// clang-format on
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_SPINEL_CORE_H_
