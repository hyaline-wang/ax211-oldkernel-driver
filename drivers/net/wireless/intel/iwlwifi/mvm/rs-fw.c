// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2017 Intel Deutschland GmbH
 * Copyright (C) 2018-2022 Intel Corporation
 */
#include "rs.h"
#include "fw-api.h"
#include "sta.h"
#include "iwl-op-mode.h"
#include "mvm.h"

static u8 rs_fw_bw_from_sta_bw(const struct ieee80211_sta *sta)
{
	switch (sta->bandwidth) {
	case IEEE80211_STA_RX_BW_320:
		return IWL_TLC_MNG_CH_WIDTH_320MHZ;
	case IEEE80211_STA_RX_BW_160:
		return IWL_TLC_MNG_CH_WIDTH_160MHZ;
	case IEEE80211_STA_RX_BW_80:
		return IWL_TLC_MNG_CH_WIDTH_80MHZ;
	case IEEE80211_STA_RX_BW_40:
		return IWL_TLC_MNG_CH_WIDTH_40MHZ;
	case IEEE80211_STA_RX_BW_20:
	default:
		return IWL_TLC_MNG_CH_WIDTH_20MHZ;
	}
}

static u8 rs_fw_set_active_chains(u8 chains)
{
	u8 fw_chains = 0;

	if (chains & ANT_A)
		fw_chains |= IWL_TLC_MNG_CHAIN_A_MSK;
	if (chains & ANT_B)
		fw_chains |= IWL_TLC_MNG_CHAIN_B_MSK;

	return fw_chains;
}

static u8 rs_fw_sgi_cw_support(struct ieee80211_sta *sta)
{
	struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	struct ieee80211_sta_he_cap *he_cap = &sta->he_cap;
	u8 supp = 0;

	if (he_cap->has_he)
		return 0;

	if (ht_cap->cap & IEEE80211_HT_CAP_SGI_20)
		supp |= BIT(IWL_TLC_MNG_CH_WIDTH_20MHZ);
	if (ht_cap->cap & IEEE80211_HT_CAP_SGI_40)
		supp |= BIT(IWL_TLC_MNG_CH_WIDTH_40MHZ);
	if (vht_cap->cap & IEEE80211_VHT_CAP_SHORT_GI_80)
		supp |= BIT(IWL_TLC_MNG_CH_WIDTH_80MHZ);
	if (vht_cap->cap & IEEE80211_VHT_CAP_SHORT_GI_160)
		supp |= BIT(IWL_TLC_MNG_CH_WIDTH_160MHZ);

	return supp;
}

static u16 rs_fw_get_config_flags(struct iwl_mvm *mvm,
				  struct ieee80211_sta *sta,
				  struct ieee80211_supported_band *sband)
{
	struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	struct ieee80211_sta_he_cap *he_cap = &sta->he_cap;
	bool vht_ena = vht_cap->vht_supported;
	u16 flags = 0;

	/* get STBC flags */
	if (mvm->cfg->ht_params->stbc &&
	    (num_of_ant(iwl_mvm_get_valid_tx_ant(mvm)) > 1)) {
		if (he_cap->has_he && he_cap->he_cap_elem.phy_cap_info[2] &
				      IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ)
			flags |= IWL_TLC_MNG_CFG_FLAGS_STBC_MSK;
		else if (vht_cap->cap & IEEE80211_VHT_CAP_RXSTBC_MASK)
			flags |= IWL_TLC_MNG_CFG_FLAGS_STBC_MSK;
		else if (ht_cap->cap & IEEE80211_HT_CAP_RX_STBC)
			flags |= IWL_TLC_MNG_CFG_FLAGS_STBC_MSK;
	}

	if (mvm->cfg->ht_params->ldpc &&
	    ((ht_cap->cap & IEEE80211_HT_CAP_LDPC_CODING) ||
	     (vht_ena && (vht_cap->cap & IEEE80211_VHT_CAP_RXLDPC))))
		flags |= IWL_TLC_MNG_CFG_FLAGS_LDPC_MSK;

	/* consider LDPC support in case of HE */
	if (he_cap->has_he && (he_cap->he_cap_elem.phy_cap_info[1] &
	    IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD))
		flags |= IWL_TLC_MNG_CFG_FLAGS_LDPC_MSK;

	if (sband->iftype_data && sband->iftype_data->he_cap.has_he &&
	    !(sband->iftype_data->he_cap.he_cap_elem.phy_cap_info[1] &
	     IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD))
		flags &= ~IWL_TLC_MNG_CFG_FLAGS_LDPC_MSK;

	if (he_cap->has_he &&
	    (he_cap->he_cap_elem.phy_cap_info[3] &
	     IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_MASK &&
	     sband->iftype_data &&
	     sband->iftype_data->he_cap.he_cap_elem.phy_cap_info[3] &
	     IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_MASK))
		flags |= IWL_TLC_MNG_CFG_FLAGS_HE_DCM_NSS_1_MSK;

	return flags;
}

static
int rs_fw_vht_highest_rx_mcs_index(const struct ieee80211_sta_vht_cap *vht_cap,
				   int nss)
{
	u16 rx_mcs = le16_to_cpu(vht_cap->vht_mcs.rx_mcs_map) &
		(0x3 << (2 * (nss - 1)));
	rx_mcs >>= (2 * (nss - 1));

	switch (rx_mcs) {
	case IEEE80211_VHT_MCS_SUPPORT_0_7:
		return IWL_TLC_MNG_HT_RATE_MCS7;
	case IEEE80211_VHT_MCS_SUPPORT_0_8:
		return IWL_TLC_MNG_HT_RATE_MCS8;
	case IEEE80211_VHT_MCS_SUPPORT_0_9:
		return IWL_TLC_MNG_HT_RATE_MCS9;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	return 0;
}

static void
rs_fw_vht_set_enabled_rates(const struct ieee80211_sta *sta,
			    const struct ieee80211_sta_vht_cap *vht_cap,
			    struct iwl_tlc_config_cmd_v4 *cmd)
{
	u16 supp;
	int i, highest_mcs;
	u8 max_nss = sta->rx_nss;
	struct ieee80211_vht_cap ieee_vht_cap = {
		.vht_cap_info = cpu_to_le32(vht_cap->cap),
		.supp_mcs = vht_cap->vht_mcs,
	};

	/* the station support only a single receive chain */
	if (sta->smps_mode == IEEE80211_SMPS_STATIC)
		max_nss = 1;

	for (i = 0; i < max_nss && i < IWL_TLC_NSS_MAX; i++) {
		int nss = i + 1;

		highest_mcs = rs_fw_vht_highest_rx_mcs_index(vht_cap, nss);
		if (!highest_mcs)
			continue;

		supp = BIT(highest_mcs + 1) - 1;
		if (sta->bandwidth == IEEE80211_STA_RX_BW_20)
			supp &= ~BIT(IWL_TLC_MNG_HT_RATE_MCS9);

		cmd->ht_rates[i][IWL_TLC_MCS_PER_BW_80] = cpu_to_le16(supp);
		/*
		 * Check if VHT extended NSS indicates that the bandwidth/NSS
		 * configuration is supported - only for MCS 0 since we already
		 * decoded the MCS bits anyway ourselves.
		 */
		if (sta->bandwidth == IEEE80211_STA_RX_BW_160 &&
		    ieee80211_get_vht_max_nss(&ieee_vht_cap,
					      IEEE80211_VHT_CHANWIDTH_160MHZ,
					      0, true, nss) >= nss)
			cmd->ht_rates[i][IWL_TLC_MCS_PER_BW_160] =
				cmd->ht_rates[i][IWL_TLC_MCS_PER_BW_80];
	}
}

static u16 rs_fw_he_ieee80211_mcs_to_rs_mcs(u16 mcs)
{
	switch (mcs) {
	case IEEE80211_HE_MCS_SUPPORT_0_7:
		return BIT(IWL_TLC_MNG_HT_RATE_MCS7 + 1) - 1;
	case IEEE80211_HE_MCS_SUPPORT_0_9:
		return BIT(IWL_TLC_MNG_HT_RATE_MCS9 + 1) - 1;
	case IEEE80211_HE_MCS_SUPPORT_0_11:
		return BIT(IWL_TLC_MNG_HT_RATE_MCS11 + 1) - 1;
	case IEEE80211_HE_MCS_NOT_SUPPORTED:
		return 0;
	}

	WARN(1, "invalid HE MCS %d\n", mcs);
	return 0;
}

static void
rs_fw_he_set_enabled_rates(const struct ieee80211_sta *sta,
			   struct ieee80211_supported_band *sband,
			   struct iwl_tlc_config_cmd_v4 *cmd)
{
	const struct ieee80211_sta_he_cap *he_cap = &sta->he_cap;
	u16 mcs_160 = le16_to_cpu(he_cap->he_mcs_nss_supp.rx_mcs_160);
	u16 mcs_80 = le16_to_cpu(he_cap->he_mcs_nss_supp.rx_mcs_80);
	u16 tx_mcs_80 =
		le16_to_cpu(sband->iftype_data->he_cap.he_mcs_nss_supp.tx_mcs_80);
	u16 tx_mcs_160 =
		le16_to_cpu(sband->iftype_data->he_cap.he_mcs_nss_supp.tx_mcs_160);
	int i;
	u8 nss = sta->rx_nss;

	/* the station support only a single receive chain */
	if (sta->smps_mode == IEEE80211_SMPS_STATIC)
		nss = 1;

	for (i = 0; i < nss && i < IWL_TLC_NSS_MAX; i++) {
		u16 _mcs_160 = (mcs_160 >> (2 * i)) & 0x3;
		u16 _mcs_80 = (mcs_80 >> (2 * i)) & 0x3;
		u16 _tx_mcs_160 = (tx_mcs_160 >> (2 * i)) & 0x3;
		u16 _tx_mcs_80 = (tx_mcs_80 >> (2 * i)) & 0x3;

		/* If one side doesn't support - mark both as not supporting */
		if (_mcs_80 == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    _tx_mcs_80 == IEEE80211_HE_MCS_NOT_SUPPORTED) {
			_mcs_80 = IEEE80211_HE_MCS_NOT_SUPPORTED;
			_tx_mcs_80 = IEEE80211_HE_MCS_NOT_SUPPORTED;
		}
		if (_mcs_80 > _tx_mcs_80)
			_mcs_80 = _tx_mcs_80;
		cmd->ht_rates[i][IWL_TLC_MCS_PER_BW_80] =
			cpu_to_le16(rs_fw_he_ieee80211_mcs_to_rs_mcs(_mcs_80));

		/* If one side doesn't support - mark both as not supporting */
		if (_mcs_160 == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    _tx_mcs_160 == IEEE80211_HE_MCS_NOT_SUPPORTED) {
			_mcs_160 = IEEE80211_HE_MCS_NOT_SUPPORTED;
			_tx_mcs_160 = IEEE80211_HE_MCS_NOT_SUPPORTED;
		}
		if (_mcs_160 > _tx_mcs_160)
			_mcs_160 = _tx_mcs_160;
		cmd->ht_rates[i][IWL_TLC_MCS_PER_BW_160] =
			cpu_to_le16(rs_fw_he_ieee80211_mcs_to_rs_mcs(_mcs_160));
	}
}

static u8 rs_fw_eht_max_nss(u8 rx_nss, u8 tx_nss)
{
	u8 tx = u8_get_bits(tx_nss, IEEE80211_EHT_MCS_NSS_TX);
	u8 rx = u8_get_bits(rx_nss, IEEE80211_EHT_MCS_NSS_RX);
	/* the max nss that can be used,
	 * is the min with our tx capa and the peer rx capa.
	 */
	return min(tx, rx);
}

#define MAX_NSS_MCS(mcs_num, rx, tx) \
	rs_fw_eht_max_nss((rx)->rx_tx_mcs ##mcs_num## _max_nss, \
			  (tx)->rx_tx_mcs ##mcs_num## _max_nss)

static void rs_fw_set_eht_mcs_nss(__le16 ht_rates[][3],
				  enum IWL_TLC_MCS_PER_BW bw,
				  u8 max_nss, u16 mcs_msk)
{
	if (max_nss >= 2)
		ht_rates[IWL_TLC_NSS_2][bw] |= cpu_to_le16(mcs_msk);

	if (max_nss >= 1)
		ht_rates[IWL_TLC_NSS_1][bw] |= cpu_to_le16(mcs_msk);
}

static const
struct ieee80211_eht_mcs_nss_supp_bw *
rs_fw_rs_mcs2eth_mcs(enum IWL_TLC_MCS_PER_BW bw,
		     const struct ieee80211_eht_mcs_nss_supp *eth_mcs)
{
	switch (bw) {
	case IWL_TLC_MCS_PER_BW_80:
		return &eth_mcs->bw_80;
	case IWL_TLC_MCS_PER_BW_160:
		return &eth_mcs->bw_160;
	case IWL_TLC_MCS_PER_BW_320:
		return &eth_mcs->bw_320;
	default:
		return NULL;
	}
}

static void rs_fw_eht_set_enabled_rates(const struct ieee80211_sta *sta,
					struct ieee80211_supported_band *sband,
					struct iwl_tlc_config_cmd_v4 *cmd)
{
	/* peer RX mcs capa */
	const struct ieee80211_eht_mcs_nss_supp *eth_rx_mcs =
		&sta->eht_cap.eht_mcs_nss_supp;
	/* our TX mcs capa */
	const struct ieee80211_eht_mcs_nss_supp *eth_tx_mcs =
		&sband->iftype_data->eht_cap.eht_mcs_nss_supp;

	enum IWL_TLC_MCS_PER_BW bw;
	struct ieee80211_eht_mcs_nss_supp_20mhz_only mcs_rx_20;
	struct ieee80211_eht_mcs_nss_supp_20mhz_only mcs_tx_20;

	/* peer is 20Mhz only */
	if (!(sta->he_cap.he_cap_elem.phy_cap_info[0] &
	      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK_ALL)) {
		mcs_rx_20 = eth_rx_mcs->only_20mhz;
	} else {
		mcs_rx_20.rx_tx_mcs7_max_nss = eth_rx_mcs->bw_80.rx_tx_mcs9_max_nss;
		mcs_rx_20.rx_tx_mcs9_max_nss = eth_rx_mcs->bw_80.rx_tx_mcs9_max_nss;
		mcs_rx_20.rx_tx_mcs11_max_nss = eth_rx_mcs->bw_80.rx_tx_mcs11_max_nss;
		mcs_rx_20.rx_tx_mcs13_max_nss = eth_rx_mcs->bw_80.rx_tx_mcs13_max_nss;
	}

	/* nic is 20Mhz only */
	if (!(sband->iftype_data->he_cap.he_cap_elem.phy_cap_info[0] &
	      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK_ALL)) {
		mcs_tx_20 = eth_tx_mcs->only_20mhz;
	} else {
		mcs_tx_20.rx_tx_mcs7_max_nss = eth_tx_mcs->bw_80.rx_tx_mcs9_max_nss;
		mcs_tx_20.rx_tx_mcs9_max_nss = eth_tx_mcs->bw_80.rx_tx_mcs9_max_nss;
		mcs_tx_20.rx_tx_mcs11_max_nss = eth_tx_mcs->bw_80.rx_tx_mcs11_max_nss;
		mcs_tx_20.rx_tx_mcs13_max_nss = eth_tx_mcs->bw_80.rx_tx_mcs13_max_nss;
	}

	/* rates for 20/40/80 bw */
	bw = IWL_TLC_MCS_PER_BW_80;
	rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
			      MAX_NSS_MCS(7, &mcs_rx_20, &mcs_tx_20), GENMASK(7, 0));
	rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
			      MAX_NSS_MCS(9, &mcs_rx_20, &mcs_tx_20), GENMASK(9, 8));
	rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
			      MAX_NSS_MCS(11, &mcs_rx_20, &mcs_tx_20), GENMASK(11, 10));
	rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
			      MAX_NSS_MCS(13, &mcs_rx_20, &mcs_tx_20), GENMASK(13, 12));

	/* rate for 160/320 bw */
	for (bw = IWL_TLC_MCS_PER_BW_160; bw <= IWL_TLC_MCS_PER_BW_320; bw++) {
		const struct ieee80211_eht_mcs_nss_supp_bw *mcs_rx =
			rs_fw_rs_mcs2eth_mcs(bw, eth_rx_mcs);
		const struct ieee80211_eht_mcs_nss_supp_bw *mcs_tx =
			rs_fw_rs_mcs2eth_mcs(bw, eth_tx_mcs);

		/* got unsuppored index for bw */
		if (!mcs_rx || !mcs_tx)
			continue;

		rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
				      MAX_NSS_MCS(9, mcs_rx, mcs_tx), GENMASK(9, 0));
		rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
				      MAX_NSS_MCS(11, mcs_rx, mcs_tx), GENMASK(11, 10));
		rs_fw_set_eht_mcs_nss(cmd->ht_rates, bw,
				      MAX_NSS_MCS(13, mcs_rx, mcs_tx), GENMASK(13, 12));
	}

	/* the station support only a single receive chain */
	if (sta->smps_mode == IEEE80211_SMPS_STATIC || sta->rx_nss < 2)
		memset(cmd->ht_rates[IWL_TLC_NSS_2], 0,
		       sizeof(cmd->ht_rates[IWL_TLC_NSS_2]));
}

static void rs_fw_set_supp_rates(struct ieee80211_sta *sta,
				 struct ieee80211_supported_band *sband,
				 struct iwl_tlc_config_cmd_v4 *cmd)
{
	int i;
	u16 supp = 0;
	unsigned long tmp; /* must be unsigned long for for_each_set_bit */
	const struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	const struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	const struct ieee80211_sta_he_cap *he_cap = &sta->he_cap;

	/* non HT rates */
	tmp = sta->supp_rates[sband->band];
	for_each_set_bit(i, &tmp, BITS_PER_LONG)
		supp |= BIT(sband->bitrates[i].hw_value);

	cmd->non_ht_rates = cpu_to_le16(supp);
	cmd->mode = IWL_TLC_MNG_MODE_NON_HT;

	/* HT/VHT rates */
	if (sta->eht_cap.has_eht) {
		cmd->mode = IWL_TLC_MNG_MODE_EHT;
		rs_fw_eht_set_enabled_rates(sta, sband, cmd);
	} else if (he_cap->has_he) {
		cmd->mode = IWL_TLC_MNG_MODE_HE;
		rs_fw_he_set_enabled_rates(sta, sband, cmd);
	} else if (vht_cap->vht_supported) {
		cmd->mode = IWL_TLC_MNG_MODE_VHT;
		rs_fw_vht_set_enabled_rates(sta, vht_cap, cmd);
	} else if (ht_cap->ht_supported) {
		cmd->mode = IWL_TLC_MNG_MODE_HT;
		cmd->ht_rates[IWL_TLC_NSS_1][IWL_TLC_MCS_PER_BW_80] =
			cpu_to_le16(ht_cap->mcs.rx_mask[0]);

		/* the station support only a single receive chain */
		if (sta->smps_mode == IEEE80211_SMPS_STATIC)
			cmd->ht_rates[IWL_TLC_NSS_2][IWL_TLC_MCS_PER_BW_80] =
				0;
		else
			cmd->ht_rates[IWL_TLC_NSS_2][IWL_TLC_MCS_PER_BW_80] =
				cpu_to_le16(ht_cap->mcs.rx_mask[1]);
	}
}

void iwl_mvm_tlc_update_notif(struct iwl_mvm *mvm,
			      struct iwl_rx_cmd_buffer *rxb)
{
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_tlc_update_notif *notif;
	struct ieee80211_sta *sta;
	struct iwl_mvm_sta *mvmsta;
	struct iwl_lq_sta_rs_fw *lq_sta;
	u32 flags;

	rcu_read_lock();

	notif = (void *)pkt->data;
	sta = rcu_dereference(mvm->fw_id_to_mac_id[notif->sta_id]);
	if (IS_ERR_OR_NULL(sta)) {
		/* can happen in remove station flow where mvm removed internally
		 * the station before removing from FW
		 */
		IWL_DEBUG_RATE(mvm,
			       "Invalid mvm RCU pointer for sta id (%d) in TLC notification\n",
			       notif->sta_id);
		goto out;
	}

	mvmsta = iwl_mvm_sta_from_mac80211(sta);

	if (!mvmsta) {
		IWL_ERR(mvm, "Invalid sta id (%d) in FW TLC notification\n",
			notif->sta_id);
		goto out;
	}

	flags = le32_to_cpu(notif->flags);

	lq_sta = &mvmsta->lq_sta.rs_fw;

	if (flags & IWL_TLC_NOTIF_FLAG_RATE) {
		char pretty_rate[100];

		if (iwl_fw_lookup_notif_ver(mvm->fw, DATA_PATH_GROUP,
					    TLC_MNG_UPDATE_NOTIF, 0) < 3) {
			rs_pretty_print_rate_v1(pretty_rate,
						sizeof(pretty_rate),
						le32_to_cpu(notif->rate));
			IWL_DEBUG_RATE(mvm,
				       "Got rate in old format. Rate: %s. Converting.\n",
				       pretty_rate);
			lq_sta->last_rate_n_flags =
				iwl_new_rate_from_v1(le32_to_cpu(notif->rate));
		} else {
			lq_sta->last_rate_n_flags = le32_to_cpu(notif->rate);
		}
		rs_pretty_print_rate(pretty_rate, sizeof(pretty_rate),
				     lq_sta->last_rate_n_flags);
		IWL_DEBUG_RATE(mvm, "new rate: %s\n", pretty_rate);
	}

	if (flags & IWL_TLC_NOTIF_FLAG_AMSDU && !mvmsta->orig_amsdu_len) {
		u16 size = le32_to_cpu(notif->amsdu_size);
		int i;

		if (sta->max_amsdu_len < size) {
			/*
			 * In debug sta->max_amsdu_len < size
			 * so also check with orig_amsdu_len which holds the
			 * original data before debugfs changed the value
			 */
			WARN_ON(mvmsta->orig_amsdu_len < size);
			goto out;
		}

		mvmsta->amsdu_enabled = le32_to_cpu(notif->amsdu_enabled);
		mvmsta->max_amsdu_len = size;
		sta->max_rc_amsdu_len = mvmsta->max_amsdu_len;

		for (i = 0; i < IWL_MAX_TID_COUNT; i++) {
			if (mvmsta->amsdu_enabled & BIT(i))
				sta->max_tid_amsdu_len[i] =
					iwl_mvm_max_amsdu_size(mvm, sta, i);
			else
				/*
				 * Not so elegant, but this will effectively
				 * prevent AMSDU on this TID
				 */
				sta->max_tid_amsdu_len[i] = 1;
		}

		IWL_DEBUG_RATE(mvm,
			       "AMSDU update. AMSDU size: %d, AMSDU selected size: %d, AMSDU TID bitmap 0x%X\n",
			       le32_to_cpu(notif->amsdu_size), size,
			       mvmsta->amsdu_enabled);
	}
out:
	rcu_read_unlock();
}

int iwl_rs_send_dhc(struct iwl_mvm *mvm, u8 sta_id, u32 type, u32 data)
{
	int ret;
	struct iwl_dhc_cmd *dhc_cmd;
	struct iwl_dhc_tlc_cmd *dhc_tlc_cmd;
	u32 cmd_id = WIDE_ID(IWL_ALWAYS_LONG_GROUP, DEBUG_HOST_COMMAND);

	dhc_cmd = kzalloc(sizeof(*dhc_cmd) + sizeof(*dhc_tlc_cmd), GFP_KERNEL);
	if (!dhc_cmd)
		return -ENOMEM;

	dhc_tlc_cmd = (void *)dhc_cmd->data;
	dhc_tlc_cmd->sta_id = sta_id;
	dhc_tlc_cmd->type = cpu_to_le32(type);
	dhc_tlc_cmd->data[0] = cpu_to_le32(data);
	dhc_cmd->length = cpu_to_le32(sizeof(*dhc_tlc_cmd) >> 2);
	dhc_cmd->index_and_mask =
		cpu_to_le32(DHC_TABLE_INTEGRATION | DHC_TARGET_UMAC |
			    DHC_INTEGRATION_TLC_DEBUG_CONFIG);

	ret = iwl_mvm_send_cmd_pdu(mvm, cmd_id, CMD_ASYNC,
				   sizeof(*dhc_cmd) + sizeof(*dhc_tlc_cmd),
				   dhc_cmd);
	kfree(dhc_cmd);

	IWL_DEBUG_RATE(mvm, "sta_id %d, type: 0x%X, value: 0x%X, ret%d\n",
		       sta_id, type, data, ret);
	return ret;
}

#if defined(CPTCFG_MAC80211_DEBUGFS) && defined(CPTCFG_IWLWIFI_DHC_PRIVATE)
int iwl_rs_dhc_set_ampdu_size(struct ieee80211_sta *sta, u32 ampdu_size)
{
	struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);
	struct iwl_lq_sta_rs_fw *lq_sta = &mvmsta->lq_sta.rs_fw;
	struct iwl_mvm *mvm = lq_sta->pers.drv;

	int ret = iwl_rs_send_dhc(mvm, lq_sta->pers.sta_id,
				  IWL_TLC_DEBUG_AGG_FRAME_CNT_LIM,
				  ampdu_size);
	if (!ret)
		return ret;

	lq_sta->pers.dbg_agg_frame_count_lim = ampdu_size;

	IWL_DEBUG_RATE(mvm, "sta_id ret: %d, %d agg_frame_cmdt_lim %d\n",
		       ret,
		       lq_sta->pers.sta_id,
		       lq_sta->pers.dbg_agg_frame_count_lim);

	return 0;
}
#endif /* CPTCFG_MAC80211_DEBUGFS */

u16 rs_fw_get_max_amsdu_len(struct ieee80211_sta *sta)
{
	struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);
	const struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	const struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;

	if (mvmsta->vif->bss_conf.chandef.chan->band == NL80211_BAND_6GHZ) {
		switch (le16_get_bits(sta->he_6ghz_capa.capa,
				      IEEE80211_HE_6GHZ_CAP_MAX_MPDU_LEN)) {
		case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454:
			return IEEE80211_MAX_MPDU_LEN_VHT_11454;
		case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991:
			return IEEE80211_MAX_MPDU_LEN_VHT_7991;
		default:
			return IEEE80211_MAX_MPDU_LEN_VHT_3895;
		}
	} else
	if (vht_cap->vht_supported) {
		switch (vht_cap->cap & IEEE80211_VHT_CAP_MAX_MPDU_MASK) {
		case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454:
			return IEEE80211_MAX_MPDU_LEN_VHT_11454;
		case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991:
			return IEEE80211_MAX_MPDU_LEN_VHT_7991;
		default:
			return IEEE80211_MAX_MPDU_LEN_VHT_3895;
		}
	} else if (ht_cap->ht_supported) {
		if (ht_cap->cap & IEEE80211_HT_CAP_MAX_AMSDU)
			/*
			 * agg is offloaded so we need to assume that agg
			 * are enabled and max mpdu in ampdu is 4095
			 * (spec 802.11-2016 9.3.2.1)
			 */
			return IEEE80211_MAX_MPDU_LEN_HT_BA;
		else
			return IEEE80211_MAX_MPDU_LEN_HT_3839;
	}

	/* in legacy mode no amsdu is enabled so return zero */
	return 0;
}

void rs_fw_rate_init(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
		     enum nl80211_band band, bool update)
{
	struct ieee80211_hw *hw = mvm->hw;
	struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);
	struct iwl_lq_sta_rs_fw *lq_sta = &mvmsta->lq_sta.rs_fw;
	u32 cmd_id = WIDE_ID(DATA_PATH_GROUP, TLC_MNG_CONFIG_CMD);
	struct ieee80211_supported_band *sband = hw->wiphy->bands[band];
	u16 max_amsdu_len = rs_fw_get_max_amsdu_len(sta);
	struct iwl_tlc_config_cmd_v4 cfg_cmd = {
		.sta_id = mvmsta->sta_id,
		.max_ch_width = update ?
			rs_fw_bw_from_sta_bw(sta) : RATE_MCS_CHAN_WIDTH_20,
		.flags = cpu_to_le16(rs_fw_get_config_flags(mvm, sta, sband)),
		.chains = rs_fw_set_active_chains(iwl_mvm_get_valid_tx_ant(mvm)),
		.sgi_ch_width_supp = rs_fw_sgi_cw_support(sta),
		.max_mpdu_len = iwl_mvm_is_csum_supported(mvm) ?
				cpu_to_le16(max_amsdu_len) : 0,
	};
	int ret;
	int cmd_ver;

	memset(lq_sta, 0, offsetof(typeof(*lq_sta), pers));

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	iwl_mvm_reset_frame_stats(mvm);
#endif
	rs_fw_set_supp_rates(sta, sband, &cfg_cmd);
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	/*
	 * if AP disables mimo on 160bw
	 * (!cfg_cmd.ht_rates[IWL_TLC_NSS_2][IWL_TLC_MCS_PER_BW_160])
	 * and AP enables siso on 160
	 * cfg_cmd.ht_rates[IWL_TLC_NSS_1][IWL_TLC_MCS_PER_BW_160]
	 * we disable mimo on 80bw cmd->ht_rates[1][0]
	 */
	if (mvm->trans->dbg_cfg.tx_siso_80bw_like_160bw &&
	    cfg_cmd.ht_rates[IWL_TLC_NSS_1][IWL_TLC_MCS_PER_BW_160] &&
	    !cfg_cmd.ht_rates[IWL_TLC_NSS_2][IWL_TLC_MCS_PER_BW_160])
		cfg_cmd.ht_rates[IWL_TLC_NSS_2][IWL_TLC_MCS_PER_BW_80] = 0;
#endif

#ifdef CPTCFG_IWLWIFI_DHC_PRIVATE
	if (iwlwifi_mod_params.disable_11n & IWL_DISABLE_HT_TXAGG)
		iwl_rs_dhc_set_ampdu_size(sta, 1);
#endif
	/*
	 * since TLC offload works with one mode we can assume
	 * that only vht/ht is used and also set it as station max amsdu
	 */
	sta->max_amsdu_len = max_amsdu_len;

	cmd_ver = iwl_fw_lookup_cmd_ver(mvm->fw,
					WIDE_ID(DATA_PATH_GROUP,
						TLC_MNG_CONFIG_CMD),
					0);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, sta_id=%d, max_ch_width=%d, mode=%d\n",
		       cfg_cmd.sta_id, cfg_cmd.max_ch_width, cfg_cmd.mode);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, chains=0x%X, ch_wid_supp=%d, flags=0x%X\n",
		       cfg_cmd.chains, cfg_cmd.sgi_ch_width_supp, cfg_cmd.flags);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, mpdu_len=%d, no_ht_rate=0x%X, tx_op=%d\n",
		       cfg_cmd.max_mpdu_len, cfg_cmd.non_ht_rates, cfg_cmd.max_tx_op);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, ht_rate[0][0]=0x%X, ht_rate[1][0]=0x%X\n",
		       cfg_cmd.ht_rates[0][0], cfg_cmd.ht_rates[1][0]);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, ht_rate[0][1]=0x%X, ht_rate[1][1]=0x%X\n",
		       cfg_cmd.ht_rates[0][1], cfg_cmd.ht_rates[1][1]);
	IWL_DEBUG_RATE(mvm, "TLC CONFIG CMD, ht_rate[0][2]=0x%X, ht_rate[1][2]=0x%X\n",
		       cfg_cmd.ht_rates[0][2], cfg_cmd.ht_rates[1][2]);
	if (cmd_ver == 4) {
		ret = iwl_mvm_send_cmd_pdu(mvm, cmd_id, CMD_ASYNC,
					   sizeof(cfg_cmd), &cfg_cmd);
	} else if (cmd_ver < 4) {
		struct iwl_tlc_config_cmd_v3 cfg_cmd_v3 = {
			.sta_id = cfg_cmd.sta_id,
			.max_ch_width = cfg_cmd.max_ch_width,
			.mode = cfg_cmd.mode,
			.chains = cfg_cmd.chains,
			.amsdu = !!cfg_cmd.max_mpdu_len,
			.flags = cfg_cmd.flags,
			.non_ht_rates = cfg_cmd.non_ht_rates,
			.ht_rates[0][0] = cfg_cmd.ht_rates[0][0],
			.ht_rates[0][1] = cfg_cmd.ht_rates[0][1],
			.ht_rates[1][0] = cfg_cmd.ht_rates[1][0],
			.ht_rates[1][1] = cfg_cmd.ht_rates[1][1],
			.sgi_ch_width_supp = cfg_cmd.sgi_ch_width_supp,
			.max_mpdu_len = cfg_cmd.max_mpdu_len,
		};

		u16 cmd_size = sizeof(cfg_cmd_v3);

		/* In old versions of the API the struct is 4 bytes smaller */
		if (iwl_fw_lookup_cmd_ver(mvm->fw,
					  WIDE_ID(DATA_PATH_GROUP,
						  TLC_MNG_CONFIG_CMD), 0) < 3)
			cmd_size -= 4;

		ret = iwl_mvm_send_cmd_pdu(mvm, cmd_id, CMD_ASYNC, cmd_size,
					   &cfg_cmd_v3);
	} else {
		ret = -EINVAL;
	}

	if (ret)
		IWL_ERR(mvm, "Failed to send rate scale config (%d)\n", ret);

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	if (mvm->trans->dbg_cfg.ampdu_limit)
		iwl_rs_dhc_set_ampdu_size(sta,
					  mvm->trans->dbg_cfg.ampdu_limit);
#endif
}

void iwl_mvm_rs_add_sta(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta)
{
	struct iwl_lq_sta_rs_fw *lq_sta = &mvmsta->lq_sta.rs_fw;

	IWL_DEBUG_RATE(mvm, "create station rate scale window\n");

	lq_sta->pers.drv = mvm;
	lq_sta->pers.sta_id = mvmsta->sta_id;
	lq_sta->pers.chains = 0;
	memset(lq_sta->pers.chain_signal, 0, sizeof(lq_sta->pers.chain_signal));
	lq_sta->pers.last_rssi = S8_MIN;
	lq_sta->last_rate_n_flags = 0;

#ifdef CPTCFG_MAC80211_DEBUGFS
	lq_sta->pers.dbg_fixed_rate = 0;
#endif
}

int rs_fw_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta,
			bool enable)
{
	/* TODO: need to introduce a new FW cmd since LQ cmd is not relevant */
	IWL_DEBUG_RATE(mvm, "tx protection - not implemented yet.\n");
	return 0;
}
