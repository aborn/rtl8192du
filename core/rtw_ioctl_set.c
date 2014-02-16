/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/
#define _RTW_IOCTL_SET_C_


#include <drv_conf.h>
#include <osdep_service.h>
#include <drv_types.h>
#include <rtw_ioctl_set.h>
#include <hal_intf.h>
#include <usb_osintf.h>
#include <usb_ops.h>
#include <mlme_osdep.h>

#define IS_MAC_ADDRESS_BROADCAST(addr) \
(\
	((addr[0] == 0xff) && (addr[1] == 0xff) && \
	 (addr[2] == 0xff) && (addr[3] == 0xff) && \
	 (addr[4] == 0xff) && (addr[5] == 0xff))  ? true : false \
)

u8 rtw_validate_ssid(struct ndis_802_11_ssid *ssid)
{
	u8	 i;
	u8	ret = true;

_func_enter_;

	if (ssid->SsidLength > 32) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("ssid length >32\n"));
		ret = false;
		goto exit;
	}

	for (i = 0; i < ssid->SsidLength; i++) {
		/* wifi, printable ascii code must be supported */
		if (!((ssid->Ssid[i] >= 0x20) && (ssid->Ssid[i] <= 0x7e))) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("ssid has nonprintabl ascii\n"));
			ret = false;
			break;
		}
	}

exit:

_func_exit_;

	return ret;
}

u8 rtw_do_join(struct rtw_adapter *padapter)
{
	struct list_head *plist, *phead;
	u8 *pibss = NULL;
	struct	mlme_priv	*pmlmepriv = &(padapter->mlmepriv);
	struct __queue *queue = &(pmlmepriv->scanned_queue);
	u8 ret = _SUCCESS;

_func_enter_;

	spin_lock_bh(&(pmlmepriv->scanned_queue.lock));
	phead = get_list_head(queue);
	plist = get_next(phead);

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("\n rtw_do_join: phead = %p; plist = %p\n\n\n", phead, plist));

	pmlmepriv->cur_network.join_res = -2;

	set_fwstate(pmlmepriv, _FW_UNDER_LINKING);

	pmlmepriv->pscanned = plist;

	pmlmepriv->to_join = true;

	if (_rtw_queue_empty(queue) == true) {
		spin_unlock_bh(&(pmlmepriv->scanned_queue.lock));
		_clr_fwstate_(pmlmepriv, _FW_UNDER_LINKING);

		/* when set_ssid/set_bssid for rtw_do_join(), but scanning queue is empty */
		/* we try to issue sitesurvey firstly */

		if (pmlmepriv->LinkDetectInfo.bBusyTraffic == false || rtw_to_roaming(padapter) > 0) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_,
				 ("rtw_do_join(): site survey if scanned_queue is empty\n."));
			/*  submit site_survey_cmd */
			ret = rtw_sitesurvey_cmd(padapter, &pmlmepriv->assoc_ssid, 1, NULL, 0);
			if (_SUCCESS != ret) {
				RT_TRACE(_module_rtl871x_ioctl_set_c_,
					 _drv_err_,
					 ("rtw_do_join(): site survey return error\n."));
			}
		}

		goto exit;
	} else {
		int select_ret;
		spin_unlock_bh(&(pmlmepriv->scanned_queue.lock));
		select_ret = rtw_select_and_join_from_scanned_queue(pmlmepriv);
		if (select_ret == _SUCCESS) {
			pmlmepriv->to_join = false;
			_set_timer(&pmlmepriv->assoc_timer, MAX_JOIN_TIMEOUT);
		} else if (ret == 2) { /* there is no need to wait for join */
			ret = _SUCCESS;
			_clr_fwstate_(pmlmepriv, _FW_UNDER_LINKING);
			rtw_indicate_connect(padapter);
		} else {
			if (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) == true) {
				/*  submit createbss_cmd to change to a ADHOC_MASTER */

				/* pmlmepriv->lock has been acquired by caller... */
				struct wlan_bssid_ex    *pdev_network = &(padapter->registrypriv.dev_network);

				pmlmepriv->fw_state = WIFI_ADHOC_MASTER_STATE;

				pibss = padapter->registrypriv.dev_network.MacAddress;

				memset(&pdev_network->Ssid, 0, sizeof(struct ndis_802_11_ssid));
				memcpy(&pdev_network->Ssid, &pmlmepriv->assoc_ssid, sizeof(struct ndis_802_11_ssid));

				rtw_update_registrypriv_dev_network(padapter);

				rtw_generate_random_ibss(pibss);

				if (rtw_createbss_cmd(padapter) != _SUCCESS) {
					RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("***Error=>do_goin: rtw_createbss_cmd status FAIL***\n "));
					ret =  false;
					goto exit;
				}

				pmlmepriv->to_join = false;

				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("***Error=> rtw_select_and_join_from_scanned_queue FAIL under STA_Mode***\n "));

			} else {
				/*  can't associate; reset under-linking */
				_clr_fwstate_(pmlmepriv, _FW_UNDER_LINKING);

				/* when set_ssid/set_bssid for rtw_do_join(), but there are no desired bss in scanning queue */
				/* we try to issue sitesurvey firstly */
				if (pmlmepriv->LinkDetectInfo.bBusyTraffic == false ||
				    rtw_to_roaming(padapter) > 0) {
					ret = rtw_sitesurvey_cmd(padapter, &pmlmepriv->assoc_ssid, 1, NULL, 0);
					if (_SUCCESS != ret)
						RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
							 ("do_join(): site survey return error\n."));
				}
			}
		}
	}

exit:

_func_exit_;

	return ret;
}

u8 rtw_set_802_11_bssid(struct rtw_adapter *padapter, u8 *bssid)
{
	u8 status = _SUCCESS;
	u32 cur_time = 0;

	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;

_func_enter_;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_notice_,
		 ("+rtw_set_802_11_bssid: bssid =%pM\n", bssid));

	if ((bssid[0] == 0x00 && bssid[1] == 0x00 && bssid[2] == 0x00 && bssid[3] == 0x00 && bssid[4] == 0x00 && bssid[5] == 0x00) ||
	    (bssid[0] == 0xFF && bssid[1] == 0xFF && bssid[2] == 0xFF && bssid[3] == 0xFF && bssid[4] == 0xFF && bssid[5] == 0xFF)) {
		status = _FAIL;
		goto exit;
	}

	spin_lock_bh(&pmlmepriv->lock);

	DBG_8192D("Set BSSID under fw_state= 0x%08x\n", get_fwstate(pmlmepriv));
	if (check_fwstate(pmlmepriv, _FW_UNDER_SURVEY) == true)
		goto handle_tkip_countermeasure;
	else if (check_fwstate(pmlmepriv, _FW_UNDER_LINKING) == true)
		goto release_mlme_lock;

	if (check_fwstate(pmlmepriv, _FW_LINKED|WIFI_ADHOC_MASTER_STATE) == true) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("set_bssid: _FW_LINKED||WIFI_ADHOC_MASTER_STATE\n"));

		if (_rtw_memcmp(&pmlmepriv->cur_network.network.MacAddress, bssid, ETH_ALEN) == true) {
			if (check_fwstate(pmlmepriv, WIFI_STATION_STATE) == false)
				goto release_mlme_lock;/* it means driver is in WIFI_ADHOC_MASTER_STATE, we needn't create bss again. */
		} else {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("Set BSSID not the same bssid\n"));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("set_bssid =%pM\n", bssid));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("cur_bssid =%pM\n", pmlmepriv->cur_network.network.MacAddress));

			rtw_disassoc_cmd(padapter, 0, true);

			if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
				rtw_indicate_disconnect(padapter);

			rtw_free_assoc_resources(padapter, 1);

			if ((check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true)) {
				_clr_fwstate_(pmlmepriv, WIFI_ADHOC_MASTER_STATE);
				set_fwstate(pmlmepriv, WIFI_ADHOC_STATE);
			}
		}
	}

handle_tkip_countermeasure:
	/* should we add something here...? */

	if (padapter->securitypriv.btkip_countermeasure == true) {
		cur_time = rtw_get_current_time();

		if ((cur_time - padapter->securitypriv.btkip_countermeasure_time) > 60 * HZ) {
			padapter->securitypriv.btkip_countermeasure = false;
			padapter->securitypriv.btkip_countermeasure_time = 0;
		} else {
			status = _FAIL;
			goto release_mlme_lock;
		}
	}

	memcpy(&pmlmepriv->assoc_bssid, bssid, ETH_ALEN);
	pmlmepriv->assoc_by_bssid = true;

	if (check_fwstate(pmlmepriv, _FW_UNDER_SURVEY) == true)
		pmlmepriv->to_join = true;
	else
		status = rtw_do_join(padapter);

release_mlme_lock:
	spin_unlock_bh(&pmlmepriv->lock);

exit:
	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
		 ("rtw_set_802_11_bssid: status=%d\n", status));

_func_exit_;

	return status;
}

u8 rtw_set_802_11_ssid(struct rtw_adapter *padapter, struct ndis_802_11_ssid *ssid)
{
	u8 status = _SUCCESS;
	u32 cur_time = 0;

	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;
	struct wlan_network *pnetwork = &pmlmepriv->cur_network;

_func_enter_;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_notice_,
		 ("+rtw_set_802_11_ssid: ssid =[%s] fw_state= 0x%08x\n",
		  ssid->Ssid, get_fwstate(pmlmepriv)));

	if (padapter->hw_init_completed == false) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
			 ("set_ssid: hw_init_completed== false=>exit!!!\n"));
		status = _FAIL;
		goto exit;
	}

	spin_lock_bh(&pmlmepriv->lock);

	DBG_8192D("Set SSID under fw_state= 0x%08x\n", get_fwstate(pmlmepriv));
	if (check_fwstate(pmlmepriv, _FW_UNDER_SURVEY) == true)
		goto handle_tkip_countermeasure;
	else if (check_fwstate(pmlmepriv, _FW_UNDER_LINKING) == true)
		goto release_mlme_lock;

	if (check_fwstate(pmlmepriv, _FW_LINKED|WIFI_ADHOC_MASTER_STATE) == true) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_,
			 ("set_ssid: _FW_LINKED||WIFI_ADHOC_MASTER_STATE\n"));

		if ((pmlmepriv->assoc_ssid.SsidLength == ssid->SsidLength) &&
		    (_rtw_memcmp(&pmlmepriv->assoc_ssid.Ssid, ssid->Ssid, ssid->SsidLength) == true)) {
			if ((check_fwstate(pmlmepriv, WIFI_STATION_STATE) == false)) {
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("Set SSID is the same ssid, fw_state= 0x%08x\n",
					  get_fwstate(pmlmepriv)));

				if (rtw_is_same_ibss(padapter, pnetwork) == false) {
					/* if in WIFI_ADHOC_MASTER_STATE | WIFI_ADHOC_STATE, create bss or rejoin again */
					rtw_disassoc_cmd(padapter, 0, true);

					if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
						rtw_indicate_disconnect(padapter);

					rtw_free_assoc_resources(padapter, 1);

					if (check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true) {
						_clr_fwstate_(pmlmepriv, WIFI_ADHOC_MASTER_STATE);
						set_fwstate(pmlmepriv, WIFI_ADHOC_STATE);
					}
				} else {
					goto release_mlme_lock;/* it means driver is in WIFI_ADHOC_MASTER_STATE, we needn't create bss again. */
				}
			}
#ifdef CONFIG_LPS
			else {
				rtw_lps_ctrl_wk_cmd(padapter, LPS_CTRL_JOINBSS, 1);
			}
#endif
		} else {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("Set SSID not the same ssid\n"));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("set_ssid =[%s] len= 0x%x\n", ssid->Ssid, (unsigned int)ssid->SsidLength));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("assoc_ssid =[%s] len= 0x%x\n", pmlmepriv->assoc_ssid.Ssid, (unsigned int)pmlmepriv->assoc_ssid.SsidLength));

			rtw_disassoc_cmd(padapter, 0, true);

			if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
				rtw_indicate_disconnect(padapter);

			rtw_free_assoc_resources(padapter, 1);

			if (check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true) {
				_clr_fwstate_(pmlmepriv, WIFI_ADHOC_MASTER_STATE);
				set_fwstate(pmlmepriv, WIFI_ADHOC_STATE);
			}
		}
	}

handle_tkip_countermeasure:

	if (padapter->securitypriv.btkip_countermeasure == true) {
		cur_time = rtw_get_current_time();

		if ((cur_time - padapter->securitypriv.btkip_countermeasure_time) > 60 * HZ) {
			padapter->securitypriv.btkip_countermeasure = false;
			padapter->securitypriv.btkip_countermeasure_time = 0;
		} else {
			status = _FAIL;
			goto release_mlme_lock;
		}
	}

	#ifdef CONFIG_VALIDATE_SSID
	if (rtw_validate_ssid(ssid) == false) {
		status = _FAIL;
		goto release_mlme_lock;
	}
	#endif

	memcpy(&pmlmepriv->assoc_ssid, ssid, sizeof(struct ndis_802_11_ssid));
	pmlmepriv->assoc_by_bssid = false;

	if (check_fwstate(pmlmepriv, _FW_UNDER_SURVEY) == true)
		pmlmepriv->to_join = true;
	else
		status = rtw_do_join(padapter);

release_mlme_lock:
	spin_unlock_bh(&pmlmepriv->lock);

exit:
	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
		 ("-rtw_set_802_11_ssid: status=%d\n", status));

_func_exit_;

	return status;
}

u8 rtw_set_802_11_infrastructure_mode(struct rtw_adapter *padapter,
	enum NDIS_802_11_NETWORK_INFRASTRUCTURE networktype)
{
	struct	mlme_priv	*pmlmepriv = &padapter->mlmepriv;
	struct	wlan_network	*cur_network = &pmlmepriv->cur_network;
	enum NDIS_802_11_NETWORK_INFRASTRUCTURE *pold_state = &(cur_network->network.InfrastructureMode);

_func_enter_;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_notice_,
		 ("+rtw_set_802_11_infrastructure_mode: old=%d new=%d fw_state= 0x%08x\n",
		  *pold_state, networktype, get_fwstate(pmlmepriv)));

	if (*pold_state != networktype) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, (" change mode!"));

		if (*pold_state == NDIS802_11APMODE) {
			/* change to other mode from Ndis802_11APMode */
			cur_network->join_res = -1;

#ifdef CONFIG_NATIVEAP_MLME
			stop_ap_mode(padapter);
#endif
		}

		if ((check_fwstate(pmlmepriv, _FW_LINKED) == true) || (*pold_state == NDIS802_11IBSS))
			rtw_disassoc_cmd(padapter, 0, true);

		if ((check_fwstate(pmlmepriv, _FW_LINKED) == true) ||
		    (check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true))
			rtw_free_assoc_resources(padapter, 0);

		if ((*pold_state == NDIS802_11INFRA) || (*pold_state == NDIS802_11IBSS)) {
			if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
				rtw_indicate_disconnect(padapter); /* will clr Linked_state; before this function, we must have chked whether  issue dis-assoc_cmd or not */
	       }

		*pold_state = networktype;

		_clr_fwstate_(pmlmepriv, ~WIFI_NULL_STATE);

		switch (networktype) {
		case NDIS802_11IBSS:
			set_fwstate(pmlmepriv, WIFI_ADHOC_STATE);
			break;
		case NDIS802_11INFRA:
			set_fwstate(pmlmepriv, WIFI_STATION_STATE);
			break;
		case NDIS802_11APMODE:
			set_fwstate(pmlmepriv, WIFI_AP_STATE);
#ifdef CONFIG_NATIVEAP_MLME
			start_ap_mode(padapter);
#endif
			break;
		case NDIS802_11AUTOUNK:
		case NDIS802_11INFRA_MAX:
			break;
		}
	}

_func_exit_;

	return true;
}


u8 rtw_set_802_11_disassociate(struct rtw_adapter *padapter)
{
	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;

_func_enter_;

	spin_lock_bh(&pmlmepriv->lock);

	if (check_fwstate(pmlmepriv, _FW_LINKED) == true) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("MgntActrtw_set_802_11_disassociate: rtw_indicate_disconnect\n"));

		rtw_disassoc_cmd(padapter, 0, true);
		rtw_indicate_disconnect(padapter);
		rtw_free_assoc_resources(padapter, 1);
	}

	spin_unlock_bh(&pmlmepriv->lock);

_func_exit_;

	return true;
}

u8 rtw_set_802_11_bssid_list_scan(struct rtw_adapter *padapter, struct ndis_802_11_ssid *pssid, int ssid_max_num)
{
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;
	u8	res = true;

_func_enter_;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("+rtw_set_802_11_bssid_list_scan(), fw_state=%x\n", get_fwstate(pmlmepriv)));

	if (padapter == NULL) {
		res = false;
		goto exit;
	}
	if (padapter->hw_init_completed == false) {
		res = false;
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n=== rtw_set_802_11_bssid_list_scan:hw_init_completed== false===\n"));
		goto exit;
	}

	if ((check_fwstate(pmlmepriv, _FW_UNDER_SURVEY|_FW_UNDER_LINKING) == true) ||
	    (pmlmepriv->LinkDetectInfo.bBusyTraffic == true)) {
		/*  Scan or linking is in progress, do nothing. */
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("rtw_set_802_11_bssid_list_scan fail since fw_state = %x\n", get_fwstate(pmlmepriv)));
		res = true;

		if (check_fwstate(pmlmepriv, (_FW_UNDER_SURVEY|_FW_UNDER_LINKING)) == true)
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n###_FW_UNDER_SURVEY|_FW_UNDER_LINKING\n\n"));
		else
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n###pmlmepriv->sitesurveyctrl.traffic_busy== true\n\n"));
	} else {
		if (rtw_is_scan_deny(padapter)) {
			DBG_8192D(FUNC_ADPT_FMT": scan deny\n", FUNC_ADPT_ARG(padapter));
			indicate_wx_scan_complete_event(padapter);
			return _SUCCESS;
		}

		spin_lock_bh(&pmlmepriv->lock);

		res = rtw_sitesurvey_cmd(padapter, pssid, ssid_max_num, NULL, 0);

		spin_unlock_bh(&pmlmepriv->lock);
	}
exit:

_func_exit_;

	return res;
}

u8 rtw_set_802_11_authentication_mode(struct rtw_adapter *padapter, enum NDIS_802_11_AUTHENTICATION_MODE authmode)
{
	struct security_priv *psecuritypriv = &padapter->securitypriv;
	int res;
	u8 ret;

_func_enter_;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("set_802_11_auth.mode(): mode=%x\n", authmode));

	psecuritypriv->ndisauthtype = authmode;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_,
		 ("rtw_set_802_11_authentication_mode:psecuritypriv->ndisauthtype =%d",
		 psecuritypriv->ndisauthtype));

	if (psecuritypriv->ndisauthtype > 3)
		psecuritypriv->dot11AuthAlgrthm = dot11AuthAlgrthm_8021X;

	res = rtw_set_auth(padapter, psecuritypriv);

	if (res == _SUCCESS)
		ret = true;
	else
		ret = false;

_func_exit_;

	return ret;
}

u8 rtw_set_802_11_add_wep(struct rtw_adapter *padapter, struct ndis_802_11_wep *wep)
{
	int		keyid, res;
	struct security_priv *psecuritypriv = &(padapter->securitypriv);
	u8		ret = _SUCCESS;

_func_enter_;

	keyid = wep->KeyIndex & 0x3fffffff;

	if (keyid >= 4) {
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("MgntActrtw_set_802_11_add_wep:keyid>4=>fail\n"));
		ret = false;
		goto exit;
	}

	switch (wep->KeyLength) {
	case 5:
		psecuritypriv->dot11PrivacyAlgrthm = _WEP40_;
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("MgntActrtw_set_802_11_add_wep:wep->KeyLength= 5\n"));
		break;
	case 13:
		psecuritypriv->dot11PrivacyAlgrthm = _WEP104_;
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("MgntActrtw_set_802_11_add_wep:wep->KeyLength= 13\n"));
		break;
	default:
		psecuritypriv->dot11PrivacyAlgrthm = _NO_PRIVACY_;
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("MgntActrtw_set_802_11_add_wep:wep->KeyLength!= 5 or 13\n"));
		break;
	}

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("rtw_set_802_11_add_wep:befor memcpy, wep->KeyLength= 0x%x wep->KeyIndex = 0x%x  keyid =%x\n", wep->KeyLength, wep->KeyIndex, keyid));

	memcpy(&(psecuritypriv->dot11DefKey[keyid].skey[0]), &(wep->KeyMaterial), wep->KeyLength);

	psecuritypriv->dot11DefKeylen[keyid] = wep->KeyLength;

	psecuritypriv->dot11PrivacyKeyIndex = keyid;

	RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_,
		 ("rtw_set_802_11_add_wep:security key material : %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
		 psecuritypriv->dot11DefKey[keyid].skey[0], psecuritypriv->dot11DefKey[keyid].skey[1], psecuritypriv->dot11DefKey[keyid].skey[2],
		 psecuritypriv->dot11DefKey[keyid].skey[3], psecuritypriv->dot11DefKey[keyid].skey[4], psecuritypriv->dot11DefKey[keyid].skey[5],
		 psecuritypriv->dot11DefKey[keyid].skey[6], psecuritypriv->dot11DefKey[keyid].skey[7], psecuritypriv->dot11DefKey[keyid].skey[8],
		 psecuritypriv->dot11DefKey[keyid].skey[9], psecuritypriv->dot11DefKey[keyid].skey[10], psecuritypriv->dot11DefKey[keyid].skey[11],
		 psecuritypriv->dot11DefKey[keyid].skey[12]));

	res = rtw_set_key(padapter, psecuritypriv, keyid, 1);

	if (res == _FAIL)
		ret = false;
exit:

_func_exit_;

	return ret;
}

u8 rtw_set_802_11_remove_wep(struct rtw_adapter *padapter, u32 keyindex)
{
	u8 ret = _SUCCESS;

_func_enter_;

	if (keyindex >= 0x80000000 || padapter == NULL) {
		ret = false;
		goto exit;
	} else {
		int res;
		struct security_priv *psecuritypriv = &(padapter->securitypriv);
		if (keyindex < 4) {
			memset(&psecuritypriv->dot11DefKey[keyindex], 0, 16);

			res = rtw_set_key(padapter, psecuritypriv, keyindex, 0);

			psecuritypriv->dot11DefKeylen[keyindex] = 0;

			if (res == _FAIL)
				ret = _FAIL;
		} else {
			ret = _FAIL;
		}
	}

exit:

_func_exit_;

	return ret;
}

u8 rtw_set_802_11_add_key(struct rtw_adapter *padapter, struct ndis_802_11_key *key)
{
	uint	encryptionalgo;
	u8 *pbssid;
	struct sta_info *stainfo;
	u8	bgroup = false;
	u8	bgrouptkey = false;/* can be remove later */
	u8	ret = _SUCCESS;

_func_enter_;

	if (((key->KeyIndex & 0x80000000) == 0) && ((key->KeyIndex & 0x40000000) > 0)) {
		/*  It is invalid to clear bit 31 and set bit 30. If the miniport driver encounters this combination, */
		/*  it must fail the request and return NDIS_STATUS_INVALID_DATA. */
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("rtw_set_802_11_add_key: ((key->KeyIndex & 0x80000000) == 0)[=%d] ", (int)(key->KeyIndex & 0x80000000) == 0));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("rtw_set_802_11_add_key:((key->KeyIndex & 0x40000000) > 0)[=%d]", (int)(key->KeyIndex & 0x40000000) > 0));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_info_, ("rtw_set_802_11_add_key: key->KeyIndex =%d\n", (int)key->KeyIndex));
		ret = _FAIL;
		goto exit;
	}

	if (key->KeyIndex & 0x40000000) {
		/*  Pairwise key */

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY: +++++ Pairwise key +++++\n"));

		pbssid = get_bssid(&padapter->mlmepriv);
		stainfo = rtw_get_stainfo(&padapter->stapriv, pbssid);

		if ((stainfo != NULL) && (padapter->securitypriv.dot11AuthAlgrthm == dot11AuthAlgrthm_8021X)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY:(stainfo != NULL) && (adapter->securitypriv.dot11AuthAlgrthm == dot11AuthAlgrthm_8021X)\n"));
			encryptionalgo = stainfo->dot118021XPrivacy;
		} else {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY: stainfo == NULL)||(adapter->securitypriv.dot11AuthAlgrthm!= dot11AuthAlgrthm_8021X)\n"));
			encryptionalgo = padapter->securitypriv.dot11PrivacyAlgrthm;
		}

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
			 ("rtw_set_802_11_add_key: (encryptionalgo ==%d)!\n", encryptionalgo));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
			 ("rtw_set_802_11_add_key: (adapter->securitypriv.dot11PrivacyAlgrthm ==%d)!\n",
			 padapter->securitypriv.dot11PrivacyAlgrthm));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
			 ("rtw_set_802_11_add_key: (adapter->securitypriv.dot11AuthAlgrthm ==%d)!\n",
			 padapter->securitypriv.dot11AuthAlgrthm));

		if ((stainfo != NULL))
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 ("rtw_set_802_11_add_key: (stainfo->dot118021XPrivacy ==%d)!\n",
				 stainfo->dot118021XPrivacy));

		if (key->KeyIndex & 0x000000FF) {
			/*  The key index is specified in the lower 8 bits by values of zero to 255. */
			/*  The key index should be set to zero for a Pairwise key, and the driver should fail with */
			/*  NDIS_STATUS_INVALID_DATA if the lower 8 bits is not zero */
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, (" key->KeyIndex & 0x000000FF.\n"));
			ret = _FAIL;
			goto exit;
		}

		/*  check BSSID */
		if (IS_MAC_ADDRESS_BROADCAST(key->BSSID) == true) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("MacAddr_isBcst(key->BSSID)\n"));
			ret = false;
			goto exit;
		}

		/*  Check key length for TKIP. */
		/* if (encryptionAlgorithm == RT_ENC_TKIP_ENCRYPTION && key->KeyLength != 32) */
		if ((encryptionalgo == _TKIP_) && (key->KeyLength != 32)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("TKIP KeyLength:0x%x != 32\n", key->KeyLength));
			ret = _FAIL;
			goto exit;
		}

		/*  Check key length for AES. */
		if ((encryptionalgo == _AES_) && (key->KeyLength != 16)) {
			/*  For our supplicant, EAPPkt9x.vxd, cannot differentiate TKIP and AES case. */
			if (key->KeyLength == 32) {
				key->KeyLength = 16;
			} else {
				ret = _FAIL;
				goto exit;
			}
		}

		/*  Check key length for WEP. For NDTEST, 2005.01.27, by rcnjko. */
		if ((encryptionalgo == _WEP40_ || encryptionalgo == _WEP104_) &&
		    (key->KeyLength != 5 && key->KeyLength != 13)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("WEP KeyLength:0x%x != 5 or 13\n", key->KeyLength));
			ret = _FAIL;
			goto exit;
		}

		bgroup = false;

		/*  Check the pairwise key. Added by Annie, 2005-07-06. */
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("[Pairwise Key set]\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("key index: 0x%8x(0x%8x)\n", key->KeyIndex, (key->KeyIndex&0x3)));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("key Length: %d\n", key->KeyLength));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
	} else {
		/*  Group key - KeyIndex(BIT30== 0) */
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY: +++++ Group key +++++\n"));


		/*  when add wep key through add key and didn't assigned encryption type before */
		if ((padapter->securitypriv.ndisauthtype <= 3) &&
		    (padapter->securitypriv.dot118021XGrpPrivacy == 0)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 ("keylen=%d(adapter->securitypriv.dot11PrivacyAlgrthm =%x )padapter->securitypriv.dot118021XGrpPrivacy(%x)\n",
				 key->KeyLength, padapter->securitypriv.dot11PrivacyAlgrthm,
				 padapter->securitypriv.dot118021XGrpPrivacy));

			switch (key->KeyLength) {
			case 5:
				padapter->securitypriv.dot11PrivacyAlgrthm = _WEP40_;
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("adapter->securitypriv.dot11PrivacyAlgrthm = %x key->KeyLength=%u\n",
					 padapter->securitypriv.dot11PrivacyAlgrthm, key->KeyLength));
				break;
			case 13:
				padapter->securitypriv.dot11PrivacyAlgrthm = _WEP104_;
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("adapter->securitypriv.dot11PrivacyAlgrthm = %x key->KeyLength=%u\n",
					 padapter->securitypriv.dot11PrivacyAlgrthm, key->KeyLength));
				break;
			default:
				padapter->securitypriv.dot11PrivacyAlgrthm = _NO_PRIVACY_;
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("adapter->securitypriv.dot11PrivacyAlgrthm = %x key->KeyLength=%u\n",
					 padapter->securitypriv.dot11PrivacyAlgrthm, key->KeyLength));
				break;
			}
			encryptionalgo = padapter->securitypriv.dot11PrivacyAlgrthm;

			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 (" adapter->securitypriv.dot11PrivacyAlgrthm =%x\n",
				 padapter->securitypriv.dot11PrivacyAlgrthm));
		} else {
			encryptionalgo = padapter->securitypriv.dot118021XGrpPrivacy;
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 ("(adapter->securitypriv.dot11PrivacyAlgrthm =%x )encryptionalgo(%x) = padapter->securitypriv.dot118021XGrpPrivacy(%x)keylen=%d\n",
				 padapter->securitypriv.dot11PrivacyAlgrthm, encryptionalgo, padapter->securitypriv.dot118021XGrpPrivacy, key->KeyLength));
		}

		if ((check_fwstate(&padapter->mlmepriv, WIFI_ADHOC_STATE) == true) && (IS_MAC_ADDRESS_BROADCAST(key->BSSID) == false)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, (" IBSS but BSSID is not Broadcast Address.\n"));
			ret = _FAIL;
			goto exit;
		}

		/*  Check key length for TKIP */
		if ((encryptionalgo == _TKIP_) && (key->KeyLength != 32)) {
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, (" TKIP GTK KeyLength:%u != 32\n", key->KeyLength));
			ret = _FAIL;
			goto exit;

		} else if (encryptionalgo == _AES_ && (key->KeyLength != 16 && key->KeyLength != 32)) {
			/*  Check key length for AES */
			/*  For NDTEST, we allow keylen= 32 in this case. 2005.01.27, by rcnjko. */
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("<=== SetInfo, OID_802_11_ADD_KEY: AES GTK KeyLength:%u != 16 or 32\n", key->KeyLength));
			ret = _FAIL;
			goto exit;
		}

		/*  Change the key length for EAPPkt9x.vxd. Added by Annie, 2005-11-03. */
		if ((encryptionalgo ==  _AES_) && (key->KeyLength == 32)) {
			key->KeyLength = 16;
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("AES key length changed: %u\n", key->KeyLength));
		}

		if (key->KeyIndex & 0x8000000) /* error ??? 0x8000_0000 */
			bgrouptkey = true;

		if ((check_fwstate(&padapter->mlmepriv, WIFI_ADHOC_STATE) == true) &&
		    (check_fwstate(&padapter->mlmepriv, _FW_LINKED) == true))
			bgrouptkey = true;

		bgroup = true;

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("[Group Key set]\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("key index: 0x%8x(0x%8x)\n", key->KeyIndex, (key->KeyIndex&0x3)));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("key Length: %d\n", key->KeyLength));
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("------------------------------------------\n"));
	}

	/*  If WEP encryption algorithm, just call rtw_set_802_11_add_wep(). */
	if ((padapter->securitypriv.dot11AuthAlgrthm != dot11AuthAlgrthm_8021X) && (encryptionalgo == _WEP40_ ||
	    encryptionalgo == _WEP104_)) {
		u32 keyindex;
		u32 len = FIELD_OFFSET(struct ndis_802_11_key, KeyMaterial) + key->KeyLength;
		struct ndis_802_11_wep *wep = &padapter->securitypriv.ndiswep;

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY: +++++ WEP key +++++\n"));

		wep->Length = len;
		keyindex = key->KeyIndex&0x7fffffff;
		wep->KeyIndex = keyindex;
		wep->KeyLength = key->KeyLength;

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY:Before memcpy\n"));

		memcpy(wep->KeyMaterial, key->KeyMaterial, key->KeyLength);
		memcpy(&(padapter->securitypriv.dot11DefKey[keyindex].skey[0]), key->KeyMaterial, key->KeyLength);

		padapter->securitypriv.dot11DefKeylen[keyindex] = key->KeyLength;
		padapter->securitypriv.dot11PrivacyKeyIndex = keyindex;

		ret = rtw_set_802_11_add_wep(padapter, wep);

		goto exit;
	}

	if (key->KeyIndex & 0x20000000) {
		/*  SetRSC */
		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("OID_802_11_ADD_KEY: +++++ SetRSC+++++\n"));
		if (bgroup == true) {
			unsigned long long keysrc = key->KeyRSC & 0x00FFFFFFFFFFFFULL;
			memcpy(&padapter->securitypriv.dot11Grprxpn, &keysrc, 8);
		} else {
			unsigned long long keysrc = key->KeyRSC & 0x00FFFFFFFFFFFFULL;
			memcpy(&padapter->securitypriv.dot11Grptxpn, &keysrc, 8);
		}
	}

	/*  Indicate this key idx is used for TX */
	/*  Save the key in KeyMaterial */
	if (bgroup == true) { /*  Group transmit key */
		int res;

		if (bgrouptkey == true)
			padapter->securitypriv.dot118021XGrpKeyid = (u8)key->KeyIndex;

		if ((key->KeyIndex&0x3) == 0) {
			ret = _FAIL;
			goto exit;
		}

		memset(&padapter->securitypriv.dot118021XGrpKey[(u8)((key->KeyIndex) & 0x03)], 0, 16);
		memset(&padapter->securitypriv.dot118021XGrptxmickey[(u8)((key->KeyIndex) & 0x03)], 0, 16);
		memset(&padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)], 0, 16);

		if ((key->KeyIndex & 0x10000000)) {
			memcpy(&padapter->securitypriv.dot118021XGrptxmickey[(u8)((key->KeyIndex) & 0x03)], key->KeyMaterial + 16, 8);
			memcpy(&padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)], key->KeyMaterial + 24, 8);

			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 ("\n rtw_set_802_11_add_key:rx mic :0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n",
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[0],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[1],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[2],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[3],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[4],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[5],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[6],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[7]));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n rtw_set_802_11_add_key:set Group mic key!!!!!!!!\n"));

		} else {
			memcpy(&padapter->securitypriv.dot118021XGrptxmickey[(u8)((key->KeyIndex) & 0x03)], key->KeyMaterial + 24, 8);
			memcpy(&padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)], key->KeyMaterial + 16, 8);

			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
				 ("\n rtw_set_802_11_add_key:rx mic :0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n",
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[0],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[1],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[2],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[3],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[4],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[5],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex) & 0x03)].skey[6],
				 padapter->securitypriv.dot118021XGrprxmickey[(u8)((key->KeyIndex-1) & 0x03)].skey[7]));
			RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n rtw_set_802_11_add_key:set Group mic key!!!!!!!!\n"));
		}

		/* set group key by index */
		memcpy(&padapter->securitypriv.dot118021XGrpKey[(u8)((key->KeyIndex) & 0x03)], key->KeyMaterial, key->KeyLength);

		key->KeyIndex = key->KeyIndex & 0x03;

		padapter->securitypriv.binstallGrpkey = true;

		padapter->securitypriv.bcheck_grpkey = false;

		RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("reset group key"));

		res = rtw_set_key(padapter, &padapter->securitypriv, key->KeyIndex, 1);

		if (res == _FAIL)
			ret = _FAIL;

		goto exit;

	} else { /*  Pairwise Key */
		u8 res;

		pbssid = get_bssid(&padapter->mlmepriv);
		stainfo = rtw_get_stainfo(&padapter->stapriv, pbssid);

		if (stainfo != NULL) {
			memset(&stainfo->dot118021x_UncstKey, 0, 16);/*  clear keybuffer */

			memcpy(&stainfo->dot118021x_UncstKey, key->KeyMaterial, 16);

			if (encryptionalgo == _TKIP_) {
				padapter->securitypriv.busetkipkey = false;

				/* _set_timer(&padapter->securitypriv.tkip_timer, 50); */

				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_, ("\n ========== _set_timer\n"));

				/*  if TKIP, save the Receive/Transmit MIC key in KeyMaterial[128-255] */
				if ((key->KeyIndex & 0x10000000)) {
					memcpy(&stainfo->dot11tkiptxmickey, key->KeyMaterial + 16, 8);
					memcpy(&stainfo->dot11tkiprxmickey, key->KeyMaterial + 24, 8);

				} else {
					memcpy(&stainfo->dot11tkiptxmickey, key->KeyMaterial + 24, 8);
					memcpy(&stainfo->dot11tkiprxmickey, key->KeyMaterial + 16, 8);
				}
			}

			/* Set key to CAM through H2C command */
			if (bgrouptkey) { /* never go to here */
				res = rtw_setstakey_cmd(padapter, (unsigned char *)stainfo, false);
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("\n rtw_set_802_11_add_key:rtw_setstakey_cmd(group)\n"));
			} else {
				res = rtw_setstakey_cmd(padapter, (unsigned char *)stainfo, true);
				RT_TRACE(_module_rtl871x_ioctl_set_c_, _drv_err_,
					 ("\n rtw_set_802_11_add_key:rtw_setstakey_cmd(unicast)\n"));
			}

			if (res == false)
				ret = _FAIL;
		}
	}

exit:

_func_exit_;

	return ret;
}

u8 rtw_set_802_11_remove_key(struct rtw_adapter *padapter,
			     struct ndis_802_11_remove_key *key)
{
	u8 *pbssid;
	struct sta_info *stainfo;
	u8	bgroup = (key->KeyIndex & 0x4000000) > 0 ? false : true;
	u8	keyIndex = (u8)key->KeyIndex & 0x03;
	u8	ret = _SUCCESS;

_func_enter_;

	if ((key->KeyIndex & 0xbffffffc) > 0) {
		ret = _FAIL;
		goto exit;
	}
	if (bgroup == true) {
		/*  clear group key by index */

		memset(&padapter->securitypriv.dot118021XGrpKey[keyIndex], 0, 16);

		/*  \todo Send a H2C Command to Firmware for removing this Key in CAM Entry. */
	} else {
		pbssid = get_bssid(&padapter->mlmepriv);
		stainfo = rtw_get_stainfo(&padapter->stapriv, pbssid);
		if (stainfo != NULL) {
			/*  clear key by BSSID */
			memset(&stainfo->dot118021x_UncstKey, 0, 16);
			/*  \todo Send a H2C Command to Firmware for disable this Key in CAM Entry. */
		} else {
			ret = _FAIL;
			goto exit;
		}
	}

exit:

_func_exit_;

	return ret;
}

/*
* rtw_get_cur_max_rate -
* @adapter: pointer to _adapter structure
*
* Return 0 or 100Kbps
*/
u16 rtw_get_cur_max_rate(struct rtw_adapter *adapter)
{
	int	i = 0;
	u8	*p;
	u16	rate = 0, max_rate = 0;
	struct mlme_ext_priv	*pmlmeext = &adapter->mlmeextpriv;
	struct mlme_ext_info	*pmlmeinfo = &(pmlmeext->mlmext_info);
	struct registry_priv *pregistrypriv = &adapter->registrypriv;
	struct mlme_priv	*pmlmepriv = &adapter->mlmepriv;
	struct wlan_bssid_ex  *pcur_bss = &pmlmepriv->cur_network.network;
#ifdef CONFIG_80211N_HT
	struct rtw_ieee80211_ht_cap *pht_capie;
	u8	rf_type = 0;
	u8	bw_40MHz = 0, short_GI_20 = 0, short_GI_40 = 0;
	u16	mcs_rate = 0;
	u32	ht_ielen = 0;
#endif

	if ((check_fwstate(pmlmepriv, _FW_LINKED) != true) &&
	    (check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) != true))
		return 0;

#ifdef CONFIG_80211N_HT
	if (pmlmeext->cur_wireless_mode & (WIRELESS_11_24N|WIRELESS_11_5N)) {
		p = rtw_get_ie(&pcur_bss->IEs[12], _HT_CAPABILITY_IE_, &ht_ielen, pcur_bss->IELength-12);
		if (p && ht_ielen > 0) {
			pht_capie = (struct rtw_ieee80211_ht_cap *)(p+2);

			memcpy(&mcs_rate, pht_capie->supp_mcs_set, 2);

			/* cur_bwmod is updated by beacon, pmlmeinfo is updated by association response */
			bw_40MHz = (pmlmeext->cur_bwmode && (HT_INFO_HT_PARAM_REC_TRANS_CHNL_WIDTH & pmlmeinfo->HT_info.infos[0])) ? 1 : 0;

			short_GI_20 = (le16_to_cpu(pmlmeinfo->HT_caps.u.HT_cap_element.HT_caps_info) & IEEE80211_HT_CAP_SGI_20) ? 1 : 0;
			short_GI_40 = (le16_to_cpu(pmlmeinfo->HT_caps.u.HT_cap_element.HT_caps_info) & IEEE80211_HT_CAP_SGI_40) ? 1 : 0;

			rtw_hal_get_hwreg(adapter, HW_VAR_RF_TYPE, (u8 *)(&rf_type));
			max_rate = rtw_mcs_rate(
				rf_type,
				bw_40MHz & (pregistrypriv->cbw40_enable),
				short_GI_20,
				short_GI_40,
				pmlmeinfo->HT_caps.u.HT_cap_element.MCS_rate
			);
		}
	} else
#endif /* CONFIG_80211N_HT */
	{
		while ((pcur_bss->SupportedRates[i] != 0) && (pcur_bss->SupportedRates[i] != 0xFF)) {
			rate = pcur_bss->SupportedRates[i]&0x7F;
			if (rate > max_rate)
				max_rate = rate;
			i++;
		}

		max_rate = max_rate*10/2;
	}

	return max_rate;
}

/*
* rtw_set_scan_mode -
* @adapter: pointer to _adapter structure
* @scan_mode:
*
* Return _SUCCESS or _FAIL
*/
int rtw_set_scan_mode(struct rtw_adapter *adapter, enum RT_SCAN_TYPE scan_mode)
{
	if (scan_mode != SCAN_ACTIVE && scan_mode != SCAN_PASSIVE)
		return _FAIL;

	adapter->mlmepriv.scan_mode = scan_mode;

	return _SUCCESS;
}

/*
* rtw_set_channel_plan -
* @adapter: pointer to _adapter structure
* @channel_plan:
*
* Return _SUCCESS or _FAIL
*/
int rtw_set_channel_plan(struct rtw_adapter *adapter, u8 channel_plan)
{
	struct registry_priv *pregistrypriv = &adapter->registrypriv;
	struct mlme_priv *pmlmepriv = &adapter->mlmepriv;

	/* handle by cmd_thread to sync with scan operation */
	return rtw_set_chplan_cmd(adapter, channel_plan, 1);
}

/*
* rtw_set_country -
* @adapter: pointer to _adapter structure
* @country_code: string of country code
*
* Return _SUCCESS or _FAIL
*/
int rtw_set_country(struct rtw_adapter *adapter, const char *country_code)
{
	int channel_plan = RT_CHANNEL_DOMAIN_WORLD_WIDE_5G;

	DBG_8192D("%s country_code:%s\n", __func__, country_code);

	/* TODO: should have a table to match country code and RT_CHANNEL_DOMAIN */
	/* TODO: should consider 2-character and 3-character country code */
	if (0 == strcmp(country_code, "US"))
		channel_plan = RT_CHANNEL_DOMAIN_FCC;
	else if (0 == strcmp(country_code, "EU"))
		channel_plan = RT_CHANNEL_DOMAIN_ETSI;
	else if (0 == strcmp(country_code, "JP"))
		channel_plan = RT_CHANNEL_DOMAIN_MKK;
	else if (0 == strcmp(country_code, "CN"))
		channel_plan = RT_CHANNEL_DOMAIN_CHINA;
	else
		DBG_8192D("%s unknown country_code:%s\n", __func__, country_code);

	return rtw_set_channel_plan(adapter, channel_plan);
}
