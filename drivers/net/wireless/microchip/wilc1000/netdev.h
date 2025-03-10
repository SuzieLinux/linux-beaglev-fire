/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2012 - 2018 Microchip Technology Inc., and its subsidiaries.
 * All rights reserved.
 */

#ifndef WILC_NETDEV_H
#define WILC_NETDEV_H

#include <linux/tcp.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/ieee80211_radiotap.h>
#include <linux/if_arp.h>
#include <linux/gpio/consumer.h>

#include "hif.h"
#include "wlan.h"
#include "wlan_cfg.h"
#include "wilcs02_loopback.h"

extern int wait_for_recovery;

#define FLOW_CONTROL_LOWER_THRESHOLD		128
#define FLOW_CONTROL_UPPER_THRESHOLD		256

#define PMKID_FOUND				1
#define NUM_STA_ASSOCIATED			8

#define TCP_ACK_FILTER_LINK_SPEED_THRESH	54
#define DEFAULT_LINK_SPEED			72

#define TX_BACKOFF_WEIGHT_MS			1

#define ANT_SWTCH_INVALID_GPIO_CTRL		0
#define ANT_SWTCH_SNGL_GPIO_CTRL		1
#define ANT_SWTCH_DUAL_GPIO_CTRL		2

struct wilc_wfi_stats {
	unsigned long rx_packets;
	unsigned long tx_packets;
	unsigned long rx_bytes;
	unsigned long tx_bytes;
	u64 rx_time;
	u64 tx_time;

};

struct wilc_wfi_key {
	u8 *key;
	u8 *seq;
	int key_len;
	int seq_len;
	u32 cipher;
};

struct sta_info {
	u8 sta_associated_bss[WILC_MAX_NUM_STA][ETH_ALEN];
};

/* Parameters needed for host interface for remaining on channel */
struct wilc_wfi_p2p_listen_params {
	struct ieee80211_channel *listen_ch;
	u32 listen_duration;
	u64 listen_cookie;
};

/* Struct to buffer eapol 1/4 frame */
struct wilc_buffered_eap {
	unsigned int size;
	unsigned int pkt_offset;
	u8 *buff;
};

struct wilc_p2p_var {
	u8 local_random;
	u8 recv_random;
	bool is_wilc_ie;
};

static const u32 wilc_cipher_suites[] = {
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_AES_CMAC
};

#define CHAN2G(_channel, _freq, _flags) {	 \
	.band             = NL80211_BAND_2GHZ, \
	.center_freq      = (_freq),		 \
	.hw_value         = (_channel),		 \
	.flags            = (_flags),		 \
	.max_antenna_gain = 0,			 \
	.max_power        = 30,			 \
}

static const struct ieee80211_channel wilc_2ghz_channels[] = {
	CHAN2G(1,  2412, 0),
	CHAN2G(2,  2417, 0),
	CHAN2G(3,  2422, 0),
	CHAN2G(4,  2427, 0),
	CHAN2G(5,  2432, 0),
	CHAN2G(6,  2437, 0),
	CHAN2G(7,  2442, 0),
	CHAN2G(8,  2447, 0),
	CHAN2G(9,  2452, 0),
	CHAN2G(10, 2457, 0),
	CHAN2G(11, 2462, 0),
	CHAN2G(12, 2467, 0),
	CHAN2G(13, 2472, 0),
	CHAN2G(14, 2484, 0)
};

#define RATETAB_ENT(_rate, _hw_value, _flags) {	\
	.bitrate  = (_rate),			\
	.hw_value = (_hw_value),		\
	.flags    = (_flags),			\
}

static struct ieee80211_rate wilc_bitrates[] = {
	RATETAB_ENT(10,  0,  0),
	RATETAB_ENT(20,  1,  0),
	RATETAB_ENT(55,  2,  0),
	RATETAB_ENT(110, 3,  0),
	RATETAB_ENT(60,  9,  0),
	RATETAB_ENT(90,  6,  0),
	RATETAB_ENT(120, 7,  0),
	RATETAB_ENT(180, 8,  0),
	RATETAB_ENT(240, 9,  0),
	RATETAB_ENT(360, 10, 0),
	RATETAB_ENT(480, 11, 0),
	RATETAB_ENT(540, 12, 0)
};

struct wilc_priv {
	struct wireless_dev wdev;
	struct cfg80211_scan_request *scan_req;

	struct wilc_wfi_p2p_listen_params remain_on_ch_params;
	u64 tx_cookie;

	bool cfg_scanning;

	u8 associated_bss[ETH_ALEN];
	struct sta_info assoc_stainfo;
	struct sk_buff *skb;
	struct net_device *dev;
	struct host_if_drv *hif_drv;
	struct wilc_pmkid_attr pmkid_list;

	/* The real interface that the monitor is on */
	struct net_device *real_ndev;
	struct wilc_wfi_key *wilc_gtk[WILC_MAX_NUM_STA];
	struct wilc_wfi_key *wilc_ptk[WILC_MAX_NUM_STA];
	struct wilc_wfi_key *wilc_igtk[2];
	u8 wilc_groupkey;

	/* mutexes */
	struct mutex scan_req_lock;

	struct wilc_buffered_eap *buffered_eap;

	struct timer_list eap_buff_timer;
	int scanned_cnt;
	struct wilc_p2p_var p2p;
	u64 inc_roc_cookie;
};

#define MAX_TCP_SESSION                25
#define MAX_PENDING_ACKS               256

struct ack_session_info {
	u32 seq_num;
	u32 bigger_ack_num;
	u16 src_port;
	u16 dst_port;
	u16 status;
};

struct pending_acks {
	u32 ack_num;
	u32 session_index;
	struct txq_entry_t  *txqe;
};

struct tcp_ack_filter {
	struct ack_session_info ack_session_info[2 * MAX_TCP_SESSION];
	struct pending_acks pending_acks[MAX_PENDING_ACKS];
	u32 pending_base;
	u32 tcp_session;
	u32 pending_acks_idx;
	bool enabled;
};

#define WILC_P2P_ROLE_GO	1
#define WILC_P2P_ROLE_CLIENT	0

struct sysfs_attr_group {
	bool p2p_mode;
	u8 ant_swtch_mode;
	u8 antenna1;
	u8 antenna2;
	u8 fw_dbg_level;
};

struct wilc_vif {
	u8 idx;
	u8 iftype;
	int monitor_flag;
	int mac_opened;
	u32 mgmt_reg_stypes;
	struct net_device_stats netstats;
	struct wilc *wilc;
	u8 bssid[ETH_ALEN];
	struct host_if_drv *hif_drv;
	struct net_device *ndev;

	struct timer_list periodic_rssi;
	struct rf_info periodic_stat;
	struct tcp_ack_filter ack_filter;
	bool connecting;
	struct wilc_priv priv;
	struct list_head list;
	u8 restart;
	bool p2p_listen_state;
	struct cfg80211_bss *bss;
	struct cfg80211_external_auth_params auth;
};

struct wilc_power_gpios {
	int reset;
	int chip_en;
};

struct wilc_power {
	struct wilc_power_gpios gpios;
	u8 status[DEV_MAX];
};

struct wilc_tx_queue_status {
	u8 buffer[AC_BUFFER_SIZE];
	u16 end_index;
	u16 cnt[NQUEUES];
	u16 sum;
	bool initialized;
};

struct wilc {
	struct wiphy *wiphy;
	const struct wilc_hif_func *hif_func;
	int io_type;
	s8 mac_status;
	struct clk *rtc_clk;
	bool initialized;
	u32 chipid;
	int dev_irq_num;
	int close;
	u8 vif_num;
	struct list_head vif_list;

	/* protect vif list */
	struct mutex vif_mutex;
	struct srcu_struct srcu;
	u8 open_ifcs;

	/* protect head of transmit queue */
	struct mutex txq_add_to_head_cs;

	/* protect txq_entry_t transmit queue */
	spinlock_t txq_spinlock;

	/* protect rxq_entry_t receiver queue */
	struct mutex rxq_cs;

	/* lock to protect hif access */
	struct mutex hif_cs;

	struct completion cfg_event;
	struct completion sync_event;
	struct completion txq_event;
	struct completion txq_thread_started;
	struct completion debug_thread_started;
	struct task_struct *txq_thread;
	struct task_struct *debug_thread;

	int quit;

	bool is_mmc_spi;

	/* lock to protect issue of wid command to firmware */
	struct mutex cfg_cmd_lock;
	struct wilc_cfg_frame cfg_frame;
	u32 cfg_frame_offset;
	u8 cfg_seq_no;

	u8 *rx_buffer;
	u32 rx_buffer_offset;
	u8 *tx_buffer;
	u32 *vmm_table;

	struct txq_handle txq[NQUEUES];
	int txq_entries;

	struct wilc_tx_queue_status tx_q_limit;
	struct rxq_entry_t rxq_head;

	const struct firmware *firmware;

	struct device *dev;
	struct device *dt_dev;

	enum wilc_chip_type chip;
	struct wilc_vmm_ctl vmm_ctl;
	struct wilc_power power;
	uint8_t keep_awake[DEV_MAX];
	struct mutex cs;
	struct workqueue_struct *hif_workqueue;
	struct wilc_cfg cfg;
	void *bus_data;
	struct net_device *monitor_dev;

	/* deinit lock */
	struct mutex deinit_lock;
	u8 sta_ch;
	u8 op_ch;
	struct sysfs_attr_group attr_sysfs;
	struct ieee80211_channel channels[ARRAY_SIZE(wilc_2ghz_channels)];
	struct ieee80211_rate bitrates[ARRAY_SIZE(wilc_bitrates)];
	struct ieee80211_supported_band band;
	u32 cipher_suites[ARRAY_SIZE(wilc_cipher_suites)];
};

struct wilc_wfi_mon_priv {
	struct net_device *real_ndev;
};

#ifdef WILC_S02_TEST_BUS_INTERFACE
extern bool is_test_mode;
#endif

void wilc_frmw_to_host(struct wilc_vif *vif, u8 *buff, u32 size,
		       u32 pkt_offset, u8 status);
void wilc_mac_indicate(struct wilc *wilc);
void wilc_netdev_cleanup(struct wilc *wilc);
void wilc_wfi_mgmt_rx(struct wilc *wilc, u8 *buff, u32 size, bool is_auth);
void wilc_wlan_set_bssid(struct net_device *wilc_netdev, const u8 *bssid,
			 u8 mode);
struct wilc_vif *wilc_netdev_ifc_init(struct wilc *wl, const char *name,
				      int vif_type, enum nl80211_iftype type,
				      bool rtnl_locked);
int wilc_bt_power_up(struct wilc *wilc, int source);
int wilc_bt_power_down(struct wilc *wilc, int source);
int wilc_s02_reset_firmware(struct wilc *wilc, u32 type);
int wilc_s02_check_firmware_download(struct wilc *wl);
#endif
