// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netstack

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"

	"syslog"

	"netstack/dhcp"
	"netstack/dns"
	"netstack/fidlconv"
	"netstack/filter"
	"netstack/link"
	"netstack/link/bridge"
	"netstack/link/eth"
	"netstack/routes"
	"netstack/util"

	"fidl/fuchsia/devicesettings"
	"fidl/fuchsia/hardware/ethernet"
	"fidl/fuchsia/net"
	"fidl/fuchsia/netstack"

	"github.com/google/netstack/tcpip"
	"github.com/google/netstack/tcpip/header"
	"github.com/google/netstack/tcpip/link/loopback"
	"github.com/google/netstack/tcpip/link/sniffer"
	"github.com/google/netstack/tcpip/network/arp"
	"github.com/google/netstack/tcpip/network/ipv4"
	"github.com/google/netstack/tcpip/network/ipv6"
	"github.com/google/netstack/tcpip/stack"
)

const (
	deviceSettingsManagerNodenameKey = "DeviceName"
	defaultNodename                  = "fuchsia-unset-device-name"

	defaultInterfaceMetric routes.Metric = 100

	metricNotSet routes.Metric = 0

	lowPriorityRoute routes.Metric = 99999

	ipv4Loopback tcpip.Address = "\x7f\x00\x00\x01"
	ipv6Loopback tcpip.Address = "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"

	// Values used to indicate no IP is assigned to an interface.
	zeroIpAddr tcpip.Address     = header.IPv4Any
	zeroIpMask tcpip.AddressMask = "\xff\xff\xff\xff"
)

// A Netstack tracks all of the running state of the network stack.
type Netstack struct {
	arena *eth.Arena

	deviceSettings *devicesettings.DeviceSettingsManagerInterface
	dnsClient      *dns.Client

	mu struct {
		sync.Mutex
		stack              *stack.Stack
		routeTable         routes.RouteTable
		transactionRequest *netstack.RouteTableTransactionInterfaceRequest
		countNIC           tcpip.NICID
		ifStates           map[tcpip.NICID]*ifState
	}
	nodename string
	sniff    bool

	filter *filter.Filter

	OnInterfacesChanged func([]netstack.NetInterface2)
}

// Each ifState tracks the state of a network interface.
type ifState struct {
	ns    *Netstack
	eth   link.Controller
	nicid tcpip.NICID
	// features can include any value that's valid in fuchsia.hardware.ethernet.Info.features.
	features uint32
	mu       struct {
		sync.Mutex
		state          link.State
		hasDynamicAddr bool
		name           string
		// metric is used by default for routes that originate from this NIC.
		metric     routes.Metric
		dnsServers []tcpip.Address
		dhcp       struct {
			*dhcp.Client
			// running must not be nil.
			running func() bool
			// cancel must not be nil.
			cancel context.CancelFunc
			// Used to restart the DHCP client when we go from link.StateDown to
			// link.StateStarted.
			enabled bool
		}
	}

	// The "outermost" LinkEndpoint implementation (the composition of link
	// endpoint functionality happens by wrapping other link endpoints).
	endpoint stack.LinkEndpoint

	bridgeable *bridge.BridgeableEndpoint

	filterEndpoint *filter.FilterEndpoint
}

// defaultRoutes returns the IPv4 and IPv6 default routes.
func defaultRoutes(nicid tcpip.NICID, gateway tcpip.Address) []tcpip.Route {
	return []tcpip.Route{
		{
			Destination: tcpip.Address(strings.Repeat("\x00", 4)),
			Mask:        tcpip.AddressMask(strings.Repeat("\x00", 4)),
			Gateway:     gateway,
			NIC:         nicid,
		},
		{
			Destination: tcpip.Address(strings.Repeat("\x00", 16)),
			Mask:        tcpip.AddressMask(strings.Repeat("\x00", 16)),
			NIC:         nicid,
		},
	}
}

func subnetRoute(addr tcpip.Address, mask tcpip.AddressMask, nicid tcpip.NICID) tcpip.Route {
	return tcpip.Route{
		Destination: util.ApplyMask(addr, mask),
		Mask:        mask,
		NIC:         nicid,
	}
}

// AddRoute adds a single route to the route table in a sorted fashion. This
// takes the lock.
func (ns *Netstack) AddRoute(r tcpip.Route, metric routes.Metric, dynamic bool) error {
	syslog.Infof("adding route %+v metric:%d dynamic=%v", r, metric, dynamic)
	ns.mu.Lock()
	defer ns.mu.Unlock()
	return ns.AddRouteLocked(r, metric, dynamic)
}

// AddRouteLocked adds a single route to the route table in a sorted fashion. It
// assumes the lock has already been taken.
func (ns *Netstack) AddRouteLocked(r tcpip.Route, metric routes.Metric, dynamic bool) error {
	return ns.AddRoutesLocked([]tcpip.Route{r}, metric, dynamic)
}

// AddRoutesLocked adds one or more routes to the route table in a sorted
// fashion. It assumes the lock has already been taken.
func (ns *Netstack) AddRoutesLocked(rs []tcpip.Route, metric routes.Metric, dynamic bool) error {
	metricTracksInterface := false
	if metric == metricNotSet {
		metricTracksInterface = true
	}

	for _, r := range rs {
		switch len(r.Destination) {
		case header.IPv4AddressSize, header.IPv6AddressSize:
		default:
			// TODO(NET-2244): update this to return an error; panicing here enables syzkaller to find
			// the given state management bug more quickly.
			panic(fmt.Sprintf("invalid destination for route: %+v\nroute table: %+v", r, ns.mu.routeTable.GetExtendedRouteTable()))
		}

		// If we don't have an interface set, find it using the gateway address.
		if r.NIC == 0 {
			nic, err := ns.mu.routeTable.FindNIC(r.Gateway)
			if err != nil {
				return fmt.Errorf("error finding NIC for gateway %v: %s", r.Gateway, err)
			}
			r.NIC = nic
		}

		ifs, ok := ns.mu.ifStates[r.NIC]
		if !ok {
			return fmt.Errorf("error getting ifState for NIC %d, not in map", r.NIC)
		}

		enabled := ifs.mu.state == link.StateStarted
		if metricTracksInterface {
			metric = ifs.mu.metric
		}

		ns.mu.routeTable.AddRoute(r, metric, metricTracksInterface, dynamic, enabled)
	}
	ns.mu.stack.SetRouteTable(ns.mu.routeTable.GetNetstackTable())
	return nil
}

// DelRoute deletes a single route from the route table. This takes the lock.
func (ns *Netstack) DelRoute(r tcpip.Route) error {
	syslog.Infof("deleting route %+v", r)
	ns.mu.Lock()
	defer ns.mu.Unlock()
	return ns.DelRouteLocked(r)
}

// DelRoute deletes a single route from the route table. It assumes the lock has
// already been taken.
func (ns *Netstack) DelRouteLocked(r tcpip.Route) error {
	if err := ns.mu.routeTable.DelRoute(r); err != nil {
		return fmt.Errorf("error deleting route, %s", err)
	}
	ns.mu.stack.SetRouteTable(ns.mu.routeTable.GetNetstackTable())
	return nil
}

// GetExtendedRouteTable returns a copy of the current extended route table.
// This takes the lock.
func (ns *Netstack) GetExtendedRouteTable() []routes.ExtendedRoute {
	ns.mu.Lock()
	defer ns.mu.Unlock()
	return ns.mu.routeTable.GetExtendedRouteTable()
}

// UpdateRoutesByInterfaceLocked applies update actions to the routes for a
// given interface. It assumes the lock has already been taken.
func (ns *Netstack) UpdateRoutesByInterfaceLocked(nicid tcpip.NICID, action routes.Action) {
	ns.mu.routeTable.UpdateRoutesByInterface(nicid, action)
	ns.mu.stack.SetRouteTable(ns.mu.routeTable.GetNetstackTable())
}

// UpdateInterfaceMetric changes the metric for an interface and updates all
// routes tracking that interface metric. This takes the lock.
func (ns *Netstack) UpdateInterfaceMetric(nicid tcpip.NICID, metric routes.Metric) error {
	syslog.Infof("update interface metric for NIC %d to metric=%d", nicid, metric)

	ns.mu.Lock()
	defer ns.mu.Unlock()

	ifState, ok := ns.mu.ifStates[tcpip.NICID(nicid)]
	if !ok {
		return fmt.Errorf("error getting ifState for NIC %d, not in map", nicid)
	}
	ifState.updateMetric(metric)

	ns.mu.routeTable.UpdateMetricByInterface(nicid, metric)
	ns.mu.stack.SetRouteTable(ns.mu.routeTable.GetNetstackTable())
	return nil
}

func (ns *Netstack) removeInterfaceAddress(nic tcpip.NICID, protocol tcpip.NetworkProtocolNumber, addr tcpip.Address, prefixLen uint8) error {
	subnet, err := toSubnet(addr, prefixLen)
	if err != nil {
		return fmt.Errorf("error parsing subnet format for NIC ID %d: %s", nic, err)
	}
	route := subnetRoute(addr, subnet.Mask(), nic)
	syslog.Infof("removing static IP %s/%d from NIC %d, deleting subnet route %+v", addr, prefixLen, nic, route)

	ns.mu.Lock()
	if err := func() error {
		addresses, subnets := ns.getAddressesLocked(nic)
		allOnes := int(prefixLen) == 8*len(addr)
		hasAddr := containsAddress(addresses, protocol, addr)
		hasSubnet := containsSubnet(subnets, addr, prefixLen)
		if !hasAddr && !hasSubnet {
			return fmt.Errorf("neither address nor subnet %s/%d exists on NIC ID %d", addr, prefixLen, nic)
		}

		if !allOnes {
			if hasSubnet {
				if err := ns.mu.stack.RemoveSubnet(nic, subnet); err == tcpip.ErrUnknownNICID {
					panic(fmt.Sprintf("stack.RemoveSubnet(_): NIC [%d] not found", nic))
				} else if err != nil {
					return fmt.Errorf("error removing subnet %s/%d from NIC ID %d: %s", addr, prefixLen, nic, err)
				}
			} else {
				return fmt.Errorf("no such subnet %s/%d for NIC ID %d", addr, prefixLen, nic)
			}
		}

		ns.DelRouteLocked(route)

		if allOnes && hasAddr {
			if err := ns.mu.stack.RemoveAddress(nic, addr); err == tcpip.ErrUnknownNICID {
				panic(fmt.Sprintf("stack.RemoveAddress(_): NIC [%d] not found", nic))
			} else if err != nil {
				return fmt.Errorf("error removing address %s from NIC ID %d: %s", addr, nic, err)
			}
		}

		return nil
	}(); err != nil {
		ns.mu.Unlock()
		return err
	}

	interfaces := ns.getNetInterfaces2Locked()
	ns.mu.Unlock()
	ns.OnInterfacesChanged(interfaces)
	return nil
}

func toSubnet(address tcpip.Address, prefixLen uint8) (tcpip.Subnet, error) {
	m := util.CIDRMask(int(prefixLen), int(len(address)*8))
	if len(m) == 0 {
		return tcpip.Subnet{}, fmt.Errorf("net.CIDRMask(%d, %d) = nil", prefixLen, len(address)*8)
	}
	return tcpip.NewSubnet(util.ApplyMask(address, m), m)
}

func (ns *Netstack) addInterfaceAddress(nic tcpip.NICID, protocol tcpip.NetworkProtocolNumber, addr tcpip.Address, prefixLen uint8) error {
	subnet, err := toSubnet(addr, prefixLen)
	if err != nil {
		return fmt.Errorf("error parsing subnet format for NIC ID %d: %s", nic, err)
	}
	route := subnetRoute(addr, subnet.Mask(), nic)
	syslog.Infof("adding static IP %v/%d to NIC %d, creating subnet route %+v with metric=<not-set>, dynamic=false", addr, prefixLen, nic, route)

	ns.mu.Lock()
	if err := func() error {
		addresses, subnets := ns.getAddressesLocked(nic)
		hasAddr := containsAddress(addresses, protocol, addr)
		hasSubnet := containsSubnet(subnets, addr, prefixLen)
		if hasAddr && hasSubnet {
			return fmt.Errorf("address/prefix combination %s/%d already exists on NIC ID %d", addr, prefixLen, nic)
		}
		if !hasAddr {
			if err := ns.mu.stack.AddAddress(nic, protocol, addr); err != nil {
				return fmt.Errorf("error adding address %s to NIC ID %d: %s", addr, nic, err)
			}
		}

		if !hasSubnet {
			if err := ns.mu.stack.AddSubnet(nic, protocol, subnet); err != nil {
				return fmt.Errorf("error adding subnet %+v to NIC ID %d: %s", subnet, nic, err)
			}
		}

		if err := ns.AddRouteLocked(route, metricNotSet, false); err != nil {
			return fmt.Errorf("error adding subnet route %v to NIC ID %d: %s", route, nic, err)
		}
		return nil
	}(); err != nil {
		ns.mu.Unlock()
		return err
	}

	interfaces := ns.getNetInterfaces2Locked()
	ns.mu.Unlock()
	ns.OnInterfacesChanged(interfaces)
	return nil
}

func (ifs *ifState) updateMetric(metric routes.Metric) {
	ifs.mu.Lock()
	ifs.mu.metric = metric
	ifs.mu.Unlock()
}

func (ifs *ifState) dhcpAcquired(oldAddr, newAddr tcpip.Address, oldSubnet, newSubnet tcpip.Subnet, config dhcp.Config) {
	ifs.mu.Lock()
	name := ifs.mu.name
	ifs.mu.Unlock()

	ifs.ns.mu.Lock()
	if oldAddr != newAddr {
		if len(oldAddr) != 0 {
			if err := ifs.ns.mu.stack.RemoveAddress(ifs.nicid, oldAddr); err != nil {
				syslog.Infof("NIC %s: failed to remove expired DHCP address %s: %s", name, oldAddr, err)
			} else {
				syslog.Infof("NIC %s: removed expired DHCP address %s", name, oldAddr)
			}
		}
		if len(newAddr) != 0 {
			if err := ifs.ns.mu.stack.AddAddressWithOptions(ifs.nicid, ipv4.ProtocolNumber, newAddr, stack.FirstPrimaryEndpoint); err != nil {
				syslog.Infof("NIC %s: failed to add DHCP acquired address %s: %s", name, newAddr, err)
			} else {
				syslog.Infof("NIC %s: DHCP acquired address %s for %s", name, newAddr, config.LeaseLength)
			}
		} else {
			syslog.Errorf("NIC %s: DHCP could not acquire address", name)
		}
	}
	if oldSubnet != newSubnet {
		if oldSubnet != (tcpip.Subnet{}) {
			if err := ifs.ns.mu.stack.RemoveSubnet(ifs.nicid, oldSubnet); err != nil {
				syslog.Infof("NIC %s: failed to remove expired DHCP subnet %s/%d: %s", name, oldSubnet.ID(), oldSubnet.Prefix(), err)
			} else {
				syslog.Infof("NIC %s: removed expired DHCP subnet %s/%d", name, oldSubnet.ID(), oldSubnet.Prefix())
			}
		}
		if newSubnet != (tcpip.Subnet{}) {
			if err := ifs.ns.mu.stack.AddSubnet(ifs.nicid, ipv4.ProtocolNumber, newSubnet); err != nil {
				syslog.Infof("NIC %s: failed to add DHCP acquired subnet %s/%d: %s", name, newSubnet.ID(), oldSubnet.Prefix(), err)
			} else {
				syslog.Infof("NIC %s: DHCP acquired subnet %s/%d for %s", name, newSubnet.ID(), newSubnet.Prefix(), config.LeaseLength)
			}
		} else {
			syslog.Errorf("NIC %s: DHCP could not acquire subnet", name)
		}
	}
	ifs.ns.mu.Unlock()

	if len(newAddr) == 0 || newSubnet == (tcpip.Subnet{}) {
		return
	}

	syslog.Infof("NIC %s: Adding DNS servers: %v", name, config.DNS)

	ifs.mu.Lock()
	ifs.mu.hasDynamicAddr = true
	ifs.mu.dnsServers = config.DNS
	ifs.mu.Unlock()

	// Add a default route and a route for the local subnet.
	rs := defaultRoutes(ifs.nicid, config.Gateway)
	rs = append(rs, subnetRoute(newAddr, config.SubnetMask, ifs.nicid))
	syslog.Infof("adding routes %+v with metric=<not-set> dynamic=true", rs)

	ifs.ns.mu.Lock()
	if err := ifs.ns.AddRoutesLocked(rs, metricNotSet, true /* dynamic */); err != nil {
		syslog.Infof("error adding routes for DHCP address/gateway: %s", err)
	}
	interfaces := ifs.ns.getNetInterfaces2Locked()
	ifs.ns.mu.Unlock()

	ifs.ns.dnsClient.SetRuntimeServers(ifs.ns.getRuntimeDNSServerRefs())
	ifs.ns.OnInterfacesChanged(interfaces)
}

func (ifs *ifState) setDHCPStatusLocked(enabled bool) {
	ifs.mu.dhcp.enabled = enabled
	ifs.mu.dhcp.cancel()
	if ifs.mu.dhcp.enabled && ifs.mu.state == link.StateStarted {
		ifs.runDHCPLocked()
	}
}

// Runs the DHCP client with a fresh context and initializes ifs.mu.dhcp.cancel.
// Call the old cancel function before calling this function.
func (ifs *ifState) runDHCPLocked() {
	ctx, cancel := context.WithCancel(context.Background())
	ifs.mu.dhcp.cancel = cancel
	ifs.mu.dhcp.running = func() bool {
		return ctx.Err() == nil
	}
	if c := ifs.mu.dhcp.Client; c != nil {
		c.Run(ctx)
	} else {
		panic(fmt.Sprintf("nil DHCP client on interface %s", ifs.mu.name))
	}
}

func (ifs *ifState) dhcpEnabled() bool {
	ifs.mu.Lock()
	defer ifs.mu.Unlock()
	return ifs.mu.dhcp.enabled
}

func (ifs *ifState) stateChange(s link.State) {
	ifs.ns.mu.Lock()
	ifs.mu.Lock()
	switch s {
	case link.StateClosed:
		syslog.Infof("NIC %s: link.StateClosed", ifs.mu.name)
		delete(ifs.ns.mu.ifStates, ifs.nicid)
		fallthrough
	case link.StateDown:
		syslog.Infof("NIC %s: link.StateDown", ifs.mu.name)
		ifs.mu.dhcp.cancel()

		// TODO(crawshaw): more cleanup to be done here:
		// 	- remove link endpoint
		//	- reclaim NICID?

		if ifs.mu.hasDynamicAddr || s == link.StateClosed {
			syslog.Infof("removing IP from NIC %d", ifs.nicid)
			ifs.mu.dnsServers = nil
		}

		if s == link.StateClosed {
			// The interface is removed, force all of its routes to be removed.
			ifs.ns.UpdateRoutesByInterfaceLocked(ifs.nicid, routes.ActionDeleteAll)
		} else {
			// The interface is down, delete dynamic routes, disable static ones.
			ifs.ns.UpdateRoutesByInterfaceLocked(ifs.nicid, routes.ActionDeleteDynamicDisableStatic)
		}

	case link.StateStarted:
		syslog.Infof("NIC %s: link.StateStarted", ifs.mu.name)
		// Re-enable static routes out this interface.
		ifs.ns.UpdateRoutesByInterfaceLocked(ifs.nicid, routes.ActionEnableStatic)
		if ifs.mu.dhcp.enabled {
			ifs.mu.dhcp.cancel()
			ifs.runDHCPLocked()
		}
		// TODO(ckuiper): Remove this, as we shouldn't create default routes w/o a
		// gateway given. Before doing so make sure nothing is still relying on
		// this.
		// Update the state before adding the routes, so they are properly enabled.
		ifs.mu.state = s
		if err := ifs.ns.AddRoutesLocked(defaultRoutes(ifs.nicid, ""), lowPriorityRoute, true /* dynamic */); err != nil {
			syslog.Infof("error adding default routes: %v", err)
		}
	}
	ifs.mu.state = s
	ifs.mu.Unlock()

	interfaces := ifs.ns.getNetInterfaces2Locked()
	ifs.ns.mu.Unlock()

	ifs.ns.dnsClient.SetRuntimeServers(ifs.ns.getRuntimeDNSServerRefs())
	ifs.ns.OnInterfacesChanged(interfaces)
}

// Return a slice of references to each NIC's DNS servers.
// The caller takes ownership of the returned slice.
func (ns *Netstack) getRuntimeDNSServerRefs() []*[]tcpip.Address {
	ns.mu.Lock()
	defer ns.mu.Unlock()

	refs := make([]*[]tcpip.Address, 0, len(ns.mu.ifStates))
	for _, ifs := range ns.mu.ifStates {
		ifs.mu.Lock()
		refs = append(refs, &ifs.mu.dnsServers)
		ifs.mu.Unlock()
	}
	return refs
}

func (ns *Netstack) getdnsServers() []tcpip.Address {
	defaultServers := ns.dnsClient.GetDefaultServers()
	uniqServers := make(map[tcpip.Address]struct{})

	ns.mu.Lock()
	for _, ifs := range ns.mu.ifStates {
		ifs.mu.Lock()
		for _, server := range ifs.mu.dnsServers {
			uniqServers[server] = struct{}{}
		}
		ifs.mu.Unlock()
	}
	ns.mu.Unlock()

	out := make([]tcpip.Address, 0, len(defaultServers)+len(uniqServers))
	out = append(out, defaultServers...)
	for server := range uniqServers {
		out = append(out, server)
	}
	return out
}

var deviceSettingsErrorLogged uint32 = 0

func (ns *Netstack) getNodeName() string {
	nodename, status, err := ns.deviceSettings.GetString(deviceSettingsManagerNodenameKey)
	if err != nil {
		if atomic.CompareAndSwapUint32(&deviceSettingsErrorLogged, 0, 1) {
			syslog.Warnf("getNodeName: error accessing device settings: %s", err)
		}
		return defaultNodename
	}

	if status != devicesettings.StatusOk {
		var reportStatus string
		switch status {
		case devicesettings.StatusErrNotSet:
			reportStatus = "key not set"
		case devicesettings.StatusErrInvalidSetting:
			reportStatus = "invalid setting"
		case devicesettings.StatusErrRead:
			reportStatus = "error reading key"
		case devicesettings.StatusErrIncorrectType:
			reportStatus = "value type was incorrect"
		case devicesettings.StatusErrUnknown:
			reportStatus = "unknown"
		default:
			reportStatus = fmt.Sprintf("unknown status code: %d", status)
		}
		if atomic.CompareAndSwapUint32(&deviceSettingsErrorLogged, 0, 1) {
			syslog.Warnf("getNodeName: device settings error: %s", reportStatus)
		}
		return defaultNodename
	}

	atomic.StoreUint32(&deviceSettingsErrorLogged, 0)
	return nodename
}

// TODO(stijlist): figure out a way to make it impossible to accidentally
// enable DHCP on loopback interfaces.
func (ns *Netstack) addLoopback() error {
	ifs, err := ns.addEndpoint(func(tcpip.NICID) string {
		return "lo"
	}, stack.FindLinkEndpoint(loopback.New()), link.NewLoopbackController(), false, defaultInterfaceMetric, ethernet.InfoFeatureLoopback)
	if err != nil {
		return err
	}

	ifs.mu.Lock()
	ifs.mu.state = link.StateStarted
	nicid := ifs.nicid
	ifs.mu.Unlock()

	if err := ns.mu.stack.AddAddress(nicid, ipv4.ProtocolNumber, ipv4Loopback); err != nil {
		return fmt.Errorf("loopback: adding ipv4 address failed: %v", err)
	}
	if err := ns.mu.stack.AddAddress(nicid, ipv6.ProtocolNumber, ipv6Loopback); err != nil {
		return fmt.Errorf("loopback: adding ipv6 address failed: %v", err)
	}

	if err := ns.AddRoutesLocked(
		[]tcpip.Route{
			{
				Destination: ipv4Loopback,
				Mask:        tcpip.AddressMask(strings.Repeat("\xff", 4)),
				NIC:         nicid,
			},
			{
				Destination: ipv6Loopback,
				Mask:        tcpip.AddressMask(strings.Repeat("\xff", 16)),
				NIC:         nicid,
			},
		},
		metricNotSet, /* use interface metric */
		false,        /* dynamic */
	); err != nil {
		return fmt.Errorf("loopback: adding routes failed: %v", err)
	}

	return nil
}

func (ns *Netstack) Bridge(nics []tcpip.NICID) (*ifState, error) {
	links := make([]*bridge.BridgeableEndpoint, 0, len(nics))
	ns.mu.Lock()
	for _, nicid := range nics {
		ifs, ok := ns.mu.ifStates[nicid]
		if !ok {
			panic("NIC known by netstack not in interface table")
		}
		if err := ifs.eth.SetPromiscuousMode(true); err != nil {
			return nil, err
		}
		links = append(links, ifs.bridgeable)
	}
	ns.mu.Unlock()

	b := bridge.New(links)
	return ns.addEndpoint(func(nicid tcpip.NICID) string {
		return fmt.Sprintf("br%d", nicid)
	}, b, b, false, defaultInterfaceMetric, 0)
}

func (ns *Netstack) addEth(topological_path string, config netstack.InterfaceConfig, device ethernet.Device) (*ifState, error) {
	client, err := eth.NewClient("netstack", topological_path, device, ns.arena)
	if err != nil {
		return nil, err
	}

	return ns.addEndpoint(func(nicid tcpip.NICID) string {
		if len(config.Name) == 0 {
			return fmt.Sprintf("eth%d", nicid)
		}
		return config.Name
	}, eth.NewLinkEndpoint(client), client, true, routes.Metric(config.Metric), client.Info.Features)
}

func (ns *Netstack) addEndpoint(
	nameFn func(nicid tcpip.NICID) string,
	ep stack.LinkEndpoint,
	controller link.Controller,
	doFilter bool,
	metric routes.Metric,
	features uint32,
) (*ifState, error) {
	ifs := &ifState{
		ns:       ns,
		eth:      controller,
		features: features,
	}
	ifs.mu.state = link.StateUnknown
	ifs.mu.metric = metric
	ifs.mu.dhcp.running = func() bool { return false }
	ifs.mu.dhcp.cancel = func() {}

	ifs.eth.SetOnStateChange(ifs.stateChange)
	linkID := stack.RegisterLinkEndpoint(ep)

	// LinkEndpoint chains:
	// Put sniffer as close as the NIC.
	if ns.sniff {
		// A wrapper LinkEndpoint should encapsulate the underlying
		// one, and manifest itself to 3rd party netstack.
		linkID = sniffer.New(linkID)
	}

	if doFilter {
		linkID, ifs.filterEndpoint = filter.NewFilterEndpoint(ns.filter, linkID)
	}
	linkID, ifs.bridgeable = bridge.NewEndpoint(linkID)
	ifs.endpoint = ifs.bridgeable

	ns.mu.Lock()
	defer ns.mu.Unlock()

	ifs.nicid = ns.mu.countNIC + 1
	name := nameFn(ifs.nicid)
	ns.mu.ifStates[ifs.nicid] = ifs
	ns.mu.countNIC++

	syslog.Infof("NIC %s added [sniff = %t]", name, ns.sniff)

	if err := ns.mu.stack.CreateNIC(ifs.nicid, linkID); err != nil {
		return nil, fmt.Errorf("NIC %s: could not create NIC: %v", ifs.mu.name, err)
	}
	if ep.Capabilities()&stack.CapabilityResolutionRequired > 0 {
		if err := ns.mu.stack.AddAddress(ifs.nicid, arp.ProtocolNumber, arp.ProtocolAddress); err != nil {
			return nil, fmt.Errorf("NIC %s: adding arp address failed: %v", ifs.mu.name, err)
		}
	}

	ifs.mu.Lock()
	defer ifs.mu.Unlock()

	if linkAddr := ep.LinkAddress(); len(linkAddr) > 0 {
		lladdr := header.LinkLocalAddr(linkAddr)
		if err := ns.mu.stack.AddAddress(ifs.nicid, ipv6.ProtocolNumber, lladdr); err != nil {
			return nil, fmt.Errorf("NIC %s: adding link-local IPv6 %v failed: %v", ifs.mu.name, lladdr, err)
		}
		snaddr := header.SolicitedNodeAddr(lladdr)
		if err := ns.mu.stack.AddAddress(ifs.nicid, ipv6.ProtocolNumber, snaddr); err != nil {
			return nil, fmt.Errorf("NIC %s: adding solicited-node IPv6 %v (link-local IPv6 %v) failed: %v", ifs.mu.name, snaddr, lladdr, err)
		}

		ifs.mu.dhcp.Client = dhcp.NewClient(ns.mu.stack, ifs.nicid, linkAddr, ifs.dhcpAcquired)

		syslog.Infof("NIC %s: link-local IPv6: %v", name, lladdr)
	}

	ifs.mu.name = name

	return ifs, nil
}

func (ns *Netstack) validateInterfaceAddress(address net.IpAddress, prefixLen uint8) (tcpip.NetworkProtocolNumber, tcpip.Address, netstack.NetErr) {
	var protocol tcpip.NetworkProtocolNumber
	switch address.Which() {
	case net.IpAddressIpv4:
		protocol = ipv4.ProtocolNumber
	case net.IpAddressIpv6:
		return 0, "", netstack.NetErr{Status: netstack.StatusIpv4Only, Message: "IPv6 not yet supported"}
	}

	addr := fidlconv.ToTCPIPAddress(address)

	if (8 * len(addr)) < int(prefixLen) {
		return 0, "", netstack.NetErr{Status: netstack.StatusParseError, Message: "prefix length exceeds address length"}
	}

	return protocol, addr, netstack.NetErr{Status: netstack.StatusOk}
}

func (ns *Netstack) getAddressesLocked(nic tcpip.NICID) ([]tcpip.ProtocolAddress, []tcpip.Subnet) {
	nicInfo := ns.mu.stack.NICInfo()
	nicSubnets := ns.mu.stack.NICSubnets()

	info, ok := nicInfo[nic]
	if !ok {
		panic(fmt.Sprintf("NIC [%d] not found in %+v", nic, nicInfo))
	}
	subnets, ok := nicSubnets[nic]
	if !ok {
		panic(fmt.Sprintf("NIC [%d] not found in %+v", nic, nicSubnets))
	}

	return info.ProtocolAddresses, subnets
}

func containsAddress(addresses []tcpip.ProtocolAddress, protocol tcpip.NetworkProtocolNumber, addr tcpip.Address) bool {
	for _, a := range addresses {
		if a.Protocol == protocol && a.Address == addr {
			return true
		}
	}
	return false
}

func containsSubnet(subnets []tcpip.Subnet, addr tcpip.Address, prefixLen uint8) bool {
	for _, s := range subnets {
		if s.ID() == util.ApplyMask(addr, util.CIDRMask(int(prefixLen), 8*len(addr))) && uint8(s.Prefix()) == prefixLen {
			return true
		}
	}
	return false
}
