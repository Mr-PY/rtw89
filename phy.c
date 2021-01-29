// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2019-2020  Realtek Corporation
 */

#include "debug.h"
#include "fw.h"
#include "phy.h"
#include "reg.h"

static u16 get_max_amsdu_len(struct rtw89_dev *rtwdev, u32 bit_rate)
{
	/* lower than ofdm, do not aggregate */
	if (bit_rate < 550)
		return 1;

	/* lower than 20M vht 2ss mcs8, make it small */
	if (bit_rate < 1800)
		return 1200;

	/* lower than 40M vht 2ss mcs9, make it medium */
	if (bit_rate < 4000)
		return 2600;

	/* not yet 80M vht 2ss mcs8/9, make it twice regular packet size */
	if (bit_rate < 7000)
		return 3500;

	return rtwdev->chip->max_amsdu_limit;
}

static u64 get_mcs_ra_mask(u16 mcs_map, u8 highest_mcs, u8 gap)
{
	u64 ra_mask = 0;
	u8 mcs_cap;
	int i, nss;

	for (i = 0, nss = 12; i < 4; i++, mcs_map >>= 2, nss += 12) {
		mcs_cap = mcs_map & 0x3;
		switch (mcs_cap) {
		case 2:
			ra_mask |= GENMASK_ULL(highest_mcs, 0) << nss;
			break;
		case 1:
			ra_mask |= GENMASK_ULL(highest_mcs - gap, 0) << nss;
			break;
		case 0:
			ra_mask |= GENMASK_ULL(highest_mcs - gap * 2, 0) << nss;
			break;
		default:
			break;
		}
	}

	return ra_mask;
}

static u64 get_he_ra_mask(struct ieee80211_sta *sta)
{
	struct ieee80211_sta_he_cap cap = sta->he_cap;
	u16 mcs_map;

	switch (sta->bandwidth) {
	case IEEE80211_STA_RX_BW_160:
		if (cap.he_cap_elem.phy_cap_info[0] &
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G)
			mcs_map = le16_to_cpu(cap.he_mcs_nss_supp.rx_mcs_80p80);
		else
			mcs_map = le16_to_cpu(cap.he_mcs_nss_supp.rx_mcs_160);
		break;
	default:
		mcs_map = le16_to_cpu(cap.he_mcs_nss_supp.rx_mcs_80);
	}

	/* MCS11, MCS9, MCS7 */
	return get_mcs_ra_mask(mcs_map, 11, 2);
}

#define RA_FLOOR_TABLE_SIZE	7
#define RA_FLOOR_UP_GAP		3
static u64 rtw89_phy_ra_mask_rssi(struct rtw89_dev *rtwdev, u8 rssi,
				  u8 ratr_state)
{
	u8 rssi_lv_t[RA_FLOOR_TABLE_SIZE] = {30, 44, 48, 52, 56, 60, 100};
	u8 rssi_lv = 0;
	u8 i;

	rssi >>= 1;
	for (i = 0; i < RA_FLOOR_TABLE_SIZE; i++) {
		if (i >= ratr_state)
			rssi_lv_t[i] += RA_FLOOR_UP_GAP;
		if (rssi < rssi_lv_t[i]) {
			rssi_lv = i;
			break;
		}
	}
	if (rssi_lv == 0)
		return 0xffffffffffffffffULL;
	else if (rssi_lv == 1)
		return 0xfffffffffffffff0ULL;
	else if (rssi_lv == 2)
		return 0xffffffffffffffe0ULL;
	else if (rssi_lv == 3)
		return 0xffffffffffffffc0ULL;
	else if (rssi_lv == 4)
		return 0xffffffffffffff80ULL;
	else if (rssi_lv >= 5)
		return 0xffffffffffffff00ULL;

	return 0xffffffffffffffffULL;
}

static const u64
rtw89_ra_mask_ht_rates[4] = {RA_MASK_HT_1SS_RATES, RA_MASK_HT_2SS_RATES,
			     RA_MASK_HT_3SS_RATES, RA_MASK_HT_4SS_RATES};
static const u64
rtw89_ra_mask_vht_rates[4] = {RA_MASK_VHT_1SS_RATES, RA_MASK_VHT_2SS_RATES,
			      RA_MASK_VHT_3SS_RATES, RA_MASK_VHT_4SS_RATES};
static const u64
rtw89_ra_mask_he_rates[4] = {RA_MASK_HE_1SS_RATES, RA_MASK_HE_2SS_RATES,
			     RA_MASK_HE_3SS_RATES, RA_MASK_HE_4SS_RATES};

static void rtw89_phy_ra_sta_update(struct rtw89_dev *rtwdev,
				    struct ieee80211_sta *sta)
{
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_ra_info *ra = &rtwsta->ra;
	const u64 *high_rate_masks = rtw89_ra_mask_ht_rates;
	u8 rssi = ewma_rssi_read(&rtwsta->avg_rssi);
	u64 high_rate_mask = 0;
	u64 ra_mask = 0;
	u8 mode = 0;
	u8 bw_mode = 0;
	u8 stbc_en = 0;
	u8 ldpc_en = 0;
	u8 i;
	bool sgi = false;

	memset(ra, 0, sizeof(*ra));
	/* Set the ra mask from sta's capability */
	if (sta->he_cap.has_he) {
		mode |= RTW89_RA_MODE_HE;
		ra_mask |= get_he_ra_mask(sta);
		high_rate_masks = rtw89_ra_mask_he_rates;
		if (sta->he_cap.he_cap_elem.phy_cap_info[2] &
		    IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ)
			stbc_en = 1;
		if (sta->he_cap.he_cap_elem.phy_cap_info[1] &
		    IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD)
			ldpc_en = 1;
	} else if (sta->vht_cap.vht_supported) {
		u16 mcs_map = le16_to_cpu(sta->vht_cap.vht_mcs.rx_mcs_map);

		mode |= RTW89_RA_MODE_VHT;
		/* MCS9, MCS8, MCS7 */
		ra_mask |= get_mcs_ra_mask(mcs_map, 9, 1);
		high_rate_masks = rtw89_ra_mask_vht_rates;
		if (sta->vht_cap.cap & IEEE80211_VHT_CAP_RXSTBC_MASK)
			stbc_en = 1;
		if (sta->vht_cap.cap & IEEE80211_VHT_CAP_RXLDPC)
			ldpc_en = 1;
	} else if (sta->ht_cap.ht_supported) {
		mode |= RTW89_RA_MODE_HT;
		ra_mask |= ((u64)sta->ht_cap.mcs.rx_mask[3] << 48) |
			   ((u64)sta->ht_cap.mcs.rx_mask[2] << 36) |
			   (sta->ht_cap.mcs.rx_mask[1] << 24) |
			   (sta->ht_cap.mcs.rx_mask[0] << 12);
		high_rate_masks = rtw89_ra_mask_ht_rates;
		if (sta->ht_cap.cap & IEEE80211_HT_CAP_RX_STBC)
			stbc_en = 1;
		if (sta->ht_cap.cap & IEEE80211_HT_CAP_LDPC_CODING)
			ldpc_en = 1;
	}

	if (rtwdev->hal.current_band_type == RTW89_BAND_2G) {
		if (sta->supp_rates[NL80211_BAND_2GHZ] <= 0xf)
			mode |= RTW89_RA_MODE_CCK;
		else
			mode |= RTW89_RA_MODE_CCK | RTW89_RA_MODE_OFDM;
	} else {
		mode |= RTW89_RA_MODE_OFDM;
	}

	if (mode >= RTW89_RA_MODE_HT) {
		for (i = 0; i < rtwdev->chip->tx_nss; i++)
			high_rate_mask |= high_rate_masks[i];
		ra_mask &= high_rate_mask;
		if (mode & RTW89_RA_MODE_OFDM)
			ra_mask |= RA_MASK_SUBOFDM_RATES;
		if (mode & RTW89_RA_MODE_CCK)
			ra_mask |= RA_MASK_SUBCCK_RATES;
	} else if (mode & RTW89_RA_MODE_OFDM) {
		if (mode & RTW89_RA_MODE_CCK)
			ra_mask |= RA_MASK_SUBCCK_RATES;
		ra_mask |= RA_MASK_OFDM_RATES;
	} else {
		ra_mask = RA_MASK_CCK_RATES;
	}

	if (mode != RTW89_RA_MODE_CCK)
		ra_mask &= rtw89_phy_ra_mask_rssi(rtwdev, rssi, 0);

	switch (sta->bandwidth) {
	case IEEE80211_STA_RX_BW_80:
		bw_mode = RTW89_CHANNEL_WIDTH_80;
		sgi = sta->vht_cap.vht_supported &&
		      (sta->vht_cap.cap & IEEE80211_VHT_CAP_SHORT_GI_80);
		break;
	case IEEE80211_STA_RX_BW_40:
		bw_mode = RTW89_CHANNEL_WIDTH_40;
		sgi = sta->ht_cap.ht_supported &&
		      (sta->ht_cap.cap & IEEE80211_HT_CAP_SGI_40);
		break;
	default:
		bw_mode = RTW89_CHANNEL_WIDTH_20;
		sgi = sta->ht_cap.ht_supported &&
		      (sta->ht_cap.cap & IEEE80211_HT_CAP_SGI_20);
		break;
	}

	if (sta->he_cap.he_cap_elem.phy_cap_info[3] &
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_16_QAM)
		ra->dcm_cap = 1;

	ra->bw_cap = bw_mode;
	ra->mode_ctrl = mode;
	ra->macid = rtwsta->mac_id;
	ra->stbc_cap = stbc_en;
	ra->ldpc_cap = ldpc_en;
	ra->ss_num = min(sta->rx_nss, rtwdev->chip->tx_nss) - 1;
	ra->en_sgi = sgi;
	ra->ra_mask = ra_mask;
}

static void rtw89_phy_ra_updata_sta_iter(void *data, struct ieee80211_sta *sta)
{
	struct rtw89_dev *rtwdev = (struct rtw89_dev *)data;
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_ra_info *ra = &rtwsta->ra;

	rtw89_phy_ra_sta_update(rtwdev, sta);
	ra->upd_mask = 1;
	rtw89_debug(rtwdev, RTW89_DBG_RA,
		    "ra updat: macid = %d, bw = %d, nss = %d, gi = %d %d",
		    ra->macid,
		    ra->bw_cap,
		    ra->ss_num,
		    ra->en_sgi,
		    ra->giltf);

	rtw89_fw_h2c_ra(rtwdev, ra);
}

void rtw89_phy_ra_update(struct rtw89_dev *rtwdev)
{
	ieee80211_iterate_stations_atomic(rtwdev->hw,
					  rtw89_phy_ra_updata_sta_iter,
					  rtwdev);
}

void rtw89_phy_ra_assoc(struct rtw89_dev *rtwdev, struct ieee80211_sta *sta)
{
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_ra_info *ra = &rtwsta->ra;
	u8 rssi = ewma_rssi_read(&rtwsta->avg_rssi) >> 1;

	rtw89_phy_ra_sta_update(rtwdev, sta);

	if (rssi > 40)
		ra->init_rate_lv = 1;
	else if (rssi > 20)
		ra->init_rate_lv = 2;
	else if (rssi > 1)
		ra->init_rate_lv = 3;
	else
		ra->init_rate_lv = 0;
	ra->upd_all = 1;
	rtw89_debug(rtwdev, RTW89_DBG_RA,
		    "ra assoc: macid = %d, mode = %d, bw = %d, nss = %d, lv = %d",
		    ra->macid,
		    ra->mode_ctrl,
		    ra->bw_cap,
		    ra->ss_num,
		    ra->init_rate_lv);
	rtw89_debug(rtwdev, RTW89_DBG_RA,
		    "ra assoc: dcm = %d, er = %d, ldpc = %d, stbc = %d, gi = %d %d",
		    ra->dcm_cap,
		    ra->er_cap,
		    ra->ldpc_cap,
		    ra->stbc_cap,
		    ra->en_sgi,
		    ra->giltf);

	rtw89_fw_h2c_ra(rtwdev, ra);
}

u8 rtw89_phy_get_txsc(struct rtw89_dev *rtwdev,
		      struct rtw89_channel_params *param,
		      enum rtw89_bandwidth dbw)
{
	enum rtw89_bandwidth cbw = param->bandwidth;
	u8 pri_ch = param->primary_chan;
	u8 central_ch = param->center_chan;
	u8 txsc_idx = 0;
	u8 tmp = 0;

	if (cbw == dbw || cbw == RTW89_CHANNEL_WIDTH_20)
		return txsc_idx;

	switch (cbw) {
	case RTW89_CHANNEL_WIDTH_40:
		txsc_idx = pri_ch > central_ch ? 1 : 2;
		break;
	case RTW89_CHANNEL_WIDTH_80:
		if (dbw == RTW89_CHANNEL_WIDTH_20) {
			if (pri_ch > central_ch)
				txsc_idx = (pri_ch - central_ch) >> 1;
			else
				txsc_idx = ((central_ch - pri_ch) >> 1) + 1;
		} else {
			txsc_idx = pri_ch > central_ch ? 9 : 10;
		}
		break;
	case RTW89_CHANNEL_WIDTH_160:
		if (pri_ch > central_ch)
			tmp = (pri_ch - central_ch) >> 1;
		else
			tmp = ((central_ch - pri_ch) >> 1) + 1;

		if (dbw == RTW89_CHANNEL_WIDTH_20) {
			txsc_idx = tmp;
		} else if (dbw == RTW89_CHANNEL_WIDTH_40) {
			if (tmp == 1 || tmp == 3)
				txsc_idx = 9;
			else if (tmp == 5 || tmp == 7)
				txsc_idx = 11;
			else if (tmp == 2 || tmp == 4)
				txsc_idx = 10;
			else if (tmp == 6 || tmp == 8)
				txsc_idx = 12;
			else
				return 0xff;
		} else {
			txsc_idx = pri_ch > central_ch ? 13 : 14;
		}
		break;
	case RTW89_CHANNEL_WIDTH_80_80:
		if (dbw == RTW89_CHANNEL_WIDTH_20) {
			if (pri_ch > central_ch)
				txsc_idx = (10 - (pri_ch - central_ch)) >> 1;
			else
				txsc_idx = ((central_ch - pri_ch) >> 1) + 5;
		} else if (dbw == RTW89_CHANNEL_WIDTH_40) {
			txsc_idx = pri_ch > central_ch ? 10 : 12;
		} else {
			txsc_idx = 14;
		}
		break;
	default:
		break;
	}

	return txsc_idx;
}

u32 rtw89_phy_read_rf(struct rtw89_dev *rtwdev, enum rtw89_rf_path rf_path,
		      u32 addr, u32 mask)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	const u32 *base_addr = chip->rf_base_addr;
	u32 val, direct_addr;

	if (rf_path >= rtwdev->chip->rf_path_num) {
		rtw89_err(rtwdev, "unsupported rf path (%d)\n", rf_path);
		return INV_RF_DATA;
	}

	addr &= 0xff;
	direct_addr = base_addr[rf_path] + (addr << 2);
	mask &= RFREG_MASK;

	val = rtw89_phy_read32_mask(rtwdev, direct_addr, mask);

	return val;
}
EXPORT_SYMBOL(rtw89_phy_read_rf);

bool rtw89_phy_write_rf(struct rtw89_dev *rtwdev, enum rtw89_rf_path rf_path,
			u32 addr, u32 mask, u32 data)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	const u32 *base_addr = chip->rf_base_addr;
	u32 direct_addr;

	if (rf_path >= rtwdev->chip->rf_path_num) {
		rtw89_err(rtwdev, "unsupported rf path (%d)\n", rf_path);
		return false;
	}

	addr &= 0xff;
	direct_addr = base_addr[rf_path] + (addr << 2);
	mask &= RFREG_MASK;

	rtw89_phy_write32_mask(rtwdev, direct_addr, mask, data);

	udelay(1);

	return true;
}
EXPORT_SYMBOL(rtw89_phy_write_rf);

static void rtw89_phy_bb_reset(struct rtw89_dev *rtwdev,
			       enum rtw89_phy_idx phy_idx)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;

	chip->ops->bb_reset(rtwdev, phy_idx);
}

static void rtw89_phy_config_bb_reg(struct rtw89_dev *rtwdev,
				    const struct rtw89_reg2_def *reg,
				    enum rtw89_rf_path rf_path,
				    void *extra_data)
{
	if (reg->addr == 0xfe)
		mdelay(50);
	else if (reg->addr == 0xfd)
		mdelay(5);
	else if (reg->addr == 0xfc)
		mdelay(1);
	else if (reg->addr == 0xfb)
		udelay(50);
	else if (reg->addr == 0xfa)
		udelay(5);
	else if (reg->addr == 0xf9)
		udelay(1);
	else
		rtw89_phy_write32(rtwdev, reg->addr, reg->data);
}

static void
rtw89_phy_cofig_rf_reg_store(struct rtw89_dev *rtwdev,
			     const struct rtw89_reg2_def *reg,
			     enum rtw89_rf_path rf_path,
			     struct rtw89_fw_h2c_rf_reg_info *info)
{
	u16 idx = info->curr_idx % RTW89_H2C_RF_PAGE_SIZE;
	u8 page = info->curr_idx / RTW89_H2C_RF_PAGE_SIZE;

	info->rtw89_phy_config_rf_h2c[page][idx] =
		cpu_to_le32((reg->addr << 20) | reg->data);
	info->curr_idx++;
}

static int rtw89_phy_config_rf_reg_fw(struct rtw89_dev *rtwdev,
				      struct rtw89_fw_h2c_rf_reg_info *info)
{
	u16 page = info->curr_idx / RTW89_H2C_RF_PAGE_SIZE;
	u16 len = (info->curr_idx % RTW89_H2C_RF_PAGE_SIZE) * 4;
	u8 i;
	int ret = 0;

	if (page > RTW89_H2C_RF_PAGE_NUM) {
		rtw89_warn(rtwdev,
			   "rf reg h2c total page num %d larger than %d (RTW89_H2C_RF_PAGE_NUM)\n",
			   page, RTW89_H2C_RF_PAGE_NUM);
		return -EINVAL;
	}

	for (i = 0; i < page; i++) {
		ret = rtw89_fw_h2c_rf_reg(rtwdev, info,
					  RTW89_H2C_RF_PAGE_SIZE * 4, i);
		if (ret)
			return ret;
	}
	ret = rtw89_fw_h2c_rf_reg(rtwdev, info, len, i);
	if (ret)
		return ret;
	info->curr_idx = 0;

	return 0;
}

static void rtw89_phy_config_rf_reg(struct rtw89_dev *rtwdev,
				    const struct rtw89_reg2_def *reg,
				    enum rtw89_rf_path rf_path,
				    void *extra_data)
{
	if (reg->addr == 0xfe) {
		mdelay(50);
	} else if (reg->addr == 0xfd) {
		mdelay(5);
	} else if (reg->addr == 0xfc) {
		mdelay(1);
	} else if (reg->addr == 0xfb) {
		udelay(50);
	} else if (reg->addr == 0xfa) {
		udelay(5);
	} else if (reg->addr == 0xf9) {
		udelay(1);
	} else {
		rtw89_write_rf(rtwdev, rf_path, reg->addr, 0xfffff, reg->data);
		rtw89_phy_cofig_rf_reg_store(rtwdev, reg, rf_path,
					     (struct rtw89_fw_h2c_rf_reg_info *)extra_data);
	}
}

static int rtw89_phy_sel_headline(struct rtw89_dev *rtwdev,
				  const struct rtw89_phy_table *table,
				  u32 *headline_size, u32 *headline_idx,
				  u8 rfe, u8 cut)
{
	const struct rtw89_reg2_def *reg;
	u32 headline;
	u32 compare, target;
	u8 rfe_para, cut_para;
	u8 cut_max = 0;
	bool case_matched = false;
	u32 i;

	for (i = 0; i < table->n_regs; i++) {
		reg = &table->regs[i];
		headline = get_phy_headline(reg->addr);
		if (headline != PHY_HEADLINE_VALID)
			break;
	}
	*headline_size = i;
	if (*headline_size == 0)
		return 0;

	/* case 1: RFE match, CUT match */
	compare = get_phy_compare(rfe, cut);
	for (i = 0; i < *headline_size; i++) {
		reg = &table->regs[i];
		target = get_phy_target(reg->addr);
		if (target == compare) {
			*headline_idx = i;
			return 0;
		}
	}

	/* case 2: RFE match, CUT don't care */
	compare = get_phy_compare(rfe, PHY_COND_DONT_CARE);
	for (i = 0; i < *headline_size; i++) {
		reg = &table->regs[i];
		target = get_phy_target(reg->addr);
		if (target == compare) {
			*headline_idx = i;
			return 0;
		}
	}

	/* case 3: RFE match, CUT max in table */
	for (i = 0; i < *headline_size; i++) {
		reg = &table->regs[i];
		rfe_para = get_phy_cond_rfe(reg->addr);
		cut_para = get_phy_cond_cut(reg->addr);
		if (rfe_para == rfe) {
			if (cut_para >= cut_max) {
				cut_max = cut_para;
				*headline_idx = i;
				case_matched = true;
			}
		}
	}

	if (case_matched)
		return 0;

	/* case 4: RFE don't care, CUT max in table */
	for (i = 0; i < *headline_size; i++) {
		reg = &table->regs[i];
		rfe_para = get_phy_cond_rfe(reg->addr);
		cut_para = get_phy_cond_cut(reg->addr);
		if (rfe_para == PHY_COND_DONT_CARE) {
			if (cut_para >= cut_max) {
				cut_max = cut_para;
				*headline_idx = i;
				case_matched = true;
			}
		}
	}

	if (case_matched)
		return 0;

	return -EINVAL;
}

static void rtw89_phy_init_reg(struct rtw89_dev *rtwdev,
			       const struct rtw89_phy_table *table,
			       void (*config)(struct rtw89_dev *rtwdev,
					      const struct rtw89_reg2_def *reg,
					      enum rtw89_rf_path rf_path,
					      void *data),
			       void *extra_data)
{
	const struct rtw89_reg2_def *reg;
	enum rtw89_rf_path rf_path = table->rf_path;
	u32 i;
	u32 headline_size = 0, headline_idx = 0;
	u32 target = 0, cfg_target;
	u8 cond;
	u8 rfe = 1, cut = 1;
	bool is_matched = true;
	bool target_found = false;
	int ret;

	ret = rtw89_phy_sel_headline(rtwdev, table, &headline_size,
				     &headline_idx, rfe, cut);
	if (ret) {
		rtw89_err(rtwdev, "invalid PHY package: %d/%d\n", rfe, cut);
		return;
	}

	cfg_target = get_phy_target(table->regs[headline_idx].addr);
	for (i = headline_size; i < table->n_regs; i++) {
		reg = &table->regs[i];
		cond = get_phy_cond(reg->addr);
		switch (cond) {
		case PHY_COND_BRANCH_IF:
		case PHY_COND_BRANCH_ELIF:
			target = get_phy_target(reg->addr);
			break;
		case PHY_COND_BRANCH_ELSE:
			is_matched = false;
			if (!target_found) {
				rtw89_warn(rtwdev, "failed to load CR %x/%x\n",
					   reg->addr, reg->data);
				return;
			}
			break;
		case PHY_COND_BRANCH_END:
			is_matched = true;
			target_found = false;
			break;
		case PHY_COND_CHECK:
			if (target_found) {
				is_matched = false;
				break;
			}

			if (target == cfg_target) {
				is_matched = true;
				target_found = true;
			} else {
				is_matched = false;
				target_found = false;
			}
			break;
		default:
			if (is_matched)
				config(rtwdev, reg, rf_path, extra_data);
			break;
		}
	}
}

void rtw89_phy_init_bb_reg(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	const struct rtw89_phy_table *bb_table = chip->bb_table;

	rtw89_phy_init_reg(rtwdev, bb_table, rtw89_phy_config_bb_reg, NULL);
	rtw89_chip_init_txpwr_unit(rtwdev, RTW89_PHY_0);
	rtw89_phy_bb_reset(rtwdev, RTW89_PHY_0);
}

static u32 rtw89_phy_nctl_poll(struct rtw89_dev *rtwdev)
{
	rtw89_phy_write32(rtwdev, 0x8080, 0x4);
	udelay(1);
	return rtw89_phy_read32(rtwdev, 0x8080);
}

void rtw89_phy_init_rf_reg(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	const struct rtw89_phy_table *rf_table;
	struct rtw89_fw_h2c_rf_reg_info *rf_reg_info;
	u8 path;

	rf_reg_info = kzalloc(sizeof(*rf_reg_info), GFP_KERNEL);
	if (!rf_reg_info)
		return;

	for (path = RF_PATH_A; path < chip->rf_path_num; path++) {
		rf_reg_info->rf_path = path;
		rf_table = chip->rf_table[path];
		rtw89_phy_init_reg(rtwdev, rf_table, rtw89_phy_config_rf_reg,
				   (void *)rf_reg_info);
		if (rtw89_phy_config_rf_reg_fw(rtwdev, rf_reg_info))
			rtw89_warn(rtwdev, "rf path %d reg h2c config failed\n",
				   path);
	}
	kfree(rf_reg_info);
}

static void rtw89_phy_init_rf_nctl(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	const struct rtw89_phy_table *nctl_table;
	u32 val;
	int ret;

	/* IQK/DPK clock & reset */
	rtw89_phy_write32_set(rtwdev, 0x0c60, 0x3);
	rtw89_phy_write32_set(rtwdev, 0x0c6c, 0x1);
	rtw89_phy_write32_set(rtwdev, 0x58ac, 0x8000000);
	rtw89_phy_write32_set(rtwdev, 0x78ac, 0x8000000);

	/* check 0x8080 */
	rtw89_phy_write32(rtwdev, 0x8000, 0x8);

	ret = read_poll_timeout(rtw89_phy_nctl_poll, val, val == 0x4, 10,
				1000, false, rtwdev);
	if (ret)
		rtw89_err(rtwdev, "failed to poll nctl block\n");

	nctl_table = chip->nctl_table;
	rtw89_phy_init_reg(rtwdev, nctl_table, rtw89_phy_config_bb_reg, NULL);
}

static u32 rtw89_phy0_phy1_offset(struct rtw89_dev *rtwdev, u32 addr)
{
	u32 phy_page = addr >> 8;
	u32 ofst = 0;

	switch (phy_page) {
	case 0x6:
	case 0x7:
	case 0x8:
	case 0x9:
	case 0xa:
	case 0xb:
	case 0xc:
	case 0xd:
	case 0x19:
	case 0x1a:
	case 0x1b:
		ofst = 0x2000;
		break;
	default:
		/* warning case */
		ofst = 0;
		break;
	}

	if (phy_page >= 0x40 && phy_page <= 0x4f)
		ofst = 0x2000;

	return ofst;
}

void rtw89_phy_write32_idx(struct rtw89_dev *rtwdev, u32 addr, u32 mask,
			   u32 data, enum rtw89_phy_idx phy_idx)
{
	if (rtwdev->dbcc_en && phy_idx == RTW89_PHY_1)
		addr += rtw89_phy0_phy1_offset(rtwdev, addr);
	rtw89_phy_write32_mask(rtwdev, addr, mask, data);
}

void rtw89_phy_set_phy_regs(struct rtw89_dev *rtwdev, u32 addr, u32 mask,
			    u32 val)
{
	rtw89_phy_write32_idx(rtwdev, addr, mask, val, RTW89_PHY_0);

	if (!rtwdev->dbcc_en)
		return;

	rtw89_phy_write32_idx(rtwdev, addr, mask, val, RTW89_PHY_1);
}

const u8 rtw89_rs_idx_max[] = {
	[RTW89_RS_CCK] = RTW89_RATE_CCK_MAX,
	[RTW89_RS_OFDM] = RTW89_RATE_OFDM_MAX,
	[RTW89_RS_MCS] = RTW89_RATE_MCS_MAX,
	[RTW89_RS_HEDCM] = RTW89_RATE_HEDCM_MAX,
	[RTW89_RS_OFFSET] = RTW89_RATE_OFFSET_MAX,
};

const u8 rtw89_rs_nss_max[] = {
	[RTW89_RS_CCK] = 1,
	[RTW89_RS_OFDM] = 1,
	[RTW89_RS_MCS] = RTW89_NSS_MAX,
	[RTW89_RS_HEDCM] = RTW89_NSS_HEDCM_MAX,
	[RTW89_RS_OFFSET] = 1,
};

static const u8 _byr_of_rs[] = {
	[RTW89_RS_CCK] = offsetof(struct rtw89_txpwr_byrate, cck),
	[RTW89_RS_OFDM] = offsetof(struct rtw89_txpwr_byrate, ofdm),
	[RTW89_RS_MCS] = offsetof(struct rtw89_txpwr_byrate, mcs),
	[RTW89_RS_HEDCM] = offsetof(struct rtw89_txpwr_byrate, hedcm),
	[RTW89_RS_OFFSET] = offsetof(struct rtw89_txpwr_byrate, offset),
};

#define _byr_seek(rs, raw) ((s8 *)(raw) + _byr_of_rs[rs])
#define _byr_idx(rs, nss, idx) ((nss) * rtw89_rs_idx_max[rs] + (idx))
#define _byr_chk(rs, nss, idx) \
	((nss) < rtw89_rs_nss_max[rs] && (idx) < rtw89_rs_idx_max[rs])

void rtw89_phy_load_txpwr_byrate(struct rtw89_dev *rtwdev,
				 const struct rtw89_txpwr_table *tbl)
{
	const struct rtw89_txpwr_byrate_cfg *cfg = tbl->data;
	const struct rtw89_txpwr_byrate_cfg *end = cfg + tbl->size;
	s8 *byr;
	u32 data;
	u8 i, idx;

	for (; cfg < end; cfg++) {
		byr = _byr_seek(cfg->rs, &rtwdev->byr[cfg->band]);
		data = cfg->data;

		for (i = 0; i < cfg->len; i++, data >>= 8) {
			idx = _byr_idx(cfg->rs, cfg->nss, (cfg->shf + i));
			byr[idx] = (s8)(data & 0xff);
		}
	}
}

#define _phy_txpwr_rf_to_mac(rtwdev, txpwr_rf)				\
({									\
	const struct rtw89_chip_info *__c = (rtwdev)->chip;		\
	(txpwr_rf) >> (__c->txpwr_factor_rf - __c->txpwr_factor_mac);	\
})

s8 rtw89_phy_read_txpwr_byrate(struct rtw89_dev *rtwdev,
			       const struct rtw89_rate_desc *rate_desc)
{
	enum rtw89_band band = rtwdev->hal.current_band_type;
	s8 *byr;
	u8 idx;

	if (rate_desc->rs == RTW89_RS_CCK)
		band = RTW89_BAND_2G;

	if (!_byr_chk(rate_desc->rs, rate_desc->nss, rate_desc->idx)) {
		rtw89_debug(rtwdev, RTW89_DBG_TXPWR,
			    "[TXPWR] unknown byrate desc rs=%d nss=%d idx=%d\n",
			    rate_desc->rs, rate_desc->nss, rate_desc->idx);

		return 0;
	}

	byr = _byr_seek(rate_desc->rs, &rtwdev->byr[band]);
	idx = _byr_idx(rate_desc->rs, rate_desc->nss, rate_desc->idx);

	return _phy_txpwr_rf_to_mac(rtwdev, byr[idx]);
}

static u8 rtw89_channel_to_idx(struct rtw89_dev *rtwdev, u8 channel)
{
	switch (channel) {
	case 1 ... 14:
		return channel - 1;
	case 36 ... 64:
		return (channel - 36) / 2;
	case 100 ... 144:
		return ((channel - 100) / 2) + 15;
	case 149 ... 177:
		return ((channel - 149) / 2) + 38;
	default:
		rtw89_warn(rtwdev, "unknown channel: %d\n", channel);
		return 0;
	}
}

static s8 rtw89_phy_read_txpwr_limit(struct rtw89_dev *rtwdev,
				     u8 bw, u8 ntx, u8 rs, u8 bf, u8 ch)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	u8 ch_idx = rtw89_channel_to_idx(rtwdev, ch);
	u8 band = rtwdev->hal.current_band_type;
	u8 regd = rtw89_regd_get(rtwdev, band);
	s8 lmt = 0;

	switch (band) {
	case RTW89_BAND_2G:
		lmt = (*chip->txpwr_lmt_2g)[bw][ntx][rs][bf][regd][ch_idx];
		break;
	case RTW89_BAND_5G:
		lmt = (*chip->txpwr_lmt_5g)[bw][ntx][rs][bf][regd][ch_idx];
		break;
	default:
		rtw89_warn(rtwdev, "unknown band type: %d\n", band);
		return 0;
	}

	return _phy_txpwr_rf_to_mac(rtwdev, lmt);
}

#define __fill_txpwr_limit_nonbf_bf(ptr, bw, ntx, rs, ch)		\
	do {								\
		u8 __i;							\
		for (__i = 0; __i < RTW89_BF_NUM; __i++)		\
			ptr[__i] = rtw89_phy_read_txpwr_limit(rtwdev,	\
							      bw, ntx,	\
							      rs, __i,	\
							      (ch));	\
	} while (0)

static void rtw89_phy_fill_txpwr_limit_20m(struct rtw89_dev *rtwdev,
					   struct rtw89_txpwr_limit *lmt,
					   u8 ntx, u8 ch)
{
	__fill_txpwr_limit_nonbf_bf(lmt->cck_20m, RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_CCK, ch);
	__fill_txpwr_limit_nonbf_bf(lmt->cck_40m, RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_CCK, ch);
	__fill_txpwr_limit_nonbf_bf(lmt->ofdm, RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_OFDM, ch);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[0], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch);
}

static void rtw89_phy_fill_txpwr_limit_40m(struct rtw89_dev *rtwdev,
					   struct rtw89_txpwr_limit *lmt,
					   u8 ntx, u8 ch)
{
	__fill_txpwr_limit_nonbf_bf(lmt->cck_20m, RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_CCK, ch - 2);
	__fill_txpwr_limit_nonbf_bf(lmt->cck_40m, RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_CCK, ch);
	__fill_txpwr_limit_nonbf_bf(lmt->ofdm, RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_OFDM, ch - 2);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[0], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch - 2);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[1], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch + 2);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_40m[0], RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_MCS, ch);
}

static void rtw89_phy_fill_txpwr_limit_80m(struct rtw89_dev *rtwdev,
					   struct rtw89_txpwr_limit *lmt,
					   u8 ntx, u8 ch)
{
	s8 val_0p5_n[RTW89_BF_NUM];
	s8 val_0p5_p[RTW89_BF_NUM];
	u8 i;

	__fill_txpwr_limit_nonbf_bf(lmt->ofdm, RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_OFDM, ch - 6);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[0], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch - 6);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[1], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch - 2);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[2], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch + 2);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_20m[3], RTW89_CHANNEL_WIDTH_20,
				    ntx, RTW89_RS_MCS, ch + 6);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_40m[0], RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_MCS, ch - 4);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_40m[1], RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_MCS, ch + 4);
	__fill_txpwr_limit_nonbf_bf(lmt->mcs_80m[0], RTW89_CHANNEL_WIDTH_80,
				    ntx, RTW89_RS_MCS, ch);

	__fill_txpwr_limit_nonbf_bf(val_0p5_n, RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_MCS, ch - 4);
	__fill_txpwr_limit_nonbf_bf(val_0p5_p, RTW89_CHANNEL_WIDTH_40,
				    ntx, RTW89_RS_MCS, ch + 4);

	for (i = 0; i < RTW89_BF_NUM; i++)
		lmt->mcs_40m_0p5[i] = min_t(s8, val_0p5_n[i], val_0p5_p[i]);
}

void rtw89_phy_fill_txpwr_limit(struct rtw89_dev *rtwdev,
				struct rtw89_txpwr_limit *lmt,
				u8 ntx)
{
	u8 ch = rtwdev->hal.current_channel;
	u8 bw = rtwdev->hal.current_band_width;

	memset(lmt, 0, sizeof(*lmt));

	switch (bw) {
	case RTW89_CHANNEL_WIDTH_20:
		rtw89_phy_fill_txpwr_limit_20m(rtwdev, lmt, ntx, ch);
		break;
	case RTW89_CHANNEL_WIDTH_40:
		rtw89_phy_fill_txpwr_limit_40m(rtwdev, lmt, ntx, ch);
		break;
	case RTW89_CHANNEL_WIDTH_80:
		rtw89_phy_fill_txpwr_limit_80m(rtwdev, lmt, ntx, ch);
		break;
	}
}

static s8 rtw89_phy_read_txpwr_limit_ru(struct rtw89_dev *rtwdev,
					u8 ru, u8 ntx, u8 ch)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	u8 ch_idx = rtw89_channel_to_idx(rtwdev, ch);
	u8 band = rtwdev->hal.current_band_type;
	u8 regd = rtw89_regd_get(rtwdev, band);
	s8 lmt_ru = 0;

	switch (band) {
	case RTW89_BAND_2G:
		lmt_ru = (*chip->txpwr_lmt_ru_2g)[ru][ntx][regd][ch_idx];
		break;
	case RTW89_BAND_5G:
		lmt_ru = (*chip->txpwr_lmt_ru_5g)[ru][ntx][regd][ch_idx];
		break;
	default:
		rtw89_warn(rtwdev, "unknown band type: %d\n", band);
		return 0;
	}

	return _phy_txpwr_rf_to_mac(rtwdev, lmt_ru);
}

static void
rtw89_phy_fill_txpwr_limit_ru_20m(struct rtw89_dev *rtwdev,
				  struct rtw89_txpwr_limit_ru *lmt_ru,
				  u8 ntx, u8 ch)
{
	lmt_ru->ru26[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch);
	lmt_ru->ru52[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch);
	lmt_ru->ru106[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch);
}

static void
rtw89_phy_fill_txpwr_limit_ru_40m(struct rtw89_dev *rtwdev,
				  struct rtw89_txpwr_limit_ru *lmt_ru,
				  u8 ntx, u8 ch)
{
	lmt_ru->ru26[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch - 2);
	lmt_ru->ru26[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch + 2);
	lmt_ru->ru52[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch - 2);
	lmt_ru->ru52[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch + 2);
	lmt_ru->ru106[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch - 2);
	lmt_ru->ru106[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch + 2);
}

static void
rtw89_phy_fill_txpwr_limit_ru_80m(struct rtw89_dev *rtwdev,
				  struct rtw89_txpwr_limit_ru *lmt_ru,
				  u8 ntx, u8 ch)
{
	lmt_ru->ru26[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch - 6);
	lmt_ru->ru26[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch - 2);
	lmt_ru->ru26[2] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch + 2);
	lmt_ru->ru26[3] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU26,
							ntx, ch + 6);
	lmt_ru->ru52[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch - 6);
	lmt_ru->ru52[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch - 2);
	lmt_ru->ru52[2] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch + 2);
	lmt_ru->ru52[3] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU52,
							ntx, ch + 6);
	lmt_ru->ru106[0] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch - 6);
	lmt_ru->ru106[1] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch - 2);
	lmt_ru->ru106[2] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch + 2);
	lmt_ru->ru106[3] = rtw89_phy_read_txpwr_limit_ru(rtwdev, RTW89_RU106,
							 ntx, ch + 6);
}

void rtw89_phy_fill_txpwr_limit_ru(struct rtw89_dev *rtwdev,
				   struct rtw89_txpwr_limit_ru *lmt_ru,
				   u8 ntx)
{
	u8 ch = rtwdev->hal.current_channel;
	u8 bw = rtwdev->hal.current_band_width;

	memset(lmt_ru, 0, sizeof(*lmt_ru));

	switch (bw) {
	case RTW89_CHANNEL_WIDTH_20:
		rtw89_phy_fill_txpwr_limit_ru_20m(rtwdev, lmt_ru, ntx, ch);
		break;
	case RTW89_CHANNEL_WIDTH_40:
		rtw89_phy_fill_txpwr_limit_ru_40m(rtwdev, lmt_ru, ntx, ch);
		break;
	case RTW89_CHANNEL_WIDTH_80:
		rtw89_phy_fill_txpwr_limit_ru_80m(rtwdev, lmt_ru, ntx, ch);
		break;
	}
}

struct rtw89_phy_iter_ra_data {
	struct rtw89_dev *rtwdev;
	struct sk_buff *c2h;
};

static void rtw89_phy_c2h_ra_rpt_iter(void *data, struct ieee80211_sta *sta)
{
	struct rtw89_phy_iter_ra_data *ra_data = (struct rtw89_phy_iter_ra_data *)data;
	struct rtw89_dev *rtwdev = ra_data->rtwdev;
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_ra_report *ra_report = &rtwsta->ra_report;
	struct sk_buff *c2h = ra_data->c2h;
	u8 mode, rate, bw, giltf, mac_id;

	mac_id = RTW89_GET_PHY_C2H_RA_RPT_MACID(c2h->data);
	if (mac_id != rtwsta->mac_id)
		return;

	memset(ra_report, 0, sizeof(*ra_report));

	rate = RTW89_GET_PHY_C2H_RA_RPT_MCSNSS(c2h->data);
	bw = RTW89_GET_PHY_C2H_RA_RPT_BW(c2h->data);
	giltf = RTW89_GET_PHY_C2H_RA_RPT_GILTF(c2h->data);
	mode = RTW89_GET_PHY_C2H_RA_RPT_MD_SEL(c2h->data);

	switch (mode) {
	case RTW89_RA_RPT_MODE_LEGACY:
		ra_report->txrate.legacy = rtw89_ra_report_to_bitrate(rtwdev, rate);
		break;
	case RTW89_RA_RPT_MODE_HT:
		ra_report->txrate.flags |= RATE_INFO_FLAGS_MCS;
		ra_report->txrate.mcs = rate & 0x1f;
		if (giltf)
			ra_report->txrate.flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case RTW89_RA_RPT_MODE_VHT:
		ra_report->txrate.flags |= RATE_INFO_FLAGS_VHT_MCS;
		ra_report->txrate.mcs = rate & 0xf;
		ra_report->txrate.nss = FIELD_GET(GENMASK(6, 4), rate) + 1;
		if (giltf)
			ra_report->txrate.flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case RTW89_RA_RPT_MODE_HE:
		ra_report->txrate.flags |= RATE_INFO_FLAGS_HE_MCS;
		ra_report->txrate.mcs = rate & 0xf;
		ra_report->txrate.nss = FIELD_GET(GENMASK(6, 4), rate) + 1;
		if (giltf == RTW89_GILTF_2XHE08 || giltf == RTW89_GILTF_1XHE08)
			ra_report->txrate.he_gi = NL80211_RATE_INFO_HE_GI_0_8;
		else if (giltf == RTW89_GILTF_2XHE16 || giltf == RTW89_GILTF_1XHE16)
			ra_report->txrate.he_gi = NL80211_RATE_INFO_HE_GI_1_6;
		break;
	}

	if (bw == RTW89_CHANNEL_WIDTH_80)
		ra_report->txrate.bw = RATE_INFO_BW_80;
	else if (bw == RTW89_CHANNEL_WIDTH_40)
		ra_report->txrate.bw = RATE_INFO_BW_40;
	else
		ra_report->txrate.bw = RATE_INFO_BW_20;

	ra_report->bit_rate =  cfg80211_calculate_bitrate(&ra_report->txrate);
	sta->max_rc_amsdu_len = get_max_amsdu_len(rtwdev, ra_report->bit_rate);
}

static void
rtw89_phy_c2h_ra_rpt(struct rtw89_dev *rtwdev, struct sk_buff *c2h, u32 len)
{
	struct rtw89_phy_iter_ra_data ra_data;

	ra_data.rtwdev = rtwdev;
	ra_data.c2h = c2h;
	ieee80211_iterate_stations_atomic(rtwdev->hw,
					  rtw89_phy_c2h_ra_rpt_iter,
					  &ra_data);
}

static void (*rtw89_phy_c2h_ra_handler[])(struct rtw89_dev *rtwdev,
					    struct sk_buff *c2h, u32 len) = {
	[RTW89_PHY_C2H_FUNC_STS_RPT] = rtw89_phy_c2h_ra_rpt,
	[RTW89_PHY_C2H_FUNC_MU_GPTBL_RPT] = NULL,
	[RTW89_PHY_C2H_FUNC_TXSTS] = NULL,
};

void rtw89_phy_c2h_handle(struct rtw89_dev *rtwdev, struct sk_buff *skb,
			  u32 len, u8 class, u8 func)
{
	void (*handler)(struct rtw89_dev *rtwdev,
			struct sk_buff *c2h, u32 len) = NULL;

	switch (class) {
	case RTW89_PHY_C2H_CLASS_RA:
		if (func < RTW89_PHY_C2H_FUNC_RA_MAX)
			handler = rtw89_phy_c2h_ra_handler[func];
		break;
	default:
		rtw89_info(rtwdev, "c2h class %d not support\n", class);
		return;
	}
	if (!handler) {
		rtw89_info(rtwdev, "c2h class %d func %d not support\n", class,
			   func);
		return;
	}
	handler(rtwdev, skb, len);
}

static u8 rtw89_phy_cfo_get_xcap_reg(struct rtw89_dev *rtwdev, bool sc_xo)
{
	u32 reg_mask;

	if (sc_xo)
		reg_mask = B_AX_XTAL_SC_XO_MSK;
	else
		reg_mask = B_AX_XTAL_SC_XI_MSK;

	return (u8)rtw89_read32_mask(rtwdev, R_AX_XTAL_ON_CTRL0, reg_mask);
}

static void rtw89_phy_cfo_set_xcap_reg(struct rtw89_dev *rtwdev, bool sc_xo,
				       u8 val)
{
	u32 reg_mask;

	if (sc_xo)
		reg_mask = B_AX_XTAL_SC_XO_MSK;
	else
		reg_mask = B_AX_XTAL_SC_XI_MSK;

	rtw89_write32_mask(rtwdev, R_AX_XTAL_ON_CTRL0, reg_mask, val);
}

static void rtw89_phy_cfo_set_crystal_cap(struct rtw89_dev *rtwdev,
					  u8 crystal_cap)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	u8 sc_xi_val, sc_xo_val;

	if (cfo->crystal_cap == crystal_cap)
		return;

	rtw89_phy_cfo_set_xcap_reg(rtwdev, true, crystal_cap);
	rtw89_phy_cfo_set_xcap_reg(rtwdev, false, crystal_cap);
	sc_xo_val = rtw89_phy_cfo_get_xcap_reg(rtwdev, true);
	sc_xi_val = rtw89_phy_cfo_get_xcap_reg(rtwdev, false);
	cfo->crystal_cap = sc_xi_val;

	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Set sc_xi=0x%x\n", sc_xi_val);
	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Set sc_xo=0x%x\n", sc_xo_val);
	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Set xcap OK\n");
}

static void rtw89_phy_cfo_reset(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	u8 cap;

	cfo->def_x_cap = cfo->crystal_cap_default & B_AX_XTAL_SC_MSK;
	cfo->is_adjust = true;
	if (cfo->crystal_cap == cfo->def_x_cap)
		return;
	cap = cfo->crystal_cap;
	cap += (cap > cfo->def_x_cap ? -1 : 1);
	rtw89_phy_cfo_set_crystal_cap(rtwdev, cap);

	rtw89_debug(rtwdev, RTW89_DBG_CFO,
		    "X-cap approach to init-val (0x%x)\n", cfo->crystal_cap);
}

static void rtw89_phy_cfo_init(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	struct rtw89_efuse *efuse = &rtwdev->efuse;

	cfo->crystal_cap_default = efuse->xtal_cap;
	cfo->crystal_cap = cfo->crystal_cap_default & B_AX_XTAL_SC_MSK;
	cfo->def_x_cap = cfo->crystal_cap;
	cfo->is_adjust = true;
	cfo->apply_compensation = false;
	cfo->residual_cfo_acc = 0;

	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Default xcap=%0x\n",
		    cfo->crystal_cap_default);
}

static void rtw89_phy_digital_cfo_compensation(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	s16 cfo_avg_312p5khz;

	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Digital cfo compensation\n");
	rtw89_debug(rtwdev, RTW89_DBG_CFO, "Residual cfo: ((%dK))\n",
		    cfo->cfo_avg_pre >> 2);

	cfo->residual_cfo_acc = cfo->residual_cfo_acc + cfo->cfo_avg_pre;
	cfo_avg_312p5khz = -1 * (s16)(cfo->residual_cfo_acc << 10) / 625;

	rtw89_debug(rtwdev, RTW89_DBG_CFO,
		    "r_cfo_comp_312p5khz=0x%x\n",
		    (s16)(cfo_avg_312p5khz & B_CFO_COMP_VAL_MSK));

	cfo_avg_312p5khz &= B_CFO_COMP_VAL_MSK;
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG0_L, B_CFO_COMP_VAL_MSK,
			       cfo_avg_312p5khz);
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG1_L, B_CFO_COMP_VAL_MSK,
			       cfo_avg_312p5khz);
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG0_H,
			       B_CFO_COMP_WEIGHT_MSK, CFO_COMP_WEIGHT);
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG1_H,
			       B_CFO_COMP_WEIGHT_MSK, CFO_COMP_WEIGHT);
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG0_CTRL,
			       B_CFO_COMP_VALID_BIT, 1);
	rtw89_phy_write32_mask(rtwdev, R_CFO_COMP_SEG1_CTRL,
			       B_CFO_COMP_VALID_BIT, 1);
	rtw89_write32_clr(rtwdev, R_AX_PWR_UL_CTRL2, B_AX_PWR_UL_CTRL2_MSK);
}

static void rtw89_phy_cfo_crystal_cap_adjust(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	s8 crystal_cap = cfo->crystal_cap;
	int cfo_previous, sign;

	cfo_previous = abs(cfo->cfo_avg_pre);
	if (!cfo->is_adjust) {
		if (cfo_previous > CFO_TRK_ENABLE_TH)
			cfo->is_adjust = true;
	}

	sign = cfo->cfo_avg_pre > 0 ? 1 : -1;
	if (cfo->is_adjust) {
		cfo_previous = abs(cfo->cfo_avg_pre);
		if (cfo_previous > CFO_TRK_STOP_TH_4)
			crystal_cap += 7 * sign;
		else if (cfo_previous > CFO_TRK_STOP_TH_3)
			crystal_cap += 5 * sign;
		else if (cfo_previous > CFO_TRK_STOP_TH_2)
			crystal_cap += 3 * sign;
		else if (cfo_previous > CFO_TRK_STOP_TH)
			crystal_cap += 1 * sign;
		else if (cfo_previous <= CFO_TRK_STOP_TH)
			cfo->is_adjust = false;

		if (crystal_cap > B_AX_XTAL_SC_MSK)
			crystal_cap = B_AX_XTAL_SC_MSK;
		else if (crystal_cap < 0)
			crystal_cap = 0;
		rtw89_phy_cfo_set_crystal_cap(rtwdev, (u8)crystal_cap);

		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "X_cap{Curr,Default}={0x%x,0x%x}\n",
			    cfo->crystal_cap, cfo->def_x_cap);

	} else {
		if (cfo->apply_compensation)
			rtw89_phy_digital_cfo_compensation(rtwdev);
	}
}

static void rtw89_phy_average_cfo_calc(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	s32 cfo_khz_avg[CFO_TRACK_MAX_USER] = {0};
	s32 cfo_max = 0, cfo_min = U16_MAX, cfo_khz_all = 0;
	s32 cnt_max = 0, cnt_min = U16_MAX, cfo_cnt_all = 0;
	s16 val;
	u8 cnt_max_macid = 0, cnt_min_macid = 0;
	u8 cfo_max_macid = 0, cfo_min_macid = 0, i;

	rtw89_debug(rtwdev, RTW89_DBG_CFO, "one_entry_only=%d\n",
		    rtwdev->total_sta_assoc == 1);

	if (rtwdev->total_sta_assoc == 1) {
		for (i = 0; i < CFO_TRACK_MAX_USER; i++) {
			if (cfo->cfo_cnt[i] == 0)
				continue;
			cfo_khz_all += cfo->cfo_tail[i];
			cfo_cnt_all += cfo->cfo_cnt[i];
			if (cfo_cnt_all == 0)
				cfo->cfo_avg_pre = 0;
			else
				cfo->cfo_avg_pre = cfo_khz_all / cfo_cnt_all;
		}
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "CFO track for one entry only\n");
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "Total cfo=(%dK), pkt_cnt=(%d), avg_cfo=(%dK)\n",
			    cfo_khz_all >> 2, cfo_cnt_all,
			    cfo->cfo_avg_pre >> 2);
		return;
	}

	for (i = 0; i < CFO_TRACK_MAX_USER; i++) {
		if (cfo->cfo_cnt[i] == 0)
			continue;

		cfo_khz_all += cfo->cfo_tail[i];
		cfo_cnt_all += cfo->cfo_cnt[i];
		if (cfo->cfo_cnt[i] == 0)
			cfo_khz_avg[i] = 0;
		else
			cfo_khz_avg[i] = cfo->cfo_tail[i] / cfo->cfo_cnt[i];

		if (cfo->cfo_cnt[i] > cnt_max) {
			cnt_max = cfo->cfo_cnt[i];
			cnt_max_macid = i;
		}
		if (cfo->cfo_cnt[i] < cnt_min) {
			cnt_min = cfo->cfo_cnt[i];
			cnt_min_macid = i;
		}
		if (cfo_khz_avg[i] > cfo_max) {
			cfo_max = cfo_khz_avg[i];
			cfo_max_macid = i;
		}
		if (cfo_khz_avg[i] < cfo_min) {
			cfo_min = cfo_khz_avg[i];
			cfo_min_macid = i;
		}
	}

	rtw89_debug(rtwdev, RTW89_DBG_CFO,
		    "cnt macid = {%d, %d}, cfo macid = {%d, %d}\n",
		    cnt_min_macid, cnt_max_macid, cfo_min_macid, cfo_max_macid);

	/* Multi-sta CFO tracking strategy */
	val = (s16)abs(cfo_max - cfo_min);
	if (val < MAX_CFO_TOLERANCE || val > (MAX_CFO_TOLERANCE << 1)) {
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "CFO track for only pri-user\n");
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "Total cfo=(%dK), pkt_cnt=(%d), avg_cfo=(%dK)\n",
			    cfo->cfo_tail[cnt_max_macid] >> 2,
			    cfo->cfo_cnt[cnt_max_macid],
			    cfo_khz_avg[cnt_max_macid] >> 2);
		cfo->cfo_avg_pre = cfo_khz_avg[cnt_max_macid];
	} else {
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "CFO track for average of all user\n");
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "Total cfo=(%dK), pkt_cnt=(%d), avg_cfo=(%dK)\n",
			    cfo_khz_all >> 2, cfo_cnt_all,
			    cfo->cfo_avg_pre >> 2);
		if (cfo_cnt_all == 0)
			cfo->cfo_avg_pre = 0;
		else
			cfo->cfo_avg_pre = cfo_khz_all / cfo_cnt_all;
	}
}

static void rtw89_phy_cfo_statistics_reset(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;

	memset(&cfo->cfo_tail, 0, sizeof(cfo->cfo_tail));
	memset(&cfo->cfo_cnt, 0, sizeof(cfo->cfo_cnt));
	cfo->packet_count = 0;
	cfo->packet_count_pre = 0;
	cfo->cfo_avg_pre = 0;
}

void rtw89_phy_cfo_track(struct rtw89_dev *rtwdev)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;

	if (rtwdev->total_sta_assoc != 1) {
		rtw89_phy_cfo_reset(rtwdev);
		rtw89_debug(rtwdev, RTW89_DBG_CFO, "total_sta_assoc = %d\n",
			    rtwdev->total_sta_assoc);
		return;
	}

	if (cfo->packet_count == cfo->packet_count_pre) {
		rtw89_debug(rtwdev, RTW89_DBG_CFO,
			    "Pkt cnt doesn't change\n");
		return;
	}

	cfo->packet_count_pre = cfo->packet_count;
	rtw89_phy_average_cfo_calc(rtwdev);
	rtw89_phy_cfo_crystal_cap_adjust(rtwdev);
	rtw89_phy_cfo_statistics_reset(rtwdev);
}

void rtw89_phy_cfo_parse(struct rtw89_dev *rtwdev, s16 cfo_val,
			 struct rtw89_rx_phy_ppdu *phy_ppdu)
{
	struct rtw89_cfo_tracking_info *cfo = &rtwdev->cfo_tracking;
	u8 macid = phy_ppdu->mac_id;

	cfo->cfo_tail[macid] += cfo_val;
	cfo->cfo_cnt[macid]++;
	cfo->packet_count++;
}

static void rtw89_phy_stat_thermal_update(struct rtw89_dev *rtwdev)
{
	struct rtw89_phy_stat *phystat = &rtwdev->phystat;
	int i;
	u8 th;

	for (i = 0; i < rtwdev->chip->rf_path_num; i++) {
		th = rtw89_chip_get_thermal(rtwdev, i);
		if (th)
			ewma_thermal_add(&phystat->avg_thermal[i], th);

		rtw89_debug(rtwdev, RTW89_DBG_RFK_TRACK,
			    "path(%d) thermal cur=%u avg=%ld", i, th,
			    ewma_thermal_read(&phystat->avg_thermal[i]));
	}
}

static void rtw89_phy_stat_rssi_update_iter(void *data,
					    struct ieee80211_sta *sta)
{
	struct rtw89_sta *rtwsta = (struct rtw89_sta *)sta->drv_priv;
	struct rtw89_phy_ch_info *ch_info = (struct rtw89_phy_ch_info *)data;
	unsigned long rssi_curr;

	rssi_curr = ewma_rssi_read(&rtwsta->avg_rssi);

	if (rssi_curr < ch_info->rssi_min) {
		ch_info->rssi_min = rssi_curr;
		ch_info->rssi_min_macid = rtwsta->mac_id;
	}
}

static void rtw89_phy_stat_rssi_update(struct rtw89_dev *rtwdev)
{
	struct rtw89_phy_ch_info *ch_info = &rtwdev->ch_info;

	ch_info->rssi_min = U8_MAX;
	ieee80211_iterate_stations_atomic(rtwdev->hw,
					  rtw89_phy_stat_rssi_update_iter,
					  ch_info);
}

static void rtw89_phy_stat_init(struct rtw89_dev *rtwdev)
{
	struct rtw89_phy_stat *phystat = &rtwdev->phystat;
	int i;

	for (i = 0; i < rtwdev->chip->rf_path_num; i++)
		ewma_thermal_init(&phystat->avg_thermal[i]);

	rtw89_phy_stat_thermal_update(rtwdev);
}

void rtw89_phy_stat_track(struct rtw89_dev *rtwdev)
{
	rtw89_phy_stat_thermal_update(rtwdev);
	rtw89_phy_stat_rssi_update(rtwdev);
}

static u16 rtw89_phy_ccx_us_to_idx(struct rtw89_dev *rtwdev, u32 time_us)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;

	return time_us >> (ilog2(CCX_US_BASE_RATIO) + env->ccx_unit_idx);
}

static u32 rtw89_phy_ccx_idx_to_us(struct rtw89_dev *rtwdev, u16 idx)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;

	return idx << (ilog2(CCX_US_BASE_RATIO) + env->ccx_unit_idx);
}

static void rtw89_phy_ccx_top_setting_init(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;

	env->ccx_manual_ctrl = false;
	env->ccx_ongoing = false;
	env->ccx_rac_lv = RTW89_RAC_RELEASE;
	env->ccx_rpt_stamp = 0;
	env->ccx_period = 0;
	env->ccx_unit_idx = RTW89_CCX_32_US;
	env->ccx_trigger_time = 0;
	env->ccx_edcca_opt_bw_idx = RTW89_CCX_EDCCA_BW20_0;

	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_CCX_EN_MSK, 1);
	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_CCX_TRIG_OPT_MSK, 1);
	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_MEASUREMENT_TRIG_MSK, 1);
	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_CCX_EDCCA_OPT_MSK,
			       RTW89_CCX_EDCCA_BW20_0);
}

static u16 rtw89_phy_ccx_get_report(struct rtw89_dev *rtwdev, u16 report,
				    u16 score)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	u32 numer = 0;
	u16 ret = 0;

	numer = report * score + (env->ccx_period >> 1);
	if (env->ccx_period)
		ret = numer / env->ccx_period;

	return ret >= score ? score - 1 : ret;
}

static void rtw89_phy_ccx_ms_to_period_unit(struct rtw89_dev *rtwdev,
					    u16 time_ms, u32 *period,
					    u32 *unit_idx)
{
	u32 idx;
	u8 quotient;

	if (time_ms >= CCX_MAX_PERIOD)
		time_ms = CCX_MAX_PERIOD;

	quotient = CCX_MAX_PERIOD_UNIT * time_ms / CCX_MAX_PERIOD;

	if (quotient < 4)
		idx = RTW89_CCX_4_US;
	else if (quotient < 8)
		idx = RTW89_CCX_8_US;
	else if (quotient < 16)
		idx = RTW89_CCX_16_US;
	else
		idx = RTW89_CCX_32_US;

	*unit_idx = idx;
	*period = (time_ms * MS_TO_4US_RATIO) >> idx;

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "[Trigger Time] period:%d, unit_idx:%d\n",
		    *period, *unit_idx);
}

static void rtw89_phy_ccx_racing_release(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "lv:(%d)->(0)\n", env->ccx_rac_lv);

	env->ccx_ongoing = false;
	env->ccx_rac_lv = RTW89_RAC_RELEASE;
	env->ifs_clm_app = RTW89_IFS_CLM_BACKGROUND;
}

static bool rtw89_phy_ifs_clm_th_update_check(struct rtw89_dev *rtwdev,
					      struct rtw89_ccx_para_info *para)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	bool is_update = env->ifs_clm_app != para->ifs_clm_app;
	u8 i = 0;
	u16 *ifs_th_l = env->ifs_clm_th_l;
	u16 *ifs_th_h = env->ifs_clm_th_h;
	u32 ifs_th0_us = 0, ifs_th_times = 0;
	u32 ifs_th_h_us[RTW89_IFS_CLM_NUM] = {0};

	if (!is_update)
		goto ifs_update_finished;

	switch (para->ifs_clm_app) {
	case RTW89_IFS_CLM_INIT:
	case RTW89_IFS_CLM_BACKGROUND:
	case RTW89_IFS_CLM_ACS:
	case RTW89_IFS_CLM_DBG:
	case RTW89_IFS_CLM_DIG:
	case RTW89_IFS_CLM_TDMA_DIG:
		ifs_th0_us = IFS_CLM_TH0_UPPER;
		ifs_th_times = IFS_CLM_TH_MUL;
		break;
	case RTW89_IFS_CLM_DBG_MANUAL:
		ifs_th0_us = para->ifs_clm_manual_th0;
		ifs_th_times = para->ifs_clm_manual_th_times;
		break;
	default:
		break;
	}

	/* Set sampling threshold for 4 different regions, unit in idx_cnt.
	 * low[i] = high[i-1] + 1
	 * high[i] = high[i-1] * ifs_th_times
	 */
	ifs_th_l[IFS_CLM_TH_START_IDX] = 0;
	ifs_th_h_us[IFS_CLM_TH_START_IDX] = ifs_th0_us;
	ifs_th_h[IFS_CLM_TH_START_IDX] = rtw89_phy_ccx_us_to_idx(rtwdev,
								 ifs_th0_us);
	for (i = 1; i < RTW89_IFS_CLM_NUM; i++) {
		ifs_th_l[i] = ifs_th_h[i - 1] + 1;
		ifs_th_h_us[i] = ifs_th_h_us[i - 1] * ifs_th_times;
		ifs_th_h[i] = rtw89_phy_ccx_us_to_idx(rtwdev, ifs_th_h_us[i]);
	}

ifs_update_finished:
	if (!is_update)
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "No need to update IFS_TH\n");

	return is_update;
}

static void rtw89_phy_ifs_clm_set_th_reg(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	u8 i = 0;

	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T1, B_IFS_T1_TH_LOW_MSK,
			       env->ifs_clm_th_l[0]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T2, B_IFS_T2_TH_LOW_MSK,
			       env->ifs_clm_th_l[1]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T3, B_IFS_T3_TH_LOW_MSK,
			       env->ifs_clm_th_l[2]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T4, B_IFS_T4_TH_LOW_MSK,
			       env->ifs_clm_th_l[3]);

	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T1, B_IFS_T1_TH_HIGH_MSK,
			       env->ifs_clm_th_h[0]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T2, B_IFS_T2_TH_HIGH_MSK,
			       env->ifs_clm_th_h[1]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T3, B_IFS_T3_TH_HIGH_MSK,
			       env->ifs_clm_th_h[2]);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T4, B_IFS_T4_TH_HIGH_MSK,
			       env->ifs_clm_th_h[3]);

	for (i = 0; i < RTW89_IFS_CLM_NUM; i++)
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "Update IFS_T%d_th{low, high} : {%d, %d}\n",
			    i + 1, env->ifs_clm_th_l[i], env->ifs_clm_th_h[i]);
}

static void rtw89_phy_ifs_clm_setting_init(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	struct rtw89_ccx_para_info para = {0};

	env->ifs_clm_app = RTW89_IFS_CLM_BACKGROUND;
	env->ifs_clm_mntr_time = 0;

	para.ifs_clm_app = RTW89_IFS_CLM_INIT;
	if (rtw89_phy_ifs_clm_th_update_check(rtwdev, &para))
		rtw89_phy_ifs_clm_set_th_reg(rtwdev);

	rtw89_phy_set_phy_regs(rtwdev, R_IFS_COUNTER, B_IFS_COLLECT_EN,
			       true);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T1, B_IFS_T1_EN_MSK, true);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T2, B_IFS_T2_EN_MSK, true);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T3, B_IFS_T3_EN_MSK, true);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_T4, B_IFS_T4_EN_MSK, true);
}

static int rtw89_phy_ccx_racing_ctrl(struct rtw89_dev *rtwdev,
				     enum rtw89_env_racing_lv level)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	int ret = 0;

	if (level >= RTW89_RAC_MAX_NUM) {
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "[WARNING] Wrong LV=%d\n", level);
		return -EINVAL;
	}

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "ccx_ongoing=%d, level:(%d)->(%d)\n", env->ccx_ongoing,
		    env->ccx_rac_lv, level);

	if (env->ccx_ongoing) {
		if (level <= env->ccx_rac_lv)
			ret = -EINVAL;
		else
			env->ccx_ongoing = false;
	}

	if (ret == 0)
		env->ccx_rac_lv = level;

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK, "ccx racing success=%d\n",
		    !ret);

	return ret;
}

static void rtw89_phy_ccx_trigger(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;

	rtw89_phy_set_phy_regs(rtwdev, R_IFS_COUNTER, B_IFS_COUNTER_CLR_MSK, 0);
	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_MEASUREMENT_TRIG_MSK, 0);
	rtw89_phy_set_phy_regs(rtwdev, R_IFS_COUNTER, B_IFS_COUNTER_CLR_MSK, 1);
	rtw89_phy_set_phy_regs(rtwdev, R_CCX, B_MEASUREMENT_TRIG_MSK, 1);

	env->ccx_rpt_stamp++;
	env->ccx_ongoing = true;
}

static void rtw89_phy_ifs_clm_get_utility(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	u8 i = 0;
	u32 res = 0;

	env->ifs_clm_tx_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_tx, PERCENT);
	env->ifs_clm_edcca_excl_cca_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_edcca_excl_cca,
					 PERCENT);
	env->ifs_clm_cck_fa_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_cckfa, PERCENT);
	env->ifs_clm_ofdm_fa_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_ofdmfa, PERCENT);
	env->ifs_clm_cck_cca_excl_fa_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_cckcca_excl_fa,
					 PERCENT);
	env->ifs_clm_ofdm_cca_excl_fa_ratio =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_ofdmcca_excl_fa,
					 PERCENT);
	env->ifs_clm_cck_fa_permil =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_cckfa, PERMIL);
	env->ifs_clm_ofdm_fa_permil =
		rtw89_phy_ccx_get_report(rtwdev, env->ifs_clm_ofdmfa, PERMIL);

	for (i = 0; i < RTW89_IFS_CLM_NUM; i++) {
		if (env->ifs_clm_his[i] > ENV_MNTR_IFSCLM_HIS_MAX) {
			env->ifs_clm_ifs_avg[i] = ENV_MNTR_FAIL_DWORD;
		} else {
			env->ifs_clm_ifs_avg[i] =
				rtw89_phy_ccx_idx_to_us(rtwdev,
							env->ifs_clm_avg[i]);
		}

		res = rtw89_phy_ccx_idx_to_us(rtwdev, env->ifs_clm_cca[i]);
		res += env->ifs_clm_his[i] >> 1;
		if (env->ifs_clm_his[i])
			res /= env->ifs_clm_his[i];
		else
			res = 0;
		env->ifs_clm_cca_avg[i] = res;
	}

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM ratio {Tx, EDCCA_exclu_cca} = {%d, %d}\n",
		    env->ifs_clm_tx_ratio, env->ifs_clm_edcca_excl_cca_ratio);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM FA ratio {CCK, OFDM} = {%d, %d}\n",
		    env->ifs_clm_cck_fa_ratio, env->ifs_clm_ofdm_fa_ratio);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM FA permil {CCK, OFDM} = {%d, %d}\n",
		    env->ifs_clm_cck_fa_permil, env->ifs_clm_ofdm_fa_permil);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM CCA_exclu_FA ratio {CCK, OFDM} = {%d, %d}\n",
		    env->ifs_clm_cck_cca_excl_fa_ratio,
		    env->ifs_clm_ofdm_cca_excl_fa_ratio);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "Time:[his, ifs_avg(us), cca_avg(us)]\n");
	for (i = 0; i < RTW89_IFS_CLM_NUM; i++)
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK, "T%d:[%d, %d, %d]\n",
			    i + 1, env->ifs_clm_his[i], env->ifs_clm_ifs_avg[i],
			    env->ifs_clm_cca_avg[i]);
}

static bool rtw89_phy_ifs_clm_get_result(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	u8 i = 0;

	if (rtw89_phy_read32_mask(rtwdev, R_IFSCNT, B_IFSCNT_DONE_MSK) == 0) {
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "Get IFS_CLM report Fail\n");
		return false;
	}

	env->ifs_clm_tx =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_TX_CNT,
				      B_IFS_CLM_TX_CNT_MSK);
	env->ifs_clm_edcca_excl_cca =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_TX_CNT,
				      B_IFS_CLM_EDCCA_EXCLUDE_CCA_FA_MSK);
	env->ifs_clm_cckcca_excl_fa =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_CCA,
				      B_IFS_CLM_CCKCCA_EXCLUDE_FA_MSK);
	env->ifs_clm_ofdmcca_excl_fa =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_CCA,
				      B_IFS_CLM_OFDMCCA_EXCLUDE_FA_MSK);
	env->ifs_clm_cckfa =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_FA,
				      B_IFS_CLM_CCK_FA_MSK);
	env->ifs_clm_ofdmfa =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CLM_FA,
				      B_IFS_CLM_OFDM_FA_MSK);

	env->ifs_clm_his[0] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_HIS, B_IFS_T1_HIS_MSK);
	env->ifs_clm_his[1] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_HIS, B_IFS_T2_HIS_MSK);
	env->ifs_clm_his[2] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_HIS, B_IFS_T3_HIS_MSK);
	env->ifs_clm_his[3] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_HIS, B_IFS_T4_HIS_MSK);

	env->ifs_clm_avg[0] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_AVG_L, B_IFS_T1_AVG_MSK);
	env->ifs_clm_avg[1] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_AVG_L, B_IFS_T2_AVG_MSK);
	env->ifs_clm_avg[2] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_AVG_H, B_IFS_T3_AVG_MSK);
	env->ifs_clm_avg[3] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_AVG_H, B_IFS_T4_AVG_MSK);

	env->ifs_clm_cca[0] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CCA_L, B_IFS_T1_CCA_MSK);
	env->ifs_clm_cca[1] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CCA_L, B_IFS_T2_CCA_MSK);
	env->ifs_clm_cca[2] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CCA_H, B_IFS_T3_CCA_MSK);
	env->ifs_clm_cca[3] =
		rtw89_phy_read32_mask(rtwdev, R_IFS_CCA_H, B_IFS_T4_CCA_MSK);

	env->ifs_clm_total_ifs =
		rtw89_phy_read32_mask(rtwdev, R_IFSCNT, B_IFSCNT_TOTAL_CNT_MSK);

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK, "IFS-CLM total_ifs = %d\n",
		    env->ifs_clm_total_ifs);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "{Tx, EDCCA_exclu_cca} = {%d, %d}\n",
		    env->ifs_clm_tx, env->ifs_clm_edcca_excl_cca);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM FA{CCK, OFDM} = {%d, %d}\n",
		    env->ifs_clm_cckfa, env->ifs_clm_ofdmfa);
	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "IFS-CLM CCA_exclu_FA{CCK, OFDM} = {%d, %d}\n",
		    env->ifs_clm_cckcca_excl_fa, env->ifs_clm_ofdmcca_excl_fa);

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK, "Time:[his, avg, cca]\n");
	for (i = 0; i < RTW89_IFS_CLM_NUM; i++)
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "T%d:[%d, %d, %d]\n", i + 1, env->ifs_clm_his[i],
			    env->ifs_clm_avg[i], env->ifs_clm_cca[i]);

	rtw89_phy_ifs_clm_get_utility(rtwdev);

	return true;
}

static int rtw89_phy_ifs_clm_set(struct rtw89_dev *rtwdev,
				 struct rtw89_ccx_para_info *para)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	u32 period = 0;
	u32 unit_idx = 0;

	if (para->mntr_time == 0) {
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "[WARN] MNTR_TIME is 0\n");
		return -EINVAL;
	}

	if (rtw89_phy_ccx_racing_ctrl(rtwdev, para->rac_lv))
		return -EINVAL;

	if (para->mntr_time != env->ifs_clm_mntr_time) {
		rtw89_phy_ccx_ms_to_period_unit(rtwdev, para->mntr_time,
						&period, &unit_idx);
		rtw89_phy_set_phy_regs(rtwdev, R_IFS_COUNTER,
				       B_IFS_CLM_PERIOD_MSK, period);
		rtw89_phy_set_phy_regs(rtwdev, R_IFS_COUNTER,
				       B_IFS_CLM_COUNTER_UNIT_MSK, unit_idx);

		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "Update IFS-CLM time ((%d)) -> ((%d))\n",
			    env->ifs_clm_mntr_time, para->mntr_time);

		env->ifs_clm_mntr_time = para->mntr_time;
		env->ccx_period = (u16)period;
		env->ccx_unit_idx = (u8)unit_idx;
	}

	if (rtw89_phy_ifs_clm_th_update_check(rtwdev, para)) {
		env->ifs_clm_app = para->ifs_clm_app;
		rtw89_phy_ifs_clm_set_th_reg(rtwdev);
	}

	return 0;
}

void rtw89_phy_env_monitor_track(struct rtw89_dev *rtwdev)
{
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	struct rtw89_ccx_para_info para = {0};
	u8 chk_result = RTW89_PHY_ENV_MON_CCX_FAIL;

	env->ccx_watchdog_result = RTW89_PHY_ENV_MON_CCX_FAIL;
	if (env->ccx_manual_ctrl) {
		rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
			    "CCX in manual ctrl\n");
		return;
	}

	/* only ifs_clm for now */
	if (rtw89_phy_ifs_clm_get_result(rtwdev))
		env->ccx_watchdog_result |= RTW89_PHY_ENV_MON_IFS_CLM;

	rtw89_phy_ccx_racing_release(rtwdev);
	para.mntr_time = 1900;
	para.rac_lv = RTW89_RAC_LV_1;
	para.ifs_clm_app = RTW89_IFS_CLM_BACKGROUND;

	if (rtw89_phy_ifs_clm_set(rtwdev, &para) == 0)
		chk_result |= RTW89_PHY_ENV_MON_IFS_CLM;
	if (chk_result)
		rtw89_phy_ccx_trigger(rtwdev);

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK,
		    "get_result=0x%x, chk_result:0x%x\n",
		    env->ccx_watchdog_result, chk_result);
}

static void rtw89_phy_dig_read_gain_table(struct rtw89_dev *rtwdev, int type)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;
	struct rtw89_dig_info *dig = &rtwdev->dig;
	const struct rtw89_phy_dig_gain_cfg *cfg;
	const char *msg;
	u8 i;
	s8 gain_base;
	s8 *gain_arr;
	u32 tmp;

	switch (type) {
	case RTW89_DIG_GAIN_LNA_G:
		gain_arr = dig->lna_gain_g;
		gain_base = LNA0_GAIN;
		cfg = chip->dig_table->cfg_lna_g;
		msg = "lna_gain_g";
		break;
	case RTW89_DIG_GAIN_TIA_G:
		gain_arr = dig->tia_gain_g;
		gain_base = TIA0_GAIN_G;
		cfg = chip->dig_table->cfg_tia_g;
		msg = "tia_gain_g";
		break;
	case RTW89_DIG_GAIN_LNA_A:
		gain_arr = dig->lna_gain_a;
		gain_base = LNA0_GAIN;
		cfg = chip->dig_table->cfg_lna_a;
		msg = "lna_gain_a";
		break;
	case RTW89_DIG_GAIN_TIA_A:
		gain_arr = dig->tia_gain_a;
		gain_base = TIA0_GAIN_A;
		cfg = chip->dig_table->cfg_tia_a;
		msg = "tia_gain_a";
		break;
	default:
		return;
	}

	for (i = 0; i < cfg->size; i++) {
		tmp = rtw89_phy_read32_mask(rtwdev, cfg->table[i].addr,
					    cfg->table[i].mask);
		tmp >>= DIG_GAIN_SHIFT;
		gain_arr[i] = sign_extend32(tmp, U4_MAX_BIT) + gain_base;
		gain_base += DIG_GAIN;

		rtw89_debug(rtwdev, RTW89_DBG_DIG, "%s[%d]=%d\n",
			    msg, i, gain_arr[i]);
	}
}

static void rtw89_phy_dig_update_gain_para(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	u32 tmp;
	u8 i;

	tmp = rtw89_phy_read32_mask(rtwdev, R_PATH0_IB_PKPW,
				    B_PATH0_IB_PKPW_MSK);
	dig->ib_pkpwr = sign_extend32(tmp >> DIG_GAIN_SHIFT, U8_MAX_BIT);
	dig->ib_pbk = rtw89_phy_read32_mask(rtwdev, R_PATH0_IB_PBK,
					    B_PATH0_IB_PBK_MSK);
	rtw89_debug(rtwdev, RTW89_DBG_DIG, "ib_pkpwr=%d, ib_pbk=%d\n",
		    dig->ib_pkpwr, dig->ib_pbk);

	for (i = RTW89_DIG_GAIN_LNA_G; i < RTW89_DIG_GAIN_MAX; i++)
		rtw89_phy_dig_read_gain_table(rtwdev, i);

	switch (rtwdev->hal.current_band_type) {
	case RTW89_BAND_2G:
		dig->lna_gain = dig->lna_gain_g;
		dig->tia_gain = dig->tia_gain_g;
		break;
	case RTW89_BAND_5G:
	default:
		dig->lna_gain = dig->lna_gain_a;
		dig->tia_gain = dig->tia_gain_a;
		break;
	}
}

static const u8 igi_rssi_th[5] = {68, 84, 90, 98, 104};
static const u8 fa_th_2g[4] = {22, 44, 66, 88};
static const u8 fa_th_5g[4] = {4, 8, 12, 16};
static const u8 pd_low_th_offset = 6, dynamic_igi_min = 0x20;
static const u8 igi_max_performance_mode = 0x5a;
static const u8 dynamic_pd_threshold_max;

static void rtw89_phy_dig_para_reset(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;

	dig->cur_noisy_lv = RTW89_DIG_NOISY_LEVEL0;
	dig->cur_gaincode.lna_idx = LNA_IDX_MAX;
	dig->cur_gaincode.tia_idx = TIA_IDX_MAX;
	dig->cur_gaincode.rxb_idx = RXB_IDX_MAX;
	dig->force_gaincode.lna_idx = LNA_IDX_MAX;
	dig->force_gaincode.tia_idx = TIA_IDX_MAX;
	dig->force_gaincode.rxb_idx = RXB_IDX_MAX;
	memcpy(dig->igi_rssi_th, igi_rssi_th, sizeof(dig->igi_rssi_th));
	switch (rtwdev->hal.current_band_type) {
	case RTW89_BAND_2G:
		memcpy(dig->fa_th, fa_th_2g, sizeof(dig->fa_th));
		break;
	case RTW89_BAND_5G:
	default:
		memcpy(dig->fa_th, fa_th_5g, sizeof(dig->fa_th));
		break;
	}
	dig->dyn_igi_max = igi_max_performance_mode;
	dig->dyn_igi_min = dynamic_igi_min;
	dig->dyn_pd_th_max = dynamic_pd_threshold_max;
	dig->pd_low_th_ofst = pd_low_th_offset;
}

static void rtw89_phy_dig_init(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;

	dig->reset = true;
	rtw89_phy_dig_update_gain_para(rtwdev);
	rtw89_phy_dig_para_reset(rtwdev);

	switch (rtwdev->hal.current_band_type) {
	case RTW89_BAND_2G:
		dig->force_gaincode_idx_en = false;
		dig->dyn_pd_th_en = true;
		break;
	case RTW89_BAND_5G:
	default:
		dig->force_gaincode_idx_en = true;
		dig->dyn_pd_th_en = true;
		break;
	}
}

static u8 rtw89_phy_dig_lna_idx_by_rssi(struct rtw89_dev *rtwdev, u8 rssi)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	u8 lna_idx;

	if (rssi < dig->igi_rssi_th[0])
		lna_idx = RTW89_DIG_GAIN_LNA_IDX6;
	else if (rssi < dig->igi_rssi_th[1])
		lna_idx = RTW89_DIG_GAIN_LNA_IDX5;
	else if (rssi < dig->igi_rssi_th[2])
		lna_idx = RTW89_DIG_GAIN_LNA_IDX4;
	else if (rssi < dig->igi_rssi_th[3])
		lna_idx = RTW89_DIG_GAIN_LNA_IDX3;
	else if (rssi < dig->igi_rssi_th[4])
		lna_idx = RTW89_DIG_GAIN_LNA_IDX2;
	else
		lna_idx = RTW89_DIG_GAIN_LNA_IDX1;

	return lna_idx;
}

static u8 rtw89_phy_dig_tia_idx_by_rssi(struct rtw89_dev *rtwdev, u8 rssi)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	u8 tia_idx;

	if (rssi < dig->igi_rssi_th[0])
		tia_idx = RTW89_DIG_GAIN_TIA_IDX1;
	else
		tia_idx = RTW89_DIG_GAIN_TIA_IDX0;

	return tia_idx;
}

#define IB_PBK_BASE 110
#define WB_RSSI_BASE 10
static u8 rtw89_phy_dig_rxb_idx_by_rssi(struct rtw89_dev *rtwdev, u8 rssi,
					struct rtw89_agc_gaincode_set *set)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	s8 lna_gain = dig->lna_gain[set->lna_idx];
	s8 tia_gain = dig->tia_gain[set->tia_idx];
	s32 wb_rssi = rssi + lna_gain + tia_gain;
	s32 rxb_idx_tmp = IB_PBK_BASE + WB_RSSI_BASE;
	u8 rxb_idx;

	rxb_idx_tmp += dig->ib_pkpwr - dig->ib_pbk - wb_rssi;
	rxb_idx = clamp_t(s32, rxb_idx_tmp, RXB_IDX_MIN, RXB_IDX_MAX);

	rtw89_debug(rtwdev, RTW89_DBG_DIG, "wb_rssi=%03d, rxb_idx_tmp=%03d\n",
		    wb_rssi, rxb_idx_tmp);

	return rxb_idx;
}

static void rtw89_phy_dig_gaincode_by_rssi(struct rtw89_dev *rtwdev, u8 rssi,
					   struct rtw89_agc_gaincode_set *set)
{
	set->lna_idx = rtw89_phy_dig_lna_idx_by_rssi(rtwdev, rssi);
	set->tia_idx = rtw89_phy_dig_tia_idx_by_rssi(rtwdev, rssi);
	set->rxb_idx = rtw89_phy_dig_rxb_idx_by_rssi(rtwdev, rssi, set);

	rtw89_debug(rtwdev, RTW89_DBG_DIG,
		    "final_rssi=%03d, (lna,tia,rab)=(%d,%d,%02d)\n",
		    rssi, set->lna_idx, set->tia_idx, set->rxb_idx);
}

#define IGI_OFFSET_MAX 25
#define IGI_OFFSET_MUL 2
static void rtw89_phy_dig_igi_offset_by_env(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	enum rtw89_dig_noisy_level noisy_level = dig->cur_noisy_lv;
	u8 igi_offset = dig->fa_rssi_ofst;

	if (igi_offset < 2)
		igi_offset = 0;
	else
		igi_offset += noisy_level * IGI_OFFSET_MUL;

	igi_offset = min_t(u8, igi_offset, IGI_OFFSET_MAX);
	dig->fa_rssi_ofst = igi_offset;

	rtw89_debug(rtwdev, RTW89_DBG_PHY_TRACK, "noisy_lv=%d, igi_ofst=%d\n",
		    noisy_level, igi_offset);
}

static void rtw89_phy_dig_noisy_level_decision(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	struct rtw89_env_monitor_info *env = &rtwdev->env_monitor;
	enum rtw89_dig_noisy_level noisy_lv;
	u16 fa_ratio = 0;

	fa_ratio = env->ifs_clm_cck_fa_permil + env->ifs_clm_ofdm_fa_permil;

	if (fa_ratio < dig->fa_th[0])
		noisy_lv = RTW89_DIG_NOISY_LEVEL0;
	else if (fa_ratio < dig->fa_th[1])
		noisy_lv = RTW89_DIG_NOISY_LEVEL1;
	else if (fa_ratio < dig->fa_th[2])
		noisy_lv = RTW89_DIG_NOISY_LEVEL2;
	else if (fa_ratio < dig->fa_th[3])
		noisy_lv = RTW89_DIG_NOISY_LEVEL3;
	else
		noisy_lv = RTW89_DIG_NOISY_LEVEL_MAX;

	dig->cur_noisy_lv = noisy_lv;

	rtw89_debug(rtwdev, RTW89_DBG_DIG,
		    "fa_ratio(CCK,OFDM,ALL)=(%d,%d,%d)%%, noisy_lv=%d\n",
		    env->ifs_clm_cck_fa_permil, env->ifs_clm_ofdm_fa_permil,
		    env->ifs_clm_cck_fa_permil + env->ifs_clm_ofdm_fa_permil,
		    noisy_lv);
}

static void rtw89_phy_dig_set_lna_idx(struct rtw89_dev *rtwdev, u8 lna_idx)
{
	rtw89_phy_write32_mask(rtwdev, R_PATH0_LNA_INIT,
			       B_PATH0_LNA_INIT_IDX_MSK, lna_idx);
	rtw89_phy_write32_mask(rtwdev, R_PATH1_LNA_INIT,
			       B_PATH1_LNA_INIT_IDX_MSK, lna_idx);
}

static void rtw89_phy_dig_set_tia_idx(struct rtw89_dev *rtwdev, u8 tia_idx)
{
	rtw89_phy_write32_mask(rtwdev, R_PATH0_TIA_INIT,
			       B_PATH0_TIA_INIT_IDX_MSK, tia_idx);
	rtw89_phy_write32_mask(rtwdev, R_PATH1_TIA_INIT,
			       B_PATH1_TIA_INIT_IDX_MSK, tia_idx);
}

static void rtw89_phy_dig_set_rxb_idx(struct rtw89_dev *rtwdev, u8 rxb_idx)
{
	rtw89_phy_write32_mask(rtwdev, R_PATH0_RXB_INIT,
			       B_PATH0_RXB_INIT_IDX_MSK, rxb_idx);
	rtw89_phy_write32_mask(rtwdev, R_PATH1_RXB_INIT,
			       B_PATH1_RXB_INIT_IDX_MSK, rxb_idx);
}

static void rtw89_phy_dig_set_igi_cr(struct rtw89_dev *rtwdev,
				     const struct rtw89_agc_gaincode_set set)
{
	rtw89_phy_dig_set_lna_idx(rtwdev, set.lna_idx);
	rtw89_phy_dig_set_tia_idx(rtwdev, set.tia_idx);
	rtw89_phy_dig_set_rxb_idx(rtwdev, set.rxb_idx);

	rtw89_debug(rtwdev, RTW89_DBG_DIG, "Set (lna,tia,rxb)=((%d,%d,%02d))\n",
		    set.lna_idx, set.tia_idx, set.rxb_idx);
}

static const struct rtw89_reg_def sdagc_config[4] = {
	{R_PATH0_P20_FOLLOW_BY_PAGCUGC, B_PATH0_P20_FOLLOW_BY_PAGCUGC_EN_MSK},
	{R_PATH0_S20_FOLLOW_BY_PAGCUGC, B_PATH0_S20_FOLLOW_BY_PAGCUGC_EN_MSK},
	{R_PATH1_P20_FOLLOW_BY_PAGCUGC, B_PATH1_P20_FOLLOW_BY_PAGCUGC_EN_MSK},
	{R_PATH1_S20_FOLLOW_BY_PAGCUGC, B_PATH1_S20_FOLLOW_BY_PAGCUGC_EN_MSK},
};

static void rtw89_phy_dig_sdagc_follow_pagc_config(struct rtw89_dev *rtwdev,
						   bool enable)
{
	u8 i = 0;

	for (i = 0; i < ARRAY_SIZE(sdagc_config); i++)
		rtw89_phy_write32_mask(rtwdev, sdagc_config[i].addr,
				       sdagc_config[i].mask, enable);

	rtw89_debug(rtwdev, RTW89_DBG_DIG, "sdagc_follow_pagc=%d\n", enable);
}

static void rtw89_phy_dig_dyn_pd_th(struct rtw89_dev *rtwdev, u8 rssi,
				    bool enable)
{
	enum rtw89_bandwidth cbw = rtwdev->hal.current_band_width;
	struct rtw89_dig_info *dig = &rtwdev->dig;
	u8 final_rssi = 0, under_region = dig->pd_low_th_ofst;
	u32 val = 0;

	under_region += PD_TH_SB_FLTR_CMP_VAL;

	switch (cbw) {
	case RTW89_CHANNEL_WIDTH_40:
		under_region += PD_TH_BW40_CMP_VAL;
		break;
	case RTW89_CHANNEL_WIDTH_80:
		under_region += PD_TH_BW80_CMP_VAL;
		break;
	case RTW89_CHANNEL_WIDTH_20:
		fallthrough;
	default:
		under_region += PD_TH_BW20_CMP_VAL;
		break;
	}

	dig->dyn_pd_th_max = dig->igi_rssi;

	final_rssi = min_t(u8, rssi, dig->igi_rssi);
	final_rssi = clamp_t(u8, final_rssi, PD_TH_MIN_RSSI + under_region,
			     PD_TH_MAX_RSSI + under_region);

	if (enable) {
		val = (final_rssi - under_region - PD_TH_MIN_RSSI) >> 1;
		rtw89_debug(rtwdev, RTW89_DBG_DIG,
			    "dyn_max=%d, final_rssi=%d, total=%d, PD_low=%d\n",
			    dig->igi_rssi, final_rssi, under_region, val);
	} else {
		rtw89_debug(rtwdev, RTW89_DBG_DIG,
			    "Dynamic PD th dsiabled, Set PD_low_bd=0\n");
	}

	rtw89_phy_write32_mask(rtwdev, R_SEG0R_PD, B_SEG0R_PD_LOWER_BOUND_MSK,
			       val);
	rtw89_phy_write32_mask(rtwdev, R_SEG0R_PD,
			       B_SEG0R_PD_SPATIAL_REUSE_EN_MSK, enable);
}

void rtw89_phy_dig_reset(struct rtw89_dev *rtwdev)
{
	struct rtw89_dig_info *dig = &rtwdev->dig;
	struct rtw89_phy_ch_info *ch_info = &rtwdev->ch_info;
	u8 rssi_min = ch_info->rssi_min >> 1;

	rtw89_phy_dig_para_reset(rtwdev);
	rtw89_phy_dig_set_igi_cr(rtwdev, dig->force_gaincode);
	rtw89_phy_dig_dyn_pd_th(rtwdev, rssi_min, false);
	rtw89_phy_dig_sdagc_follow_pagc_config(rtwdev, false);
}

#define IGI_RSSI_MIN 10
void rtw89_phy_dig(struct rtw89_dev *rtwdev)
{
	struct rtw89_phy_ch_info *ch_info = &rtwdev->ch_info;
	struct rtw89_dig_info *dig = &rtwdev->dig;
	u8 total_sta = rtwdev->total_sta_assoc;
	u8 rssi_min = ch_info->rssi_min >> 1;

	if (total_sta == 0) {
		if (!dig->reset)
			return;
		rtw89_phy_dig_reset(rtwdev);
		dig->reset = false;
		rtw89_debug(rtwdev, RTW89_DBG_DIG,
			    "is_linked=%d, one_entry_only=%d\n",
			    total_sta == 0, total_sta == 1);
		return;
	}

	dig->reset = true;
	rtw89_phy_dig_noisy_level_decision(rtwdev);
	rtw89_phy_dig_igi_offset_by_env(rtwdev);

	if (rssi_min > 0)
		dig->igi_rssi = rssi_min;

	dig->dyn_igi_min = (dig->igi_rssi > IGI_RSSI_MIN) ?
			    dig->igi_rssi - IGI_RSSI_MIN : 0;
	dig->dyn_igi_max = dig->dyn_igi_min + IGI_OFFSET_MAX;
	dig->igi_fa_rssi = dig->dyn_igi_min + dig->fa_rssi_ofst;

	dig->igi_fa_rssi = clamp(dig->igi_fa_rssi, dig->dyn_igi_min,
				 dig->dyn_igi_max);

	rtw89_debug(rtwdev, RTW89_DBG_DIG,
		    "rssi=%03d, dyn(max,min)=(%d,%d), final_rssi=%d.\n",
		    dig->igi_rssi, dig->dyn_igi_max, dig->dyn_igi_min,
		    dig->igi_fa_rssi);

	if (dig->force_gaincode_idx_en) {
		rtw89_phy_dig_set_igi_cr(rtwdev, dig->force_gaincode);
		rtw89_debug(rtwdev, RTW89_DBG_DIG,
			    "Force gaincode index enabled.\n");
	} else {
		rtw89_phy_dig_gaincode_by_rssi(rtwdev, dig->igi_fa_rssi,
					       &dig->cur_gaincode);
		rtw89_phy_dig_set_igi_cr(rtwdev, dig->cur_gaincode);
	}

	rtw89_phy_dig_dyn_pd_th(rtwdev, dig->igi_fa_rssi, dig->dyn_pd_th_en);

	if (dig->dyn_pd_th_en && dig->igi_fa_rssi > dig->dyn_pd_th_max)
		rtw89_phy_dig_sdagc_follow_pagc_config(rtwdev, true);
	else
		rtw89_phy_dig_sdagc_follow_pagc_config(rtwdev, false);
}

static void rtw89_phy_env_monitor_init(struct rtw89_dev *rtwdev)
{
	rtw89_phy_ccx_top_setting_init(rtwdev);
	rtw89_phy_ifs_clm_setting_init(rtwdev);
}

void rtw89_phy_dm_init(struct rtw89_dev *rtwdev)
{
	const struct rtw89_chip_info *chip = rtwdev->chip;

	rtw89_phy_stat_init(rtwdev);

	rtw89_chip_bb_sethw(rtwdev);

	rtw89_phy_env_monitor_init(rtwdev);
	rtw89_phy_dig_init(rtwdev);
	rtw89_phy_cfo_init(rtwdev);

	rtw89_phy_init_rf_nctl(rtwdev);
	rtw89_chip_rfk_init(rtwdev);
	rtw89_load_txpwr_table(rtwdev, chip->byr_table);
	rtw89_chip_set_txpwr_ctrl(rtwdev);
	rtw89_chip_power_trim(rtwdev);
}

void rtw89_phy_set_bss_color(struct rtw89_dev *rtwdev, struct ieee80211_vif *vif)
{
	enum rtw89_phy_idx phy_idx = RTW89_PHY_0;
	u8 bss_color;

	if (!vif->bss_conf.he_support || !vif->bss_conf.assoc)
		return;

	bss_color = vif->bss_conf.he_bss_color.color;

	rtw89_phy_write32_idx(rtwdev, R_BSS_CLR_MAP, B_BSS_CLR_MAP_VLD0, 0x1,
			      phy_idx);
	rtw89_phy_write32_idx(rtwdev, R_BSS_CLR_MAP, B_BSS_CLR_MAP_TGT, bss_color,
			      phy_idx);
	rtw89_phy_write32_idx(rtwdev, R_BSS_CLR_MAP, B_BSS_CLR_MAP_STAID,
			      vif->bss_conf.aid, phy_idx);
}
