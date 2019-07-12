// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"fmt"
	"strings"
	"syscall/zx"
	"testing"
	"time"

	"fidl/fuchsia/hardware/ethernet"
	"fidl/fuchsia/net"
	"fidl/fuchsia/net/stack"
	"fidl/fuchsia/netstack"
	ethernetext "fidlext/fuchsia/hardware/ethernet"

	"netstack/dhcp"
	"netstack/dns"
	"netstack/fidlconv"
	"netstack/link/eth"
	"netstack/routes"
	"netstack/util"

	"github.com/google/go-cmp/cmp"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/header"
	"github.com/google/netstack/tcpip/network/arp"
	"github.com/google/netstack/tcpip/network/ipv4"
	"github.com/google/netstack/tcpip/network/ipv6"
	tcpipstack "github.com/google/netstack/tcpip/stack"
)

const (
	testDeviceName string        = "testdevice"
	testTopoPath   string        = "/fake/ethernet/device"
	testV4Address  tcpip.Address = tcpip.Address("\xc0\xa8\x2a\x10")
	testV6Address  tcpip.Address = tcpip.Address("\xc0\xa8\x2a\x10\xc0\xa8\x2a\x10\xc0\xa8\x2a\x10\xc0\xa8\x2a\x10")
)

func TestNicName(t *testing.T) {
	ns := newNetstack(t)

	eth := deviceForAddEth(ethernet.Info{}, t)
	ifs, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName}, &eth)
	if err != nil {
		t.Fatal(err)
	}
	ifs.mu.Lock()
	if ifs.mu.name != testDeviceName {
		t.Errorf("ifs.mu.name = %v, want = %v", ifs.mu.name, testDeviceName)
	}
	ifs.mu.Unlock()
}

func TestNotStartedByDefault(t *testing.T) {
	ns := newNetstack(t)

	startCalled := false
	eth := deviceForAddEth(ethernet.Info{}, t)
	eth.StartImpl = func() (int32, error) {
		startCalled = true
		return int32(zx.ErrOk), nil
	}

	if _, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName}, &eth); err != nil {
		t.Fatal(err)
	}

	if startCalled {
		t.Error("expected no calls to ethernet.Device.Start by addEth")
	}
}

func TestMulticastPromiscuousModeEnabledByDefault(t *testing.T) {
	ns := newNetstack(t)

	multicastPromiscuousModeEnabled := false
	eth := deviceForAddEth(ethernet.Info{}, t)
	eth.ConfigMulticastSetPromiscuousModeImpl = func(enabled bool) (int32, error) {
		multicastPromiscuousModeEnabled = enabled
		return int32(zx.ErrOk), nil
	}

	if _, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName}, &eth); err != nil {
		t.Fatal(err)
	}

	if !multicastPromiscuousModeEnabled {
		t.Error("expected a call to ConfigMulticastSetPromiscuousMode(true) by addEth")
	}
}

func TestDhcpConfiguration(t *testing.T) {
	ns := newNetstack(t)

	ipAddressConfig := netstack.IpAddressConfig{}
	ipAddressConfig.SetDhcp(true)

	d := deviceForAddEth(ethernet.Info{}, t)
	d.StopImpl = func() error { return nil }
	ifs, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName, IpAddressConfig: ipAddressConfig}, &d)
	if err != nil {
		t.Fatal(err)
	}

	ifs.mu.Lock()
	if ifs.mu.dhcp.Client == nil {
		t.Error("no dhcp client")
	}

	if ifs.mu.dhcp.enabled {
		t.Error("expected dhcp to be disabled")
	}

	if ifs.mu.dhcp.running() {
		t.Error("expected dhcp client to be stopped initially")
	}

	ifs.setDHCPStatusLocked(true)
	ifs.mu.Unlock()

	ifs.eth.Up()

	ifs.mu.Lock()
	if !ifs.mu.dhcp.enabled {
		t.Error("expected dhcp to be enabled")
	}

	if !ifs.mu.dhcp.running() {
		t.Error("expected dhcp client to be running")
	}
	ifs.mu.Unlock()

	ifs.eth.Down()

	ifs.mu.Lock()
	if ifs.mu.dhcp.running() {
		t.Error("expected dhcp client to be stopped on eth down")
	}
	if !ifs.mu.dhcp.enabled {
		t.Error("expected dhcp configuration to be preserved on eth down")
	}
	ifs.mu.Unlock()

	ifs.eth.Up()

	ifs.mu.Lock()
	if !ifs.mu.dhcp.running() {
		t.Error("expected dhcp client to be running on eth restart")
	}
	if !ifs.mu.dhcp.enabled {
		t.Error("expected dhcp configuration to be preserved on eth restart")
	}
	ifs.mu.Unlock()
}

func TestUniqueFallbackNICNames(t *testing.T) {
	ns := newNetstack(t)

	d1 := deviceForAddEth(ethernet.Info{}, t)
	ifs1, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{}, &d1)
	if err != nil {
		t.Fatal(err)
	}

	d2 := deviceForAddEth(ethernet.Info{}, t)
	ifs2, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{}, &d2)
	if err != nil {
		t.Fatal(err)
	}
	if ifs1.mu.name == ifs2.mu.name {
		t.Fatalf("got (%+v).Name == (%+v).Name, want non-equal", ifs1, ifs2)
	}
}

func TestStaticIPConfiguration(t *testing.T) {
	ns := newNetstack(t)

	addr := fidlconv.ToNetIpAddress(testV4Address)
	ifAddr := stack.InterfaceAddress{IpAddress: addr, PrefixLen: 32}
	d := deviceForAddEth(ethernet.Info{}, t)
	d.StopImpl = func() error { return nil }
	ifs, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName}, &d)
	if err != nil {
		t.Fatal(err)
	}

	if err := ns.addInterfaceAddr(uint64(ifs.nicid), ifAddr); err != nil {
		t.Fatal(err)
	}

	ifs.mu.Lock()
	if info, err := ifs.toNetInterface2Locked(); err != nil {
		t.Errorf("couldn't get interface info: %s", err)
	} else if got := fidlconv.ToTCPIPAddress(info.Addr); got != testV4Address {
		t.Errorf("got ifs.toNetInterface2Locked().Addr = %+v, want = %+v", got, testV4Address)
	}

	if ifs.mu.dhcp.enabled {
		t.Error("expected dhcp state to be disabled initially")
	}
	ifs.mu.Unlock()

	ifs.eth.Down()

	ifs.mu.Lock()
	if ifs.mu.dhcp.enabled {
		t.Error("expected dhcp state to remain disabled after bringing interface down")
	}
	if ifs.mu.dhcp.running() {
		t.Error("expected dhcp state to remain stopped after bringing interface down")
	}
	ifs.mu.Unlock()

	ifs.eth.Up()

	ifs.mu.Lock()
	if ifs.mu.dhcp.enabled {
		t.Error("expected dhcp state to remain disabled after restarting interface")
	}

	ifs.setDHCPStatusLocked(true)
	if !ifs.mu.dhcp.enabled {
		t.Error("expected dhcp state to become enabled after manually enabling it")
	}
	if !ifs.mu.dhcp.running() {
		t.Error("expected dhcp state running")
	}
	ifs.mu.Unlock()
}

func TestWLANStaticIPConfiguration(t *testing.T) {
	arena, err := eth.NewArena()
	if err != nil {
		t.Fatal(err)
	}
	ns := &Netstack{
		arena: arena,
	}
	ns.mu.ifStates = make(map[tcpip.NICID]*ifState)
	ns.mu.stack = tcpipstack.New(
		[]string{
			ipv4.ProtocolName,
			ipv6.ProtocolName,
			arp.ProtocolName,
		}, nil, tcpipstack.Options{})

	ns.OnInterfacesChanged = func([]netstack.NetInterface2) {}
	addr := fidlconv.ToNetIpAddress(testV4Address)
	ifAddr := stack.InterfaceAddress{IpAddress: addr, PrefixLen: 32}
	d := deviceForAddEth(ethernet.Info{Features: ethernet.InfoFeatureWlan}, t)
	ifs, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{Name: testDeviceName}, &d)
	if err != nil {
		t.Fatal(err)
	}

	if err := ns.addInterfaceAddr(uint64(ifs.nicid), ifAddr); err != nil {
		t.Fatal(err)
	}

	if info, err := ifs.toNetInterface2Locked(); err != nil {
		t.Errorf("couldn't get interface info: %s", err)
	} else if got := fidlconv.ToTCPIPAddress(info.Addr); got != testV4Address {
		t.Errorf("got ifs.toNetInterface2Locked().Addr = %+v, want = %+v", got, testV4Address)
	}
}

func newNetstack(t *testing.T) *Netstack {
	arena, err := eth.NewArena()
	if err != nil {
		t.Fatal(err)
	}
	ns := &Netstack{
		arena: arena,
	}
	ns.mu.ifStates = make(map[tcpip.NICID]*ifState)
	ns.mu.stack = tcpipstack.New(
		[]string{
			ipv4.ProtocolName,
			ipv6.ProtocolName,
			arp.ProtocolName,
		}, nil, tcpipstack.Options{})

	// We need to initialize the DNS client, since adding/removing interfaces
	// sets the DNS servers on that interface, which requires that dnsClient
	// exist.
	ns.dnsClient = dns.NewClient(ns.mu.stack)
	ns.OnInterfacesChanged = func([]netstack.NetInterface2) {}
	return ns
}

func TestAddRemoveListInterfaceAddresses(t *testing.T) {
	ns := newNetstack(t)
	d := deviceForAddEth(ethernet.Info{}, t)
	ifState, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{}, &d)
	if err != nil {
		t.Fatal(err)
	}

	checkDefaultAddress := func(t *testing.T) {
		t.Helper()
		var info netstack.NetInterface2
		interfaces, found := ns.getNetInterfaces2Locked(), false
		for _, ni := range interfaces {
			if ni.Id == uint32(ifState.nicid) {
				found = true
				info = ni
			}
		}
		if !found {
			t.Fatalf("NIC %d not found in %+v", ifState.nicid, interfaces)
		}
		if got, want := info.Addr, fidlconv.ToNetIpAddress(zeroIpAddr); got != want {
			t.Errorf("got Addr = %+v, want = %+v", got, want)
		}
		if got, want := info.Netmask, fidlconv.ToNetIpAddress(tcpip.Address(zeroIpMask)); got != want {
			t.Errorf("got Netmask = %+v, want = %+v", got, want)
		}
	}

	t.Run("defaults", checkDefaultAddress)

	tests := []struct {
		name                  string
		protocol              tcpip.NetworkProtocolNumber
		ip                    tcpip.Address
		prefixesBySpecificity []uint8
	}{
		{ipv4.ProtocolName, ipv4.ProtocolNumber, tcpip.Address(strings.Repeat("\x01", header.IPv4AddressSize)), []uint8{32, 24, 16, 8}},
		{ipv6.ProtocolName, ipv6.ProtocolNumber, tcpip.Address(strings.Repeat("\x01", header.IPv6AddressSize)), []uint8{128, 64, 32, 8}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			// Because prefixesBySpecificity is ordered from most to least constrained and we check that the
			// most recently added prefix length lines up with the netmask we read, this test implicitly
			// asserts that the least-constrained subnet is used to compute the netmask.
			for _, prefixLenToAdd := range test.prefixesBySpecificity {
				t.Run(fmt.Sprintf("prefixLenToAdd=%d", prefixLenToAdd), func(t *testing.T) {
					addr := stack.InterfaceAddress{
						IpAddress: fidlconv.ToNetIpAddress(test.ip),
						PrefixLen: prefixLenToAdd,
					}

					if err := ns.addInterfaceAddr(uint64(ifState.nicid), addr); err != nil {
						t.Fatalf("got ns.addInterfaceAddr(_) = %s want = nil", err)
					}

					t.Run(netstack.NetstackName, func(t *testing.T) {
						interfaces := ns.getNetInterfaces2Locked()
						info, found := netstack.NetInterface2{}, false
						for _, i := range interfaces {
							if tcpip.NICID(i.Id) == ifState.nicid {
								info = i
								found = true
								break
							}
						}
						if !found {
							t.Fatalf("couldn't find NIC ID %d in %+v", ifState.nicid, interfaces)
						}

						switch test.protocol {
						case ipv4.ProtocolNumber:
							if got, want := info.Addr, addr.IpAddress; got != want {
								t.Errorf("got Addr = %+v, want = %+v", got, want)
							}
							if got, want := info.Netmask, getNetmask(prefixLenToAdd, 8*header.IPv4AddressSize); got != want {
								t.Errorf("got Netmask = %+v, want = %+v", got, want)
							}
						case ipv6.ProtocolNumber:
							found := false
							want := net.Subnet{
								Addr:      addr.IpAddress,
								PrefixLen: addr.PrefixLen,
							}
							for _, got := range info.Ipv6addrs {
								if got == want {
									found = true
									break
								}
							}
							if !found {
								t.Errorf("could not find addr %+v in %+v", addr, info.Ipv6addrs)
							}
						default:
							t.Fatalf("protocol number %d not covered", test.protocol)
						}
					})

					t.Run(stack.StackName, func(t *testing.T) {
						interfaces := ns.getNetInterfaces()
						for _, i := range interfaces {
							for _, a := range i.Properties.Addresses {
								if a == addr {
									return
								}
							}
						}
						t.Errorf("could not find addr %+v in %+v", addr, interfaces)
					})
				})
			}

			// From least to most specific, remove each interface address and assert that the
			// next-most-specific interface address' prefix length is reflected in the netmask read.
			for i := len(test.prefixesBySpecificity) - 1; i >= 0; i-- {
				prefixLenToRemove := test.prefixesBySpecificity[i]
				t.Run(fmt.Sprintf("prefixLenToRemove=%d", prefixLenToRemove), func(t *testing.T) {
					addr := stack.InterfaceAddress{
						IpAddress: fidlconv.ToNetIpAddress(test.ip),
						PrefixLen: prefixLenToRemove,
					}
					if err := ns.removeInterfaceAddress(ifState.nicid, test.protocol, fidlconv.ToTCPIPAddress(addr.IpAddress), addr.PrefixLen); err != nil {
						t.Fatalf("got ns.removeInterfaceAddress(_) = %s want = nil", err)
					}

					t.Run(stack.StackName, func(t *testing.T) {
						interfaces := ns.getNetInterfaces()
						for _, i := range interfaces {
							for _, a := range i.Properties.Addresses {
								if a == addr {
									t.Errorf("unexpectedly found addr %+v in %+v", addr, interfaces)
								}
							}
						}
					})

					t.Run(netstack.NetstackName, func(t *testing.T) {
						var info netstack.NetInterface2
						interfaces, found := ns.getNetInterfaces2Locked(), false
						for _, ni := range interfaces {
							if ni.Id == uint32(ifState.nicid) {
								info = ni
								found = true
							}
						}

						if !found {
							t.Fatalf("couldn't find NIC %d in %+v", ifState.nicid, interfaces)
						}

						switch test.protocol {
						case ipv4.ProtocolNumber:
							if i > 0 {
								want := getNetmask(test.prefixesBySpecificity[i-1], 8*header.IPv4AddressSize)
								if got := info.Netmask; got != want {
									t.Errorf("got Netmask = %+v, want = %+v", got, want)
								}
							} else {
								checkDefaultAddress(t)
							}

						case ipv6.ProtocolNumber:
							if i > 0 {
								prefixesRemaining := test.prefixesBySpecificity[:i-1]
								for _, p := range prefixesRemaining {
									want, found := net.Subnet{PrefixLen: p, Addr: addr.IpAddress}, false
									removed := net.Subnet{PrefixLen: prefixLenToRemove, Addr: addr.IpAddress}
									for _, got := range info.Ipv6addrs {
										if got == want {
											found = true
										}
										if got == removed {
											t.Fatalf("got Ipv6addrs = %+v contained removed = %+v", got, removed)
										}
									}
									if !found {
										t.Errorf("got Ipv6addrs = %+v did not contain want = %+v", info.Ipv6addrs, want)
									}
								}
							}
						default:
							t.Fatalf("protocol number %d not covered", test.protocol)
						}
					})
				})
			}
		})
	}
}

func TestAddRouteParameterValidation(t *testing.T) {
	ns := newNetstack(t)
	d := deviceForAddEth(ethernet.Info{}, t)
	interfaceAddress, prefix := tcpip.Address("\xf0\xf0\xf0\xf0"), uint8(24)
	subnetLocalAddress := tcpip.Address("\xf0\xf0\xf0\xf1")
	ifState, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{}, &d)
	if err != nil {
		t.Fatalf("got ns.addEth(_) = _, %s want = _, nil", err)
	}

	if err := ns.addInterfaceAddress(ifState.nicid, ipv4.ProtocolNumber, interfaceAddress, prefix); err != nil {
		t.Fatalf("ns.addInterfaceAddress(%d, %d, %s, %d) = %s", ifState.nicid, ipv4.ProtocolNumber, interfaceAddress, prefix, err)
	}

	tests := []struct {
		name        string
		route       tcpip.Route
		metric      routes.Metric
		dynamic     bool
		shouldPanic bool
		shouldError bool
	}{
		{
			// TODO(NET-2244): don't panic when given invalid route destinations
			name: "zero-length destination",
			route: tcpip.Route{
				Destination: tcpip.Address(""),
				Mask:        tcpip.AddressMask(header.IPv4Broadcast),
				Gateway:     testV4Address,
				NIC:         ifState.nicid,
			},
			metric:      routes.Metric(0),
			shouldPanic: true,
		},
		{
			// TODO(NET-2244): don't panic when given invalid route destinations
			name: "invalid destination",
			route: tcpip.Route{
				Destination: tcpip.Address("\xff"),
				Mask:        tcpip.AddressMask(header.IPv4Broadcast),
				Gateway:     testV4Address,
				NIC:         ifState.nicid,
			},
			metric:      routes.Metric(0),
			shouldPanic: true,
		},
		{
			name: "IPv4 destination no NIC invalid gateway",
			route: tcpip.Route{
				Destination: testV4Address,
				Mask:        tcpip.AddressMask(header.IPv4Broadcast),
				Gateway:     testV4Address,
				NIC:         0,
			},
			metric:      routes.Metric(0),
			shouldError: true,
		},
		{
			name: "IPv6 destination no NIC invalid gateway",
			route: tcpip.Route{
				Destination: testV6Address,
				Mask:        tcpip.AddressMask(strings.Repeat("\xff", 16)),
				Gateway:     testV6Address,
				NIC:         0,
			},
			metric:      routes.Metric(0),
			shouldError: true,
		},
		{
			name: "IPv4 destination no NIC valid gateway",
			route: tcpip.Route{
				Destination: testV4Address,
				Mask:        tcpip.AddressMask(header.IPv4Broadcast),
				Gateway:     subnetLocalAddress,
				NIC:         0,
			},
		},
		{
			name: "zero length gateway",
			route: tcpip.Route{
				Destination: testV4Address,
				Mask:        tcpip.AddressMask(header.IPv4Broadcast),
				Gateway:     tcpip.Address(""),
				NIC:         ifState.nicid,
			},
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			defer func() {
				r := recover()
				if got := r != nil; got != test.shouldPanic {
					t.Logf("recover() = %v", r)
					t.Errorf("got (recover() != nil) = %t; want = %t", got, test.shouldPanic)
				}
			}()

			err := ns.AddRoute(test.route, test.metric, test.dynamic)
			if got := err != nil; got != test.shouldError {
				t.Logf("err = %v", err)
				t.Errorf("got (ns.AddRoute(_) != nil) = %t, want = %t", got, test.shouldError)
			}
		})
	}
}

func TestDHCPAcquired(t *testing.T) {
	ns := newNetstack(t)
	d := deviceForAddEth(ethernet.Info{}, t)
	ifState, err := ns.addEth(testTopoPath, netstack.InterfaceConfig{}, &d)
	if err != nil {
		t.Fatal(err)
	}

	serverAddress := []byte(testV4Address)
	serverAddress[len(serverAddress)-1]++
	gatewayAddress := serverAddress
	gatewayAddress[len(gatewayAddress)-1]++
	const defaultLeaseLength = 60 * time.Second

	tests := []struct {
		name                 string
		oldAddr, newAddr     tcpip.Address
		oldSubnet, newSubnet tcpip.Subnet
		config               dhcp.Config
		expectedRouteTable   []routes.ExtendedRoute
	}{
		{
			name:      "subnet mask provided",
			oldAddr:   "",
			newAddr:   testV4Address,
			oldSubnet: tcpip.Subnet{},
			newSubnet: func() tcpip.Subnet {
				subnet, err := tcpip.NewSubnet(util.ApplyMask(testV4Address, util.DefaultMask(testV4Address)), util.DefaultMask(testV4Address))
				if err != nil {
					t.Fatal(err)
				}
				return subnet
			}(),
			config: dhcp.Config{
				ServerAddress: tcpip.Address(serverAddress),
				Gateway:       tcpip.Address(serverAddress),
				SubnetMask:    util.DefaultMask(testV4Address),
				DNS:           []tcpip.Address{tcpip.Address(gatewayAddress)},
				LeaseLength:   defaultLeaseLength,
			},
			expectedRouteTable: []routes.ExtendedRoute{
				{
					Route: tcpip.Route{
						Destination: util.Parse("192.168.42.0"),
						Mask:        tcpip.AddressMask(util.Parse("255.255.255.0")),
						NIC:         1,
					},
					Metric:                0,
					MetricTracksInterface: true,
					Dynamic:               true,
					Enabled:               false,
				},
				{
					Route: tcpip.Route{
						Destination: util.Parse("0.0.0.0"),
						Mask:        tcpip.AddressMask(util.Parse("0.0.0.0")),
						Gateway:     util.Parse("192.168.42.18"),
						NIC:         1,
					},
					Metric:                0,
					MetricTracksInterface: true,
					Dynamic:               true,
					Enabled:               false,
				},
				{
					Route: tcpip.Route{
						Destination: util.Parse("::"),
						Mask:        tcpip.AddressMask(util.Parse("::")),
						NIC:         1,
					},
					Metric:                0,
					MetricTracksInterface: true,
					Dynamic:               true,
					Enabled:               false,
				},
			},
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			ifState.dhcpAcquired(test.oldAddr, test.newAddr, test.oldSubnet, test.newSubnet, test.config)
			ifState.mu.Lock()
			hasDynamicAddr := ifState.mu.hasDynamicAddr
			dnsServers := ifState.mu.dnsServers
			ifState.mu.Unlock()

			if got, want := hasDynamicAddr, true; got != want {
				t.Errorf("got ifState.mu.hasDynamicAddr = %t, want = %t", got, want)
			}

			if diff := cmp.Diff(dnsServers, test.config.DNS); diff != "" {
				t.Errorf("ifState.mu.dnsServers mismatch (-want +got):\n%s", diff)
			}

			if diff := cmp.Diff(ifState.ns.GetExtendedRouteTable(), test.expectedRouteTable); diff != "" {
				t.Errorf("GetExtendedRouteTable() mismatch (-want +got):\n%s", diff)
			}

			ns.mu.Lock()
			infoMap := ns.mu.stack.NICInfo()
			subnetMap := ns.mu.stack.NICSubnets()
			ns.mu.Unlock()
			if info, ok := infoMap[ifState.nicid]; ok {
				found := false
				for _, address := range info.ProtocolAddresses {
					if address.Protocol == ipv4.ProtocolNumber {
						switch address.Address {
						case test.oldAddr:
							t.Errorf("expired address %s was not removed from NIC addresses %v", test.oldAddr, info.ProtocolAddresses)
						case test.newAddr:
							found = true
						}
					}
				}

				if !found {
					t.Errorf("new address %s was not added to NIC addresses %v", test.newAddr, info.ProtocolAddresses)
				}
			} else {
				t.Errorf("NIC %d not found in %v", ifState.nicid, infoMap)
			}
			if subnets, ok := subnetMap[ifState.nicid]; ok {
				found := false
				for _, subnet := range subnets {
					switch subnet {
					case test.oldSubnet:
						t.Errorf("expired subnet %s/%d was not removed from NIC subnets", test.oldSubnet.ID(), test.oldSubnet.Prefix())
						for _, subnet := range subnets {
							t.Logf("%s/%d", subnet.ID(), subnet.Prefix())
						}
					case test.newSubnet:
						found = true
					}
				}

				if !found {
					t.Errorf("new subnet %s/%d was not added to NIC subnets", test.newSubnet.ID(), test.newSubnet.Prefix())
					for _, subnet := range subnets {
						t.Logf("%s/%d", subnet.ID(), subnet.Prefix())
					}
				}
			} else {
				t.Errorf("NIC %d not found in %v", ifState.nicid, subnetMap)
			}
		})
	}
}

func getNetmask(prefix uint8, bits int) net.IpAddress {
	return fidlconv.ToNetIpAddress(tcpip.Address(util.CIDRMask(int(prefix), bits)))
}

// Returns an ethernetext.Device struct that implements
// ethernet.Device and can be started and stopped.
//
// Reports the passed in ethernet.Info when Device#GetInfo is called.
func deviceForAddEth(info ethernet.Info, t *testing.T) ethernetext.Device {
	return ethernetext.Device{
		TB:                t,
		GetInfoImpl:       func() (ethernet.Info, error) { return info, nil },
		SetClientNameImpl: func(string) (int32, error) { return 0, nil },
		GetStatusImpl: func() (uint32, error) {
			return uint32(eth.LinkUp), nil
		},
		GetFifosImpl: func() (int32, *ethernet.Fifos, error) {
			return int32(zx.ErrOk), &ethernet.Fifos{
				TxDepth: 1,
			}, nil
		},
		SetIoBufferImpl: func(zx.VMO) (int32, error) {
			return int32(zx.ErrOk), nil
		},
		StartImpl: func() (int32, error) {
			return int32(zx.ErrOk), nil
		},
		ConfigMulticastSetPromiscuousModeImpl: func(bool) (int32, error) {
			return int32(zx.ErrOk), nil
		},
	}
}
