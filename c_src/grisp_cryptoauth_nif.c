#include "erl_nif.h"
#include "atca_basic.h"
#include "atcacert/atcacert_client.h"


ATCAIfaceCfg grisp_atcab_default_config = {
    .iface_type                 = ATCA_I2C_IFACE,
    .devtype                    = ATECC608B,
    {
        /*
         * ATECC608B-TFLXTLSS default address;
         * unconfigured chips usually have 0xCO
         */
        .atcai2c.address        = 0x6C,
        .atcai2c.bus            = 1,
        .atcai2c.baud           = 100000,
    },
    .wake_delay                 = 1500,
    .rx_retries                 = 20
};

/*
 * Device Configuration, shamelessly stolen from Microchip's ATECC608B-TFLXTLSS. This configuration
 * is supposed to support all our envisioned usecases based on the TrustFLEX configuration, in
 * particular a custom Public Key Infrastructure (PKI) is supported.
 *
 * There are way more supported usecases, you can checkout the Trust Platform for explanations.
 * In the following the supposed usage and purpose of each slot is explained: 
 *
 *
 * Slot 0   Primary private key; Primary authentication key; Permanent, Ext Sign, ECDH
 * Slot 1   Internal sign private key; Private key that can only be used to attest internal keys and
 *          state of the a device; Can't be used to sign arbitrary messages; Permanent, Int Sign
 * Slot 2   Secondary private key 1; Secondary private key for other uses; Updatable, Ext Sign, ECDH, Lockable
 * Slot 3   Secondary private key 2; Secondary private key for other uses; Updatable, Ext Sign, ECDH, Lockable
 * Slot 4   Secondary private key 3; Secondary private key for other uses; Updatable, Ext Sign, ECDH, Lockable
 * Slot 5   Secret key; Storage for a secret key; No Read, Encrypted write(6), Lockable, AES key
 * Slot 6   IO protection key; Key used to protect the I2C bus communication (IO) of certain commands;
 *          Requires setup before use; No read, Clear write, Lockable
 * Slot 7   Secure boot digest; Storage location for secureboot digest; This is an internal function, so no
 *          reads or writes are enabled; No read, No write
 * Slot 8   General data; General public data storage (416 bytes); Clear read, Always write, Lockable
 * Slot 9   AES key; Intermediate key storage for ECDH and KDF output; No read, Always write, AES key
 * Slot 10  Device compressed certificate; Certificate primary public key in the Crypto Authentication
 *          compressed format; Clear read, No write
 * Slot 11  Signer public key; Public key for the CA (signer) that signed the device cert; Clear read, No write
 * Slot 12  Signer compressed certificate; Certificate for the CA (signer) certificate for the device
 *          certificate in the CryptoAuthentication compressed format; Clear read, No write
 * Slot 13  Parent public key or general data; Parent public key for validating/invalidating the validated
 *          public key; Can also be used just as a public key or general data storage (72 bytes);
 *          Clear read, Always write, Lockable
 * Slot 14  Validated public key; Validated public key cannot be used (Verify command) or changed without
 *          authorization via the parent public key; Clear read, Always write, Validated (13)
 * Slot 15  Secure boot public key; Secure boot public key; Clear read, Always write, Lockable
 *
 *
 * The following configuration can be written at the very beginning of the provisioning process onto an
 * unconfigured device. Don't touch this without informing yourself, be very careful.
 */
static const uint8_t grisp_device_default_config[] = {
    0x01, 0x23, 0x00, 0x00, 0x00, 0x00, 0x60, 0x01,  // 0   - 7      ignored on write (dummy data)
    0x00, 0x00, 0x00, 0x00, 0xEE, 0x01, 0x01, 0x00,  // 8   - 15     ignored on write (dummy data)
    0x6C, 0x00, 0x00, 0x01,                          // 16  - 19     16: I2C address; 19: ChipMode
    // Start of Slot configuration, two bytes per slot
    0x85, 0x00, 0x82, 0x00, 0x85, 0x20, 0x85, 0x20,  // 20  - 27     Slots 0  - 3
    0x85, 0x20, 0x8F, 0x46, 0x8F, 0x0F, 0x9F, 0x8F,  // 28  - 35     Slots 4  - 7
    0x0F, 0x0F, 0x8F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,  // 36  - 43     Slots 8  - 11
    0x0F, 0x0F, 0x0F, 0x0F, 0x0D, 0x1F, 0x0F, 0x0F,  // 44  - 51     Slots 12 - 15
    // End of Slot configuration, next comes more general stuff
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,  // 52  - 59     Monotonic Counter connected to keys
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,  // 60  - 67     Monotonic Counter (not connected to keys)
    0x00, 0x00, 0x03, 0xF7, 0x00, 0x69, 0x76, 0x00,  // 68  - 75     UseLock, VolatileKey, SecureBoot, KDF
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 76  - 83     unknown
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x0E, 0x60,  // 84  - 91     85: UserExtraAdd; Lock Bytes
    0x00, 0x00, 0x00, 0x00,                          // 92  - 95     X.509 certificate formatting
    // Slot Key configuration, two bytes per slot
    0x53, 0x00, 0x53, 0x00, 0x73, 0x00, 0x73, 0x00,  // 96  - 103    Slots 0  - 3
    0x73, 0x00, 0x38, 0x00, 0x7C, 0x00, 0x1C, 0x00,  // 104 - 111    Slots 4  - 7
    0x3C, 0x00, 0x1A, 0x00, 0x3C, 0x00, 0x30, 0x00,  // 112 - 119    Slots 8  - 11
    0x3C, 0x00, 0x30, 0x00, 0x12, 0x00, 0x30, 0x00,  // 120 - 127    Slots 12 - 15
};

// TFLXTLS template, will be replaced by Stritzinger template
const uint8_t grisp_cert_template_signer[520] = {
    0x30, 0x82, 0x02, 0x04, 0x30, 0x82, 0x01, 0xaa, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x10, 0x44,
    0x0e, 0xe4, 0x17, 0x0c, 0xb5, 0x45, 0xce, 0x59, 0x69, 0x8e, 0x30, 0x56, 0x99, 0x0a, 0x5d, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x4f, 0x31, 0x21, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x18, 0x4d, 0x69, 0x63, 0x72, 0x6f, 0x63, 0x68, 0x69,
    0x70, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6e, 0x6f, 0x6c, 0x6f, 0x67, 0x79, 0x20, 0x49, 0x6e, 0x63,
    0x31, 0x2a, 0x30, 0x28, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x21, 0x43, 0x72, 0x79, 0x70, 0x74,
    0x6f, 0x20, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6e, 0x74, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e,
    0x20, 0x52, 0x6f, 0x6f, 0x74, 0x20, 0x43, 0x41, 0x20, 0x30, 0x30, 0x32, 0x30, 0x20, 0x17, 0x0d,
    0x31, 0x38, 0x31, 0x31, 0x30, 0x38, 0x30, 0x34, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x18, 0x0f, 0x32,
    0x30, 0x34, 0x39, 0x31, 0x31, 0x30, 0x38, 0x30, 0x34, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x30, 0x4f,
    0x31, 0x21, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x18, 0x4d, 0x69, 0x63, 0x72, 0x6f,
    0x63, 0x68, 0x69, 0x70, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6e, 0x6f, 0x6c, 0x6f, 0x67, 0x79, 0x20,
    0x49, 0x6e, 0x63, 0x31, 0x2a, 0x30, 0x28, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x21, 0x43, 0x72,
    0x79, 0x70, 0x74, 0x6f, 0x20, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6e, 0x74, 0x69, 0x63, 0x61, 0x74,
    0x69, 0x6f, 0x6e, 0x20, 0x53, 0x69, 0x67, 0x6e, 0x65, 0x72, 0x20, 0x46, 0x46, 0x46, 0x46, 0x30,
    0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x84, 0x98, 0x44, 0x0a, 0x31, 0x9b,
    0x3f, 0x71, 0xe2, 0x5d, 0x52, 0x26, 0x00, 0x90, 0x00, 0xc7, 0x56, 0xbd, 0x5c, 0x0f, 0xae, 0x4a,
    0x1b, 0x84, 0x1a, 0xd4, 0xa3, 0x3f, 0x21, 0xab, 0xa0, 0x9a, 0x48, 0x10, 0x1c, 0x75, 0xc8, 0x28,
    0x24, 0x90, 0xb3, 0xb6, 0x5a, 0x52, 0x80, 0x27, 0x29, 0xbd, 0x3a, 0x75, 0x2c, 0x3d, 0xf0, 0xdd,
    0x1b, 0x04, 0xa2, 0xa1, 0xb5, 0x7e, 0x0c, 0x92, 0x24, 0x47, 0xa3, 0x66, 0x30, 0x64, 0x30, 0x0e,
    0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x01, 0x86, 0x30, 0x12,
    0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x08, 0x30, 0x06, 0x01, 0x01, 0xff, 0x02,
    0x01, 0x00, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xbc, 0xd4, 0xfd,
    0xe8, 0x80, 0x8a, 0x2d, 0xc9, 0x0b, 0x6d, 0x01, 0xa8, 0xc5, 0xb9, 0xb2, 0x47, 0x33, 0x7e, 0xbd,
    0xda, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x7a, 0xed,
    0x7d, 0x6d, 0xc6, 0xb7, 0x78, 0x9d, 0xb2, 0x38, 0x01, 0xa5, 0xe8, 0x4a, 0x8c, 0xb0, 0xa4, 0x0e,
    0x2a, 0x8c, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48,
    0x00, 0x30, 0x45, 0x02, 0x21, 0x00, 0xc5, 0x07, 0xb8, 0x2a, 0x7b, 0xf9, 0xa3, 0x3a, 0x1b, 0x78,
    0xdc, 0xeb, 0x01, 0xc9, 0x26, 0x92, 0x9e, 0xf3, 0x78, 0x3d, 0x46, 0x8e, 0x69, 0xa2, 0x84, 0xd3,
    0x6a, 0xba, 0xb9, 0x25, 0x1b, 0xef, 0x02, 0x20, 0x0e, 0x6d, 0x7f, 0x76, 0x8d, 0x65, 0xa7, 0x49,
    0xfa, 0x71, 0x2d, 0xda, 0x2b, 0x69, 0x25, 0x35, 0xcd, 0x57, 0x7d, 0x65, 0x01, 0x96, 0xa3, 0xd2,
    0xbf, 0x3b, 0x22, 0x78, 0x8e, 0x75, 0x41, 0x86
};

atcacert_cert_element_t grisp_cert_elements_signer[] = {
    {
        .id = "subject",
        .device_loc         = {
            .zone           = DEVZONE_NONE,
        },
        .cert_loc           = {
            .offset         = 158,
            .count          = 81,
        }
    }
};

const atcacert_def_t grisp_cert_def_signer = {
    .type                   = CERTTYPE_X509,
    .template_id            = 1,
    .chain_id               = 0,
    .private_key_slot       = 0,
    .sn_source              = SNSRC_PUB_KEY_HASH,
    .cert_sn_dev_loc        = {
        .zone               = DEVZONE_NONE,
        .slot               = 0,
        .is_genkey          = 0,
        .offset             = 0,
        .count              = 0,
    },
    .issue_date_format      = DATEFMT_RFC5280_UTC,
    .expire_date_format     = DATEFMT_RFC5280_GEN,
    .tbs_cert_loc           = {
        .offset             = 4,
        .count              = 430,
    },
    .expire_years           = 31,
    .public_key_dev_loc     = {
        .zone               = DEVZONE_DATA,
        .slot               = 11,
        .is_genkey          = 0,
        .offset             = 0,
        .count              = 72,
    },
    .comp_cert_dev_loc      = {
        .zone               = DEVZONE_DATA,
        .slot               = 12,
        .is_genkey          = 0,
        .offset             = 0,
        .count              = 72,
    },
    .std_cert_elements      = {
        {   // STDCERT_PUBLIC_KEY
            .offset         = 266,
            .count          = 64,
        },
        {   // STDCERT_SIGNATURE
            .offset         = 446,
            .count          = 64,
        },
        {   // STDCERT_ISSUE_DATE
            .offset         = 128,
            .count          = 13,
        },
        {   // STDCERT_EXPIRE_DATE
            .offset         = 143,
            .count          = 15,
        },
        {   // STDCERT_SIGNER_ID
            .offset         = 235,
            .count          = 4,
        },
        {   // STDCERT_CERT_SN
            .offset         = 15,
            .count          = 16,
        },
        {   // STDCERT_AUTH_KEY_ID
            .offset         = 414,
            .count          = 20,
        },
        {   // STDCERT_SUBJ_KEY_ID
            .offset         = 381,
            .count          = 20,
        }
    },
    .cert_elements          = grisp_cert_elements_signer,
    .cert_elements_count    = \
        sizeof(grisp_cert_elements_signer) / \
        sizeof(grisp_cert_elements_signer[0]),
    .cert_template          = grisp_cert_template_signer,
    .cert_template_size     = sizeof(grisp_cert_template_signer),
    .ca_cert_def            = NULL,
};

// TFLXTLS template, will be replaced by Stritzinger template
const uint8_t grisp_cert_template_device[500] = {
    0x30, 0x82, 0x01, 0xF0, 0x30, 0x82, 0x01, 0x97, 0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x10, 0x55,
    0xCE, 0x2E, 0x8F, 0xF6, 0x1C, 0x62, 0x50, 0xB7, 0xE1, 0x68, 0x03, 0x54, 0x14, 0x1C, 0x94, 0x30,
    0x0A, 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02, 0x30, 0x4F, 0x31, 0x21, 0x30,
    0x1F, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x0C, 0x18, 0x4D, 0x69, 0x63, 0x72, 0x6F, 0x63, 0x68, 0x69,
    0x70, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6E, 0x6F, 0x6C, 0x6F, 0x67, 0x79, 0x20, 0x49, 0x6E, 0x63,
    0x31, 0x2A, 0x30, 0x28, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x21, 0x43, 0x72, 0x79, 0x70, 0x74,
    0x6F, 0x20, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6E, 0x74, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E,
    0x20, 0x53, 0x69, 0x67, 0x6E, 0x65, 0x72, 0x20, 0x46, 0x46, 0x46, 0x46, 0x30, 0x20, 0x17, 0x0D,
    0x31, 0x38, 0x31, 0x31, 0x30, 0x38, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x5A, 0x18, 0x0F, 0x32,
    0x30, 0x34, 0x36, 0x31, 0x31, 0x30, 0x38, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x5A, 0x30, 0x42,
    0x31, 0x21, 0x30, 0x1F, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x0C, 0x18, 0x4D, 0x69, 0x63, 0x72, 0x6F,
    0x63, 0x68, 0x69, 0x70, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6E, 0x6F, 0x6C, 0x6F, 0x67, 0x79, 0x20,
    0x49, 0x6E, 0x63, 0x31, 0x1D, 0x30, 0x1B, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x14, 0x73, 0x6E,
    0x30, 0x31, 0x32, 0x33, 0x30, 0x31, 0x30, 0x32, 0x30, 0x33, 0x30, 0x34, 0x30, 0x35, 0x30, 0x36,
    0x30, 0x31, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01, 0x06,
    0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x71, 0xF1, 0xA7,
    0x0D, 0xA3, 0x79, 0xA3, 0xFD, 0xED, 0x6B, 0x50, 0x10, 0xBD, 0xAD, 0x6E, 0x1F, 0xB9, 0xE8, 0xEB,
    0xA7, 0xDF, 0x2C, 0x4B, 0x5C, 0x67, 0xD3, 0x5E, 0xBA, 0x84, 0xDA, 0x09, 0xE7, 0x7A, 0xE8, 0xDB,
    0x2C, 0xCB, 0x96, 0x28, 0xEE, 0xEB, 0x85, 0xCD, 0xAA, 0xB3, 0x5C, 0x92, 0xE5, 0x3E, 0x1C, 0x44,
    0xD5, 0x5A, 0x2B, 0xA7, 0xA0, 0x24, 0xAA, 0x92, 0x60, 0x3B, 0x68, 0x94, 0x8A, 0xA3, 0x60, 0x30,
    0x5E, 0x30, 0x0C, 0x06, 0x03, 0x55, 0x1D, 0x13, 0x01, 0x01, 0xFF, 0x04, 0x02, 0x30, 0x00, 0x30,
    0x0E, 0x06, 0x03, 0x55, 0x1D, 0x0F, 0x01, 0x01, 0xFF, 0x04, 0x04, 0x03, 0x02, 0x03, 0x88, 0x30,
    0x1D, 0x06, 0x03, 0x55, 0x1D, 0x0E, 0x04, 0x16, 0x04, 0x14, 0x1A, 0x90, 0xB2, 0x22, 0x37, 0xA4,
    0x51, 0xB7, 0x57, 0xDD, 0x36, 0xD1, 0x3A, 0x85, 0x2B, 0xE1, 0x3D, 0x2E, 0xF2, 0xCA, 0x30, 0x1F,
    0x06, 0x03, 0x55, 0x1D, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xBC, 0xD4, 0xFD, 0xE8, 0x80,
    0x8A, 0x2D, 0xC9, 0x0B, 0x6D, 0x01, 0xA8, 0xC5, 0xB9, 0xB2, 0x47, 0x33, 0x7E, 0xBD, 0xDA, 0x30,
    0x0A, 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02, 0x03, 0x47, 0x00, 0x30, 0x44,
    0x02, 0x20, 0x56, 0x73, 0x96, 0xE4, 0x9C, 0x0A, 0xA7, 0x4E, 0x61, 0x60, 0x12, 0xE3, 0x8A, 0x60,
    0xC3, 0xA8, 0x11, 0x09, 0x7D, 0x9C, 0x5D, 0xA4, 0xCD, 0x37, 0x89, 0xC3, 0x62, 0x96, 0x88, 0x7E,
    0x2A, 0x0C, 0x02, 0x20, 0x1E, 0x69, 0xB2, 0xAF, 0x0A, 0xD6, 0xC6, 0x7E, 0xE1, 0x2D, 0x5D, 0xBE,
    0x5A, 0x44, 0x5A, 0xD9, 0x1D, 0xF1, 0xA5, 0x98, 0x35, 0x8E, 0xD0, 0x69, 0xD9, 0x8B, 0xD7, 0xDB,
    0xB2, 0x99, 0xCC, 0x34
};

const atcacert_cert_element_t grisp_cert_elements_device[] = {
    {
        .id                 = "SN03",
        .device_loc         = {
            .zone           = DEVZONE_CONFIG,
            .slot           = 0,
            .is_genkey      = 0,
            .offset         = 0,
            .count          = 4,
        },
        .cert_loc ={
            .offset         = 208,
            .count          = 8,
        },
        .transforms         = {
            TF_BIN2HEX_UC,
            TF_NONE,
        }
    },
    {
        .id                 = "SN48",
        .device_loc         = {
            .zone           = DEVZONE_CONFIG,
            .slot           = 0,
            .is_genkey      = 0,
            .offset         = 8,
            .count          = 5,
        },
        .cert_loc           = {
            .offset         = 216,
            .count          = 10,
        },
        .transforms         = {
            TF_BIN2HEX_UC,
            TF_NONE,
        }
    }
};

const atcacert_def_t grisp_cert_def_device = {
    .type                   = CERTTYPE_X509,
    .template_id            = 3,
    .chain_id               = 0,
    .private_key_slot       = 0,
    .sn_source              = SNSRC_PUB_KEY_HASH,
    .cert_sn_dev_loc        = {
        .zone               = DEVZONE_NONE,
        .slot               = 0,
        .is_genkey          = 0,
        .offset             = 0,
        .count              = 0,
    },
    .issue_date_format      = DATEFMT_RFC5280_UTC,
    .expire_date_format     = DATEFMT_RFC5280_GEN,
    .tbs_cert_loc           = {
        .offset             = 4,
        .count              = 411,
    },
    .expire_years           = 28,
    .public_key_dev_loc     = {
        .zone               = DEVZONE_DATA,
        .slot               = 0,
        .is_genkey          = 1,
        .offset             = 0,
        .count              = 64,
    },
    .comp_cert_dev_loc      = {
        .zone               = DEVZONE_DATA,
        .slot               = 10,
        .is_genkey          = 0,
        .offset             = 0,
        .count              = 72,
    },
    .std_cert_elements      = {
        {   // STDCERT_PUBLIC_KEY
            .offset         = 253,
            .count          = 64,
        },
        {   // STDCERT_SIGNATURE
            .offset         = 427,
            .count          = 64,
        },
        {   // STDCERT_ISSUE_DATE
            .offset         = 128,
            .count          = 13,
        },
        {   // STDCERT_EXPIRE_DATE
            .offset         = 143,
            .count          = 15,
        },
        {   // STDCERT_SIGNER_ID
            .offset         = 120,
            .count          = 4,
        },
        {   // STDCERT_CERT_SN
            .offset         = 15,
            .count          = 16,
        },
        {   // STDCERT_AUTH_KEY_ID
            .offset         = 395,
            .count          = 20,
        },
        {   // STDCERT_SUBJ_KEY_ID
            .offset         = 362,
            .count          = 20,
        }
    },
    .cert_elements          = grisp_cert_elements_device,
    .cert_elements_count    = \
        sizeof(grisp_cert_elements_device) / \
        sizeof(grisp_cert_elements_device[0]),
    .cert_template          = grisp_cert_template_device,
    .cert_template_size     = sizeof(grisp_cert_template_device),
    .ca_cert_def            = &grisp_cert_def_signer,
};


#define CONFIG_DEVICE_TYPE_KEY "type"
#define CONFIG_I2C_BUS_KEY "i2c_bus"
#define CONFIG_I2C_ADDRESS_KEY "i2c_address"

/* Helpers, don't use these directly */
#define EXEC_CA_FUN_STATUS(STATUS, fun, args...) { \
    ATCA_STATUS STATUS = fun(args); \
    if (STATUS != ATCA_SUCCESS) \
        return MK_ERROR_STATUS(env, #fun, STATUS); \
    }
#define EXEC_CA_CERT_FUN_STATUS(STATUS, fun, args...) { \
    int STATUS = fun(args); \
    if (STATUS != ATCACERT_E_SUCCESS) \
        return MK_ERROR_STATUS(env, #fun, STATUS); \
    }
#define UNIQ_CA_STATUS __func__##__LINE__##_status

/* Execute atcab_* functions */
#define EXEC_CA_FUN(fun, args...) EXEC_CA_FUN_STATUS(UNIQ_CA_STATUS, fun, args)
/* Execute atcacert_* functions */
#define EXEC_CA_CERT_FUN(fun, args...) EXEC_CA_CERT_FUN_STATUS(UNIQ_CA_STATUS, fun, args)
/* Init device, call before other API calls */
#define INIT_CA_FUN { \
    ATCAIfaceCfg ATCAB_CONFIG = grisp_atcab_default_config; \
    build_atcab_config(env, &ATCAB_CONFIG, argv[0]); \
    EXEC_CA_FUN(atcab_init, &ATCAB_CONFIG) \
    }

/* Return value macros */
#define MK_OK(env) mk_atom(env, "ok")
#define MK_ERROR(env, msg) enif_make_tuple2(env, mk_atom(env, "error"), mk_atom(env, msg))
#define MK_ERROR_STATUS(env, msg, status) enif_make_tuple3(env, mk_atom(env, "error"), mk_atom(env, msg), enif_make_int(env, status))
#define MK_SUCCESS(env, term) enif_make_tuple2(env, mk_atom(env, "ok"), term)
#define MK_SUCCESS_ATOM(env, msg) enif_make_tuple2(env, mk_atom(env, "ok"), mk_atom(env, msg))

#define BINARY_FROM_RAW(env, bin_term, raw, size) memcpy(enif_make_new_binary(env, size, &bin_term), raw, size)


struct device_type_nif {
    ATCADeviceType type;
    const char *name;
};


static ERL_NIF_TERM mk_atom(ErlNifEnv* env, const char* atom)
{
    ERL_NIF_TERM ret;

    if (!enif_make_existing_atom(env, atom, &ret, ERL_NIF_LATIN1))
        return enif_make_atom(env, atom);

    return ret;
}


static void build_atcab_config(ErlNifEnv* env, ATCAIfaceCfg *atcab_config, ERL_NIF_TERM config_map)
{
    int i2c_bus, i2c_address;
    bool device_type_present, i2c_bus_present, i2c_address_present;
    ERL_NIF_TERM device_type_value, i2c_bus_value, i2c_address_value;

    ERL_NIF_TERM device_type_key = mk_atom(env, CONFIG_DEVICE_TYPE_KEY);
    ERL_NIF_TERM i2c_bus_key = mk_atom(env, CONFIG_I2C_BUS_KEY);
    ERL_NIF_TERM i2c_address_key = mk_atom(env, CONFIG_I2C_ADDRESS_KEY);

    device_type_present = enif_get_map_value(env, config_map, device_type_key, &device_type_value);
    i2c_bus_present = enif_get_map_value(env, config_map, i2c_bus_key, &i2c_bus_value);
    i2c_address_present = enif_get_map_value(env, config_map, i2c_address_key, &i2c_address_value);

    if (i2c_bus_present) {
        enif_get_int(env, i2c_bus_value, &i2c_bus);
        atcab_config->atcai2c.bus = (uint16_t) i2c_bus;
    }
    if (i2c_address_present) {
        enif_get_int(env, i2c_address_value, &i2c_address);
        atcab_config->atcai2c.address = (uint16_t) i2c_address;
    }
    if (device_type_present) {
        if (enif_compare(mk_atom(env, "ATECC508A"), device_type_value))
            atcab_config->devtype = ATECC508A;
        if (enif_compare(mk_atom(env, "ATECC608A"), device_type_value))
            atcab_config->devtype = ATECC608A;
        if (enif_compare(mk_atom(env, "ATECC608B"), device_type_value))
            atcab_config->devtype = ATECC608B;
    }
}


static ERL_NIF_TERM device_info_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    const struct device_type_nif types[] =
    { { ATECC508A, "ATECC508A" },
      { ATECC608A, "ATECC608A" },
      { ATECC608B, "ATECC608B" } };

    ATCADeviceType dt = atcab_get_device_type();
    char* name = NULL;

    for (size_t i = 0; i < sizeof(types)/sizeof(struct device_type_nif); ++i) {
        if (types[i].type == dt) {
            name = (char *) types[i].name;
            break;
        }
    }

    return name ? MK_SUCCESS_ATOM(env, name) : MK_ERROR(env, "unknown_device");
}


static ERL_NIF_TERM config_locked_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    bool is_locked = false;
    EXEC_CA_FUN(atcab_is_config_locked, &is_locked);

    return MK_SUCCESS_ATOM(env, is_locked ? "true" : "false");
}


static ERL_NIF_TERM data_locked_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    bool is_locked = false;
    EXEC_CA_FUN(atcab_is_data_locked, &is_locked);

    return MK_SUCCESS_ATOM(env, is_locked ? "true" : "false");
}


static ERL_NIF_TERM slot_locked_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;
    bool is_locked = false;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    EXEC_CA_FUN(atcab_is_slot_locked, (uint16_t) slot_idx, &is_locked);

    return MK_SUCCESS_ATOM(env, is_locked ? "true" : "false");
}


static ERL_NIF_TERM serial_number_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    uint8_t sn[9];
    EXEC_CA_FUN(atcab_read_serial_number, sn);

    ERL_NIF_TERM bin_sn;
    BINARY_FROM_RAW(env, bin_sn, sn, 9);

    return MK_SUCCESS(env, bin_sn);
}

static ERL_NIF_TERM read_config_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    uint8_t config_zone[ATCA_ECC_CONFIG_SIZE];
    EXEC_CA_FUN(atcab_read_config_zone, config_zone);

    ERL_NIF_TERM bin_config_zone;
    BINARY_FROM_RAW(env, bin_config_zone, config_zone, ATCA_ECC_CONFIG_SIZE);

    return MK_SUCCESS(env, bin_config_zone);
}


static ERL_NIF_TERM write_config_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    EXEC_CA_FUN(atcab_write_config_zone, grisp_device_default_config);

    return MK_OK(env);
}


static ERL_NIF_TERM lock_config_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    EXEC_CA_FUN(atcab_lock_config_zone);

    return MK_OK(env);
}


static ERL_NIF_TERM lock_data_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    EXEC_CA_FUN(atcab_lock_data_zone);

    return MK_OK(env);
}


static ERL_NIF_TERM lock_slot_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    EXEC_CA_FUN(atcab_lock_data_slot, (uint16_t) slot_idx);

    return MK_OK(env);
}


static ERL_NIF_TERM gen_private_key_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    uint8_t pubkey[ATCA_ECCP256_PUBKEY_SIZE];
    EXEC_CA_FUN(atcab_genkey, (uint16_t) slot_idx, pubkey);

    ERL_NIF_TERM bin_pubkey;
    BINARY_FROM_RAW(env, bin_pubkey, pubkey, ATCA_ECCP256_PUBKEY_SIZE);

    return MK_SUCCESS(env, bin_pubkey);
}


static ERL_NIF_TERM gen_public_key_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    uint8_t pubkey[ATCA_ECCP256_PUBKEY_SIZE];
    EXEC_CA_FUN(atcab_get_pubkey, (uint16_t) slot_idx, pubkey);

    ERL_NIF_TERM bin_pubkey;
    BINARY_FROM_RAW(env, bin_pubkey, pubkey, ATCA_ECCP256_PUBKEY_SIZE);

    return MK_SUCCESS(env, bin_pubkey);
}


static ERL_NIF_TERM sign_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;
    ErlNifBinary bin_msg;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[2], &bin_msg) || (bin_msg.size != ATCA_SHA256_DIGEST_SIZE))
        return enif_make_badarg(env);

    uint8_t sig[ATCA_ECCP256_SIG_SIZE];
    EXEC_CA_FUN(atcab_sign, (uint16_t) slot_idx, (uint8_t *) bin_msg.data, sig);

    ERL_NIF_TERM bin_sig;
    BINARY_FROM_RAW(env, bin_sig, sig, ATCA_ECCP256_SIG_SIZE);

    return MK_SUCCESS(env, bin_sig);
}


static ERL_NIF_TERM verify_extern_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    ErlNifBinary bin_pubkey;
    ErlNifBinary bin_msg;
    ErlNifBinary bin_sig;

    if (!enif_inspect_binary(env, argv[1], &bin_pubkey) || (bin_pubkey.size != ATCA_ECCP256_PUBKEY_SIZE))
        return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[2], &bin_msg) || (bin_msg.size != ATCA_SHA256_DIGEST_SIZE))
        return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[3], &bin_sig) || (bin_sig.size != ATCA_ECCP256_SIG_SIZE))
        return enif_make_badarg(env);

    bool is_verified;
    EXEC_CA_FUN(atcab_verify_extern, (uint8_t *) bin_msg.data, (uint8_t *) bin_sig.data, (uint8_t *) bin_pubkey.data, &is_verified);

    return MK_SUCCESS_ATOM(env, is_verified ? "true" : "false");
}


static ERL_NIF_TERM verify_stored_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int slot_idx;
    ErlNifBinary bin_msg;
    ErlNifBinary bin_sig;

    if (!enif_get_int(env, argv[1], &slot_idx))
	    return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[2], &bin_msg) || (bin_msg.size != ATCA_SHA256_DIGEST_SIZE))
        return enif_make_badarg(env);

    if (!enif_inspect_binary(env, argv[3], &bin_sig) || (bin_msg.size != ATCA_ECCP256_SIG_SIZE))
        return enif_make_badarg(env);

    bool is_verified;
    EXEC_CA_FUN(atcab_verify_stored, (uint8_t *) bin_msg.data, (uint8_t *) bin_sig.data, (uint16_t) slot_idx, &is_verified);

    return MK_SUCCESS_ATOM(env, is_verified ? "true" : "false");
}


// will be replaces with Stritzinger CA root certificate (public key might be enough)
const uint8_t g_cryptoauth_root_ca_002_cert[501] = {
    0x30, 0x82, 0x01, 0xf1, 0x30, 0x82, 0x01, 0x97, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x10, 0x77, 0xd3, 0x6d, 0x95, 0x6e, 0xc8, 0xae, 0x62, 0x05,
    0xe5, 0x8e, 0x3a, 0xcb, 0x98, 0x5a, 0x81, 0x30, 0x0a, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x4f, 0x31, 0x21, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x18, 0x4d, 0x69, 0x63, 0x72,
    0x6f, 0x63, 0x68, 0x69, 0x70, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6e, 0x6f,
    0x6c, 0x6f, 0x67, 0x79, 0x20, 0x49, 0x6e, 0x63, 0x31, 0x2a, 0x30, 0x28,
    0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x21, 0x43, 0x72, 0x79, 0x70, 0x74,
    0x6f, 0x20, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6e, 0x74, 0x69, 0x63, 0x61,
    0x74, 0x69, 0x6f, 0x6e, 0x20, 0x52, 0x6f, 0x6f, 0x74, 0x20, 0x43, 0x41,
    0x20, 0x30, 0x30, 0x32, 0x30, 0x20, 0x17, 0x0d, 0x31, 0x38, 0x31, 0x31,
    0x30, 0x38, 0x31, 0x39, 0x31, 0x32, 0x31, 0x39, 0x5a, 0x18, 0x0f, 0x32,
    0x30, 0x35, 0x38, 0x31, 0x31, 0x30, 0x38, 0x31, 0x39, 0x31, 0x32, 0x31,
    0x39, 0x5a, 0x30, 0x4f, 0x31, 0x21, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x04,
    0x0a, 0x0c, 0x18, 0x4d, 0x69, 0x63, 0x72, 0x6f, 0x63, 0x68, 0x69, 0x70,
    0x20, 0x54, 0x65, 0x63, 0x68, 0x6e, 0x6f, 0x6c, 0x6f, 0x67, 0x79, 0x20,
    0x49, 0x6e, 0x63, 0x31, 0x2a, 0x30, 0x28, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x21, 0x43, 0x72, 0x79, 0x70, 0x74, 0x6f, 0x20, 0x41, 0x75, 0x74,
    0x68, 0x65, 0x6e, 0x74, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x20,
    0x52, 0x6f, 0x6f, 0x74, 0x20, 0x43, 0x41, 0x20, 0x30, 0x30, 0x32, 0x30,
    0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42,
    0x00, 0x04, 0xbd, 0x54, 0xe6, 0x6d, 0xe3, 0x87, 0x54, 0x84, 0x00, 0x6b,
    0x53, 0xae, 0x15, 0x80, 0xd5, 0x0a, 0xa0, 0x69, 0xe7, 0x8a, 0xdf, 0x55,
    0x78, 0xd8, 0x5c, 0xe2, 0xd5, 0x4d, 0xd5, 0xb8, 0x30, 0x29, 0x6b, 0xff,
    0xdd, 0x6e, 0x6f, 0x72, 0x56, 0xfb, 0xd9, 0x9e, 0xf1, 0xa1, 0x16, 0xb1,
    0x1d, 0x33, 0xad, 0x49, 0x10, 0x3a, 0xa1, 0x85, 0x87, 0x39, 0xdc, 0xfa,
    0xe4, 0x37, 0xe1, 0x9d, 0x63, 0x4e, 0xa3, 0x53, 0x30, 0x51, 0x30, 0x1d,
    0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x7a, 0xed, 0x7d,
    0x6d, 0xc6, 0xb7, 0x78, 0x9d, 0xb2, 0x38, 0x01, 0xa5, 0xe8, 0x4a, 0x8c,
    0xb0, 0xa4, 0x0e, 0x2a, 0x8c, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23,
    0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x7a, 0xed, 0x7d, 0x6d, 0xc6, 0xb7,
    0x78, 0x9d, 0xb2, 0x38, 0x01, 0xa5, 0xe8, 0x4a, 0x8c, 0xb0, 0xa4, 0x0e,
    0x2a, 0x8c, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff,
    0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0a, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45,
    0x02, 0x21, 0x00, 0xa1, 0xdc, 0x63, 0x45, 0x90, 0xec, 0x81, 0x9e, 0xe1,
    0xde, 0x5b, 0x81, 0x12, 0x65, 0x51, 0xad, 0xd4, 0xc2, 0xc4, 0xf8, 0xe5,
    0x95, 0x28, 0x2e, 0xe0, 0x4b, 0xe7, 0x68, 0xec, 0x7c, 0x02, 0x73, 0x02,
    0x20, 0x3e, 0x6b, 0xa7, 0x4e, 0x9e, 0x4c, 0x0a, 0xd6, 0x8c, 0x24, 0xb0,
    0xfb, 0x2e, 0xe7, 0x93, 0xd2, 0xe6, 0xbe, 0x94, 0x65, 0xca, 0x15, 0xd0,
    0xea, 0x5b, 0xc8, 0x7f, 0x55, 0x79, 0x99, 0x5c, 0xad
};


static ERL_NIF_TERM gen_cert_signer_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int ret;
    const atcacert_def_t* cert_def = &grisp_cert_def_signer;
    const uint8_t* ca_public_key = &g_cryptoauth_root_ca_002_cert[266];
    uint8_t cert[1024];
    size_t cert_size = sizeof(cert);

    EXEC_CA_CERT_FUN(atcacert_read_cert, cert_def, ca_public_key, cert, &cert_size);

    ERL_NIF_TERM bin_cert;
    BINARY_FROM_RAW(env, bin_cert, cert, cert_size);

    return MK_SUCCESS(env, bin_cert);
}


static ERL_NIF_TERM gen_cert_device_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    INIT_CA_FUN;

    int ret;
    ErlNifBinary bin_signer_cert;
    const atcacert_def_t* cert_def = &grisp_cert_def_device;
    uint8_t ca_public_key[72];
    uint8_t cert[1024];
    size_t cert_size = sizeof(cert);

    if (!enif_inspect_binary(env, argv[1], &bin_signer_cert))
        return enif_make_badarg(env);

    EXEC_CA_CERT_FUN(atcacert_get_subj_public_key, &grisp_cert_def_signer, (uint8_t *) bin_signer_cert.data, 1024, ca_public_key);

    EXEC_CA_CERT_FUN(atcacert_read_cert, cert_def, ca_public_key, cert, &cert_size);

    ERL_NIF_TERM bin_cert;
    BINARY_FROM_RAW(env, bin_cert, cert, cert_size);

    return MK_SUCCESS(env, bin_cert);
}


static ErlNifFunc nif_funcs[] = {
    {"device_info",     1, device_info_nif},
    {"config_locked",   1, config_locked_nif},
    {"data_locked",     1, data_locked_nif},
    {"slot_locked",     2, slot_locked_nif},
    {"serial_number",   1, serial_number_nif},
    {"read_config",     1, read_config_nif},
    {"write_config",    1, write_config_nif},
    {"lock_config",     1, lock_config_nif},
    {"lock_data",       1, lock_data_nif},
    {"lock_slot",       2, lock_slot_nif},
    {"gen_private_key", 2, gen_private_key_nif},
    {"gen_public_key",  2, gen_public_key_nif},
    {"sign",            3, sign_nif},
    {"verify_extern",   4, verify_extern_nif},
    {"verify_stored",   4, verify_stored_nif},
    {"gen_cert_signer", 1, gen_cert_signer_nif},
    {"gen_cert_device", 2, gen_cert_device_nif},
};

ERL_NIF_INIT(grisp_cryptoauth_nif, nif_funcs, NULL, NULL, NULL, NULL);
