// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

namespace component {

// Maximum transfer size we can proxy.
static constexpr size_t kProxyMaxTransferSize = 4096;

/// Header for RPC requests.
struct ProxyRequest {
    uint32_t txid;
    uint32_t proto_id;
};

/// Header for RPC responses.
struct ProxyResponse {
    uint32_t txid;
    zx_status_t status;
};

// ZX_PROTOCOL_PDEV proxy support.
enum class PdevOp {
    GET_MMIO,
    GET_INTERRUPT,
    GET_BTI,
    GET_SMC,
    GET_DEVICE_INFO,
    GET_BOARD_INFO,
};

struct PdevProxyRequest {
    ProxyRequest header;
    PdevOp op;
    uint32_t index;
    uint32_t flags;
};

struct PdevProxyResponse {
    ProxyResponse header;
    zx_off_t offset;
    size_t size;
    uint32_t flags;
    pdev_device_info_t device_info;
    pdev_board_info_t board_info;
};

// Maximum metadata size that can be returned via PDEV_DEVICE_GET_METADATA.
static constexpr uint32_t PROXY_MAX_METADATA_SIZE =
    (kProxyMaxTransferSize - sizeof(PdevProxyResponse));

struct rpc_pdev_metadata_rsp_t {
    PdevProxyResponse pdev;
    uint8_t metadata[PROXY_MAX_METADATA_SIZE];
};

// Maximum I2C transfer is I2C_MAX_TRANSFER_SIZE minus size of largest header.
static constexpr uint32_t I2C_MAX_TRANSFER_SIZE =
    (kProxyMaxTransferSize - (sizeof(PdevProxyRequest) > sizeof(PdevProxyResponse)
                                    ? sizeof(PdevProxyRequest)
                                    : sizeof(PdevProxyResponse)));

// ZX_PROTOCOL_GPIO proxy support.
enum class GpioOp {
    CONFIG_IN,
    CONFIG_OUT,
    SET_ALT_FUNCTION,
    READ,
    WRITE,
    GET_INTERRUPT,
    RELEASE_INTERRUPT,
    SET_POLARITY,
};

struct GpioProxyRequest {
    ProxyRequest header;
    GpioOp op;
    uint32_t flags;
    uint32_t polarity;
    uint64_t alt_function;
    uint8_t value;
};

struct GpioProxyResponse {
    ProxyResponse header;
    uint8_t value;
};

// ZX_PROTOCOL_I2C proxy support.
enum class I2cOp {
    GET_MAX_TRANSFER,
    TRANSACT,
    GET_INTERRUPT,
};

struct I2cProxyRequest {
    ProxyRequest header;
    I2cOp op;
    uint32_t flags;
    i2c_transact_callback transact_cb;
    void* cookie;
    size_t cnt;
};

struct I2cProxyResponse {
    ProxyResponse header;
    size_t max_transfer;
    i2c_transact_callback transact_cb;
    void* cookie;
};

struct I2cRpcOp {
    size_t length;
    bool is_read;
    bool stop;
};

// ZX_PROTOCOL_CLOCK proxy support.
enum class ClockOp {
    ENABLE,
    DISABLE,
};

struct ClockProxyRequest {
    ProxyRequest header;
    ClockOp op;
    uint32_t index;
};

// ZX_PROTOCOL_POWER proxy support.
enum class PowerOp {
    ENABLE,
    DISABLE,
    GET_STATUS,
};

struct PowerProxyRequest {
    ProxyRequest header;
    PowerOp op;
};

struct PowerProxyResponse {
    ProxyResponse header;
    power_domain_status_t status;
};

// ZX_PROTOCOL_SYSMEM proxy support.
enum class SysmemOp {
    CONNECT,
};

struct SysmemProxyRequest {
    ProxyRequest header;
    SysmemOp op;
};

// ZX_PROTOCOL_AMLOGIC_CANVAS proxy support.
enum class AmlogicCanvasOp {
    CONFIG,
    FREE,
};

struct AmlogicCanvasProxyRequest {
    ProxyRequest header;
    AmlogicCanvasOp op;
    size_t offset;
    canvas_info_t info;
    uint8_t canvas_idx;
};

struct AmlogicCanvasProxyResponse {
    ProxyResponse header;
    uint8_t canvas_idx;
};

} // namespace component
