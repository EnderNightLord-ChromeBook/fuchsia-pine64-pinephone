// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
//
//

#include "path_builder.h"

#include "handle.h"

//
// Verify that prim count is in sync with macro
//

#undef SPN_PATH_BUILDER_PRIM_TYPE_EXPAND_X
#define SPN_PATH_BUILDER_PRIM_TYPE_EXPAND_X(_p, _i, _n) +1

STATIC_ASSERT_MACRO_1((0 SPN_PATH_BUILDER_PRIM_TYPE_EXPAND()) == SPN_PATH_BUILDER_PRIM_TYPE_COUNT);

//
//
//

spn_result
spn_path_builder_retain(spn_path_builder_t path_builder)
{
  ++path_builder->refcount;

  return SPN_SUCCESS;
}

spn_result
spn_path_builder_release(spn_path_builder_t path_builder)
{
  SPN_ASSERT_STATE_ASSERT(SPN_PATH_BUILDER_STATE_READY, path_builder);

  return path_builder->release(path_builder->impl);
}

spn_result
spn_path_builder_flush(spn_path_builder_t path_builder)
{
  return path_builder->flush(path_builder->impl);
}

//
// PATH BODY
//

spn_result
spn_path_begin(spn_path_builder_t path_builder)
{
  SPN_ASSERT_STATE_TRANSITION(SPN_PATH_BUILDER_STATE_READY,
                              SPN_PATH_BUILDER_STATE_BUILDING,
                              path_builder);

  // begin the path
  return path_builder->begin(path_builder->impl);
}

spn_result
spn_path_end(spn_path_builder_t path_builder, spn_path_t * path)
{
  SPN_ASSERT_STATE_TRANSITION(SPN_PATH_BUILDER_STATE_BUILDING,
                              SPN_PATH_BUILDER_STATE_READY,
                              path_builder);

  // update path header with proper counts
  return path_builder->end(path_builder->impl, path);
}

//
// PATH SEGMENT OPS
//

static void
spn_path_move_to_1(spn_path_builder_t path_builder, float x0, float y0)
{
  path_builder->curr[0].x = x0;
  path_builder->curr[0].y = y0;
  path_builder->curr[1].x = x0;
  path_builder->curr[1].y = y0;
}

static void
spn_path_move_to_2(spn_path_builder_t path_builder, float x0, float y0, float x1, float y1)
{
  path_builder->curr[0].x = x0;
  path_builder->curr[0].y = y0;
  path_builder->curr[1].x = x1;
  path_builder->curr[1].y = y1;
}

spn_result
spn_path_move_to(spn_path_builder_t path_builder, float x0, float y0)
{
  spn_path_move_to_1(path_builder, x0, y0);

  return SPN_SUCCESS;
}

//
// Simplifying macros
//
// FIXME -- return DEVICE_LOST if a single path fills the ring
//

#define SPN_PB_CN_COORDS_APPEND(_pb, _p, _n, _c) *_pb->cn.coords._p[_n]++ = _c

#define SPN_PB_CN_ACQUIRE(_pb, _p)                                                                 \
  {                                                                                                \
    if (_pb->cn.rem._p == 0)                                                                       \
      {                                                                                            \
        spn_result const err = _pb->_p(path_builder->impl);                                        \
        if (err != SPN_SUCCESS)                                                                    \
          return err;                                                                              \
      }                                                                                            \
    _pb->cn.rem._p -= 1;                                                                           \
  }

//
//
//

spn_result
spn_path_line_to(spn_path_builder_t path_builder, float x1, float y1)
{
  SPN_PB_CN_ACQUIRE(path_builder, line);

  SPN_PB_CN_COORDS_APPEND(path_builder, line, 0, path_builder->curr[0].x);
  SPN_PB_CN_COORDS_APPEND(path_builder, line, 1, path_builder->curr[0].y);
  SPN_PB_CN_COORDS_APPEND(path_builder, line, 2, x1);
  SPN_PB_CN_COORDS_APPEND(path_builder, line, 3, y1);

  spn_path_move_to_1(path_builder, x1, y1);

  return SPN_SUCCESS;
}

spn_result
spn_path_quad_to(spn_path_builder_t path_builder, float x1, float y1, float x2, float y2)
{
  SPN_PB_CN_ACQUIRE(path_builder, quad);

  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 0, path_builder->curr[0].x);
  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 1, path_builder->curr[0].y);
  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 2, x1);
  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 3, y1);
  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 4, x2);
  SPN_PB_CN_COORDS_APPEND(path_builder, quad, 5, y2);

  spn_path_move_to_2(path_builder, x2, y2, x1, y1);

  return SPN_SUCCESS;
}

spn_result
spn_path_quad_smooth_to(spn_path_builder_t path_builder, float x2, float y2)
{
  float const x1 = path_builder->curr[0].x * 2.0f - path_builder->curr[1].x;
  float const y1 = path_builder->curr[0].y * 2.0f - path_builder->curr[1].y;

  return spn_path_quad_to(path_builder, x1, y1, x2, y2);
}

spn_result
spn_path_cubic_to(
  spn_path_builder_t path_builder, float x1, float y1, float x2, float y2, float x3, float y3)
{
  SPN_PB_CN_ACQUIRE(path_builder, cubic);

  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 0, path_builder->curr[0].x);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 1, path_builder->curr[0].y);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 2, x1);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 3, y1);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 4, x2);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 5, y2);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 6, x3);
  SPN_PB_CN_COORDS_APPEND(path_builder, cubic, 7, y3);

  spn_path_move_to_2(path_builder, x3, y3, x2, y2);

  return SPN_SUCCESS;
}

spn_result
spn_path_cubic_smooth_to(spn_path_builder_t path_builder, float x2, float y2, float x3, float y3)
{
  float const x1 = path_builder->curr[0].x * 2.0f - path_builder->curr[1].x;
  float const y1 = path_builder->curr[0].y * 2.0f - path_builder->curr[1].y;

  return spn_path_cubic_to(path_builder, x1, y1, x2, y2, x3, y3);
}

//
//
//

spn_result
spn_path_rat_quad_to(
  spn_path_builder_t path_builder, float x1, float y1, float x2, float y2, float w0)
{
  SPN_PB_CN_ACQUIRE(path_builder, rat_quad);

  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 0, path_builder->curr[0].x);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 1, path_builder->curr[0].y);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 2, x1);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 3, y1);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 4, x2);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 5, y2);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_quad, 6, w0);

  spn_path_move_to_1(path_builder, x2, y2);

  return SPN_SUCCESS;
}

spn_result
spn_path_rat_cubic_to(spn_path_builder_t path_builder,
                      float              x1,
                      float              y1,
                      float              x2,
                      float              y2,
                      float              x3,
                      float              y3,
                      float              w0,
                      float              w1)
{
  SPN_PB_CN_ACQUIRE(path_builder, rat_cubic);

  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 0, path_builder->curr[0].x);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 1, path_builder->curr[0].y);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 2, x1);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 3, y1);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 4, x2);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 5, y2);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 6, x3);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 7, y3);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 8, w0);
  SPN_PB_CN_COORDS_APPEND(path_builder, rat_cubic, 9, w1);

  spn_path_move_to_1(path_builder, x3, y3);

  return SPN_SUCCESS;
}

//
//
//

spn_result
spn_path_ellipse(spn_path_builder_t path_builder, float cx, float cy, float rx, float ry)
{
  //
  // FIXME -- we can implement this with rationals later...
  //

  //
  // Approximate a circle with 4 cubics:
  //
  // http://en.wikipedia.org/wiki/B%C3%A9zier_spline#Approximating_circular_arcs
  //
  spn_path_move_to_1(path_builder, cx, cy + ry);

#define SPN_KAPPA_FLOAT 0.55228474983079339840f  // moar digits!

  float const kx = rx * SPN_KAPPA_FLOAT;
  float const ky = ry * SPN_KAPPA_FLOAT;

  spn_result err;

  err = spn_path_cubic_to(path_builder, cx + kx, cy + ry, cx + rx, cy + ky, cx + rx, cy);

  if (err)
    return err;

  err = spn_path_cubic_to(path_builder, cx + rx, cy - ky, cx + kx, cy - ry, cx, cy - ry);

  if (err)
    return err;

  err = spn_path_cubic_to(path_builder, cx - kx, cy - ry, cx - rx, cy - ky, cx - rx, cy);

  if (err)
    return err;

  err = spn_path_cubic_to(path_builder, cx - rx, cy + ky, cx - kx, cy + ry, cx, cy + ry);
  return err;
}

//
//
//
