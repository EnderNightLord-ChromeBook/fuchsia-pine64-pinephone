// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_packets.h"

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/bredr_connection_request.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci_constants.h"

namespace bt {
namespace testing {

namespace {

hci::SynchronousConnectionParameters ConnectionParametersToLe(
    hci::SynchronousConnectionParameters params) {
  params.transmit_bandwidth = htole32(params.transmit_bandwidth);
  params.receive_bandwidth = htole32(params.receive_bandwidth);
  params.transmit_coding_format.company_id = htole16(params.transmit_coding_format.company_id);
  params.transmit_coding_format.vendor_codec_id =
      htole16(params.transmit_coding_format.vendor_codec_id);
  params.receive_coding_format.company_id = htole16(params.receive_coding_format.company_id);
  params.receive_coding_format.vendor_codec_id =
      htole16(params.receive_coding_format.vendor_codec_id);
  params.transmit_codec_frame_size_bytes = htole16(params.transmit_codec_frame_size_bytes);
  params.receive_codec_frame_size_bytes = htole16(params.receive_codec_frame_size_bytes);
  params.input_bandwidth = htole32(params.input_bandwidth);
  params.output_bandwidth = htole32(params.output_bandwidth);
  params.input_coding_format.company_id = htole16(params.input_coding_format.company_id);
  params.input_coding_format.vendor_codec_id = htole16(params.input_coding_format.vendor_codec_id);
  params.output_coding_format.company_id = htole16(params.output_coding_format.company_id);
  params.output_coding_format.vendor_codec_id =
      htole16(params.output_coding_format.vendor_codec_id);
  params.max_latency_ms = htole16(params.max_latency_ms);
  return params;
}

}  // namespace

// clang-format off
#define COMMAND_STATUS_RSP(opcode, statuscode)                       \
CreateStaticByteBuffer(hci::kCommandStatusEventCode, 0x04,         \
                                (statuscode), 0xF0,                 \
                                LowerBits((opcode)), UpperBits((opcode)))
// clang-format on

DynamicByteBuffer CommandCompletePacket(hci::OpCode opcode, hci::StatusCode status) {
  return DynamicByteBuffer(StaticByteBuffer(hci::kCommandCompleteEventCode,
                                            0x04,  // size
                                            0x01,  // Num HCI command packets
                                            LowerBits(opcode), UpperBits(opcode),  // Op code
                                            status));
}

DynamicByteBuffer AcceptConnectionRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kAcceptConnectionRequest), UpperBits(hci::kAcceptConnectionRequest),
      0x07,                                                  // parameter_total_size (7 bytes)
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],  // peer address
      0x00                                                   // role (become master)
      ));
}

DynamicByteBuffer AuthenticationRequestedPacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kAuthenticationRequested), UpperBits(hci::kAuthenticationRequested),
      0x02,                             // parameter_total_size (2 bytes)
      LowerBits(conn), UpperBits(conn)  // Connection_Handle
      ));
}

DynamicByteBuffer ConnectionRequestPacket(DeviceAddress address, hci::LinkType link_type) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kConnectionRequestEventCode,
      0x0A,  // parameter_total_size (10 byte payload)
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],  // peer address
      0x00, 0x1F, 0x00,                                      // class_of_device (unspecified)
      link_type                                              // link_type
      ));
}

DynamicByteBuffer CreateConnectionPacket(DeviceAddress address) {
  auto addr = address.value().bytes();
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kCreateConnection), UpperBits(hci::kCreateConnection),
      0x0d,                                                  // parameter_total_size (13 bytes)
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],  // peer address
      LowerBits(hci::kEnableAllPacketTypes),                 // allowable packet types
      UpperBits(hci::kEnableAllPacketTypes),                 // allowable packet types
      0x02,                                                  // page_scan_repetition_mode (R2)
      0x00,                                                  // reserved
      0x00, 0x00,                                            // clock_offset
      0x00                                                   // allow_role_switch (don't)
      ));
}

DynamicByteBuffer ConnectionCompletePacket(DeviceAddress address, hci::ConnectionHandle conn) {
  auto addr = address.value().bytes();
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kConnectionCompleteEventCode,
      0x0B,                              // parameter_total_size (11 byte payload)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],  // peer address
      0x01,                                                  // link_type (ACL)
      0x00                                                   // encryption not enabled
      ));
}

DynamicByteBuffer DisconnectPacket(hci::ConnectionHandle conn, hci::StatusCode reason) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kDisconnect), UpperBits(hci::kDisconnect),
      0x03,                              // parameter_total_size (3 bytes)
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      reason                             // Reason
      ));
}

DynamicByteBuffer DisconnectStatusResponsePacket() {
  return DynamicByteBuffer(COMMAND_STATUS_RSP(hci::kDisconnect, hci::StatusCode::kSuccess));
}

DynamicByteBuffer DisconnectionCompletePacket(hci::ConnectionHandle conn, hci::StatusCode reason) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kDisconnectionCompleteEventCode,
      0x04,                              // parameter_total_size (4 bytes)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      reason                             // Reason
      ));
}

DynamicByteBuffer EncryptionChangeEventPacket(hci::StatusCode status_code,
                                              hci::ConnectionHandle conn,
                                              hci::EncryptionStatus encryption_enabled) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kEncryptionChangeEventCode,
      0x04,                                     // parameter_total_size (4 bytes)
      status_code,                              // status
      LowerBits(conn), UpperBits(conn),         // Little-Endian Connection_Handle
      static_cast<uint8_t>(encryption_enabled)  // Encryption_Enabled
      ));
}

DynamicByteBuffer EnhancedAcceptSynchronousConnectionRequestPacket(
    DeviceAddress peer_address, hci::SynchronousConnectionParameters params) {
  StaticByteBuffer<sizeof(hci::CommandHeader) +
                   sizeof(hci::EnhancedAcceptSynchronousConnectionRequestCommandParams)>
      buffer;
  auto& header = buffer.AsMutable<hci::CommandHeader>();
  header.opcode = htole16(hci::kEnhancedAcceptSynchronousConnectionRequest);
  header.parameter_total_size =
      sizeof(hci::EnhancedAcceptSynchronousConnectionRequestCommandParams);

  buffer.mutable_view(sizeof(hci::CommandHeader)).AsMutable<DeviceAddressBytes>() =
      peer_address.value();

  auto& payload = buffer.mutable_view(sizeof(hci::CommandHeader) + sizeof(DeviceAddressBytes))
                      .AsMutable<hci::SynchronousConnectionParameters>();
  payload = ConnectionParametersToLe(params);
  return DynamicByteBuffer(buffer);
}

DynamicByteBuffer EnhancedSetupSynchronousConnectionPacket(
    hci::ConnectionHandle conn, hci::SynchronousConnectionParameters params) {
  StaticByteBuffer<sizeof(hci::CommandHeader) +
                   sizeof(hci::EnhancedSetupSynchronousConnectionCommandParams)>
      buffer;
  auto& header = buffer.AsMutable<hci::CommandHeader>();
  header.opcode = htole16(hci::kEnhancedSetupSynchronousConnection);
  header.parameter_total_size = sizeof(hci::EnhancedSetupSynchronousConnectionCommandParams);

  buffer.mutable_view(sizeof(hci::CommandHeader)).AsMutable<hci::ConnectionHandle>() =
      htole16(conn);

  auto& payload = buffer.mutable_view(sizeof(hci::CommandHeader) + sizeof(hci::ConnectionHandle))
                      .AsMutable<hci::SynchronousConnectionParameters>();
  payload = ConnectionParametersToLe(params);
  return DynamicByteBuffer(buffer);
}

DynamicByteBuffer NumberOfCompletedPacketsPacket(hci::ConnectionHandle conn, uint16_t num_packets) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      0x13, 0x05,  // Number Of Completed Packet HCI event header, parameters length
      0x01,        // Number of handles
      LowerBits(conn), UpperBits(conn), LowerBits(num_packets), UpperBits(num_packets)));
}

DynamicByteBuffer CommandStatusPacket(hci::OpCode op_code, hci::StatusCode status_code) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci::kCommandStatusEventCode,
      0x04,  // parameter size (4 bytes)
      status_code,
      0xF0,  // number of HCI command packets allowed to be sent to controller (240)
      LowerBits(op_code), UpperBits(op_code)));
}

DynamicByteBuffer RemoteNameRequestPacket(DeviceAddress address) {
  auto addr = address.value().bytes();
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kRemoteNameRequest), UpperBits(hci::kRemoteNameRequest),
      0x0a,                                                  // parameter_total_size (10 bytes)
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],  // peer address
      0x00,                                                  // page_scan_repetition_mode (R0)
      0x00,                                                  // reserved
      0x00, 0x00                                             // clock_offset
      ));
}

DynamicByteBuffer RemoteNameRequestCompletePacket(DeviceAddress address, const std::string& name) {
  auto addr = address.value().bytes();
  auto event = DynamicByteBuffer(sizeof(hci::EventHeader) +
                                 sizeof(hci::RemoteNameRequestCompleteEventParams));
  event.SetToZeros();
  const StaticByteBuffer header(hci::kRemoteNameRequestCompleteEventCode,
                                0xff,                       // parameter_total_size (255)
                                hci::StatusCode::kSuccess,  // status
                                addr[0], addr[1], addr[2], addr[3], addr[4],
                                addr[5]  // peer address
  );
  header.Copy(&event);
  event.Write(reinterpret_cast<const uint8_t*>(name.data()), name.size(), header.size());
  return event;
}

DynamicByteBuffer ReadRemoteVersionInfoPacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kReadRemoteVersionInfo), UpperBits(hci::kReadRemoteVersionInfo),
      0x02,                             // Parameter_total_size (2 bytes)
      LowerBits(conn), UpperBits(conn)  // Little-Endian Connection_handle
      ));
}

DynamicByteBuffer ReadRemoteVersionInfoCompletePacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kReadRemoteVersionInfoCompleteEventCode,
      0x08,                              // parameter_total_size (8 bytes)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      hci::HCIVersion::k4_2,             // lmp_version
      0xE0, 0x00,                        // manufacturer_name (Google)
      0xAD, 0xDE                         // lmp_subversion (anything)
      ));
}

DynamicByteBuffer ReadRemoteSupportedFeaturesPacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kReadRemoteSupportedFeatures), UpperBits(hci::kReadRemoteSupportedFeatures),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Little-Endian Connection_handle
      UpperBits(conn)));
}

DynamicByteBuffer ReadRemoteSupportedFeaturesCompletePacket(hci::ConnectionHandle conn,
                                                            bool extended_features) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kReadRemoteSupportedFeaturesCompleteEventCode,
      0x0B,                              // parameter_total_size (11 bytes)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      0xFF, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, (extended_features ? 0x80 : 0x00)
      // lmp_features
      // Set: 3 slot packets, 5 slot packets, Encryption, Timing Accuracy,
      // Role Switch, Hold Mode, Sniff Mode, LE Supported
      // Extended Features if enabled
      ));
}

DynamicByteBuffer RejectSynchronousConnectionRequest(DeviceAddress address,
                                                     hci::StatusCode status_code) {
  auto addr_bytes = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(LowerBits(hci::kRejectSynchronousConnectionRequest),
                                            UpperBits(hci::kRejectSynchronousConnectionRequest),
                                            0x07,  // parameter total size
                                            addr_bytes[0], addr_bytes[1], addr_bytes[2],
                                            addr_bytes[3], addr_bytes[4],
                                            addr_bytes[5],  // peer address
                                            status_code     // reason
                                            ));
}
DynamicByteBuffer SetConnectionEncryption(hci::ConnectionHandle conn, bool enable) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci::kSetConnectionEncryption), UpperBits(hci::kSetConnectionEncryption),
      0x03,  // parameter total size (3 bytes)
      LowerBits(conn), UpperBits(conn), static_cast<uint8_t>(enable)));
}

DynamicByteBuffer SynchronousConnectionCompletePacket(hci::ConnectionHandle conn,
                                                      DeviceAddress address,
                                                      hci::LinkType link_type,
                                                      hci::StatusCode status) {
  auto addr_bytes = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci::kSynchronousConnectionCompleteEventCode,
      0x11,  // parameter_total_size (17 bytes)
      status, LowerBits(conn), UpperBits(conn), addr_bytes[0], addr_bytes[1], addr_bytes[2],
      addr_bytes[3], addr_bytes[4], addr_bytes[5], link_type,  // peer address
      0x00,                                                    // transmission interval
      0x00,                                                    // retransmission window
      0x00, 0x00,                                              // rx packet length
      0x00, 0x00,                                              // tx packet length
      0x00                                                     // coding format
      ));
}

DynamicByteBuffer LEReadRemoteFeaturesPacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kLEReadRemoteFeatures), UpperBits(hci::kLEReadRemoteFeatures),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Little-Endian Connection_handle
      UpperBits(conn)));
}

DynamicByteBuffer LEReadRemoteFeaturesCompletePacket(hci::ConnectionHandle conn,
                                                     hci::LESupportedFeatures le_features) {
  const BufferView features(&le_features, sizeof(le_features));
  return DynamicByteBuffer(StaticByteBuffer(hci::kLEMetaEventCode,
                                            0x0c,  // parameter total size (12 bytes)
                                            hci::kLEReadRemoteFeaturesCompleteSubeventCode,
                                            hci::StatusCode::kSuccess,  // status
                                            // Little-Endian connection handle
                                            LowerBits(conn), UpperBits(conn),
                                            // bit mask of LE features
                                            features[0], features[1], features[2], features[3],
                                            features[4], features[5], features[6], features[7]));
}

DynamicByteBuffer LEStartEncryptionPacket(hci::ConnectionHandle conn, uint64_t random_number,
                                          uint16_t encrypted_diversifier, UInt128 ltk) {
  const BufferView rand(&random_number, sizeof(random_number));
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci::kLEStartEncryption), UpperBits(hci::kLEStartEncryption),
                       0x1c,                              // parameter total size (28 bytes)
                       LowerBits(conn), UpperBits(conn),  // Connection_handle
                       rand[0], rand[1], rand[2], rand[3], rand[4], rand[5], rand[6], rand[7],
                       LowerBits(encrypted_diversifier), UpperBits(encrypted_diversifier),
                       // LTK
                       ltk[0], ltk[1], ltk[2], ltk[3], ltk[4], ltk[5], ltk[6], ltk[7], ltk[8],
                       ltk[9], ltk[10], ltk[11], ltk[12], ltk[13], ltk[14], ltk[15]));
}

DynamicByteBuffer ReadRemoteExtended1Packet(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kReadRemoteExtendedFeatures), UpperBits(hci::kReadRemoteExtendedFeatures),
      0x03,             // parameter_total_size (3 bytes)
      LowerBits(conn),  // Little-Endian Connection_handle
      UpperBits(conn),
      0x01  // page_number (1)
      ));
}

DynamicByteBuffer ReadRemoteExtended1CompletePacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,                              // parameter_total_size (13 bytes)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      0x01,                              // page_number
      0x03,                              // max_page_number (3 pages)
      0x0F, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
      // lmp_features (page 1)
      // Set: Secure Simple Pairing (Host Support), LE Supported (Host),
      //  SimultaneousLEAndBREDR, Secure Connections (Host Support)
      ));
}

DynamicByteBuffer ReadRemoteExtended2Packet(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      LowerBits(hci::kReadRemoteExtendedFeatures), UpperBits(hci::kReadRemoteExtendedFeatures),
      0x03,                              // parameter_total_size (3 bytes)
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      0x02                               // page_number (2)
      ));
}

DynamicByteBuffer ReadRemoteExtended2CompletePacket(hci::ConnectionHandle conn) {
  return DynamicByteBuffer(CreateStaticByteBuffer(
      hci::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,                              // parameter_total_size (13 bytes)
      hci::StatusCode::kSuccess,         // status
      LowerBits(conn), UpperBits(conn),  // Little-Endian Connection_handle
      0x02,                              // page_number
      0x03,                              // max_page_number (3 pages)
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0xFF, 0x00
      // lmp_features  - All the bits should be ignored.
      ));
}

DynamicByteBuffer BcmAclPriorityPacket(hci::ConnectionHandle conn, l2cap::AclPriority priority) {
  constexpr hci::OpCode op_code = hci::VendorOpCode(0x011A);
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(op_code), UpperBits(op_code),  // Op code
      0x04,                                    // Size

      LowerBits(conn),
      UpperBits(conn),                                        // Little-Endian Connection_handle
      priority == l2cap::AclPriority::kNormal ? 0x00 : 0x01,  // priority
      priority == l2cap::AclPriority::kSource ? 0x00 : 0x01   // direction
      ));
}

}  // namespace testing
}  // namespace bt
