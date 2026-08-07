#include <stddef.h>
#include <stdint.h>

#include <twilight/log.h>
#include <twilight/security.h>
#include <twilight/sha256.h>
#include <twilight/tpm.h>

extern const uint8_t __twilight_text_start[];
extern const uint8_t __twilight_text_end[];
extern const uint8_t __twilight_rodata_start[];
extern const uint8_t __twilight_rodata_end[];

static const char security_policy_descriptor[] =
    "Twilight.SecurityPolicy.v3|"
    "separate-user-CR3|CPL3|W^X|NX|no-user-IO|"
    "CR0.WP|required|SMEP-if-supported|SMAP-if-supported|UMIP-if-supported|"
    "TPM2-PCR7+PCR11+PCR12|"
    "policy-only-TPM-keys|AES128-CFB|HMAC-SHA256|"
    "fail-closed-on-PCR-policy-loss";

static void secure_zero(void *pointer, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool security_establish_tpm_root_of_trust(void) {
    if (!sha256_self_test()) return false;

    const size_t text_size = (size_t)(__twilight_text_end - __twilight_text_start);
    const size_t rodata_size = (size_t)(__twilight_rodata_end - __twilight_rodata_start);
    if (text_size == 0 || rodata_size == 0) return false;

    uint8_t kernel_measurement[SHA256_DIGEST_SIZE];
    uint8_t policy_measurement[SHA256_DIGEST_SIZE];

    struct sha256_context kernel_hash;
    sha256_init(&kernel_hash);
    sha256_update(&kernel_hash, __twilight_text_start, text_size);
    sha256_update(&kernel_hash, __twilight_rodata_start, rodata_size);
    sha256_final(&kernel_hash, kernel_measurement);

    sha256(security_policy_descriptor,
           sizeof(security_policy_descriptor) - 1u,
           policy_measurement);

    const bool success = tpm_establish_root_of_trust(kernel_measurement,
                                                     policy_measurement);
    secure_zero(kernel_measurement, sizeof(kernel_measurement));
    secure_zero(policy_measurement, sizeof(policy_measurement));

    if (success) {
        struct tpm_status status;
        tpm_get_status(&status);
        if (status.persistent_hierarchy) {
            klog("TPM trust tier: Storage/Owner hierarchy active; measured policy keys can be recreated across reboot");
        } else {
            klog("TPM trust tier: Owner hierarchy unavailable; using TPM null hierarchy for per-boot secrets only");
        }
    }
    return success;
}
