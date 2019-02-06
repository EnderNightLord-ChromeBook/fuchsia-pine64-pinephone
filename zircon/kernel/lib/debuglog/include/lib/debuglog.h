// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <list.h>
#include <fbl/mutex.h>
#include <kernel/event.h>
#include <kernel/lockdep.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

typedef struct dlog dlog_t;
typedef struct dlog_header dlog_header_t;
typedef struct dlog_record dlog_record_t;
typedef struct dlog_reader dlog_reader_t;

// TODO: Convert to full class
struct dlog {
    constexpr dlog(uint8_t* data_ptr) : data(data_ptr) {}

    DECLARE_SPINLOCK(dlog) lock;

    size_t head = 0;
    size_t tail = 0;

    uint8_t* data;

    bool panic = false;

    Event event{EVENT_FLAG_AUTOUNSIGNAL};

    DECLARE_MUTEX(dlog) readers_lock;

    struct list_node readers TA_GUARDED(readers_lock) = LIST_INITIAL_VALUE(this->readers);
};

struct dlog_reader {
    struct list_node node;

    dlog_t* log;
    size_t tail;

    void (*notify)(void* cookie);
    void *cookie;
};

#define DLOG_HDR_SET(fifosize, readsize) \
    ((((readsize) & 0xFFF) << 12) | ((fifosize) & 0xFFF))

#define DLOG_HDR_GET_FIFOLEN(n)   ((n) & 0xFFF)
#define DLOG_HDR_GET_READLEN(n)  (((n) >> 12) & 0xFFF)

#define DLOG_MIN_RECORD          (32u)
#define DLOG_MAX_DATA            (224u)
#define DLOG_MAX_RECORD          (DLOG_MIN_RECORD + DLOG_MAX_DATA)

struct dlog_header {
    uint32_t header;
    uint16_t datalen;
    uint16_t flags;
    uint64_t timestamp;
    uint64_t pid;
    uint64_t tid;
};

struct dlog_record {
    dlog_header_t hdr;
    char data[DLOG_MAX_DATA];
};

static_assert(sizeof(dlog_header_t) == DLOG_MIN_RECORD, "");
static_assert(sizeof(dlog_record_t) == DLOG_MAX_RECORD, "");

void dlog_reader_init(dlog_reader_t* rdr, void (*notify)(void*), void* cookie);
void dlog_reader_destroy(dlog_reader_t* rdr);
zx_status_t dlog_write(uint32_t flags, const void* ptr, size_t len);
zx_status_t dlog_read(dlog_reader_t* rdr, uint32_t flags, void* ptr, size_t len, size_t* actual);

// used by sys_debug_write()
void dlog_serial_write(const char* data, size_t len);

// bluescreen_init should be called at the "start" of a fatal fault or
// panic to ensure that the fault output (via kernel printf/dprintf)
// is captured or displayed to the user
void dlog_bluescreen_init();

// bluescreen_halt should be called from inside platform_halt to allow
// the bluescreen service to finalize the display of the panic data
// (for example, creating a qrcode)
void dlog_bluescreen_halt();

// Shutdown the debuglog subsystem.
//
// Note: This may block for an extended period of time.
void dlog_shutdown();

void dlog_bypass_init_early();
void dlog_bypass_init();
bool dlog_bypass();
