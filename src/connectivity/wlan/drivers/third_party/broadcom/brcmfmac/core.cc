/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "core.h"

#include <algorithm>
#include <endian.h>
#include <pthread.h>
#include <threads.h>
#include <atomic>

#include <ddk/protocol/wlanphyimpl.h>
#include <netinet/if_ether.h>
#include <wlan/common/phy.h>
#include <zircon/status.h>

#include "brcmu_utils.h"
#include "brcmu_wifi.h"
#include "bus.h"
#include "cfg80211.h"
#include "common.h"
#include "debug.h"
#include "device.h"
#include "feature.h"
#include "fwil.h"
#include "fwil_types.h"
#include "linuxisms.h"
#include "netbuf.h"
#include "p2p.h"
#include "pno.h"
#include "proto.h"
#include "workqueue.h"

#define MAX_WAIT_FOR_8021X_TX_MSEC (950)

#define BRCMF_BSSIDX_INVALID -1

static inline struct brcmf_device* if_to_dev(struct brcmf_if* ifp) {
    return ifp->drvr->bus_if->dev;
}

static inline struct brcmf_device* ndev_to_dev(struct net_device* ndev) {
    return if_to_dev(ndev_to_if(ndev));
}

const char* brcmf_ifname(struct brcmf_if* ifp) {
    if (!ifp) {
        return "<if_null>";
    }

    if (ifp->ndev) {
        return ifp->ndev->name;
    }

    return "<if_none>";
}

struct brcmf_if* brcmf_get_ifp(struct brcmf_pub* drvr, int ifidx) {
    struct brcmf_if* ifp;
    int32_t bsscfgidx;

    if (ifidx < 0 || ifidx >= BRCMF_MAX_IFS) {
        BRCMF_ERR("ifidx %d out of range\n", ifidx);
        return NULL;
    }

    ifp = NULL;
    bsscfgidx = drvr->if2bss[ifidx];
    if (bsscfgidx >= 0) {
        ifp = drvr->iflist[bsscfgidx];
    }

    return ifp;
}

void brcmf_configure_arp_nd_offload(struct brcmf_if* ifp, bool enable) {
    zx_status_t err;
    uint32_t mode;
    int32_t fw_err = 0;

    if (enable) {
        mode = BRCMF_ARP_OL_AGENT | BRCMF_ARP_OL_PEER_AUTO_REPLY;
    } else {
        mode = 0;
    }

    /* Try to set and enable ARP offload feature, this may fail, then it  */
    /* is simply not supported and err 0 will be returned                 */
    err = brcmf_fil_iovar_int_set(ifp, "arp_ol", mode, &fw_err);
    if (err != ZX_OK) {
        BRCMF_DBG(TRACE, "failed to set ARP offload mode to 0x%x, err=%s, fw_err=%s\n", mode,
                  zx_status_get_string(err), brcmf_fil_get_errstr(fw_err));
    } else {
        err = brcmf_fil_iovar_int_set(ifp, "arpoe", enable, &fw_err);
        if (err != ZX_OK) {
            BRCMF_DBG(TRACE, "failed to configure (%d) ARP offload err=%s, fw_err=%s\n", enable,
                      zx_status_get_string(err), brcmf_fil_get_errstr(fw_err));
        } else {
            BRCMF_DBG(TRACE, "successfully configured (%d) ARP offload to 0x%x\n", enable, mode);
        }
    }

    err = brcmf_fil_iovar_int_set(ifp, "ndoe", enable, &fw_err);
    if (err != ZX_OK) {
        BRCMF_DBG(TRACE, "failed to configure (%d) ND offload err=%s, fw_err=%s\n", enable,
                  zx_status_get_string(err), brcmf_fil_get_errstr(fw_err));
    } else {
        BRCMF_DBG(TRACE, "successfully configured (%d) ND offload to 0x%x\n", enable, mode);
    }
}

static void _brcmf_set_multicast_list(struct work_struct* work) {
    struct brcmf_if* ifp;
    struct net_device* ndev;
    struct netdev_hw_addr* ha;
    uint32_t cmd_value, cnt;
    uint32_t cnt_le;
    char* buf;
    char* bufp;
    uint32_t buflen;
    zx_status_t err;
    int32_t fw_err = 0;

    ifp = containerof(work, struct brcmf_if, multicast_work);

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d\n", ifp->bsscfgidx);

    ndev = ifp->ndev;

    /* Determine initial value of allmulti flag */
    cmd_value = ndev->multicast_promisc;

    /* Send down the multicast list first. */
    cnt = netdev_mc_count(ndev);
    buflen = sizeof(cnt) + (cnt * ETH_ALEN);
    buf = static_cast<decltype(buf)>(malloc(buflen));
    if (!buf) {
        return;
    }
    bufp = buf;

    cnt_le = cnt;
    memcpy(bufp, &cnt_le, sizeof(cnt_le));
    bufp += sizeof(cnt_le);

    netdev_for_each_mc_addr(ha, ndev) {
        if (!cnt) {
            break;
        }
        memcpy(bufp, ha->addr, ETH_ALEN);
        bufp += ETH_ALEN;
        cnt--;
    }

    err = brcmf_fil_iovar_data_set(ifp, "mcast_list", buf, buflen, &fw_err);
    if (err != ZX_OK) {
        BRCMF_ERR("Setting mcast_list failed: %s, fw err %s\n", zx_status_get_string(err),
                  brcmf_fil_get_errstr(fw_err));
        cmd_value = cnt ? true : cmd_value;
    }

    free(buf);

    /*
     * Now send the allmulti setting.  This is based on the setting in the
     * net_device flags, but might be modified above to be turned on if we
     * were trying to set some addresses and dongle rejected it...
     */
    err = brcmf_fil_iovar_int_set(ifp, "allmulti", cmd_value, &fw_err);
    if (err != ZX_OK) {
        BRCMF_ERR("Setting allmulti failed: %s, fw err %s\n", zx_status_get_string(err),
                  brcmf_fil_get_errstr(fw_err));
    }

    /*Finally, pick up the PROMISC flag */
    cmd_value = (ndev->flags & IFF_PROMISC) ? true : false;
    err = brcmf_fil_cmd_int_set(ifp, BRCMF_C_SET_PROMISC, cmd_value, &fw_err);
    if (err != ZX_OK) {
        BRCMF_ERR("Setting BRCMF_C_SET_PROMISC failed, %s, fw err %s\n", zx_status_get_string(err),
                  brcmf_fil_get_errstr(fw_err));
    }
    brcmf_configure_arp_nd_offload(ifp, !cmd_value);
}

zx_status_t brcmf_netdev_set_mac_address(struct net_device* ndev, void* addr) {
    struct brcmf_if* ifp = ndev_to_if(ndev);
    struct sockaddr* sa = (struct sockaddr*)addr;
    zx_status_t err;
    int32_t fw_err = 0;

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d\n", ifp->bsscfgidx);

    err = brcmf_fil_iovar_data_set(ifp, "cur_etheraddr", sa->sa_data, ETH_ALEN, &fw_err);
    if (err != ZX_OK) {
        BRCMF_ERR("Setting cur_etheraddr failed: %s, fw err %s\n", zx_status_get_string(err),
                  brcmf_fil_get_errstr(fw_err));
    } else {
        BRCMF_DBG(TRACE, "updated to %pM\n", sa->sa_data);
        memcpy(ifp->mac_addr, sa->sa_data, ETH_ALEN);
        memcpy(ifp->ndev->dev_addr, ifp->mac_addr, ETH_ALEN);
    }
    return err;
}

void brcmf_netdev_set_multicast_list(struct net_device* ndev) {
    struct brcmf_if* ifp = ndev_to_if(ndev);

    workqueue_schedule_default(&ifp->multicast_work);
}

void brcmf_netdev_start_xmit(struct net_device* ndev, ethernet_netbuf_t* ethernet_netbuf) {
    zx_status_t ret;
    struct brcmf_if* ifp = ndev_to_if(ndev);
    struct brcmf_pub* drvr = ifp->drvr;
    struct brcmf_netbuf* netbuf = nullptr;
    struct ethhdr* eh = nullptr;
    int head_delta;

    BRCMF_DBG(DATA, "Enter, bsscfgidx=%d\n", ifp->bsscfgidx);

    /* Can the device send data? */
    if (drvr->bus_if->state != BRCMF_BUS_UP) {
        BRCMF_ERR("xmit rejected state=%d\n", drvr->bus_if->state);
        netif_stop_queue(ndev);
        ret = ZX_ERR_UNAVAILABLE;
        goto done;
    }

    netbuf = brcmf_netbuf_allocate(ethernet_netbuf->data_size + drvr->hdrlen);
    brcmf_netbuf_grow_tail(netbuf, ethernet_netbuf->data_size + drvr->hdrlen);
    brcmf_netbuf_shrink_head(netbuf, drvr->hdrlen);
    memcpy(netbuf->data, ethernet_netbuf->data_buffer, ethernet_netbuf->data_size);

    /* Make sure there's enough writeable headroom */
    if (brcmf_netbuf_head_space(netbuf) < drvr->hdrlen) {
        head_delta = std::max<int>(drvr->hdrlen - brcmf_netbuf_head_space(netbuf), 0);

        BRCMF_DBG(INFO, "%s: insufficient headroom (%d)\n", brcmf_ifname(ifp), head_delta);
        drvr->bus_if->stats.pktcowed.fetch_add(1);
        ret = brcmf_netbuf_grow_realloc(netbuf, ALIGN(head_delta, NET_NETBUF_PAD), 0);
        if (ret != ZX_OK) {
            BRCMF_ERR("%s: failed to expand headroom\n", brcmf_ifname(ifp));
            drvr->bus_if->stats.pktcow_failed.fetch_add(1);
            // TODO(cphoenix): Shouldn't I brcmf_netbuf_free here?
            goto done;
        }
    }

    /* validate length for ether packet */
    if (netbuf->len < sizeof(*eh)) {
        ret = ZX_ERR_INVALID_ARGS;
        brcmf_netbuf_free(netbuf);
        goto done;
    }

    eh = (struct ethhdr*)(netbuf->data);

    if (eh->h_proto == htobe16(ETH_P_PAE)) {
        ifp->pend_8021x_cnt.fetch_add(1);
    }

    /* determine the priority */
    if ((netbuf->priority == 0) || (netbuf->priority > 7)) {
        netbuf->priority = cfg80211_classify8021d(netbuf, NULL);
    }

    ret = brcmf_proto_tx_queue_data(drvr, ifp->ifidx, netbuf);
    if (ret != ZX_OK) {
        brcmf_txfinalize(ifp, netbuf, false);
    }

done:
    if (ret != ZX_OK) {
        ndev->stats.tx_dropped++;
    } else {
        ndev->stats.tx_packets++;
        ndev->stats.tx_bytes += netbuf->len;
    }
    /* No status to return: we always eat the packet */
}

void brcmf_txflowblock_if(struct brcmf_if* ifp, enum brcmf_netif_stop_reason reason, bool state) {
    if (!ifp || !ifp->ndev) {
        return;
    }

    BRCMF_DBG(TRACE, "enter: bsscfgidx=%d stop=0x%X reason=%d state=%d\n", ifp->bsscfgidx,
              ifp->netif_stop, reason, state);

    //spin_lock_irqsave(&ifp->netif_stop_lock, flags);
    pthread_mutex_lock(&irq_callback_lock);

    if (state) {
        if (!ifp->netif_stop) {
            netif_stop_queue(ifp->ndev);
        }
        ifp->netif_stop |= reason;
    } else {
        ifp->netif_stop &= ~reason;
        if (!ifp->netif_stop) {
            brcmf_enable_tx(ifp->ndev);
        }
    }
    //spin_unlock_irqrestore(&ifp->netif_stop_lock, flags);
    pthread_mutex_unlock(&irq_callback_lock);
}

void brcmf_netif_rx(struct brcmf_if* ifp, struct brcmf_netbuf* netbuf) {
    if (netbuf->pkt_type == ADDRESSED_TO_MULTICAST) {
        ifp->ndev->stats.multicast++;
    }

    if (!(ifp->ndev->flags & IFF_UP)) {
        brcmu_pkt_buf_free_netbuf(netbuf);
        return;
    }

    ifp->ndev->stats.rx_bytes += netbuf->len;
    ifp->ndev->stats.rx_packets++;

    BRCMF_DBG(DATA, "rx proto=0x%X len %d\n", be16toh(netbuf->protocol), netbuf->len);
    brcmf_cfg80211_rx(ifp, netbuf);
}

static zx_status_t brcmf_rx_hdrpull(struct brcmf_pub* drvr, struct brcmf_netbuf* netbuf,
                                    struct brcmf_if** ifp) {
    zx_status_t ret;

    /* process and remove protocol-specific header */
    ret = brcmf_proto_hdrpull(drvr, true, netbuf, ifp);

    if (ret != ZX_OK || !(*ifp) || !(*ifp)->ndev) {
        if (ret != ZX_ERR_BUFFER_TOO_SMALL && *ifp) {
            (*ifp)->ndev->stats.rx_errors++;
        }
        brcmu_pkt_buf_free_netbuf(netbuf);
        return ZX_ERR_IO;
    }

    // TODO(cphoenix): Double-check (be paranoid) that these side effects of eth_type_trans()
    // are not used in this code.
    // - netbuf->dev
    // Also double-check that we're not using DSA in our net device (whatever that is)
    // and that we don't worry about "older Novell" IPX.
    // TODO(cphoenix): This is an ugly hack, probably buggy, to replace some of eth_type_trans.
    // See https://elixir.bootlin.com/linux/v4.17-rc7/source/net/ethernet/eth.c#L156
    if (address_is_multicast(netbuf->data)) {
        if (address_is_broadcast(netbuf->data)) {
            netbuf->pkt_type = ADDRESSED_TO_BROADCAST;
        } else {
            netbuf->pkt_type = ADDRESSED_TO_MULTICAST;
        }
    } else if (memcmp(netbuf->data, (*ifp)->ndev->dev_addr, 6)) {
        netbuf->pkt_type = ADDRESSED_TO_OTHER_HOST;
    }
    struct ethhdr* header = (struct ethhdr*)netbuf->data;
    if (header->h_proto >= ETH_P_802_3_MIN) {
        netbuf->protocol = header->h_proto;
    } else {
        netbuf->protocol = htobe16(ETH_P_802_2);
    }
    netbuf->eth_header = netbuf->data;
    //netbuf->protocol = eth_type_trans(netbuf, (*ifp)->ndev);
    return ZX_OK;
}

void brcmf_rx_frame(struct brcmf_device* dev, struct brcmf_netbuf* netbuf, bool handle_event) {
    struct brcmf_if* ifp;
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;

    BRCMF_DBG(DATA, "Enter: %s: rxp=%p\n", device_get_name(dev->zxdev), netbuf);

    if (brcmf_rx_hdrpull(drvr, netbuf, &ifp)) {
        BRCMF_DBG(TEMP, "hdrpull returned nonzero");
        return;
    }

    if (brcmf_proto_is_reorder_netbuf(netbuf)) {
        brcmf_proto_rxreorder(ifp, netbuf);
    } else {
        /* Process special event packets */
        if (handle_event) {
            brcmf_fweh_process_netbuf(ifp->drvr, netbuf);
        }

        brcmf_netif_rx(ifp, netbuf);
    }
}

void brcmf_rx_event(struct brcmf_device* dev, struct brcmf_netbuf* netbuf) {
    struct brcmf_if* ifp;
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;

    BRCMF_DBG(EVENT, "Enter: %s: rxp=%p\n", device_get_name(dev->zxdev), netbuf);

    if (brcmf_rx_hdrpull(drvr, netbuf, &ifp)) {
        return;
    }

    brcmf_fweh_process_netbuf(ifp->drvr, netbuf);
    brcmu_pkt_buf_free_netbuf(netbuf);
}

void brcmf_txfinalize(struct brcmf_if* ifp, struct brcmf_netbuf* txp, bool success) {
    struct ethhdr* eh;
    uint16_t type;

    eh = (struct ethhdr*)(txp->data);
    type = be16toh(eh->h_proto);

    if (type == ETH_P_PAE) {
        if (ifp->pend_8021x_cnt.fetch_sub(1) == 1) {
            sync_completion_signal(&ifp->pend_8021x_wait);
        }
    }

    if (!success) {
        ifp->ndev->stats.tx_errors++;
    }

    brcmu_pkt_buf_free_netbuf(txp);
}

static zx_status_t brcmf_netdev_stop(struct net_device* ndev) {
    struct brcmf_if* ifp = ndev_to_if(ndev);

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d\n", ifp->bsscfgidx);

    brcmf_cfg80211_down(ndev);

    brcmf_fil_iovar_data_set(ifp, "arp_hostip_clear", NULL, 0, nullptr);

    brcmf_net_setcarrier(ifp, false);

    return ZX_OK;
}

zx_status_t brcmf_netdev_open(struct net_device* ndev) {
    struct brcmf_if* ifp = ndev_to_if(ndev);
    struct brcmf_pub* drvr = ifp->drvr;
    struct brcmf_bus* bus_if = drvr->bus_if;
    uint32_t toe_ol;

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d\n", ifp->bsscfgidx);

    /* If bus is not ready, can't continue */
    if (bus_if->state != BRCMF_BUS_UP) {
        BRCMF_ERR("failed bus is not ready\n");
        return ZX_ERR_UNAVAILABLE;
    }

    ifp->pend_8021x_cnt.store(0);

    /* Get current TOE mode from dongle */
    if (brcmf_fil_iovar_int_get(ifp, "toe_ol", &toe_ol, nullptr) == ZX_OK &&
            (toe_ol & TOE_TX_CSUM_OL) != 0) {
        ndev->features |= NETIF_F_IP_CSUM;
    } else {
        ndev->features &= ~NETIF_F_IP_CSUM;
    }

    if (brcmf_cfg80211_up(ndev) != ZX_OK) {
        BRCMF_ERR("failed to bring up cfg80211\n");
        return ZX_ERR_IO;
    }

    /* Clear, carrier, set when connected or AP mode. */
    BRCMF_DBG(TEMP, "* * Would have called netif_carrier_off(ndev);");
    return ZX_OK;
}

static void brcmf_release_zx_phy_device(void* ctx) {
    // TODO(cphoenix): Implement release
    // Unbind - remove device from tree
    // Release - dealloc resources
    BRCMF_ERR("* * Need to unload and release all driver structs");
}

static zx_protocol_device_t phy_impl_device_ops = {
    .version = DEVICE_OPS_VERSION,
    .release = brcmf_release_zx_phy_device,
};

zx_status_t brcmf_phy_query(void* ctx, wlanphy_impl_info_t* phy_info) {
    struct brcmf_if* ifp = static_cast<decltype(ifp)>(ctx);
    // See wlan/protocol/info.h
    wlan_info_t* info = &phy_info->wlan_info;
    memset(info, 0, sizeof(*info));
    memcpy(info->mac_addr, ifp->mac_addr, ETH_ALEN);
    info->mac_role = WLAN_INFO_MAC_ROLE_CLIENT | WLAN_INFO_MAC_ROLE_AP;
    info->supported_phys = 0x1f; //WLAN_INFO_PHY_TYPE_;
    info->driver_features = WLAN_INFO_DRIVER_FEATURE_SCAN_OFFLOAD |
                            WLAN_INFO_DRIVER_FEATURE_DFS;
    info->caps = 0xf; //WLAN_INFO_HARDWARE_CAPABILITY_;
    info->bands_count = 1;
    info->bands[0].band = WLAN_INFO_BAND_2GHZ;
    // TODO(cphoenix): Once this isn't temp/stub code anymore, remove unnecessary "= 0" lines.
    info->bands[0].ht_supported = false;
    info->bands[0].ht_caps.ht_capability_info = 0;
    info->bands[0].ht_caps.ampdu_params = 0;
    // info->bands[0].ht_caps.supported_mcs_set[ 16 entries ] = 0;
    info->bands[0].ht_caps.ht_ext_capabilities = 0;
    info->bands[0].ht_caps.tx_beamforming_capabilities = 0;
    info->bands[0].ht_caps.asel_capabilities = 0;
    info->bands[0].vht_supported = false;
    info->bands[0].vht_caps.vht_capability_info = 0;
    info->bands[0].vht_caps.supported_vht_mcs_and_nss_set = 0;
    // info->bands[0].basic_rates[ 12 entries ] = 0;
    info->bands[0].supported_channels.base_freq = 0;
    // info->bands[0].supported_channels.channels[ 64 entries ] = 0;
    return ZX_OK;
}

zx_status_t brcmf_phy_destroy_iface(void* ctx, uint16_t id) {
    BRCMF_ERR("Don't know how to destroy iface yet");
    return ZX_ERR_IO;
}

zx_status_t brcmf_phy_set_country(void* ctx, const wlanphy_country_t* country) {
    if (country == nullptr) {
        return ZX_ERR_INVALID_ARGS;
    }

    BRCMF_ERR("brcmf_phy_set_country() to [%s] not implemented",
              wlan::common::Alpha2ToStr(country->alpha2).c_str());
    return ZX_ERR_NOT_SUPPORTED;
}

static wlanphy_impl_protocol_ops_t phy_impl_proto_ops = {
    .query = brcmf_phy_query,
    .create_iface = brcmf_phy_create_iface,
    .destroy_iface = brcmf_phy_destroy_iface,
    .set_country = brcmf_phy_set_country,
};

zx_status_t brcmf_net_attach(struct brcmf_if* ifp, bool rtnl_locked) {
    struct brcmf_pub* drvr = ifp->drvr;
    struct net_device* ndev = ifp->ndev;
    zx_status_t result;

    BRCMF_DBG(TRACE, "Enter-New, bsscfgidx=%d mac=%pM\n", ifp->bsscfgidx, ifp->mac_addr);

    ndev->needed_headroom += drvr->hdrlen;

    workqueue_init_work(&ifp->multicast_work, _brcmf_set_multicast_list);

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "broadcom-wlanphy",
        .ctx = ifp,
        .ops = &phy_impl_device_ops,
        .proto_id = ZX_PROTOCOL_WLANPHY_IMPL,
        .proto_ops = &phy_impl_proto_ops,
    };

    struct brcmf_device* device = if_to_dev(ifp);
    struct brcmf_bus* bus = device->bus;

    result = brcmf_bus_device_add(bus, device->zxdev, &args, &device->phy_zxdev);
    if (result != ZX_OK) {
        BRCMF_ERR("device_add failed: %s", zx_status_get_string(result));
        goto fail;
    }
    BRCMF_DBG(TEMP, "device_add() succeeded. Added phy hooks.");

    return ZX_OK;

fail:
    drvr->iflist[ifp->bsscfgidx] = NULL;
    return ZX_ERR_IO_NOT_PRESENT;
}

static void brcmf_net_detach(struct net_device* ndev, bool rtnl_locked) {
    struct brcmf_device* device = ndev_to_dev(ndev);

    // TODO(cphoenix): Make sure devices are removed and memory is freed properly. This code
    // is probably wrong. See WLAN-1057.
    brcmf_free_net_device_vif(ndev);
    brcmf_free_net_device(ndev);
    if (device->phy_zxdev != NULL) {
        device_remove(device->phy_zxdev);
        device->phy_zxdev = NULL;
    }
}

void brcmf_net_setcarrier(struct brcmf_if* ifp, bool on) {
    struct net_device* ndev;

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d carrier=%d\n", ifp->bsscfgidx, on);

    ndev = ifp->ndev;
    brcmf_txflowblock_if(ifp, BRCMF_NETIF_STOP_REASON_DISCONNECTED, !on);
    if (on) {
        if (!netif_carrier_ok(ndev)) {
            netif_carrier_on(ndev);
        }

    } else {
        if (netif_carrier_ok(ndev)) {
            netif_carrier_off(ndev);
        }
    }
}

zx_status_t brcmf_net_p2p_open(struct net_device* ndev) {
    BRCMF_DBG(TRACE, "Enter\n");

    return brcmf_cfg80211_up(ndev);
}

zx_status_t brcmf_net_p2p_stop(struct net_device* ndev) {
    BRCMF_DBG(TRACE, "Enter\n");

    return brcmf_cfg80211_down(ndev);
}

void brcmf_net_p2p_start_xmit(struct brcmf_netbuf* netbuf, struct net_device* ndev) {
    if (netbuf) {
        brcmf_netbuf_free(netbuf);
    }
}

static zx_status_t brcmf_net_p2p_attach(struct brcmf_if* ifp) {
    struct net_device* ndev;

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d mac=%pM\n", ifp->bsscfgidx, ifp->mac_addr);
    ndev = ifp->ndev;

    ndev->initialized_for_ap = false;

    /* set the mac address */
    memcpy(ndev->dev_addr, ifp->mac_addr, ETH_ALEN);

    BRCMF_ERR("* * Tried to register_netdev(ndev); do the ZX thing instead.");
    // TODO(cphoenix): Add back the appropriate "fail:" code
    // If register_netdev failed, goto fail;

    BRCMF_DBG(INFO, "%s: Broadcom Dongle Host Driver\n", ndev->name);

    return ZX_OK;

//fail:
//    ifp->drvr->iflist[ifp->bsscfgidx] = NULL;
//    ndev->netdev_ops = NULL;
//    return ZX_ERR_IO_NOT_PRESENT;
}

zx_status_t brcmf_add_if(struct brcmf_pub* drvr, int32_t bsscfgidx, int32_t ifidx, bool is_p2pdev,
                         const char* name, uint8_t* mac_addr, struct brcmf_if** if_out) {
    struct brcmf_if* ifp;
    struct net_device* ndev;

    if (if_out) {
        *if_out = NULL;
    }

    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d, ifidx=%d\n", bsscfgidx, ifidx);

    ifp = drvr->iflist[bsscfgidx];
    /*
     * Delete the existing interface before overwriting it
     * in case we missed the BRCMF_E_IF_DEL event.
     */
    if (ifp) {
        if (ifidx) {
            BRCMF_ERR("ERROR: netdev:%s already exists\n", ifp->ndev->name);
            netif_stop_queue(ifp->ndev);
            brcmf_net_detach(ifp->ndev, false);
            drvr->iflist[bsscfgidx] = NULL;
        } else {
            BRCMF_DBG(INFO, "netdev:%s ignore IF event\n", ifp->ndev->name);
            return ZX_ERR_INVALID_ARGS;
        }
    }

    if (!drvr->settings->p2p_enable && is_p2pdev) {
        /* this is P2P_DEVICE interface */
        BRCMF_DBG(INFO, "allocate non-netdev interface\n");
        ifp = static_cast<decltype(ifp)>(calloc(1, sizeof(*ifp)));
        if (!ifp) {
            return ZX_ERR_NO_MEMORY;
        }
    } else {
        BRCMF_DBG(INFO, "allocate netdev interface\n");
        /* Allocate netdev, including space for private structure */
        ndev = brcmf_allocate_net_device(sizeof(*ifp), is_p2pdev ? "p2p" : name);
        if (!ndev) {
            return ZX_ERR_NO_MEMORY;
        }

        ndev->needs_free_net_device = true;
        ifp = ndev_to_if(ndev);
        ifp->ndev = ndev;
        /* store mapping ifidx to bsscfgidx */
        if (drvr->if2bss[ifidx] == BRCMF_BSSIDX_INVALID) {
            drvr->if2bss[ifidx] = bsscfgidx;
        }
    }

    ifp->drvr = drvr;
    drvr->iflist[bsscfgidx] = ifp;
    ifp->ifidx = ifidx;
    ifp->bsscfgidx = bsscfgidx;

    ifp->pend_8021x_wait = {};
    //spin_lock_init(&ifp->netif_stop_lock);

    if (mac_addr != NULL) {
        memcpy(ifp->mac_addr, mac_addr, ETH_ALEN);
    }

    BRCMF_DBG(TRACE, " ==== if:%s (%pM) created ===\n", name, ifp->mac_addr);
    if (if_out) {
        *if_out = ifp;
    }
    // This is probably unnecessary - just test/verify after taking it out please!
    zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));
    BRCMF_DBG(TRACE, "Exit");
    return ZX_OK;
}

static void brcmf_del_if(struct brcmf_pub* drvr, int32_t bsscfgidx, bool rtnl_locked) {
    struct brcmf_if* ifp;

    ifp = drvr->iflist[bsscfgidx];
    drvr->iflist[bsscfgidx] = NULL;
    if (!ifp) {
        BRCMF_ERR("Null interface, bsscfgidx=%d\n", bsscfgidx);
        return;
    }
    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d, ifidx=%d\n", bsscfgidx, ifp->ifidx);
    if (drvr->if2bss[ifp->ifidx] == bsscfgidx) {
        drvr->if2bss[ifp->ifidx] = BRCMF_BSSIDX_INVALID;
    }
    if (ifp->ndev) {
        if (bsscfgidx == 0) {
            if (ifp->ndev->initialized_for_ap) {
                rtnl_lock();
                brcmf_netdev_stop(ifp->ndev);
                rtnl_unlock();
            }
        } else {
            netif_stop_queue(ifp->ndev);
        }

        if (ifp->ndev->initialized_for_ap) {
            workqueue_cancel_work(&ifp->multicast_work);
        }
        brcmf_net_detach(ifp->ndev, rtnl_locked);
    }
}

void brcmf_remove_interface(struct brcmf_if* ifp, bool rtnl_locked) {
    if (!ifp || WARN_ON(ifp->drvr->iflist[ifp->bsscfgidx] != ifp)) {
        return;
    }
    BRCMF_DBG(TRACE, "Enter, bsscfgidx=%d, ifidx=%d\n", ifp->bsscfgidx, ifp->ifidx);
    brcmf_proto_del_if(ifp->drvr, ifp);
    brcmf_del_if(ifp->drvr, ifp->bsscfgidx, rtnl_locked);
}

zx_status_t brcmf_attach(struct brcmf_device* dev, struct brcmf_mp_device* settings) {
    struct brcmf_pub* drvr = NULL;
    zx_status_t ret = ZX_OK;
    int i;

    BRCMF_DBG(TRACE, "Enter\n");

    /* Allocate primary brcmf_info */
    drvr = static_cast<decltype(drvr)>(calloc(1, sizeof(struct brcmf_pub)));
    if (!drvr) {
        return ZX_ERR_NO_MEMORY;
    }

    for (i = 0; i < (int)countof(drvr->if2bss); i++) {
        drvr->if2bss[i] = BRCMF_BSSIDX_INVALID;
    }

    mtx_init(&drvr->proto_block, mtx_plain);

    /* Link to bus module */
    drvr->hdrlen = 0;
    drvr->bus_if = dev_to_bus(dev);
    drvr->bus_if->drvr = drvr;
    drvr->settings = settings;

    /* Attach and link in the protocol */
    ret = brcmf_proto_attach(drvr);
    if (ret != ZX_OK) {
        BRCMF_ERR("brcmf_prot_attach failed\n");
        goto fail;
    }

    /* attach firmware event handler */
    brcmf_fweh_attach(drvr);

    return ret;

fail:
    brcmf_detach(dev);

    return ret;
}

zx_status_t brcmf_bus_started(struct brcmf_device* dev) {
    zx_status_t ret = ZX_ERR_IO;
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;
    struct brcmf_if* ifp;
    struct brcmf_if* p2p_ifp;
    zx_status_t err;

    BRCMF_DBG(TRACE, "Enter");

    /* add primary networking interface */
    // TODO(WLAN-740): Name uniqueness
    err = brcmf_add_if(drvr, 0, 0, false, "wlan", NULL, &ifp);
    if (err != ZX_OK) {
        return err;
    }
    p2p_ifp = NULL;

    /* signal bus ready */
    brcmf_bus_change_state(bus_if, BRCMF_BUS_UP);
    /* Bus is ready, do any initialization */
    ret = brcmf_c_preinit_dcmds(ifp);
    if (ret != ZX_OK) {
        goto fail;
    }

    /* assure we have chipid before feature attach */
    if (!bus_if->chip) {
        bus_if->chip = drvr->revinfo.chipnum;
        bus_if->chiprev = drvr->revinfo.chiprev;
        BRCMF_DBG(INFO, "firmware revinfo: chip %x (%d) rev %d\n", bus_if->chip, bus_if->chip,
                  bus_if->chiprev);
    }
    brcmf_feat_attach(drvr);

    ret = brcmf_proto_init_done(drvr);
    if (ret != ZX_OK) {
        goto fail;
    }

    brcmf_proto_add_if(drvr, ifp);

    drvr->config = brcmf_cfg80211_attach(drvr, bus_if->dev, drvr->settings->p2p_enable);
    if (drvr->config == NULL) {
        ret = ZX_ERR_IO;
        goto fail;
    }

    ret = brcmf_net_attach(ifp, false);

    if ((ret == ZX_OK) && (drvr->settings->p2p_enable)) {
        p2p_ifp = drvr->iflist[1];
        if (p2p_ifp) {
            ret = brcmf_net_p2p_attach(p2p_ifp);
        }
    }

    if (ret != ZX_OK) {
        goto fail;
    }

    return ZX_OK;

fail:
    BRCMF_ERR("failed: %d\n", ret);
    if (drvr->config) {
        brcmf_cfg80211_detach(drvr->config);
        drvr->config = NULL;
    }
    brcmf_net_detach(ifp->ndev, false);
    if (p2p_ifp) {
        brcmf_net_detach(p2p_ifp->ndev, false);
    }
    drvr->iflist[0] = NULL;
    drvr->iflist[1] = NULL;
    if (drvr->settings->ignore_probe_fail) {
        ret = ZX_OK;
    }

    return ret;
}

void brcmf_bus_add_txhdrlen(struct brcmf_device* dev, uint len) {
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;

    if (drvr) {
        drvr->hdrlen += len;
    }
}

void brcmf_dev_reset(struct brcmf_device* dev) {
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;

    if (drvr == NULL) {
        return;
    }

    if (drvr->iflist[0]) {
        brcmf_fil_cmd_int_set(drvr->iflist[0], BRCMF_C_TERMINATED, 1, nullptr);
    }
}

void brcmf_detach(struct brcmf_device* dev) {
    int32_t i;
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_pub* drvr = bus_if->drvr;

    BRCMF_DBG(TRACE, "Enter\n");

    if (drvr == NULL) {
        return;
    }

    /* stop firmware event handling */
    brcmf_fweh_detach(drvr);

    brcmf_bus_change_state(bus_if, BRCMF_BUS_DOWN);

    /* make sure primary interface removed last */
    for (i = BRCMF_MAX_IFS - 1; i > -1; i--) {
        brcmf_remove_interface(drvr->iflist[i], false);
    }

    brcmf_cfg80211_detach(drvr->config);

    brcmf_bus_stop(drvr->bus_if);

    brcmf_proto_detach(drvr);

    bus_if->drvr = NULL;
    free(drvr);
}

zx_status_t brcmf_iovar_data_set(struct brcmf_device* dev, const char* name, void* data,
                                 uint32_t len, int32_t* fwerr_ptr) {
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_if* ifp = bus_if->drvr->iflist[0];

    return brcmf_fil_iovar_data_set(ifp, name, data, len, fwerr_ptr);
}

static int brcmf_get_pend_8021x_cnt(struct brcmf_if* ifp) {
    return ifp->pend_8021x_cnt.load();
}

void brcmf_netdev_wait_pend8021x(struct brcmf_if* ifp) {
    zx_status_t result;

    sync_completion_reset(&ifp->pend_8021x_wait);
    if (!brcmf_get_pend_8021x_cnt(ifp)) {
        return;
    }
    result = sync_completion_wait(&ifp->pend_8021x_wait, ZX_MSEC(MAX_WAIT_FOR_8021X_TX_MSEC));

    if (result != ZX_OK) {
        BRCMF_ERR("Timed out waiting for no pending 802.1x packets\n");
    }
}

void brcmf_bus_change_state(struct brcmf_bus* bus, enum brcmf_bus_state state) {
    struct brcmf_pub* drvr = bus->drvr;
    struct net_device* ndev;
    int ifidx;

    BRCMF_DBG(TRACE, "%d -> %d\n", bus->state, state);
    bus->state = state;

    if (state == BRCMF_BUS_UP) {
        for (ifidx = 0; ifidx < BRCMF_MAX_IFS; ifidx++) {
            if ((drvr->iflist[ifidx]) && (drvr->iflist[ifidx]->ndev)) {
                ndev = drvr->iflist[ifidx]->ndev;
                // TODO(cphoenix): Implement Fuchsia equivalent of...
                // BRCMF_DBG(INFO, "This code called netif_wake_queue(ndev)");
                // BRCMF_DBG(INFO, "  if netif_queue_stopped(ndev). Do the Fuchsia equivalent.");
            }
        }
    }
}

zx_status_t brcmf_core_init(zx_device_t* device) {
    pthread_mutexattr_t pmutex_attributes;
    zx_status_t result;

    BRCMF_DBG(TEMP, "brcmfmac: core_init was called\n");

    pthread_mutexattr_init(&pmutex_attributes);
    pthread_mutexattr_settype(&pmutex_attributes, PTHREAD_MUTEX_NORMAL | PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&irq_callback_lock, &pmutex_attributes);

    result = brcmf_bus_register(device);
    if (result != ZX_OK) {
      BRCMF_ERR("Bus registration failed: %s\n", zx_status_get_string(result));
    }
    return result;
}

void brcmf_core_exit(void) {
  brcmf_bus_exit();
}
