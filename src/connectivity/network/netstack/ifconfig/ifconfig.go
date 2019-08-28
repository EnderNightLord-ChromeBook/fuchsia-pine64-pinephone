// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"syscall/zx"

	"app/context"
	"netstack/fidlconv"

	"fidl/fuchsia/hardware/ethernet"
	netfidl "fidl/fuchsia/net"
	"fidl/fuchsia/netstack"
	"fidl/fuchsia/wlan/service"

	"github.com/google/netstack/tcpip"
)

type netstackClientApp struct {
	ctx      *context.Context
	netstack *netstack.NetstackInterface
	wlan     *service.WlanInterface
}

func (a *netstackClientApp) printAll() {
	ifaces, err := a.netstack.GetInterfaces2()
	if err != nil {
		fmt.Print("ifconfig: failed to fetch interfaces\n")
		return
	}

	for _, iface := range ifaces {
		a.printIface(iface)
	}
}

func getIfaceByNameFromIfaces(name string, ifaces []netstack.NetInterface2) *netstack.NetInterface2 {
	var candidate *netstack.NetInterface2
	for i, iface := range ifaces {
		if strings.HasPrefix(iface.Name, name) {
			if candidate != nil {
				return nil
			}
			candidate = &ifaces[i]
		}
	}
	return candidate
}

func getIfaceByIdFromIfaces(id uint32, ifaces []netstack.NetInterface2) *netstack.NetInterface2 {
	for _, iface := range ifaces {
		if iface.Id == id {
			return &iface
		}
	}
	return nil
}

func (a *netstackClientApp) printIface(iface netstack.NetInterface2) {
	stats, err := a.netstack.GetStats(iface.Id)

	if err != nil {
		fmt.Printf("ifconfig: failed to fetch stats for '%v'\n\n", iface.Name)
		return
	}

	fmt.Printf("%s\tHWaddr %s Id:%d\n", iface.Name, hwAddrToString(iface.Hwaddr), iface.Id)
	fmt.Printf("\tinet addr:%s  Bcast:%s  Mask:%s\n", netAddrToString(iface.Addr), netAddrToString(iface.Broadaddr), netAddrToString(iface.Netmask))
	for _, addr := range iface.Ipv6addrs {
		// TODO: scopes
		fmt.Printf("\tinet6 addr: %s/%d Scope:Link\n", netAddrToString(addr.Addr), addr.PrefixLen)
	}
	fmt.Printf("\tmetric:%d\n", iface.Metric)
	fmt.Printf("\t%s\n", flagsToString(iface.Flags))

	if isWLAN(iface.Features) {
		fmt.Printf("\tWLAN Status: %s\n", a.wlanStatus())
	}

	fmt.Printf("\tRX packets:%d\n", stats.Rx.PktsTotal)
	fmt.Printf("\tTX packets:%d\n", stats.Tx.PktsTotal)
	fmt.Printf("\tRX bytes:%s  TX bytes:%s\n",
		bytesToString(stats.Rx.BytesTotal), bytesToString(stats.Tx.BytesTotal))
	fmt.Printf("\n")
	// TODO: more stats. MTU, RX/TX errors
}

func (a *netstackClientApp) setStatus(iface netstack.NetInterface2, up bool) {
	a.netstack.SetInterfaceStatus(iface.Id, up)
}

func (a *netstackClientApp) addIfaceAddress(iface netstack.NetInterface2, cidr string) {
	netAddr, prefixLen := validateCidr(os.Args[3])
	result, _ := a.netstack.SetInterfaceAddress(iface.Id, netAddr, prefixLen)
	if result.Status != netstack.StatusOk {
		fmt.Printf("Error setting interface address: %s\n", result.Message)
	}
}

func (a *netstackClientApp) removeIfaceAddress(iface netstack.NetInterface2, cidr string) {
	netAddr, prefixLen := validateCidr(os.Args[3])
	result, _ := a.netstack.RemoveInterfaceAddress(iface.Id, netAddr, prefixLen)
	if result.Status != netstack.StatusOk {
		fmt.Printf("Error setting interface address: %s\n", result.Message)
	}
}

func (a *netstackClientApp) parseRouteAttribute(in *netstack.RouteTableEntry2, args []string) (remaining []string, err error) {
	if len(args) < 2 {
		return args, fmt.Errorf("not enough args to make attribute")
	}
	var attr, val string
	switch attr, val, remaining = args[0], args[1], args[2:]; attr {
	case "gateway":
		gateway := toIpAddress(net.ParseIP(val))
		in.Gateway = &gateway
	case "metric":
		m, err := strconv.ParseUint(val, 10, 32)
		if err != nil {
			return remaining, fmt.Errorf("metric value '%s' is not uint32: %s", val, err)
		}
		in.Metric = uint32(m)
	case "iface":
		ifaces, err := a.netstack.GetInterfaces2()
		if err != nil {
			return remaining, err
		}
		iface := getIfaceByNameFromIfaces(val, ifaces)
		if err != nil {
			return remaining, err
		}
		if iface == nil {
			return remaining, fmt.Errorf("no such interface '%s'\n", val)
		}

		in.Nicid = iface.Id
	default:
		return remaining, fmt.Errorf("unknown route attribute: %s %s", attr, val)
	}

	return remaining, nil
}

func (a *netstackClientApp) newRouteFromArgs(args []string) (route netstack.RouteTableEntry2, err error) {
	destination, remaining := args[0], args[1:]
	_, dstSubnet, err := net.ParseCIDR(destination)
	if err != nil {
		return route, fmt.Errorf("invalid destination (destination must be provided in CIDR format): %s", destination)
	}

	route.Destination = toIpAddress(dstSubnet.IP)
	route.Netmask = toIpAddress(net.IP(dstSubnet.Mask))

	for len(remaining) > 0 {
		remaining, err = a.parseRouteAttribute(&route, remaining)
		if err != nil {
			return route, err
		}
	}

	if len(remaining) != 0 {
		return route, fmt.Errorf("could not parse all route arguments. remaining: %s", remaining)
	}

	return route, nil
}

func (a *netstackClientApp) addRoute(r netstack.RouteTableEntry2) error {
	if r.Gateway == nil && r.Nicid == 0 {
		return fmt.Errorf("either gateway or iface must be provided when adding a route")
	}

	req, transactionInterface, err := netstack.NewRouteTableTransactionInterfaceRequest()
	if err != nil {
		return fmt.Errorf("could not make a new route table transaction: %s", err)
	}
	defer req.Close()
	status, err := a.netstack.StartRouteTableTransaction(req)
	if err != nil || zx.Status(status) != zx.ErrOk {
		return fmt.Errorf("could not start a route table transaction: %s (%s)", err, zx.Status(status))
	}

	status, err = transactionInterface.AddRoute(r)
	if err != nil {
		return fmt.Errorf("could not add route due to transaction interface error: %s", err)
	}
	if zx.Status(status) != zx.ErrOk {
		return fmt.Errorf("could not add route in netstack: %s", zx.Status(status))
	}
	return nil
}

func (a *netstackClientApp) deleteRoute(r netstack.RouteTableEntry2) error {
	req, transactionInterface, err := netstack.NewRouteTableTransactionInterfaceRequest()
	if err != nil {
		return fmt.Errorf("could not make a new route table transaction: %s", err)
	}
	defer req.Close()
	status, err := a.netstack.StartRouteTableTransaction(req)
	if err != nil || zx.Status(status) != zx.ErrOk {
		return fmt.Errorf("could not start a route table transaction (maybe the route table is locked?): %s", err)
	}

	status, err = transactionInterface.DelRoute(r)
	if err != nil {
		return fmt.Errorf("could not delete route due to transaction interface error: %s", err)
	}
	if zx.Status(status) != zx.ErrOk {
		return fmt.Errorf("could not delete route in netstack: %s", zx.Status(status))
	}
	return nil
}

func routeTableEntryToString(r netstack.RouteTableEntry2, ifaces []netstack.NetInterface2) string {
	iface := getIfaceByIdFromIfaces(r.Nicid, ifaces)
	var ifaceName string
	if iface == nil {
		ifaceName = fmt.Sprintf("Nicid:%d", r.Nicid)
	} else {
		ifaceName = iface.Name
	}
	var netAndMask net.IPNet
	switch r.Destination.Which() {
	case netfidl.IpAddressIpv4:
		netAndMask = net.IPNet{IP: r.Destination.Ipv4.Addr[:], Mask: r.Netmask.Ipv4.Addr[:]}
	case netfidl.IpAddressIpv6:
		netAndMask = net.IPNet{IP: r.Destination.Ipv6.Addr[:], Mask: r.Netmask.Ipv6.Addr[:]}
	}
	if r.Gateway != nil {
		return fmt.Sprintf("%s via %s %s metric %v", netAndMask.String(), netAddrToString(*r.Gateway), ifaceName, r.Metric)
	}
	return fmt.Sprintf("%s via %s metric %v", netAndMask.String(), ifaceName, r.Metric)
}

func (a *netstackClientApp) showRoutes() error {
	rs, err := a.netstack.GetRouteTable2()
	if err != nil {
		return fmt.Errorf("Could not get route table from netstack: %s", err)
	}
	ifaces, err := a.netstack.GetInterfaces2()
	if err != nil {
		return err
	}
	for _, r := range rs {
		fmt.Printf("%s\n", routeTableEntryToString(r, ifaces))
	}
	return nil
}

func (a *netstackClientApp) bridge(ifNames []string) (uint32, error) {
	ifs := make([]*netstack.NetInterface2, len(ifNames))
	nicIDs := make([]uint32, len(ifNames))
	// first, validate that all interfaces exist
	ifaces, err := a.netstack.GetInterfaces2()
	if err != nil {
		return 0, err
	}
	for i, ifName := range ifNames {
		iface := getIfaceByNameFromIfaces(ifName, ifaces)
		if iface == nil {
			return 0, fmt.Errorf("no such interface '%s'\n", ifName)
		}
		ifs[i] = iface
		nicIDs[i] = iface.Id
	}

	result, nicid, _ := a.netstack.BridgeInterfaces(nicIDs)
	if result.Status != netstack.StatusOk {
		return 0, fmt.Errorf("error bridging interfaces: %s, result: %s", ifNames, result)
	}

	return nicid, nil
}

func (a *netstackClientApp) setDHCP(iface netstack.NetInterface2, startStop string) {
	switch startStop {
	case "start":
		a.netstack.SetDhcpClientStatus(iface.Id, true)
	case "stop":
		a.netstack.SetDhcpClientStatus(iface.Id, false)
	default:
		usage()
	}
}

func (a *netstackClientApp) wlanStatus() string {
	if a.wlan == nil {
		return "failed to query (FIDL service unintialized)"
	}
	res, err := a.wlan.Status()
	if err != nil {
		return fmt.Sprintf("failed to query (error: %v)", err)
	} else if res.Error.Code != service.ErrCodeOk {
		return fmt.Sprintf("failed to query (err: code(%v) desc(%v)", res.Error.Code, res.Error.Description)
	} else {
		status := wlanStateToStr(res.State)
		if res.CurrentAp != nil {
			ap := res.CurrentAp
			isSecureStr := ""
			if ap.IsSecure {
				isSecureStr = "*"
			}
			status += fmt.Sprintf(" BSSID: %x SSID: %q Security: %v RSSI: %d",
				ap.Bssid, ap.Ssid, isSecureStr, ap.RssiDbm)
		}
		return status
	}
}

func wlanStateToStr(state service.State) string {
	switch state {
	case service.StateBss:
		return "starting-bss"
	case service.StateQuerying:
		return "querying"
	case service.StateScanning:
		return "scanning"
	case service.StateJoining:
		return "joining"
	case service.StateAuthenticating:
		return "authenticating"
	case service.StateAssociating:
		return "associating"
	case service.StateAssociated:
		return "associated"
	default:
		return "unknown"
	}

}

func hwAddrToString(hwaddr []uint8) string {
	var b strings.Builder
	for i, d := range hwaddr {
		if i > 0 {
			b.WriteByte(':')
		}
		fmt.Fprintf(&b, "%02x", d)
	}
	return b.String()
}

func netAddrToString(addr netfidl.IpAddress) string {
	return fidlconv.ToTCPIPAddress(addr).String()
}

func flagsToString(flags uint32) string {
	if flags&netstack.NetInterfaceFlagUp != 0 {
		return "UP"
	}
	return "DOWN"
}

func toIpAddress(addr net.IP) netfidl.IpAddress {
	// TODO(eyalsoha): this doesn't correctly handle IPv6-mapped IPv4 addresses.
	if ipv4 := addr.To4(); ipv4 != nil {
		addr = ipv4
	}
	return fidlconv.ToNetIpAddress(tcpip.Address(addr))
}

func validateCidr(cidr string) (address netfidl.IpAddress, prefixLength uint8) {
	netAddr, netSubnet, err := net.ParseCIDR(cidr)
	if err != nil {
		fmt.Printf("Error parsing CIDR notation: %s, error: %v\n", cidr, err)
		usage()
	}
	prefixLen, _ := netSubnet.Mask.Size()

	return toIpAddress(netAddr), uint8(prefixLen)
}

func isWLAN(features uint32) bool {
	return features&ethernet.InfoFeatureWlan != 0
}

// bytesToString returns a human-friendly display of the given byte count.
// Values over 1K have an approximation appended in parentheses.
// bytesToString(42) => "42"
// bytesToString(42*1024 + 200) => "43208 (42.2 KB)"
func bytesToString(bytes uint64) string {
	if bytes == 0 {
		return "0"
	}

	units := "BKMGT"
	unitSize := uint64(1)
	unit := "B"

	for i := 0; i+1 < len(units) && bytes >= unitSize*1024; i++ {
		unitSize *= 1024
		unit = string(units[i+1]) + "B"
	}

	if unitSize == 1 {
		return fmt.Sprintf("%d", bytes)
	} else {
		value := float32(bytes) / float32(unitSize)
		return fmt.Sprintf("%d (%.1f %s)", bytes, value, unit)
	}
}

func usage() {
	fmt.Printf("Usage:\n")
	fmt.Printf("  %s [<interface>]\n", os.Args[0])
	fmt.Printf("  %s <interface> {up|down}\n", os.Args[0])
	fmt.Printf("  %s <interface> {add|del} <address>/<mask>\n", os.Args[0])
	fmt.Printf("  %s <interface> metric <metric>\n", os.Args[0])
	fmt.Printf("  %s <interface> dhcp {start|stop}\n", os.Args[0])
	fmt.Printf("  %s route {add|del} <address>/<mask> [iface <name>] [gateway <address>] [metric <metric>]\n", os.Args[0])
	fmt.Printf("  %s route show\n", os.Args[0])
	fmt.Printf("  %s bridge [<interface>]+\n", os.Args[0])
	os.Exit(1)
}

func main() {
	a := &netstackClientApp{ctx: context.CreateFromStartupInfo()}
	req, pxy, err := netstack.NewNetstackInterfaceRequest()
	if err != nil {
		panic(err.Error())
	}
	a.netstack = pxy
	defer a.netstack.Close()
	a.ctx.ConnectToEnvService(req)

	reqWlan, pxyWlan, errWlan := service.NewWlanInterfaceRequest()
	if errWlan == nil {
		a.wlan = pxyWlan
		defer a.wlan.Close()
		a.ctx.ConnectToEnvService(reqWlan)
	}

	if len(os.Args) == 1 {
		a.printAll()
		return
	}

	var iface *netstack.NetInterface2
	switch os.Args[1] {
	case "route":
		if len(os.Args) == 2 || os.Args[2] == "show" {
			err = a.showRoutes()
			if err != nil {
				fmt.Printf("Error showing routes: %s\n", err)
			}
			return
		}
		if len(os.Args) < 4 {
			fmt.Printf("Not enough arguments to `ifconfig route`; at least a destination and one of iface name or gateway must be provided\n")
			usage()
		}
		routeFlags := os.Args[3:]
		r, err := a.newRouteFromArgs(routeFlags)
		if err != nil {
			fmt.Printf("Error parsing route from args: %s, error: %s\n", routeFlags, err)
			usage()
		}

		op := os.Args[2]
		switch op {
		case "add":
			if err = a.addRoute(r); err != nil {
				fmt.Printf("Error adding route to route table: %s\n", err)
				usage()
			}
		case "del":
			err = a.deleteRoute(r)
			if err != nil {
				fmt.Printf("Error deleting route from route table: %s\n", err)
				usage()
			}
		default:
			fmt.Printf("Unknown route operation: %s\n", op)
			usage()
		}

		return
	case "bridge":
		ifaces := os.Args[2:]
		nicid, err := a.bridge(ifaces)
		if err != nil {
			fmt.Printf("error creating bridge: %s\n", err)
		} else {
			interfaces, _ := a.netstack.GetInterfaces2()
			bridge := getIfaceByIdFromIfaces(uint32(nicid), interfaces)
			fmt.Printf("Bridged interfaces %s.\nInterface '%s' created.\nPlease run `ifconfig %[2]s up` to enable it.\n", ifaces, bridge.Name)
		}
		return
	case "help":
		usage()
		return
	default:
		ifaces, err := a.netstack.GetInterfaces2()
		if err != nil {
			fmt.Printf("Error finding interface name: %s\n", err)
			return
		}
		iface = getIfaceByNameFromIfaces(os.Args[1], ifaces)
		if err != nil {
			fmt.Printf("Error finding interface name: %s\n", err)
			return
		}
		if iface == nil {
			fmt.Printf("ifconfig: no such interface '%s'\n", os.Args[1])
			return
		}
	}

	switch len(os.Args) {
	case 2:
		a.printIface(*iface)
	case 3:
		switch os.Args[2] {
		case "up":
			a.setStatus(*iface, true)
		case "down":
			a.setStatus(*iface, false)
		default:
			usage()
		}
	case 4:
		switch os.Args[2] {
		case "add":
			a.addIfaceAddress(*iface, os.Args[3])
		case "del":
			a.removeIfaceAddress(*iface, os.Args[3])
		case "dhcp":
			a.setDHCP(*iface, os.Args[3])
		case "metric":
			metric, err := strconv.ParseUint(os.Args[3], 10, 32)
			if err != nil {
				fmt.Printf("ifconfig: metric value '%s' is not uint32: %s\n", os.Args[3], err)
				return
			}
			a.netstack.SetInterfaceMetric(iface.Id, uint32(metric))
		default:
			usage()
		}

	default:
		usage()
	}
}
