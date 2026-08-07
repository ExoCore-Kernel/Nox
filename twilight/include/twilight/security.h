#pragma once

#include <stdbool.h>

/*
 * Compute Twilight's immutable-kernel and security-policy measurements and
 * bind the TPM vault to the resulting PCR state. tpm_init() must have already
 * established a transport.
 */
bool security_establish_tpm_root_of_trust(void);
