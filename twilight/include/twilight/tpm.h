#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TPM_VAULT_MAX_PLAINTEXT 480u

struct tpm_status {
    bool detected;
    bool transport_ready;
    bool vault_ready;
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
};

bool tpm_init(void);
void tpm_get_status(struct tpm_status *out);
bool tpm_get_random(void *buffer, size_t size);

/*
 * Per-boot confidentiality using an AES key that stays inside the TPM.
 * The current v1 format is not persistent across reboot and is not yet an
 * authenticated-encryption format; callers must not treat it as tamper proof.
 */
bool tpm_vault_protect(const void *plaintext,
                       size_t size,
                       struct tpm_protected_blob *out);
bool tpm_vault_unprotect(const struct tpm_protected_blob *blob,
                         void *plaintext,
                         size_t capacity,
                         size_t *size_out);
bool tpm_vault_self_test(void);
