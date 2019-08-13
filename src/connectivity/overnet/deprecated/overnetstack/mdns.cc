// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/overnet/deprecated/overnetstack/mdns.h"

#include <fbl/ref_counted.h>
#include <fuchsia/net/mdns/cpp/fidl.h>

#include "garnet/public/lib/fostr/fidl/fuchsia/net/mdns/formatting.h"
#include "src/connectivity/overnet/deprecated/lib/labels/node_id.h"
#include "src/connectivity/overnet/deprecated/overnetstack/fuchsia_port.h"

namespace overnetstack {

static const char* kServiceName = "_temp_overnet._udp.";

static fuchsia::net::mdns::SubscriberPtr ConnectToSubscriber(
    sys::ComponentContext* component_context, const char* why) {
  auto svc = component_context->svc()->Connect<fuchsia::net::mdns::Subscriber>();
  svc.set_error_handler([why](zx_status_t status) {
    OVERNET_TRACE(ERROR) << why << " mdns subscriber failure: " << zx_status_get_string(status);
  });
  return svc;
}

static fuchsia::net::mdns::PublisherPtr ConnectToPublisher(sys::ComponentContext* component_context,
                                                           const char* why) {
  auto svc = component_context->svc()->Connect<fuchsia::net::mdns::Publisher>();
  svc.set_error_handler([why](zx_status_t status) {
    OVERNET_TRACE(ERROR) << why << " mdns publisher failure: " << zx_status_get_string(status);
  });
  return svc;
}

class MdnsIntroducer::Impl : public fbl::RefCounted<MdnsIntroducer>,
                             public fuchsia::net::mdns::ServiceSubscriber {
 public:
  Impl(UdpNub* nub) : nub_(nub), subscriber_binding_(this) {}

  void Begin(sys::ComponentContext* component_context) {
    std::cerr << "Querying mDNS for overnet services [" << kServiceName << "]\n";
    auto svc = ConnectToSubscriber(component_context, "Introducer");
    fidl::InterfaceHandle<fuchsia::net::mdns::ServiceSubscriber> subscriber_handle;

    subscriber_binding_.Bind(subscriber_handle.NewRequest());
    subscriber_binding_.set_error_handler([this](zx_status_t status) {
      subscriber_binding_.set_error_handler(nullptr);
      subscriber_binding_.Unbind();
    });

    svc->SubscribeToService(kServiceName, std::move(subscriber_handle));
  }

 private:
  void HandleDiscoverOrUpdate(const fuchsia::net::mdns::ServiceInstance& svc, bool update) {
    if (svc.service != kServiceName) {
      std::cout << "Unexpected service name (ignored): " << svc.service << "\n";
      return;
    }
    auto parsed_instance_name = overnet::NodeId::FromString(svc.instance);
    if (parsed_instance_name.is_error()) {
      std::cout << "Failed to parse instance name: " << parsed_instance_name.AsStatus() << "\n";
      return;
    }
    auto instance_id = *parsed_instance_name.get();

    std::vector<overnet::IpAddr> addrs;
    for (const auto& endpoint : svc.endpoints) {
      auto status = ToIpAddr(endpoint);
      if (status.is_error()) {
        std::cout << "Failed to convert address: " << status << "\n";
      } else {
        addrs.emplace_back(std::move(*status));
      }
    }

    nub_->Initiate(std::move(addrs), instance_id);
  }

  static overnet::StatusOr<overnet::IpAddr> ToIpAddr(const fuchsia::net::Endpoint& endpoint) {
    const fuchsia::net::IpAddress& net_addr = endpoint.addr;
    overnet::IpAddr udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    switch (net_addr.Which()) {
      case fuchsia::net::IpAddress::Tag::Invalid:
        return overnet::Status(overnet::StatusCode::INVALID_ARGUMENT, "unknown address type");
      case fuchsia::net::IpAddress::Tag::kIpv4:
        if (!net_addr.is_ipv4()) {
          return overnet::Status(overnet::StatusCode::INVALID_ARGUMENT, "bad ipv4 address");
        }
        udp_addr.ipv4.sin_family = AF_INET;
        udp_addr.ipv4.sin_port = htons(endpoint.port);
        memcpy(&udp_addr.ipv4.sin_addr, net_addr.ipv4().addr.data(),
               sizeof(udp_addr.ipv4.sin_addr));
        return udp_addr;
      case fuchsia::net::IpAddress::Tag::kIpv6:
        if (!net_addr.is_ipv6()) {
          return overnet::Status(overnet::StatusCode::INVALID_ARGUMENT, "bad ipv6 address");
        }
        udp_addr.ipv6.sin6_family = AF_INET6;
        udp_addr.ipv6.sin6_port = htons(endpoint.port);
        memcpy(&udp_addr.ipv6.sin6_addr, net_addr.ipv6().addr.data(),
               sizeof(udp_addr.ipv6.sin6_addr));
        return udp_addr;
    }
    return overnet::Status(overnet::StatusCode::INVALID_ARGUMENT, "bad address family");
  }

  // fuchsia::net::mdns::ServiceSubscriber implementation.
  void OnInstanceDiscovered(fuchsia::net::mdns::ServiceInstance instance,
                            OnInstanceDiscoveredCallback callback) {
    HandleDiscoverOrUpdate(instance, false);
    callback();
  }

  void OnInstanceChanged(fuchsia::net::mdns::ServiceInstance instance,
                         OnInstanceChangedCallback callback) {
    HandleDiscoverOrUpdate(instance, true);
    callback();
  }

  void OnInstanceLost(std::string service, std::string instance, OnInstanceLostCallback callback) {
    callback();
  }

  UdpNub* const nub_;
  fidl::Binding<fuchsia::net::mdns::ServiceSubscriber> subscriber_binding_;
};

MdnsIntroducer::MdnsIntroducer(OvernetApp* app, UdpNub* udp_nub) : app_(app), udp_nub_(udp_nub) {}

overnet::Status MdnsIntroducer::Start() {
  auto impl = fbl::MakeRefCounted<Impl>(udp_nub_);
  impl_ = std::move(impl);
  impl_->Begin(app_->component_context());
  return overnet::Status::Ok();
}

MdnsIntroducer::~MdnsIntroducer() {}

class MdnsAdvertisement::Impl : public fuchsia::net::mdns::PublicationResponder {
 public:
  Impl(sys::ComponentContext* component_context, UdpNub* nub)
      : publisher_(ConnectToPublisher(component_context, "Advertisement")),
        node_id_(nub->node_id()),
        binding_(this),
        port_(nub->port()) {
    std::cerr << "Requesting mDNS advertisement for " << node_id_ << " on port " << nub->port()
              << "\n";
    publisher_->PublishServiceInstance(
        kServiceName, node_id_.ToString(), true, binding_.NewBinding(),
        [node_id = node_id_,
         port = port_](fuchsia::net::mdns::Publisher_PublishServiceInstance_Result result) {
          if (result.is_err()) {
            std::cout << "Advertising " << node_id << " on port " << port
                      << " via mdns gets: " << result.err() << "\n";
          } else {
            std::cout << "Advertising " << node_id << " on port " << port
                      << " via mdns succeeded\n";
          }
        });
  }
  ~Impl() = default;

 private:
  // fuchsia::net::mdns::PublicationResponder implementation.
  void OnPublication(bool query, fidl::StringPtr subtype, OnPublicationCallback callback) {
    callback(subtype->empty() ? std::make_unique<fuchsia::net::mdns::Publication>(
                                    fuchsia::net::mdns::Publication{.port = port_})
                              : nullptr);
  }

  const fuchsia::net::mdns::PublisherPtr publisher_;
  const overnet::NodeId node_id_;
  fidl::Binding<fuchsia::net::mdns::PublicationResponder> binding_;
  const uint16_t port_;
};

MdnsAdvertisement::MdnsAdvertisement(OvernetApp* app, UdpNub* udp_nub)
    : app_(app), udp_nub_(udp_nub) {}

overnet::Status MdnsAdvertisement::Start() {
  impl_.reset(new Impl(app_->component_context(), udp_nub_));
  return overnet::Status::Ok();
}

MdnsAdvertisement::~MdnsAdvertisement() {}

}  // namespace overnetstack
