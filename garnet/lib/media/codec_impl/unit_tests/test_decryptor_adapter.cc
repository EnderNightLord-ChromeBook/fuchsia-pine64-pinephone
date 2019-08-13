// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/media/drm/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/gtest/real_loop_fixture.h>
#include <zircon/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <unordered_map>
#include <variant>

#include <gtest/gtest.h>
#include <sdk/lib/sys/cpp/testing/test_with_environment.h>

#include "lib/media/codec_impl/codec_impl.h"
#include "lib/media/codec_impl/decryptor_adapter.h"

namespace {

constexpr uint64_t kBufferConstraintsVersionOrdinal = 1;
constexpr uint64_t kBufferLifetimeOrdinal = 1;
constexpr uint64_t kStreamLifetimeOrdinal = 1;
constexpr uint32_t kInputPacketSize = 8 * 1024;

auto CreateDecryptorParams() {
  fuchsia::media::drm::DecryptorParams params;
  params.mutable_input_details()->set_format_details_version_ordinal(0);
  return params;
}

auto CreateStreamBufferPartialSettings(
    const fuchsia::media::StreamBufferConstraints& constraints,
    fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) {
  fuchsia::media::StreamBufferPartialSettings settings;

  settings.set_buffer_lifetime_ordinal(kBufferLifetimeOrdinal)
      .set_buffer_constraints_version_ordinal(kBufferConstraintsVersionOrdinal)
      .set_single_buffer_mode(constraints.default_settings().single_buffer_mode())
      .set_packet_count_for_server(constraints.default_settings().packet_count_for_server())
      .set_packet_count_for_client(constraints.default_settings().packet_count_for_client())
      .set_sysmem_token(std::move(token));

  return settings;
}

auto CreateBufferCollectionConstraints(uint32_t cpu_usage) {
  fuchsia::sysmem::BufferCollectionConstraints collection_constraints;

  collection_constraints.usage.cpu = cpu_usage;
  collection_constraints.min_buffer_count_for_camping = 1;
  collection_constraints.has_buffer_memory_constraints = true;
  collection_constraints.buffer_memory_constraints.min_size_bytes = kInputPacketSize;

  // Secure buffers not allowed for test keys
  EXPECT_FALSE(collection_constraints.buffer_memory_constraints.secure_required);

  return collection_constraints;
}

auto CreateInputFormatDetails(const std::string& mode, const fuchsia::media::KeyId& key_id,
                              const std::vector<uint8_t>& init_vector) {
  constexpr uint64_t kFormatDetailsVersionOrdinal = 0;

  fuchsia::media::FormatDetails details;
  details.set_format_details_version_ordinal(kFormatDetailsVersionOrdinal);
  details.mutable_domain()->crypto().encrypted().set_mode(mode).set_key_id(key_id).set_init_vector(
      init_vector);

  return details;
}

const std::map<std::string, std::string> kServices = {
    {"fuchsia.sysmem.Allocator",
     "fuchsia-pkg://fuchsia.com/sysmem_connector#meta/sysmem_connector.cmx"},
};

class ClearTextDecryptorAdapter : public DecryptorAdapter {
 public:
  explicit ClearTextDecryptorAdapter(std::mutex& lock, CodecAdapterEvents* codec_adapter_events)
      : DecryptorAdapter(lock, codec_adapter_events, false) {}

  void set_has_keys(bool has_keys) { has_keys_ = has_keys; }

  std::optional<fuchsia::media::StreamError> Decrypt(const EncryptionParams& params,
                                                     const InputBuffer& input,
                                                     const OutputBuffer& output) override {
    if (!std::holds_alternative<ClearOutputBuffer>(output)) {
      return fuchsia::media::StreamError::DECRYPTOR_UNKNOWN;
    }
    auto& clear_output = std::get<ClearOutputBuffer>(output);

    if (input.data_length != clear_output.data_length) {
      return fuchsia::media::StreamError::DECRYPTOR_UNKNOWN;
    }

    if (!has_keys_) {
      return fuchsia::media::StreamError::DECRYPTOR_NO_KEY;
    }

    std::memcpy(clear_output.data, input.data, input.data_length);

    return std::nullopt;
  }

 private:
  bool has_keys_ = false;
};

}  // namespace

class DecryptorAdapterTest : public sys::testing::TestWithEnvironment {
 protected:
  DecryptorAdapterTest() : random_device_(), prng_(random_device_()) {
    std::unique_ptr<sys::testing::EnvironmentServices> services = CreateServices();

    for (const auto& [service_name, url] : kServices) {
      fuchsia::sys::LaunchInfo launch_info;
      launch_info.url = url;
      services->AddServiceWithLaunchInfo(std::move(launch_info), service_name);
    }

    constexpr char kEnvironment[] = "DecryptorAdapterTest";
    environment_ = CreateNewEnclosingEnvironment(kEnvironment, std::move(services));

    environment_->ConnectToService(allocator_.NewRequest());

    PopulateInputData();

    allocator_.set_error_handler([this](zx_status_t s) { sysmem_error_ = s; });
    decryptor_.set_error_handler([this](zx_status_t s) { decryptor_error_ = s; });
    input_collection_.set_error_handler([this](zx_status_t s) { input_collection_error_ = s; });
    output_collection_.set_error_handler([this](zx_status_t s) { output_collection_error_ = s; });

    decryptor_.events().OnStreamFailed2 =
        fit::bind_member(this, &DecryptorAdapterTest::OnStreamFailed);
    decryptor_.events().OnInputConstraints =
        fit::bind_member(this, &DecryptorAdapterTest::OnInputConstraints);
    decryptor_.events().OnOutputConstraints =
        fit::bind_member(this, &DecryptorAdapterTest::OnOutputConstraints);
    decryptor_.events().OnOutputFormat =
        fit::bind_member(this, &DecryptorAdapterTest::OnOutputFormat);
    decryptor_.events().OnOutputPacket =
        fit::bind_member(this, &DecryptorAdapterTest::OnOutputPacket);
    decryptor_.events().OnFreeInputPacket =
        fit::bind_member(this, &DecryptorAdapterTest::OnFreeInputPacket);
    decryptor_.events().OnOutputEndOfStream =
        fit::bind_member(this, &DecryptorAdapterTest::OnOutputEndOfStream);
  }

  void ConnectDecryptor() {
    fidl::InterfaceHandle<fuchsia::sysmem::Allocator> allocator;

    environment_->ConnectToService(allocator.NewRequest());
    codec_impl_ =
        std::make_unique<CodecImpl>(std::move(allocator), nullptr, dispatcher(), thrd_current(),
                                    CreateDecryptorParams(), decryptor_.NewRequest());
    auto adapter =
        std::make_unique<ClearTextDecryptorAdapter>(codec_impl_->lock(), codec_impl_.get());
    // Grab a non-owning reference to the adapter for test manipulation.
    decryptor_adapter_ = adapter.get();
    codec_impl_->SetCoreCodecAdapter(std::move(adapter));

    codec_impl_->BindAsync([this]() { codec_impl_.reset(); });
  }

  void OnStreamFailed(uint64_t stream_lifetime_ordinal, fuchsia::media::StreamError error) {
    stream_error_ = std::move(error);
  }

  void OnInputConstraints(fuchsia::media::StreamBufferConstraints ic) {
    auto settings = BindBufferCollection(
        input_collection_, fuchsia::sysmem::cpuUsageWrite | fuchsia::sysmem::cpuUsageWriteOften,
        ic);
    input_collection_->WaitForBuffersAllocated(
        [this](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 info) {
          ASSERT_EQ(status, ZX_OK);
          input_buffer_info_ = std::move(info);
        });

    input_collection_->Sync([this, settings = std::move(settings)]() mutable {
      decryptor_->SetInputBufferPartialSettings(std::move(settings));
    });

    input_constraints_ = std::move(ic);
  }

  void OnOutputConstraints(fuchsia::media::StreamOutputConstraints oc) {
    auto settings = BindBufferCollection(
        output_collection_, fuchsia::sysmem::cpuUsageRead | fuchsia::sysmem::cpuUsageReadOften,
        oc.buffer_constraints());
    output_collection_->WaitForBuffersAllocated(
        [this](zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 info) {
          ASSERT_EQ(status, ZX_OK);
          output_buffer_info_ = std::move(info);
        });

    output_collection_->Sync([this, settings = std::move(settings)]() mutable {
      decryptor_->SetOutputBufferPartialSettings(std::move(settings));
      decryptor_->CompleteOutputBufferPartialSettings(kBufferLifetimeOrdinal);
    });

    output_constraints_ = std::move(oc);
  }

  void OnOutputFormat(fuchsia::media::StreamOutputFormat of) { output_format_ = std::move(of); }

  void OnOutputPacket(fuchsia::media::Packet packet, bool error_before, bool error_during) {
    EXPECT_FALSE(error_before);
    EXPECT_FALSE(error_during);
    auto header = fidl::Clone(packet.header());
    output_data_.emplace_back(ExtractPayloadData(std::move(packet)));
    decryptor_->RecycleOutputPacket(std::move(header));
  }

  void OnFreeInputPacket(fuchsia::media::PacketHeader header) {
    ASSERT_TRUE(header.has_packet_index());
    FreePacket(header.packet_index());
    if (end_of_stream_set_) {
      return;
    }
    PumpInput();
  }

  void OnOutputEndOfStream(uint64_t stream_lifetime_ordinal, bool error_before) {
    end_of_stream_reached_ = true;
  }

  void PopulateInputData() {
    constexpr size_t kNumInputPackets = 50;
    std::uniform_int_distribution<uint8_t> dist;

    input_data_.reserve(kNumInputPackets);
    for (size_t i = 0; i < kNumInputPackets; i++) {
      std::vector<uint8_t> v(kInputPacketSize);
      std::generate(v.begin(), v.end(), [this, &dist]() { return dist(prng_); });
      input_data_.emplace_back(std::move(v));
    }
    input_iter_ = input_data_.begin();
  }

  fuchsia::media::StreamBufferPartialSettings BindBufferCollection(
      fuchsia::sysmem::BufferCollectionPtr& collection, uint32_t cpu_usage,
      const fuchsia::media::StreamBufferConstraints& constraints) {
    fuchsia::sysmem::BufferCollectionTokenPtr client_token;
    allocator_->AllocateSharedCollection(client_token.NewRequest());

    fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> decryptor_token;
    client_token->Duplicate(std::numeric_limits<uint32_t>::max(), decryptor_token.NewRequest());

    allocator_->BindSharedCollection(client_token.Unbind(), collection.NewRequest());
    collection->SetConstraints(true, CreateBufferCollectionConstraints(cpu_usage));

    return CreateStreamBufferPartialSettings(constraints, std::move(decryptor_token));
  }

  fuchsia::media::Packet CreateInputPacket(const std::vector<uint8_t>& data) {
    fuchsia::media::Packet packet;
    static uint64_t timestamp_ish = 42;
    uint32_t packet_index;
    uint32_t buffer_index;
    AllocatePacket(&packet_index, &buffer_index);

    auto& vmo = input_buffer_info_->buffers[buffer_index].vmo;
    uint64_t offset = input_buffer_info_->buffers[buffer_index].vmo_usable_start;
    size_t size = data.size();

    // Since test code, no particular reason to bother with mapping.
    auto status = vmo.write(data.data(), offset, size);

    EXPECT_EQ(status, ZX_OK);

    packet.mutable_header()
        ->set_buffer_lifetime_ordinal(kBufferLifetimeOrdinal)
        .set_packet_index(packet_index);
    packet.set_buffer_index(buffer_index)
        .set_stream_lifetime_ordinal(kStreamLifetimeOrdinal)
        .set_start_offset(0)
        .set_valid_length_bytes(size)
        .set_timestamp_ish(timestamp_ish++)
        .set_start_access_unit(true);

    return packet;
  }

  std::vector<uint8_t> ExtractPayloadData(fuchsia::media::Packet packet) {
    std::vector<uint8_t> data;

    uint32_t buffer_index = packet.buffer_index();
    uint32_t offset = packet.start_offset();
    uint32_t size = packet.valid_length_bytes();

    EXPECT_TRUE(buffer_index < output_buffer_info_->buffer_count);

    const auto& buffer = output_buffer_info_->buffers[buffer_index];

    data.resize(size);
    auto status = buffer.vmo.read(data.data(), offset, size);
    EXPECT_EQ(status, ZX_OK);

    return data;
  }

  bool HasFreePackets() { return !free_packets_.empty(); }

  void ConfigureInputPackets() {
    ASSERT_TRUE(input_buffer_info_);

    auto buffer_count = input_buffer_info_->buffer_count;
    std::vector<uint32_t> buffers;
    std::vector<uint32_t> packets;
    for (uint32_t i = 0; i < buffer_count; i++) {
      buffers.emplace_back(i);
      packets.emplace_back(i);
    }
    // Shuffle the packet indexes so that they don't align with the buffer indexes
    std::shuffle(packets.begin(), packets.end(), prng_);

    for (uint32_t i = 0; i < buffer_count; i++) {
      free_packets_.emplace(packets[i], buffers[i]);
    }
  }

  void AllocatePacket(uint32_t* packet_index, uint32_t* buffer_index) {
    ASSERT_TRUE(HasFreePackets());
    ASSERT_TRUE(packet_index);
    ASSERT_TRUE(buffer_index);

    auto node = free_packets_.extract(free_packets_.begin());
    *packet_index = node.key();
    *buffer_index = node.mapped();
    used_packets_.insert(std::move(node));
  }

  void FreePacket(uint32_t packet_index) {
    free_packets_.insert(used_packets_.extract(packet_index));
  }

  void PumpInput() {
    while (input_iter_ != input_data_.end() && HasFreePackets()) {
      decryptor_->QueueInputPacket(CreateInputPacket(*input_iter_));
      input_iter_++;
    }
    if (input_iter_ == input_data_.end() && !end_of_stream_set_) {
      decryptor_->QueueInputEndOfStream(kStreamLifetimeOrdinal);
      end_of_stream_set_ = true;
    }
  }

  void AssertNoChannelErrors() {
    ASSERT_FALSE(decryptor_error_) << "Decryptor error = " << *decryptor_error_;
    ASSERT_FALSE(sysmem_error_) << "Sysmem error = " << *sysmem_error_;
    ASSERT_FALSE(input_collection_error_)
        << "Input BufferCollection error = " << *input_collection_error_;
    ASSERT_FALSE(output_collection_error_)
        << "Output BufferCollection error = " << *output_collection_error_;
  }

  std::unique_ptr<sys::testing::EnclosingEnvironment> environment_;
  fuchsia::media::StreamProcessorPtr decryptor_;
  fuchsia::sysmem::AllocatorPtr allocator_;
  std::unique_ptr<CodecImpl> codec_impl_;
  ClearTextDecryptorAdapter* decryptor_adapter_;

  using DataSet = std::vector<std::vector<uint8_t>>;
  DataSet input_data_;
  DataSet output_data_;

  std::optional<fuchsia::media::StreamBufferConstraints> input_constraints_;
  std::optional<fuchsia::media::StreamOutputConstraints> output_constraints_;
  std::optional<fuchsia::media::StreamOutputFormat> output_format_;
  bool end_of_stream_set_ = false;
  bool end_of_stream_reached_ = false;
  DataSet::const_iterator input_iter_;

  fuchsia::sysmem::BufferCollectionPtr input_collection_;
  fuchsia::sysmem::BufferCollectionPtr output_collection_;

  std::optional<fuchsia::sysmem::BufferCollectionInfo_2> input_buffer_info_;
  std::optional<fuchsia::sysmem::BufferCollectionInfo_2> output_buffer_info_;

  std::optional<fuchsia::media::StreamError> stream_error_;
  std::optional<zx_status_t> sysmem_error_;
  std::optional<zx_status_t> decryptor_error_;
  std::optional<zx_status_t> input_collection_error_;
  std::optional<zx_status_t> output_collection_error_;

  using PacketMap = std::unordered_map<uint32_t /*packet_index*/, uint32_t /*buffer_index*/>;

  PacketMap free_packets_;
  PacketMap used_packets_;

  std::random_device random_device_;
  std::mt19937 prng_;
};

TEST_F(DecryptorAdapterTest, ClearTextDecrypt) {
  ConnectDecryptor();
  decryptor_adapter_->set_has_keys(true);

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return input_buffer_info_.has_value(); }));

  AssertNoChannelErrors();
  ASSERT_TRUE(input_buffer_info_);

  ConfigureInputPackets();

  decryptor_->QueueInputFormatDetails(
      kStreamLifetimeOrdinal, CreateInputFormatDetails("clear", fuchsia::media::KeyId{}, {}));

  PumpInput();

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return end_of_stream_reached_; }));

  AssertNoChannelErrors();

  EXPECT_TRUE(input_constraints_);
  EXPECT_TRUE(output_constraints_);
  EXPECT_TRUE(output_format_);

  ASSERT_TRUE(end_of_stream_reached_);
  // ClearText decryptor just copies data across
  EXPECT_EQ(output_data_, input_data_);
}

TEST_F(DecryptorAdapterTest, NoKeys) {
  ConnectDecryptor();
  decryptor_adapter_->set_has_keys(false);
  decryptor_->EnableOnStreamFailed();

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return input_buffer_info_.has_value(); }));

  AssertNoChannelErrors();
  ASSERT_TRUE(input_buffer_info_);

  ConfigureInputPackets();

  decryptor_->QueueInputFormatDetails(
      kStreamLifetimeOrdinal, CreateInputFormatDetails("clear", fuchsia::media::KeyId{}, {}));

  PumpInput();

  EXPECT_TRUE(RunLoopWithTimeoutOrUntil([this]() { return stream_error_.has_value(); }));

  AssertNoChannelErrors();

  ASSERT_TRUE(stream_error_.has_value());
  EXPECT_EQ(*stream_error_, fuchsia::media::StreamError::DECRYPTOR_NO_KEY);
}
