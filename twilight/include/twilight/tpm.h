#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TPM_VAULT_MAX_PLAINTEXT 480u
#define TPM_SHA256_DIGEST_SIZE 32u

struct tpm_status {
    bool detected;
    bool transport_ready;
    bool root_of_trust_established;
    bool pcr_policy_bound;
    bool vault_ready;
    bool integrity_failure;
    uint32_t start_method;
    uint32_t last_response_code;
    uint64_t control_area_physical;
};

struct tpm_protected_blob {
    uint32_t magic;
    uint16_t version;
    uint16_t plaintext_length;
    uint8_t iv[16];
    uint8_t ciphertext[TPM_VAULT_MAX_PLAINTEXT];
    uint8_t authentication_tag[TPM_SHA256_DIGEST_SIZE];
};

bool tpm_init(void);
void tpm_get_status(struct tpm_status *out);
bool tpm_get_random(void *buffer, size_t size);

/*
 * Establish Twilight's TPM-backed Root of Trust. The caller supplies SHA-256
 * measurements of immutable kernel code/data and of the active security
 * policy. Twilight extends those into PCR 11 and PCR 12 and binds its TPM
 * vault keys to PCR 7+11+12. PCR 7 remains firmware-owned and normally
 * reflects the platform Secure Boot policy on UEFI systems.
 */
bool tpm_establish_root_of_trust(
    const uint8_t kernel_measurement[TPM_SHA256_DIGEST_SIZE],
    const uint8_t security_policy_measurement[TPM_SHA256_DIGEST_SIZE]);

/*
 * Authenticated, PCR-policy-bound per-boot kernel secret protection. AES and
 * HMAC key material remains inside the TPM. Objects become unusable if the
 * bound PCR state changes. Current objects are intentionally ephemeral across
 * reboot; persistent sealed storage can be layered on later without weakening
 * this runtime vault.
 */
bool tpm_vault_protect(const void *plaintext,
                       size_t size,
                       struct tpm_protected_blob *out);
bool tpm_vault_unprotect(const struct tpm_protected_blob *blob,
                         void *plaintext,
                         size_t capacity,
                         size_t *size_out);
bool tpm_vault_self_test(void);
