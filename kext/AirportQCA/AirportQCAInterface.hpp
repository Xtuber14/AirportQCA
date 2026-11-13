// Tahoe IO80211Family V2 interface for the QCA9377 driver.
#ifndef _AIRPORT_QCA_INTERFACE_HPP_
#define _AIRPORT_QCA_INTERFACE_HPP_

#include "Apple80211/Apple80211.h"
#include <IOKit/IOLib.h>

// Tahoe's private protocol header refers to several private request payloads
// that are intentionally opaque to third party drivers.
struct apple80211_roam_profile_all_bands;
struct bss_blacklist;
struct apple80211_he_counters_ctl;
struct apple80211_wcl_wnm_offload_t;
struct apple80211_fw_clock_info;
struct apple80211_timesync_stats;
struct apple80211_system_sleep_config;
struct apple80211_smartcca_opmode;
struct apple80211_lqm_statistics;
struct apple80211_device_orientation;
struct apple80211_device_accessory_info;
struct apple80211_p2p_device_capability;
struct apple80211_powertable_version_data;
struct apple80211_dynamic_rssi_window_config;
struct apple80211_legacy_roam_profile_config;
struct apple80211_wcl_arp_mode;
struct apple80211_wcl_abort_join;
struct apple80211AssocCandidates;
struct apple80211_wcl_qos_params;
struct apple80211_voice_ind_state;
struct apple80211_mws_accessory_power_limit;
struct apple80211_power_profile;
struct apple80211_interface_setting;
struct apple80211_bypass_tx_power_cap;
struct apple80211_facetime_wificalling_params;
struct apple80211_ipv4_params;
struct apple80211_wcl_wnm_config_t;
struct apple80211_limited_aggregation_config;
struct apple80211_bcn_mute_config;
struct apple80211_eap_filter_config;
struct apple80211_wow_low_power_mode;
struct apple80211_dual_power_mode_params;

class AirportQCA;

class AirportQCAInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(AirportQCAInterface)

public:
    bool init() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void setDriver(AirportQCA *driver) { fDriver = driver; }
    IOReturn setMacAddress(struct mloAddrArray &addresses) APPLE_KEXT_OVERRIDE;
    IOReturn getHW_ADDR(apple80211_hw_mac_address *data) APPLE_KEXT_OVERRIDE;
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *data) APPLE_KEXT_OVERRIDE;

    // Explicitly implement the Tahoe request surface. Unsupported capabilities
    // return kIOReturnUnsupported; implemented requests are overridden below.

virtual IOReturn getCHANNEL(apple80211_channel_data *d);
virtual IOReturn getPOWERSAVE(apple80211_powersave_data *d);
virtual IOReturn getTXPOWER(apple80211_txpower_data *d);
virtual IOReturn getRATE(apple80211_rate_data *d);
virtual IOReturn getOP_MODE(apple80211_opmode_data *d);
virtual IOReturn getRSSI(apple80211_rssi_data *d);
virtual IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data *d);
virtual IOReturn getGUARD_INTERVAL(apple80211_guard_interval_data*) { return kIOReturnUnsupported; }
virtual IOReturn getMCS(apple80211_mcs_data*) { return kIOReturnUnsupported; }
virtual IOReturn getPOWER_DEBUG_INFO(apple80211_power_debug_info*) { return kIOReturnUnsupported; }
virtual IOReturn getHT_CAPABILITY(apple80211_ht_capability*) { return kIOReturnUnsupported; }
virtual IOReturn getMCS_VHT(apple80211_mcs_vht_data*) { return kIOReturnUnsupported; }
virtual IOReturn getCHANNELS_INFO(apple80211_channels_info*) { return kIOReturnUnsupported; }
virtual IOReturn getVHT_CAPABILITY(apple80211_vht_capability*) { return kIOReturnUnsupported; }
virtual IOReturn getROAM_PROFILE(apple80211_roam_profile_all_bands*) { return kIOReturnUnsupported; }
virtual IOReturn getCHIP_COUNTER_STATS(apple80211_chip_stats*) { return kIOReturnUnsupported; }
virtual IOReturn getDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params*) { return kIOReturnUnsupported; }
virtual IOReturn getLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting*) { return kIOReturnUnsupported; }
virtual IOReturn getCOUNTRY_CHANNELS(apple80211_country_channel_data*) { return kIOReturnUnsupported; }
virtual IOReturn getPRIVATE_MAC(apple80211_private_mac_data*) { return kIOReturnUnsupported; }
virtual IOReturn getRANGING_ENABLE(apple80211_ranging_enable_request_t*) { return kIOReturnUnsupported; }
virtual IOReturn getRANGING_START(apple80211_ranging_start_request_t*) { return kIOReturnUnsupported; }
virtual IOReturn getAWDL_RSDB_CAPS(apple80211_rsdb_capability*) { return kIOReturnUnsupported; }
virtual IOReturn getTKO_PARAMS(apple80211_tko_params*) { return kIOReturnUnsupported; }
virtual IOReturn getTKO_DUMP(apple80211_tko_dump*) { return kIOReturnUnsupported; }
virtual IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data*) { return kIOReturnUnsupported; }
virtual IOReturn getBTCOEX_PROFILE(apple80211_btcoex_profile*) { return kIOReturnUnsupported; }
virtual IOReturn getBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data*) { return kIOReturnUnsupported; }
virtual IOReturn getTRAP_INFO(apple80211_trap_info_data*) { return kIOReturnUnsupported; }
virtual IOReturn getTHERMAL_INDEX(apple80211_thermal_index_t*) { return kIOReturnUnsupported; }
virtual IOReturn getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data*) { return kIOReturnUnsupported; }
virtual IOReturn getBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable*) { return kIOReturnUnsupported; }
virtual IOReturn getPOWER_BUDGET(apple80211_power_budget_t*) { return kIOReturnUnsupported; }
virtual IOReturn getOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t*) { return kIOReturnUnsupported; }
virtual IOReturn getRANGING_CAPS(apple80211_ranging_capabilities_t*) { return kIOReturnUnsupported; }
virtual IOReturn getLQM_CONFIG(apple80211_lqm_config_t*) { return kIOReturnUnsupported; }
virtual IOReturn getTRAP_CRASHTRACER_MINI_DUMP(apple80211_trap_mini_dump_data*) { return kIOReturnUnsupported; }
virtual IOReturn getBEACON_INFO(apple80211_beacon_info_t*) { return kIOReturnUnsupported; }
virtual IOReturn getCHIP_POWER_RANGE(apple80211_chip_power_limit*) { return kIOReturnUnsupported; }
virtual IOReturn getNSS(apple80211_nss_data*) { return kIOReturnUnsupported; }
virtual IOReturn getCHIP_DIAGS(appl80211_chip_diags_data*) { return kIOReturnUnsupported; }
virtual IOReturn getHP2P_CTRL(apple80211_hp2p_ctrl*) { return kIOReturnUnsupported; }
virtual IOReturn getBSS_BLACKLIST(bss_blacklist*) { return kIOReturnUnsupported; }
virtual IOReturn getTXRX_CHAIN_INFO(apple80211_txrx_chain_info*) { return kIOReturnUnsupported; }
virtual IOReturn getMIMO_STATUS(apple80211_mimo_status*) { return kIOReturnUnsupported; }
virtual IOReturn getCUR_PMK(apple80211_pmk*) { return kIOReturnUnsupported; }
virtual IOReturn getDYNSAR_DETAIL(apple80211_dynsar_detail*) { return kIOReturnUnsupported; }
virtual IOReturn getCOUNTRY_CHANNELS_INFO(apple80211_channels_info*) { return kIOReturnUnsupported; }
virtual IOReturn getLQM_SUMMARY(apple80211_lqm_summary*) { return kIOReturnUnsupported; }
virtual IOReturn getSLOW_WIFI_FEATURE_ENABLED(apple80211_slow_wifi_feature_enabled*) { return kIOReturnUnsupported; }
virtual IOReturn getTIMESYNC_INFO(apple80211_timesync_info*) { return kIOReturnUnsupported; }
virtual IOReturn getSENSING_DATA(apple80211_sensing_data_t*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_BSS_INFO(apple80211_beacon_msg*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_GET_TX_BLANKING_STATUS(unsigned int*) { return kIOReturnUnsupported; }
virtual IOReturn getHE_COUNTERS(apple80211_he_counters_ctl*) { return kIOReturnUnsupported; }
virtual IOReturn getRSN_XE(apple80211_rsn_xe_data*) { return kIOReturnUnsupported; }
virtual IOReturn getSIB_COEX_STATUS(apple80211_sib_coex_status*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_LOW_LATENCY_INFO_STATS(apple80211_wcl_low_latency_stats*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_BGSCAN_CACHE_RESULT(apple80211_bgscan_cached_network_data_list*) { return kIOReturnUnsupported; }
virtual IOReturn getWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) { return kIOReturnUnsupported; }
virtual IOReturn getWIFI_NOISE_PER_ANT(apple80211_noise_per_ant_t*) { return kIOReturnUnsupported; }
virtual IOReturn getFW_CLOCK_INFO(apple80211_fw_clock_info*) { return kIOReturnUnsupported; }
virtual IOReturn getTIMESYNC_STATS(apple80211_timesync_stats*) { return kIOReturnUnsupported; }
virtual IOReturn getSYSTEM_SLEEP_CONFIG(apple80211_system_sleep_config*) { return kIOReturnUnsupported; }
virtual IOReturn getSMARTCCA_OPMODE(apple80211_smartcca_opmode*) { return kIOReturnUnsupported; }
virtual IOReturn getLQM_STATISTICS(apple80211_lqm_statistics*) { return kIOReturnUnsupported; }
virtual IOReturn getHE_CAPABILITY(apple80211_he_capability*) { return kIOReturnUnsupported; }
virtual IOReturn getDEVICE_ORIENTATION(apple80211_device_orientation*) { return kIOReturnUnsupported; }
virtual IOReturn getACCESSORY_STATE(apple80211_device_accessory_info*) { return kIOReturnUnsupported; }
virtual IOReturn getP2P_DEVICE_CAPABILITY(apple80211_p2p_device_capability*) { return kIOReturnUnsupported; }
virtual IOReturn getPOWERTABLE_VERSION(apple80211_powertable_version_data*) { return kIOReturnUnsupported; }
virtual IOReturn setCIPHER_KEY(apple80211_key *d);
virtual IOReturn setCHANNEL(apple80211_channel_data *d);
virtual IOReturn setPOWERSAVE(apple80211_powersave_data*) { return kIOReturnUnsupported; }
virtual IOReturn setTXPOWER(apple80211_txpower_data*) { return kIOReturnUnsupported; }
virtual IOReturn setRATE(apple80211_rate_data*) { return kIOReturnUnsupported; }
virtual IOReturn setIBSS_MODE(apple80211_network_data*) { return kIOReturnUnsupported; }
virtual IOReturn setAP_MODE(apple80211_apmode_data*) { return kIOReturnUnsupported; }
virtual IOReturn setIE(apple80211_ie_data*) { return kIOReturnUnsupported; }
virtual IOReturn setWOW_TEST(apple80211_wow_test_data*) { return kIOReturnUnsupported; }
virtual IOReturn setCLEAR_PMKSA_CACHE(void*) { return kIOReturnUnsupported; }
virtual IOReturn setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data*) { return kIOReturnUnsupported; }
virtual IOReturn setHT_CAPABILITY(apple80211_ht_capability*) { return kIOReturnUnsupported; }
virtual IOReturn setOFFLOAD_ARP(apple80211_offload_arp_data*) { return kIOReturnUnsupported; }
virtual IOReturn setOFFLOAD_NDP(apple80211_offload_ndp_data*) { return kIOReturnUnsupported; }
virtual IOReturn setGAS_REQ(apple80211_gas_query_t*) { return kIOReturnUnsupported; }
virtual IOReturn setVHT_CAPABILITY(apple80211_vht_capability*) { return kIOReturnUnsupported; }
virtual IOReturn setROAM_PROFILE(apple80211_roam_profile_all_bands*) { return kIOReturnUnsupported; }
virtual IOReturn setDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params*) { return kIOReturnUnsupported; }
virtual IOReturn setLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting*) { return kIOReturnUnsupported; }
virtual IOReturn setPRIVATE_MAC(apple80211_private_mac_data*) { return kIOReturnUnsupported; }
virtual IOReturn setRESET_CHIP(apple80211_reset_command*) { return kIOReturnUnsupported; }
virtual IOReturn setCRASH(apple80211_crash_command*) { return kIOReturnUnsupported; }
virtual IOReturn setRANGING_ENABLE(apple80211_ranging_enable_request_t*) { return kIOReturnUnsupported; }
virtual IOReturn setRANGING_START(apple80211_ranging_start_request_t*) { return kIOReturnUnsupported; }
virtual IOReturn setRANGING_AUTHENTICATE(apple80211_ranging_authenticate_request_t*) { return kIOReturnUnsupported; }
virtual IOReturn setTKO_PARAMS(apple80211_tko_params*) { return kIOReturnUnsupported; }
virtual IOReturn setBTCOEX_PROFILE(apple80211_btcoex_profile*) { return kIOReturnUnsupported; }
virtual IOReturn setBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data*) { return kIOReturnUnsupported; }
virtual IOReturn setTHERMAL_INDEX(apple80211_thermal_index_t*) { return kIOReturnUnsupported; }
virtual IOReturn setBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable*) { return kIOReturnUnsupported; }
virtual IOReturn setPOWER_BUDGET(apple80211_power_budget_t*) { return kIOReturnUnsupported; }
virtual IOReturn setOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t*) { return kIOReturnUnsupported; }
virtual IOReturn setLQM_CONFIG(apple80211_lqm_config_t*) { return kIOReturnUnsupported; }
virtual IOReturn setDYNAMIC_RSSI_WINDOW_CONFIG(apple80211_dynamic_rssi_window_config*) { return kIOReturnUnsupported; }
virtual IOReturn setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data*) { return kIOReturnUnsupported; }
virtual IOReturn setHP2P_CTRL(apple80211_hp2p_ctrl*) { return kIOReturnUnsupported; }
virtual IOReturn setBSS_BLACKLIST(bss_blacklist*) { return kIOReturnUnsupported; }
virtual IOReturn setSET_PROPERTY(apple80211_set_property_unserialized_data*) { return kIOReturnUnsupported; }
virtual IOReturn setROAM_CACHE_UPDATE(apple80211_roam_cache_data*) { return kIOReturnUnsupported; }
virtual IOReturn setPM_MODE(apple80211_pm_mode*) { return kIOReturnUnsupported; }
virtual IOReturn setSET_WIFI_ASSERTION_STATE(apple80211_wifi_assertion_data*) { return kIOReturnUnsupported; }
virtual IOReturn setREALTIME_QOS_MSCS(apple80211_state_data*) { return kIOReturnUnsupported; }
virtual IOReturn setSENSING_ENABLE(apple80211_sensing_enable_t*) { return kIOReturnUnsupported; }
virtual IOReturn setSENSING_DISABLE(apple80211_sensing_disable_t*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *d);
virtual IOReturn setWCL_REASSOC(apple80211_reassoc*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_LEGACY_ROAM_PROFILE_CONFIG(apple80211_legacy_roam_profile_config*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ROAM_USER_CACHE(apple80211_user_roam_cache*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_SCAN_ABORT(void*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ARP_MODE(apple80211_wcl_arp_mode*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_JOIN_ABORT(apple80211_wcl_abort_join*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_TRIGGER_CC(triggerCC*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *d);
virtual IOReturn setWCL_ASSOCIATE(apple80211AssocCandidates *request);
virtual IOReturn setWCL_QOS_PARAMS(apple80211_wcl_qos_params*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_LINK_UP_DONE(void*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime*) { return kIOReturnUnsupported; }
virtual IOReturn setVOICE_IND_STATE(apple80211_voice_ind_state*) { return kIOReturnUnsupported; }
virtual IOReturn setRSN_XE(apple80211_rsn_xe_data*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_ACCESSORY_POWER_LIMIT_WIFI_ENH(apple80211_mws_accessory_power_limit*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ULOFDMA_STATE(apple80211_wcl_ulofdma_state*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ACTION_FRAME(apple80211_wcl_action_frame*) { return kIOReturnUnsupported; }
virtual IOReturn setGAS_ABORT(void*) { return kIOReturnUnsupported; }
virtual IOReturn setOS_FEATURE_FLAGS(apple80211_feature_flags*) { return kIOReturnUnsupported; }
virtual IOReturn setDHCP_RENEWAL_DATA(apple80211_dhcp_renewal_data*) { return kIOReturnUnsupported; }
virtual IOReturn setBATTERY_POWERSAVE_CONFIG(apple80211_battery_ps_config*) { return kIOReturnUnsupported; }
virtual IOReturn setMIMO_CONFIG(apple80211_mimo_config*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_CONFIG_BG_NETWORK(apple80211_bg_network*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_CONFIG_BGSCAN(apple80211_bg_scan*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_CONFIG_BG_PARAMS(apple80211_bg_params*) { return kIOReturnUnsupported; }
virtual IOReturn setPOWER_PROFILE(apple80211_power_profile*) { return kIOReturnUnsupported; }
virtual IOReturn setHEARTBEAT(void*) { return kIOReturnUnsupported; }
virtual IOReturn setINTERFACE_SETTING(apple80211_interface_setting*) { return kIOReturnUnsupported; }
virtual IOReturn setBYPASS_TX_POWER_CAP(apple80211_bypass_tx_power_cap*) { return kIOReturnUnsupported; }
virtual IOReturn setFACETIME_WIFICALLING_PARAMS(apple80211_facetime_wificalling_params*) { return kIOReturnUnsupported; }
virtual IOReturn setIPV4_PARAMS(apple80211_ipv4_params*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_WNM_OPS(apple80211_wcl_wnm_config_t*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_LIMITED_AGGREGATION(apple80211_limited_aggregation_config*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_BCN_MUTE_CONFIG(apple80211_bcn_mute_config*) { return kIOReturnUnsupported; }
virtual IOReturn setEAP_FILTER_CONFIG(apple80211_eap_filter_config*) { return kIOReturnUnsupported; }
virtual IOReturn setWOW_LOW_POWER_MODE(apple80211_wow_low_power_mode*) { return kIOReturnUnsupported; }
virtual IOReturn setDUAL_POWER_MODE(apple80211_dual_power_mode_params*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_ASSOCIATED_SLEEP(apple80211_associated_sleep_config*) { return kIOReturnUnsupported; }
virtual IOReturn setCONGESTION_CTRL_IND(apple80211_congestion_control_indication*) { return kIOReturnUnsupported; }
virtual IOReturn setSTAND_ALONE_MODE_STATE(apple80211_standalone_state*) { return kIOReturnUnsupported; }
virtual IOReturn setIPV6_PARAMS(apple80211_ipv6_params*) { return kIOReturnUnsupported; }
virtual IOReturn setINFRA_ENUMERATED(apple80211_infra_enumerated*) { return kIOReturnUnsupported; }
virtual IOReturn setLMTPC_CONFIG(apple80211_lmtpc_config*) { return kIOReturnUnsupported; }
virtual IOReturn setTRAFFIC_ENG_PARAMS(apple80211_traffic_eng_params*) { return kIOReturnUnsupported; }
virtual IOReturn setLE_SCAN_PARAM(apple80211_le_scan_params*) { return kIOReturnUnsupported; }
virtual IOReturn setTIMESYNC_GPIO(apple80211_timesync_gpio*) { return kIOReturnUnsupported; }
virtual IOReturn setHOST_CLOCK_INFO(apple80211_host_clock_info*) { return kIOReturnUnsupported; }
virtual IOReturn setFW_CLOCK_SOURCE(apple80211_fw_clock_source*) { return kIOReturnUnsupported; }
virtual IOReturn setTIMESYNC_TX_POLICY(apple80211_timesync_tx_policy*) { return kIOReturnUnsupported; }
virtual IOReturn setTIMESYNC_RX_POLICY(apple80211_timesync_rx_policy*) { return kIOReturnUnsupported; }
virtual IOReturn setTIMESTAMPING_EN(apple80211_timestamping_en*) { return kIOReturnUnsupported; }
virtual IOReturn setWCL_SOI_CONFIG(appl80211_sleep_on_inactivity_config*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_TIME_SHARING_WIFI_ENH(apple80211_mws_time_sharing*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_WIFI_TYPE_7_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_COEX_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_DISABLE_OCL_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_RFEM_CONFIG_WIFI_ENH(apple80211_mws_rfem_config*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_ASSOC_PROTECTION_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_SCAN_FREQ_WIFI_ENH(apple80211_mws_scan_freq*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_SCAN_FREQ_MODE_WIFI_ENH(apple80211_mws_scan_freq_mode*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_CONDITION_ID_BITMAP_WIFI_ENH(apple80211_mws_condition_id_config*) { return kIOReturnUnsupported; }
virtual IOReturn setMWS_ANTENNA_SELECTION_WIFI_ENH(apple80211_mws_antenna_selection*) { return kIOReturnUnsupported; }
virtual IOReturn setNDD_REQ(apple80211_ndd_data*) { return kIOReturnUnsupported; }
virtual IOReturn setDBRG_ENTROPY(apple80211_drbg_entropy*) { return kIOReturnUnsupported; }
virtual IOReturn setSDB_ENABLE(apple80211_sdb_enable*) { return kIOReturnUnsupported; }
virtual IOReturn setBTCOEX_EXT_PROFILE(apple80211_btcoex_ext_profile*) { return kIOReturnUnsupported; }
virtual IOReturn setDEVICE_ORIENTATION(apple80211_device_orientation*) { return kIOReturnUnsupported; }
virtual IOReturn setACCESSORY_STATE(apple80211_device_accessory_info*) { return kIOReturnUnsupported; }
virtual IOReturn setOS_ELIGIBILITY(apple80211_os_eligibility*) { return kIOReturnUnsupported; }
virtual IOReturn setTX_MODE_CONFIG(apple80211_tx_mode_config*) { return kIOReturnUnsupported; }
virtual IOReturn setMITIGATE_INTERFERENCE(apple80211_mitigate_interference*) { return kIOReturnUnsupported; }

private:
    AirportQCA *fDriver;
};

#endif
