/*
 * Copyright (c) 2018 The Fuchsia Authors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// This file contains what's needed to make Linux code compile (but not run) on Fuchsia.
// As the driver is ported, symbols will be removed from this file. When the driver is
// fully ported, this file will be empty and can be deleted.
// The symbols were defined by hand, based only on information from compiler errors and
// code in this driver. Do not expect defines/enums to have correct values, or struct fields to have
// correct types. Function prototypes are even less accurate.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_LINUXISMS_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_LINUXISMS_H_

#include <ddk/debug.h>
#include <netinet/if_ether.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <zircon/assert.h>
#include <zircon/listnode.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

// FROM Josh's linuxisms.h

#define BIT(pos) (1UL << (pos))

#define DIV_ROUND_UP(n, m) (((n) + ((m)-1)) / (m))

#define GENMASK1(val) ((1UL << (val)) - 1)
#define GENMASK(start, end) ((GENMASK1((start) + 1) & ~GENMASK1(end)))

#define WARN(cond, msg)                                                                         \
  ({                                                                                            \
    bool ret_cond = cond;                                                                       \
    if (ret_cond) {                                                                             \
      BRCMF_WARN("brcmfmac: unexpected condition %s warns %s at %s:%d\n", #cond, msg, __FILE__, \
                 __LINE__);                                                                     \
    }                                                                                           \
    ret_cond;                                                                                   \
  })

// TODO(cphoenix): Looks like these evaluate cond multiple times. And maybe should
// pass cond, not #cond, into WARN.
#define WARN_ON(cond)          \
  ({                           \
    if (cond) {                \
      WARN(#cond, "it's bad"); \
    }                          \
    cond;                      \
  })

#define WARN_ON_ONCE(cond)                         \
  ({                                               \
    static bool warn_next = true;                  \
    if (cond && warn_next) {                       \
      WARN(#cond, "(future warnings suppressed)"); \
      warn_next = false;                           \
    }                                              \
    cond;                                          \
  })

#define iowrite32(value, addr)                          \
  do {                                                  \
    (*(volatile uint32_t*)(uintptr_t)(addr)) = (value); \
  } while (0)

#define ioread32(addr) (*(volatile uint32_t*)(uintptr_t)(addr))

#define iowrite16(value, addr)                          \
  do {                                                  \
    (*(volatile uint16_t*)(uintptr_t)(addr)) = (value); \
  } while (0)

#define ioread16(addr) (*(volatile uint16_t*)(uintptr_t)(addr))

#define iowrite8(value, addr)                          \
  do {                                                 \
    (*(volatile uint8_t*)(uintptr_t)(addr)) = (value); \
  } while (0)

#define ioread8(addr) (*(volatile uint8_t*)(uintptr_t)(addr))

#define msleep(ms) zx_nanosleep(zx_deadline_after(ZX_MSEC(ms)))
#define PAUSE zx_nanosleep(zx_deadline_after(ZX_MSEC(50)))

#define min_t(t, a, b) (((t)(a) < (t)(b)) ? (t)(a) : (t)(b))

#define roundup(n, m) (((n) % (m) == 0) ? (n) : (n) + ((m) - ((n) % (m))))

#define LINUX_FUNC(name, paramtype, rettype)                                               \
  static inline rettype name(paramtype foo, ...) {                                         \
    /*BRCMF_LOGF(ERROR, "brcmfmac: * * ERROR * * Called linux function %s\n", #name);   */ \
    return (rettype)0;                                                                     \
  }
#define LINUX_FUNCX(name)                                                                  \
  static inline int name() {                                                               \
    /*BRCMF_LOGF(ERROR, "brcmfmac: * * ERROR * * Called linux function %s\n", #name);   */ \
    return 0;                                                                              \
  }

// clang-format off
#define LINUX_FUNCII(name) LINUX_FUNC(name, int, int)
#define LINUX_FUNCIV(name) LINUX_FUNC(name, int, void*)
#define LINUX_FUNCVV(name) LINUX_FUNC(name, void*, void*)
#define LINUX_FUNCVI(name) LINUX_FUNC(name, void*, int)
#define LINUX_FUNCVS(name) LINUX_FUNC(name, void*, zx_status_t)
#define LINUX_FUNCcVI(name) LINUX_FUNC(name, const void*, int)
#define LINUX_FUNCcVS(name) LINUX_FUNC(name, const void*, zx_status_t)
#define LINUX_FUNCcVV(name) LINUX_FUNC(name, const void*, void*)
#define LINUX_FUNCVU(name) LINUX_FUNC(name, void*, uint16_t)
#define LINUX_FUNCUU(name) LINUX_FUNC(name, uint32_t, uint32_t)

LINUX_FUNCVI(netif_carrier_on)
LINUX_FUNCVI(netif_carrier_ok)
LINUX_FUNCVI(ether_addr_equal) // Trivial
LINUX_FUNCVI(is_valid_ether_addr)
LINUX_FUNCVI(eth_type_trans)
LINUX_FUNCII(BITS_TO_LONGS)
LINUX_FUNCII(gcd)
LINUX_FUNCX(get_random_int)
LINUX_FUNCII(round_up)
LINUX_FUNCVI(nla_put) // Add netlink attribute to netbuf
LINUX_FUNCVI(nla_put_u16) // Add u16 attribute to netbuf
LINUX_FUNCII(MBM_TO_DBM)
LINUX_FUNCX(prandom_u32)

LINUX_FUNCVI(netdev_mc_count) // In core.c - Count of multicast addresses in netdev.
LINUX_FUNCX(rtnl_lock) // In core.c and p2p.c
LINUX_FUNCX(rtnl_unlock) // In core.c and p2p.c
LINUX_FUNCVV(bcm47xx_nvram_get_contents) // In firmware.c
LINUX_FUNCVI(bcm47xx_nvram_release_contents) // In firmware.c

LINUX_FUNCVI(device_set_wakeup_enable) // USB only
LINUX_FUNCVI(usb_deregister) // USB only
LINUX_FUNCVI(driver_for_each_device) // In usb.c only

// Last parameter of this returns an error code. Must be a zx_status_t (0 or negative).
#define SDIO_DEVICE(a,b) (a)
LINUX_FUNCVI(pm_runtime_allow) // SDIO only
LINUX_FUNCVI(pm_runtime_forbid) // SDIO only
// Leave enable/disable_irq_wake() NOPs for now. TODO(cphoenix): Use the ZX equivalent.
LINUX_FUNCII(enable_irq_wake) // SDIO only
LINUX_FUNCII(disable_irq_wake) // SDIO only
LINUX_FUNCVI(of_device_is_compatible)
LINUX_FUNCVI(of_property_read_u32)
LINUX_FUNCVI(of_find_property)
LINUX_FUNCVI(irq_of_parse_and_map) // OF only
LINUX_FUNCII(irqd_get_trigger_type) // OF only
LINUX_FUNCII(irq_get_irq_data) // OF only

LINUX_FUNCVI(device_release_driver)
#define module_param_string(a, b, c, d)

LINUX_FUNCVI(netif_stop_queue)
LINUX_FUNCVI(cfg80211_classify8021d)
LINUX_FUNCVI(cfg80211_crit_proto_stopped)
LINUX_FUNCVV(cfg80211_vendor_cmd_alloc_reply_netbuf)
LINUX_FUNCVI(cfg80211_vendor_cmd_reply)
LINUX_FUNCVI(cfg80211_ready_on_channel)
LINUX_FUNCcVS(cfg80211_get_p2p_attr) // TODO(cphoenix): Can this return >0? If so, adjust usage.
LINUX_FUNCVI(cfg80211_remain_on_channel_expired)
LINUX_FUNCVI(cfg80211_unregister_wdev)
LINUX_FUNCVI(cfg80211_rx_mgmt)
LINUX_FUNCVI(cfg80211_mgmt_tx_status)
LINUX_FUNCVI(cfg80211_check_combinations)
LINUX_FUNCVI(cfg80211_roamed)
LINUX_FUNCVI(cfg80211_connect_done)
LINUX_FUNCVV(cfg80211_ibss_joined)
LINUX_FUNCVV(cfg80211_michael_mic_failure)
LINUX_FUNCII(ieee80211_channel_to_frequency)
LINUX_FUNCVV(ieee80211_get_channel)
LINUX_FUNCII(ieee80211_is_mgmt)
LINUX_FUNCII(ieee80211_is_action)
LINUX_FUNCII(ieee80211_is_probe_resp)
LINUX_FUNCVI(netif_rx)
LINUX_FUNCVI(netif_rx_ni)
LINUX_FUNCVI(netif_carrier_off)

LINUX_FUNCVI(seq_printf)
LINUX_FUNCVS(seq_write)
LINUX_FUNCVI(seq_puts)
LINUX_FUNCVI(dev_coredumpv)

#define pci_write_config_dword(pdev, offset, value) \
    pci_config_write32(&pdev->pci_proto, offset, value)
#define pci_read_config_dword(pdev, offset, value) \
    pci_config_read32(&pdev->pci_proto, offset, value)
LINUX_FUNCcVI(pci_enable_msi)
LINUX_FUNCcVI(pci_disable_msi)
LINUX_FUNCII(free_irq) // PCI
LINUX_FUNCII(request_threaded_irq) // PCI only
LINUX_FUNCVV(dma_alloc_coherent) // PCI only
LINUX_FUNCVV(dma_free_coherent) // PCI only
LINUX_FUNCVI(memcpy_fromio) // PCI only
LINUX_FUNCVS(memcpy_toio) // PCI only
LINUX_FUNCVV(dma_zalloc_coherent) // PCI only
LINUX_FUNCVI(dma_map_single) // PCI only
LINUX_FUNCVI(dma_mapping_error) // PCI only
LINUX_FUNCVI(dma_unmap_single) // PCI only

#define netdev_for_each_mc_addr(a, b) for (({BRCMF_ERR("Calling netdev_for_each_mc_addr"); \
                                             a = nullptr;});1;)
#define for_each_set_bit(a, b, c) for (({BRCMF_ERR("Calling for_each_set_bit"); a = 0;});1;)

#define KBUILD_MODNAME "brcmfmac"

#define IEEE80211_MAX_SSID_LEN (32)

enum {
    IEEE80211_P2P_ATTR_DEVICE_INFO = 2,
    IEEE80211_P2P_ATTR_DEVICE_ID = 3,
    IEEE80211_STYPE_ACTION = 0,
    IEEE80211_FCTL_STYPE = 0,
    IEEE80211_P2P_ATTR_GROUP_ID = 0,
    IEEE80211_STYPE_PROBE_REQ = 0,
    IEEE80211_P2P_ATTR_LISTEN_CHANNEL = (57),
    IFNAMSIZ = (16),
    WLAN_PMKID_LEN = (16),
    WLAN_MAX_KEY_LEN = (32),
    IRQF_SHARED, // TODO(cphoenix) - Used only in PCI
    WLAN_EID_VENDOR_SPECIFIC,
    BSS_PARAM_FLAGS_CTS_PROT,
    BSS_PARAM_FLAGS_SHORT_PREAMBLE,
    BSS_PARAM_FLAGS_SHORT_SLOT_TIME,
    UPDATE_ASSOC_IES,
    WIPHY_FLAG_SUPPORTS_TDLS,
    REGULATORY_CUSTOM_REG,
    NET_NETBUF_PAD = 1,
    IFF_PROMISC,
    NETDEV_TX_OK,
    NETIF_F_IP_CSUM,
    CHECKSUM_PARTIAL,
    CHECKSUM_UNNECESSARY,
    NL80211_SCAN_FLAG_RANDOM_ADDR,
    WLAN_AUTH_OPEN,
    BRCMF_SCAN_IE_LEN_MAX,
};

typedef enum { IRQ_WAKE_THREAD, IRQ_NONE, IRQ_HANDLED } irqreturn_t;

enum ieee80211_vht_mcs_support { FOOVMS };

enum dma_data_direction {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
};

enum nl80211_key_type {
    NL80211_KEYTYPE_GROUP,
    NL80211_KEYTYPE_PAIRWISE,
};

#define TP_PROTO(args...) args
#define MODULE_FIRMWARE(a)
#define MODULE_AUTHOR(a)
#define MODULE_DESCRIPTION(a)
#define MODULE_LICENSE(a)
#define module_param_named(a, b, c, d)
#define MODULE_PARM_DESC(a, b)
#define MODULE_SUPPORTED_DEVICE(a)

#define __iomem            // May want it later
#define IS_ENABLED(a) (a)  // not in compiler.h

struct brcmfmac_pd_cc_entry {
    uint8_t* iso3166;
    uint32_t rev;
    uint8_t* cc;
};

struct brcmfmac_pd_cc {
    int table_size;
    struct brcmfmac_pd_cc_entry* table;
};

struct ieee80211_channel {
    int hw_value;
    uint32_t flags;
    int center_freq;
    int max_antenna_gain;
    int max_power;
    int band;
    uint32_t orig_flags;
};

struct ieee80211_supported_band {
    int band;
    struct ieee80211_channel* channels;
    uint32_t n_channels;
    struct {
        int ht_supported;
        uint16_t cap;
        int ampdu_factor;
        int ampdu_density;
        struct {
            uint8_t rx_mask[32]; // At most 32 bytes are set; it's never read in this driver.
            uint32_t tx_params;
        } mcs;
    } ht_cap;
    struct {
        int vht_supported;
        uint32_t cap;
        struct {
            uint16_t rx_mcs_map;
            uint16_t tx_mcs_map;
        } vht_mcs;
    } vht_cap;
};

struct mac_address {
    uint8_t addr[ETH_ALEN];
};

struct regulatory_request {
    char alpha2[44];
    int initiator;
};

struct wiphy {
    int max_sched_scan_reqs;
    int max_sched_scan_plan_interval;
    int max_sched_scan_ie_len;
    int max_match_sets;
    int max_sched_scan_ssids;
    uint32_t rts_threshold;
    uint32_t frag_threshold;
    uint32_t retry_long;
    uint32_t retry_short;
    uint32_t interface_modes;
    uint32_t max_scan_ssids;
    uint32_t max_scan_ie_len;
    uint32_t max_num_pmkids;
    struct mac_address* addresses;
    uint32_t n_addresses;
    uint32_t signal_type;
    const uint32_t* cipher_suites;
    uint32_t n_cipher_suites;
    uint32_t bss_select_support;
    uint32_t flags;
    const struct ieee80211_txrx_stypes* mgmt_stypes;
    uint32_t max_remain_on_channel_duration;
    uint32_t n_vendor_commands;
    const struct wiphy_vendor_command* vendor_commands;
    uint8_t perm_addr[ETH_ALEN];
    struct brcmf_cfg80211_info* cfg80211_info;
    struct brcmf_device* dev;
};

struct vif_params {
    uint8_t macaddr[ETH_ALEN];
};

struct wireless_dev {
    struct net_device* netdev;
    uint16_t iftype;
    uint8_t address[ETH_ALEN];
    struct wiphy* wiphy;
    struct brcmf_cfg80211_info* cfg80211_info;
};

// This stubs the use of struct sdio_func, which we only use for locking.

struct sdio_func {
    pthread_mutex_t lock;
};

void sdio_claim_host(struct sdio_func* func);
void sdio_release_host(struct sdio_func* func);

struct cfg80211_ssid {
    size_t ssid_len;
    char* ssid;
};

struct ieee80211_mgmt {
    int u;
    uint8_t bssid[ETH_ALEN];
    uint8_t da[ETH_ALEN];
    uint8_t sa[ETH_ALEN];
    uint16_t frame_control;
};

struct notifier_block {
    int foo;
};

struct in6_addr {
    int foo;
};

typedef uint64_t dma_addr_t;

struct ieee80211_regdomain {
    int n_reg_rules;
    char* alpha2;
    struct {
        struct {
            int start_freq_khz;
            int end_freq_khz;
            int max_bandwidth_khz;
        } freq_range;
        struct {
            int max_antenna_gain;
            int max_eirp;
        } power_rule;
        uint32_t flags;
        uint32_t dfs_cac_ms;
    } reg_rules[];
};
#define REG_RULE(...) \
    { .flags = 0 }  // Fill up reg_rules

struct cfg80211_match_set {
    struct cfg80211_ssid ssid;
    uint8_t bssid[ETH_ALEN];
};

struct cfg80211_sched_scan_request {
    int n_ssids;
    int n_match_sets;
    uint64_t reqid;
    int flags;
    uint8_t mac_addr[ETH_ALEN];
    struct cfg80211_ssid* ssids;
    int n_channels;
    struct ieee80211_channel* channels[555];
    struct {
        int interval;
    } * scan_plans;
    uint8_t mac_addr_mask[ETH_ALEN];
    struct cfg80211_match_set match_sets[123];
};

struct wiphy_vendor_command {
    struct {
        int vendor_id;
        int subcmd;
    } unknown_name;
    uint32_t flags;
    zx_status_t (*doit)(struct wiphy* wiphy, struct wireless_dev* wdev, const void* data, int len);
};

struct iface_combination_params {
    int num_different_channels;
    int iftype_num[555];
};

struct key_params {
    uint32_t key_len;
    int cipher;
    void* key;
};

struct cfg80211_wowlan {
    int disconnect;
    struct {
        uint8_t* pattern;
        uint32_t pattern_len;
        uint8_t* mask;
        uint32_t pkt_offset;
    } * patterns;
    uint32_t n_patterns;
    int magic_pkt;
    void* nd_config;
    int gtk_rekey_failure;
};

struct cfg80211_wowlan_nd_match {
    struct {
        void* ssid;
        uint32_t ssid_len;
    } ssid;
    int n_channels;
    int* channels;
};

struct cfg80211_wowlan_nd_info {
    int n_matches;
    struct cfg80211_wowlan_nd_match* matches[555];
    int disconnect;
    int* patterns;
    int n_patterns;
};

struct cfg80211_pmksa {
    uint8_t bssid[ETH_ALEN];
    uint8_t* pmkid;
};

struct cfg80211_beacon_data {
    void* tail;
    int tail_len;
    void* head;
    int head_len;
    void* proberesp_ies;
    int proberesp_ies_len;
};

struct station_parameters {
    uint32_t sta_flags_mask;
    uint32_t sta_flags_set;
};

struct cfg80211_mgmt_tx_params {
    struct ieee80211_channel* chan;
    uint8_t* buf;
    size_t len;
};

struct cfg80211_pmk_conf {
    void* pmk;
    int pmk_len;
};

struct ieee80211_iface_combination {
    int num_different_channels;
    struct ieee80211_iface_limit* limits;
    int max_interfaces;
    int beacon_int_infra_match;
    int n_limits;
};

struct ieee80211_txrx_stypes {
    uint32_t tx;
    uint32_t rx;
};

struct ieee80211_iface_limit {
    int max;
    int types;
};

struct netdev_hw_addr {
    uint8_t addr[ETH_ALEN];
};

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_LINUXISMS_H_
