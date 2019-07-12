use eapol;
use failure::{bail, ensure, format_err};
use fidl_fuchsia_wlan_mlme::BssDescription;
use fidl_fuchsia_wlan_sme as fidl_sme;
use std::boxed::Box;
use wlan_common::ie::rsn::{
    akm, cipher,
    rsne::{self, Rsne},
    OUI,
};
use wlan_rsn::{
    self, nonce::NonceReader, psk, rsna::UpdateSink, NegotiatedProtection, ProtectionInfo,
};

use crate::{client::state::Protection, DeviceInfo};

#[derive(Debug)]
pub struct Rsna {
    pub negotiated_protection: NegotiatedProtection,
    pub supplicant: Box<Supplicant>,
}

impl PartialEq for Rsna {
    fn eq(&self, other: &Self) -> bool {
        self.negotiated_protection == other.negotiated_protection
    }
}

pub trait Supplicant: std::fmt::Debug + std::marker::Send {
    fn start(&mut self) -> Result<(), failure::Error>;
    fn reset(&mut self);
    fn on_eapol_frame(
        &mut self,
        update_sink: &mut UpdateSink,
        frame: eapol::Frame<&[u8]>,
    ) -> Result<(), failure::Error>;
}

impl Supplicant for wlan_rsn::Supplicant {
    fn start(&mut self) -> Result<(), failure::Error> {
        wlan_rsn::Supplicant::start(self)
    }

    fn reset(&mut self) {
        wlan_rsn::Supplicant::reset(self)
    }

    fn on_eapol_frame(
        &mut self,
        update_sink: &mut UpdateSink,
        frame: eapol::Frame<&[u8]>,
    ) -> Result<(), failure::Error> {
        wlan_rsn::Supplicant::on_eapol_frame(self, update_sink, frame)
    }
}

/// Supported Ciphers and AKMs:
/// Group Data Ciphers: CCMP-128, TKIP
/// Pairwise Cipher: CCMP-128
/// AKM: PSK
pub fn is_rsn_compatible(a_rsne: &Rsne) -> bool {
    let group_data_supported = a_rsne.group_data_cipher_suite.as_ref().map_or(false, |c| {
        // IEEE allows TKIP usage only in GTKSAs for compatibility reasons.
        // TKIP is considered broken and should never be used in a PTKSA or IGTKSA.
        c.has_known_usage() && (c.suite_type == cipher::CCMP_128 || c.suite_type == cipher::TKIP)
    });

    let pairwise_supported = a_rsne
        .pairwise_cipher_suites
        .iter()
        .any(|c| c.has_known_usage() && c.suite_type == cipher::CCMP_128);
    let akm_supported =
        a_rsne.akm_suites.iter().any(|a| a.has_known_algorithm() && a.suite_type == akm::PSK);
    let caps_supported = a_rsne.rsn_capabilities.as_ref().map_or(true, |caps| {
        !(caps.no_pairwise()
            || caps.mgmt_frame_protection_req()
            || caps.joint_multiband()
            || caps.peerkey_enabled()
            || caps.ssp_amsdu_req()
            || caps.pbac()
            || caps.extended_key_id())
    });

    group_data_supported && pairwise_supported && akm_supported && caps_supported
}

pub fn get_rsna(
    device_info: &DeviceInfo,
    credential: &fidl_sme::Credential,
    bss: &BssDescription,
) -> Result<Protection, failure::Error> {
    let a_rsne_bytes = match bss.rsn.as_ref() {
        None => bail!("RSNE not present in BSS"),
        Some(rsn) => &rsn[..],
    };

    // Credentials supplied and BSS is protected.
    let a_rsne = rsne::from_bytes(a_rsne_bytes)
        .to_full_result()
        .map_err(|e| format_err!("invalid RSNE {:02x?}: {:?}", a_rsne_bytes, e))?;
    let s_rsne = derive_s_rsne(&a_rsne)?;
    let negotiated_protection = NegotiatedProtection::from_rsne(&s_rsne)?;
    let psk = compute_psk(credential, &bss.ssid[..])?;
    let supplicant = wlan_rsn::Supplicant::new_wpa2psk_ccmp128(
        // Note: There should be one Reader per device, not per SME.
        // Follow-up with improving on this.
        NonceReader::new(&device_info.addr[..])?,
        psk,
        device_info.addr,
        ProtectionInfo::Rsne(s_rsne),
        bss.bssid,
        ProtectionInfo::Rsne(a_rsne),
    )
    .map_err(|e| format_err!("failed to create ESS-SA: {:?}", e))?;
    Ok(Protection::Rsna(Rsna { negotiated_protection, supplicant: Box::new(supplicant) }))
}

fn compute_psk(credential: &fidl_sme::Credential, ssid: &[u8]) -> Result<psk::Psk, failure::Error> {
    match credential {
        fidl_sme::Credential::Password(password) => psk::compute(&password[..], ssid),
        fidl_sme::Credential::Psk(psk) => {
            ensure!(psk.len() == 32, "PSK must be 32 octets but was {}", psk.len());
            Ok(psk.clone().into_boxed_slice())
        }
        _ => bail!("unsupported credentials configuration for computing PSK"),
    }
}

/// Constructs Supplicant's RSNE with:
/// Group Data Cipher: same as A-RSNE (CCMP-128 or TKIP)
/// Pairwise Cipher: CCMP-128
/// AKM: PSK
fn derive_s_rsne(a_rsne: &Rsne) -> Result<Rsne, failure::Error> {
    if !is_rsn_compatible(&a_rsne) {
        bail!("incompatible RSNE {:?}", a_rsne);
    }

    // If Authenticator's RSNE is supported, construct Supplicant's RSNE.
    let mut s_rsne = Rsne::new();
    s_rsne.group_data_cipher_suite = a_rsne.group_data_cipher_suite.clone();
    let pairwise_cipher = cipher::Cipher { oui: OUI, suite_type: cipher::CCMP_128 };
    s_rsne.pairwise_cipher_suites.push(pairwise_cipher);
    let akm = akm::Akm { oui: OUI, suite_type: akm::PSK };
    s_rsne.akm_suites.push(akm);
    s_rsne.rsn_capabilities = a_rsne.rsn_capabilities.clone();
    Ok(s_rsne)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        client::test_utils::{fake_protected_bss_description, fake_unprotected_bss_description},
        test_utils::{fake_device_info, make_rsne, rsne_as_bytes, wpa2_psk_ccmp_rsne_with_caps},
    };
    use wlan_common::ie::rsn::rsne::RsnCapabilities;

    const CLIENT_ADDR: [u8; 6] = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];

    #[test]
    fn test_rsn_capabilities() {
        let a_rsne = wpa2_psk_ccmp_rsne_with_caps(RsnCapabilities(0x000C));
        assert!(is_rsn_compatible(&a_rsne));

        let a_rsne = wpa2_psk_ccmp_rsne_with_caps(RsnCapabilities(0));
        assert!(is_rsn_compatible(&a_rsne));

        let a_rsne = wpa2_psk_ccmp_rsne_with_caps(RsnCapabilities(1));
        assert!(is_rsn_compatible(&a_rsne));

        let a_rsne = wpa2_psk_ccmp_rsne_with_caps(RsnCapabilities(2));
        assert!(!is_rsn_compatible(&a_rsne));
    }

    #[test]
    fn test_incompatible_group_data_cipher() {
        let a_rsne = make_rsne(Some(cipher::GCMP_256), vec![cipher::CCMP_128], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_incompatible_pairwise_cipher() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::BIP_CMAC_256], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_tkip_pairwise_cipher() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::TKIP], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_tkip_group_data_cipher() {
        let a_rsne = make_rsne(Some(cipher::TKIP), vec![cipher::CCMP_128], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), true);

        let s_rsne = derive_s_rsne(&a_rsne).unwrap();
        let expected_rsne_bytes =
            vec![48, 18, 1, 0, 0, 15, 172, 2, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 2];
        assert_eq!(rsne_as_bytes(s_rsne), expected_rsne_bytes);
    }

    #[test]
    fn test_ccmp128_group_data_pairwise_cipher_psk() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::CCMP_128], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), true);

        let s_rsne = derive_s_rsne(&a_rsne).unwrap();
        let expected_rsne_bytes =
            vec![48, 18, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 2];
        assert_eq!(rsne_as_bytes(s_rsne), expected_rsne_bytes);
    }

    #[test]
    fn test_mixed_mode() {
        let a_rsne = make_rsne(
            Some(cipher::CCMP_128),
            vec![cipher::CCMP_128, cipher::TKIP],
            vec![akm::PSK, akm::FT_PSK],
        );
        assert_eq!(is_rsn_compatible(&a_rsne), true);

        let s_rsne = derive_s_rsne(&a_rsne).unwrap();
        let expected_rsne_bytes =
            vec![48, 18, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 2];
        assert_eq!(rsne_as_bytes(s_rsne), expected_rsne_bytes);
    }

    #[test]
    fn test_no_group_data_cipher() {
        let a_rsne = make_rsne(None, vec![cipher::CCMP_128], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_no_pairwise_cipher() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![], vec![akm::PSK]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_no_akm() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::CCMP_128], vec![]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_incompatible_akm() {
        let a_rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::CCMP_128], vec![akm::EAP]);
        assert_eq!(is_rsn_compatible(&a_rsne), false);
    }

    #[test]
    fn test_get_rsna_password_for_unprotected_network() {
        let bss = fake_unprotected_bss_description(b"foo_bss".to_vec());
        let credential = fidl_sme::Credential::Password("somepass".as_bytes().to_vec());
        let rsna = get_rsna(&fake_device_info(CLIENT_ADDR), &credential, &bss);
        assert!(rsna.is_err(), "expect error when password is supplied for unprotected network")
    }

    #[test]
    fn test_get_rsna_no_password_for_protected_network() {
        let bss = fake_protected_bss_description(b"foo_bss".to_vec());
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let rsna = get_rsna(&fake_device_info(CLIENT_ADDR), &credential, &bss);
        assert!(rsna.is_err(), "expect error when no password is supplied for protected network")
    }

    #[test]
    fn test_get_rsna_psk() {
        let bss = fake_protected_bss_description(b"foo_bss".to_vec());
        let credential = fidl_sme::Credential::Psk(vec![0xAA; 32]);
        get_rsna(&fake_device_info(CLIENT_ADDR), &credential, &bss)
            .expect("expected successful RSNA with valid PSK");
    }

    #[test]
    fn test_get_rsna_invalid_psk() {
        let bss = fake_protected_bss_description(b"foo_bss".to_vec());
        // PSK too short
        let credential = fidl_sme::Credential::Psk(vec![0xAA; 31]);
        get_rsna(&fake_device_info(CLIENT_ADDR), &credential, &bss)
            .expect_err("expected RSNA failure with invalid PSK");
    }
}
