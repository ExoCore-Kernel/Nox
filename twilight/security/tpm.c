#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/acpi.h>
#include <twilight/mmio.h>
#include <twilight/timer.h>
#include <twilight/tpm.h>

#define TPM2_START_METHOD_CRB 7u

#define CRB_LOC_STATE      0x00u
#define CRB_LOC_CTRL       0x08u
#define CRB_CTRL_REQ       0x40u
#define CRB_CTRL_STS       0x44u
#define CRB_CTRL_CANCEL    0x48u
#define CRB_CTRL_START     0x4cu
#define CRB_CTRL_CMD_SIZE  0x58u
#define CRB_CTRL_CMD_LOW   0x5cu
#define CRB_CTRL_CMD_HIGH  0x60u
#define CRB_CTRL_RSP_SIZE  0x64u
#define CRB_CTRL_RSP_LOW   0x68u
#define CRB_CTRL_RSP_HIGH  0x6cu

#define CRB_LOC_REQUEST_ACCESS (1u << 0)
#define CRB_LOC_ASSIGNED       (1u << 1)
#define CRB_LOC_VALID          (1u << 7)
#define CRB_CTRL_REQ_READY     (1u << 0)
#define CRB_CTRL_STS_ERROR     (1u << 0)
#define CRB_CTRL_START_INVOKE  (1u << 0)

#define TPM_ST_NO_SESSIONS 0x8001u
#define TPM_ST_SESSIONS    0x8002u
#define TPM_RC_SUCCESS     0u

#define TPM_RH_OWNER 0x40000001u
#define TPM_RH_NULL  0x40000007u
#define TPM_RS_PW    0x40000009u

#define TPM_SE_POLICY 0x01u
#define TPMA_SESSION_CONTINUE 0x01u

#define TPM_ALG_HMAC      0x0005u
#define TPM_ALG_AES       0x0006u
#define TPM_ALG_KEYEDHASH 0x0008u
#define TPM_ALG_SHA256    0x000bu
#define TPM_ALG_NULL      0x0010u
#define TPM_ALG_SYMCIPHER 0x0025u
#define TPM_ALG_CFB       0x0043u

#define TPM_CC_CREATE_PRIMARY     0x00000131u
#define TPM_CC_HMAC               0x00000155u
#define TPM_CC_ENCRYPT_DECRYPT    0x00000164u
#define TPM_CC_FLUSH_CONTEXT      0x00000165u
#define TPM_CC_START_AUTH_SESSION 0x00000176u
#define TPM_CC_GET_RANDOM         0x0000017bu
#define TPM_CC_POLICY_PCR         0x0000017fu
#define TPM_CC_PCR_EXTEND         0x00000182u
#define TPM_CC_POLICY_GET_DIGEST  0x00000189u
#define TPM_CC_ENCRYPT_DECRYPT2   0x00000193u

#define TPMA_OBJECT_FIXED_TPM             (1u << 1)
#define TPMA_OBJECT_FIXED_PARENT          (1u << 4)
#define TPMA_OBJECT_SENSITIVE_DATA_ORIGIN (1u << 5)
#define TPMA_OBJECT_ADMIN_WITH_POLICY     (1u << 7)
#define TPMA_OBJECT_NO_DA                 (1u << 10)
#define TPMA_OBJECT_DECRYPT               (1u << 17)
#define TPMA_OBJECT_SIGN_ENCRYPT          (1u << 18)

#define TPM_SYM_POLICY_ATTRIBUTES \
    (TPMA_OBJECT_FIXED_TPM | TPMA_OBJECT_FIXED_PARENT | \
     TPMA_OBJECT_SENSITIVE_DATA_ORIGIN | TPMA_OBJECT_ADMIN_WITH_POLICY | \
     TPMA_OBJECT_NO_DA | TPMA_OBJECT_DECRYPT | TPMA_OBJECT_SIGN_ENCRYPT)

#define TPM_HMAC_POLICY_ATTRIBUTES \
    (TPMA_OBJECT_FIXED_TPM | TPMA_OBJECT_FIXED_PARENT | \
     TPMA_OBJECT_SENSITIVE_DATA_ORIGIN | TPMA_OBJECT_ADMIN_WITH_POLICY | \
     TPMA_OBJECT_NO_DA | TPMA_OBJECT_SIGN_ENCRYPT)

#define TPM_PCR_TWILIGHT_KERNEL 11u
#define TPM_PCR_TWILIGHT_POLICY 12u

#define TPM_VAULT_MAGIC 0x54564c54u
#define TPM_VAULT_VERSION 2u

#define TPM_COMMAND_TIMEOUT_MS 30000u
#define TPM_READY_TIMEOUT_MS   2000u
#define TPM_LOCALITY_TIMEOUT_MS 2000u

struct __attribute__((packed)) acpi_tpm2_table {
    struct acpi_sdt_header header;
    uint16_t platform_class;
    uint16_t reserved;
    uint64_t control_area_address;
    uint32_t start_method;
    uint8_t start_method_parameters[12];
};

struct command_writer {
    uint8_t *buffer;
    size_t capacity;
    size_t position;
    bool failed;
};

static struct tpm_status status;
static volatile uint8_t *crb_registers;
static volatile uint8_t *command_buffer;
static volatile uint8_t *response_buffer;
static void *crb_mapping;
static void *command_mapping;
static void *response_mapping;
static bool response_shares_command_mapping;
static uint32_t command_capacity;
static uint32_t response_capacity;
static uint32_t vault_encryption_handle;
static uint32_t vault_hmac_handle;
static bool root_attempted;

static void zero_bytes(void *pointer, size_t size) {
    volatile uint8_t *bytes = (volatile uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static bool bytes_equal(const void *a, const void *b, size_t size) {
    const uint8_t *aa = (const uint8_t *)a;
    const uint8_t *bb = (const uint8_t *)b;
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= (uint8_t)(aa[i] ^ bb[i]);
    return difference == 0;
}

static uint16_t read_be16(const uint8_t *buffer) {
    return (uint16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
}

static uint32_t read_be32(const uint8_t *buffer) {
    return ((uint32_t)buffer[0] << 24) |
           ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) |
           (uint32_t)buffer[3];
}

static void write_be32_at(uint8_t *buffer, size_t offset, uint32_t value) {
    buffer[offset + 0] = (uint8_t)(value >> 24);
    buffer[offset + 1] = (uint8_t)(value >> 16);
    buffer[offset + 2] = (uint8_t)(value >> 8);
    buffer[offset + 3] = (uint8_t)value;
}

static void put_u8(struct command_writer *writer, uint8_t value) {
    if (writer->position >= writer->capacity) {
        writer->failed = true;
        return;
    }
    writer->buffer[writer->position++] = value;
}

static void put_u16(struct command_writer *writer, uint16_t value) {
    put_u8(writer, (uint8_t)(value >> 8));
    put_u8(writer, (uint8_t)value);
}

static void put_u32(struct command_writer *writer, uint32_t value) {
    put_u8(writer, (uint8_t)(value >> 24));
    put_u8(writer, (uint8_t)(value >> 16));
    put_u8(writer, (uint8_t)(value >> 8));
    put_u8(writer, (uint8_t)value);
}

static void put_bytes(struct command_writer *writer, const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) put_u8(writer, bytes[i]);
}

static uint32_t mmio_read32(size_t offset) {
    return *(volatile uint32_t *)(crb_registers + offset);
}

static void mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(crb_registers + offset) = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static bool wait_register(size_t offset, uint32_t mask, uint32_t expected, uint64_t timeout_ms) {
    const uint64_t hz = timer_frequency();
    const uint64_t start = timer_ticks();

    if (hz != 0) {
        uint64_t wait_ticks = (timeout_ms * hz + 999ull) / 1000ull;
        if (wait_ticks == 0) wait_ticks = 1;
        while ((timer_ticks() - start) < wait_ticks) {
            if ((mmio_read32(offset) & mask) == expected) return true;
            __asm__ volatile ("pause");
        }
        return (mmio_read32(offset) & mask) == expected;
    }

    for (uint64_t i = 0; i < 100000000ull; ++i) {
        if ((mmio_read32(offset) & mask) == expected) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void copy_to_device(volatile uint8_t *destination, const uint8_t *source, size_t size) {
    for (size_t i = 0; i < size; ++i) destination[i] = source[i];
}

static void copy_from_device(uint8_t *destination, const volatile uint8_t *source, size_t size) {
    for (size_t i = 0; i < size; ++i) destination[i] = source[i];
}

static bool crb_command_ready(void) {
    mmio_write32(CRB_CTRL_REQ, CRB_CTRL_REQ_READY);
    return wait_register(CRB_CTRL_REQ, CRB_CTRL_REQ_READY, 0, TPM_READY_TIMEOUT_MS);
}

static bool crb_execute(const uint8_t *command,
                        size_t command_size,
                        uint8_t *response,
                        size_t response_size,
                        size_t *actual_response_size) {
    if (!status.transport_ready || command == 0 || response == 0) return false;
    if (command_size < 10 || command_size > command_capacity) return false;
    if (response_size < 10 || response_capacity < 10) return false;

    if (!crb_command_ready()) return false;
    mmio_write32(CRB_CTRL_CANCEL, 0);
    copy_to_device(command_buffer, command, command_size);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    mmio_write32(CRB_CTRL_START, CRB_CTRL_START_INVOKE);
    if (!wait_register(CRB_CTRL_START, CRB_CTRL_START_INVOKE, 0, TPM_COMMAND_TIMEOUT_MS)) {
        return false;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((mmio_read32(CRB_CTRL_STS) & CRB_CTRL_STS_ERROR) != 0) return false;

    uint8_t header[10];
    copy_from_device(header, response_buffer, sizeof(header));
    const uint32_t expected = read_be32(&header[2]);
    if (expected < sizeof(header) || expected > response_capacity || expected > response_size) {
        return false;
    }

    copy_from_device(response, response_buffer, expected);
    status.last_response_code = read_be32(&response[6]);
    if (actual_response_size != 0) *actual_response_size = expected;
    return true;
}

static void begin_command(struct command_writer *writer,
                          uint8_t *buffer,
                          size_t capacity,
                          uint16_t tag,
                          uint32_t command_code) {
    *writer = (struct command_writer){
        .buffer = buffer,
        .capacity = capacity,
        .position = 0,
        .failed = false,
    };
    put_u16(writer, tag);
    put_u32(writer, 0);
    put_u32(writer, command_code);
}

static bool finish_command(struct command_writer *writer) {
    if (writer->failed || writer->position > UINT32_MAX || writer->position < 10) return false;
    write_be32_at(writer->buffer, 2, (uint32_t)writer->position);
    return true;
}

static void put_empty_password_session(struct command_writer *writer) {
    put_u32(writer, 9u);
    put_u32(writer, TPM_RS_PW);
    put_u16(writer, 0);
    put_u8(writer, 0);
    put_u16(writer, 0);
}

static void put_policy_session_auth(struct command_writer *writer, uint32_t session_handle) {
    put_u32(writer, 9u);
    put_u32(writer, session_handle);
    put_u16(writer, 0);
    put_u8(writer, TPMA_SESSION_CONTINUE);
    put_u16(writer, 0);
}

static void put_root_pcr_selection(struct command_writer *writer) {
    put_u32(writer, 1u);
    put_u16(writer, TPM_ALG_SHA256);
    put_u8(writer, 3u);
    put_u8(writer, 0x80u);
    put_u8(writer, 0x18u);
    put_u8(writer, 0x00u);
}

static bool response_parameter_window(const uint8_t *response,
                                      size_t received,
                                      size_t *offset,
                                      size_t *size) {
    if (received < 10 || response == 0 || offset == 0 || size == 0) return false;
    if (read_be16(response) == TPM_ST_SESSIONS) {
        if (received < 14) return false;
        const uint32_t parameter_size = read_be32(&response[10]);
        if (parameter_size > received - 14u) return false;
        *offset = 14u;
        *size = parameter_size;
        return true;
    }
    *offset = 10u;
    *size = received - 10u;
    return true;
}

static bool tpm_flush_context(uint32_t handle) {
    if (handle == 0) return true;
    uint8_t command[16];
    uint8_t response[32];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_NO_SESSIONS, TPM_CC_FLUSH_CONTEXT);
    put_u32(&writer, handle);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    return crb_execute(command, writer.position, response, sizeof(response), &received) &&
           received >= 10 && status.last_response_code == TPM_RC_SUCCESS;
}

static bool start_policy_session(uint32_t *handle_out) {
    uint8_t nonce[16];
    if (!tpm_get_random(nonce, sizeof(nonce))) return false;

    uint8_t command[64];
    uint8_t response[96];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_NO_SESSIONS, TPM_CC_START_AUTH_SESSION);
    put_u32(&writer, TPM_RH_NULL);
    put_u32(&writer, TPM_RH_NULL);
    put_u16(&writer, (uint16_t)sizeof(nonce));
    put_bytes(&writer, nonce, sizeof(nonce));
    put_u16(&writer, 0);
    put_u8(&writer, TPM_SE_POLICY);
    put_u16(&writer, TPM_ALG_NULL);
    put_u16(&writer, TPM_ALG_SHA256);
    zero_bytes(nonce, sizeof(nonce));
    if (!finish_command(&writer)) return false;

    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS || received < 16) return false;
    const uint32_t handle = read_be32(&response[10]);
    if ((handle & 0xff000000u) != 0x03000000u) return false;
    *handle_out = handle;
    return true;
}

static bool policy_pcr_current(uint32_t session_handle) {
    uint8_t command[64];
    uint8_t response[32];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_NO_SESSIONS, TPM_CC_POLICY_PCR);
    put_u32(&writer, session_handle);
    put_u16(&writer, 0);
    put_root_pcr_selection(&writer);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    return crb_execute(command, writer.position, response, sizeof(response), &received) &&
           received >= 10 && status.last_response_code == TPM_RC_SUCCESS;
}

static bool policy_get_digest(uint32_t session_handle, uint8_t digest[TPM_SHA256_DIGEST_SIZE]) {
    uint8_t command[16];
    uint8_t response[64];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_NO_SESSIONS, TPM_CC_POLICY_GET_DIGEST);
    put_u32(&writer, session_handle);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS || received < 12) return false;
    const uint16_t size = read_be16(&response[10]);
    if (size != TPM_SHA256_DIGEST_SIZE || received < 12u + size) return false;
    for (size_t i = 0; i < TPM_SHA256_DIGEST_SIZE; ++i) digest[i] = response[12u + i];
    return true;
}

static bool current_root_policy_digest(uint8_t digest[TPM_SHA256_DIGEST_SIZE]) {
    uint32_t session = 0;
    if (!start_policy_session(&session)) return false;
    bool success = policy_pcr_current(session) && policy_get_digest(session, digest);
    if (!tpm_flush_context(session)) success = false;
    return success;
}

static bool current_root_policy_session(uint32_t *session_out) {
    uint32_t session = 0;
    if (!start_policy_session(&session)) return false;
    if (!policy_pcr_current(session)) {
        (void)tpm_flush_context(session);
        return false;
    }
    *session_out = session;
    return true;
}

static bool pcr_extend_sha256(uint32_t pcr_index,
                              const uint8_t digest[TPM_SHA256_DIGEST_SIZE]) {
    uint8_t command[96];
    uint8_t response[32];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, TPM_CC_PCR_EXTEND);
    put_u32(&writer, pcr_index);
    put_empty_password_session(&writer);
    put_u32(&writer, 1u);
    put_u16(&writer, TPM_ALG_SHA256);
    put_bytes(&writer, digest, TPM_SHA256_DIGEST_SIZE);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    return crb_execute(command, writer.position, response, sizeof(response), &received) &&
           received >= 10 && status.last_response_code == TPM_RC_SUCCESS;
}

static bool create_symmetric_policy_key(
    uint32_t hierarchy,
    const uint8_t policy_digest[TPM_SHA256_DIGEST_SIZE],
    uint32_t *handle_out) {
    uint8_t command[192];
    uint8_t response[1024];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, TPM_CC_CREATE_PRIMARY);
    put_u32(&writer, hierarchy);
    put_empty_password_session(&writer);
    put_u16(&writer, 4u);
    put_u16(&writer, 0);
    put_u16(&writer, 0);
    put_u16(&writer, 50u);
    put_u16(&writer, TPM_ALG_SYMCIPHER);
    put_u16(&writer, TPM_ALG_SHA256);
    put_u32(&writer, TPM_SYM_POLICY_ATTRIBUTES);
    put_u16(&writer, TPM_SHA256_DIGEST_SIZE);
    put_bytes(&writer, policy_digest, TPM_SHA256_DIGEST_SIZE);
    put_u16(&writer, TPM_ALG_AES);
    put_u16(&writer, 128u);
    put_u16(&writer, TPM_ALG_CFB);
    put_u16(&writer, 0);
    put_u16(&writer, 0);
    put_u32(&writer, 0);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS || received < 14) return false;
    const uint32_t handle = read_be32(&response[10]);
    if ((handle & 0xff000000u) != 0x80000000u) return false;
    *handle_out = handle;
    return true;
}

static bool create_hmac_policy_key(
    uint32_t hierarchy,
    const uint8_t policy_digest[TPM_SHA256_DIGEST_SIZE],
    uint32_t *handle_out) {
    uint8_t command[192];
    uint8_t response[1024];
    struct command_writer writer;
    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, TPM_CC_CREATE_PRIMARY);
    put_u32(&writer, hierarchy);
    put_empty_password_session(&writer);
    put_u16(&writer, 4u);
    put_u16(&writer, 0);
    put_u16(&writer, 0);
    put_u16(&writer, 48u);
    put_u16(&writer, TPM_ALG_KEYEDHASH);
    put_u16(&writer, TPM_ALG_SHA256);
    put_u32(&writer, TPM_HMAC_POLICY_ATTRIBUTES);
    put_u16(&writer, TPM_SHA256_DIGEST_SIZE);
    put_bytes(&writer, policy_digest, TPM_SHA256_DIGEST_SIZE);
    put_u16(&writer, TPM_ALG_HMAC);
    put_u16(&writer, TPM_ALG_SHA256);
    put_u16(&writer, 0);
    put_u16(&writer, 0);
    put_u32(&writer, 0);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS || received < 14) return false;
    const uint32_t handle = read_be32(&response[10]);
    if ((handle & 0xff000000u) != 0x80000000u) return false;
    *handle_out = handle;
    return true;
}

static bool create_policy_key_pair(
    uint32_t hierarchy,
    const uint8_t policy_digest[TPM_SHA256_DIGEST_SIZE],
    uint32_t *encryption_out,
    uint32_t *hmac_out) {
    uint32_t encryption = 0;
    uint32_t hmac = 0;
    if (!create_symmetric_policy_key(hierarchy, policy_digest, &encryption) ||
        !create_hmac_policy_key(hierarchy, policy_digest, &hmac)) {
        if (encryption != 0) (void)tpm_flush_context(encryption);
        if (hmac != 0) (void)tpm_flush_context(hmac);
        return false;
    }
    *encryption_out = encryption;
    *hmac_out = hmac;
    return true;
}

static void destroy_vault_keys(void) {
    if (vault_encryption_handle != 0) (void)tpm_flush_context(vault_encryption_handle);
    if (vault_hmac_handle != 0) (void)tpm_flush_context(vault_hmac_handle);
    vault_encryption_handle = 0;
    vault_hmac_handle = 0;
    status.vault_ready = false;
}

static void trip_root_integrity_failure(void) {
    status.integrity_failure = true;
    status.root_of_trust_established = false;
    status.pcr_policy_bound = false;
    status.persistent_hierarchy = false;
    destroy_vault_keys();
}

static bool encrypt_decrypt_command(uint32_t command_code,
                                    uint32_t policy_session,
                                    bool decrypt,
                                    const uint8_t iv[16],
                                    const uint8_t *input,
                                    size_t size,
                                    uint8_t *output) {
    uint8_t command[640];
    uint8_t response[640];
    struct command_writer writer;
    if (size > TPM_VAULT_MAX_PLAINTEXT || size > UINT16_MAX) return false;
    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, command_code);
    put_u32(&writer, vault_encryption_handle);
    put_policy_session_auth(&writer, policy_session);
    if (command_code == TPM_CC_ENCRYPT_DECRYPT2) {
        put_u16(&writer, (uint16_t)size);
        put_bytes(&writer, input, size);
        put_u8(&writer, decrypt ? 1u : 0u);
        put_u16(&writer, TPM_ALG_CFB);
        put_u16(&writer, 16u);
        put_bytes(&writer, iv, 16u);
    } else {
        put_u8(&writer, decrypt ? 1u : 0u);
        put_u16(&writer, TPM_ALG_CFB);
        put_u16(&writer, 16u);
        put_bytes(&writer, iv, 16u);
        put_u16(&writer, (uint16_t)size);
        put_bytes(&writer, input, size);
    }
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS) return false;
    size_t parameters = 0;
    size_t parameter_size = 0;
    if (!response_parameter_window(response, received, &parameters, &parameter_size)) return false;
    if (parameter_size < 2) return false;
    const uint16_t output_size = read_be16(&response[parameters]);
    if (output_size != size || parameter_size < 2u + output_size) return false;
    for (size_t i = 0; i < size; ++i) output[i] = response[parameters + 2u + i];
    return true;
}

static bool policy_crypt_once(uint32_t command_code,
                              bool decrypt,
                              const uint8_t iv[16],
                              const uint8_t *input,
                              size_t size,
                              uint8_t *output) {
    uint32_t session = 0;
    if (!current_root_policy_session(&session)) return false;
    const bool success = encrypt_decrypt_command(command_code, session, decrypt, iv, input, size, output);
    (void)tpm_flush_context(session);
    return success;
}

static bool vault_crypt(bool decrypt,
                        const uint8_t iv[16],
                        const uint8_t *input,
                        size_t size,
                        uint8_t *output) {
    if (!status.vault_ready || !status.root_of_trust_established || vault_encryption_handle == 0) {
        return false;
    }
    if (policy_crypt_once(TPM_CC_ENCRYPT_DECRYPT2, decrypt, iv, input, size, output)) return true;
    if (policy_crypt_once(TPM_CC_ENCRYPT_DECRYPT, decrypt, iv, input, size, output)) return true;
    trip_root_integrity_failure();
    return false;
}

static bool hmac_command(uint32_t policy_session,
                         const uint8_t *input,
                         size_t size,
                         uint8_t output[TPM_SHA256_DIGEST_SIZE]) {
    uint8_t command[640];
    uint8_t response[96];
    struct command_writer writer;
    if (size > 512u || size > UINT16_MAX) return false;
    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, TPM_CC_HMAC);
    put_u32(&writer, vault_hmac_handle);
    put_policy_session_auth(&writer, policy_session);
    put_u16(&writer, (uint16_t)size);
    put_bytes(&writer, input, size);
    put_u16(&writer, TPM_ALG_SHA256);
    if (!finish_command(&writer)) return false;
    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS) return false;
    size_t parameters = 0;
    size_t parameter_size = 0;
    if (!response_parameter_window(response, received, &parameters, &parameter_size)) return false;
    if (parameter_size < 2u + TPM_SHA256_DIGEST_SIZE) return false;
    const uint16_t digest_size = read_be16(&response[parameters]);
    if (digest_size != TPM_SHA256_DIGEST_SIZE) return false;
    for (size_t i = 0; i < TPM_SHA256_DIGEST_SIZE; ++i) output[i] = response[parameters + 2u + i];
    return true;
}

static bool vault_hmac(const uint8_t *input,
                       size_t size,
                       uint8_t output[TPM_SHA256_DIGEST_SIZE]) {
    if (!status.vault_ready || !status.root_of_trust_established || vault_hmac_handle == 0) {
        return false;
    }
    uint32_t session = 0;
    if (!current_root_policy_session(&session)) {
        trip_root_integrity_failure();
        return false;
    }
    const bool success = hmac_command(session, input, size, output);
    (void)tpm_flush_context(session);
    if (!success) trip_root_integrity_failure();
    return success;
}

static bool build_vault_mac_input(const struct tpm_protected_blob *blob,
                                  uint8_t output[512],
                                  size_t *size_out) {
    if (blob == 0 || output == 0 || size_out == 0) return false;
    if (blob->plaintext_length == 0 || blob->plaintext_length > TPM_VAULT_MAX_PLAINTEXT) return false;
    struct command_writer writer = {
        .buffer = output,
        .capacity = 512u,
        .position = 0,
        .failed = false,
    };
    put_u32(&writer, blob->magic);
    put_u16(&writer, blob->version);
    put_u16(&writer, blob->plaintext_length);
    put_bytes(&writer, blob->iv, sizeof(blob->iv));
    put_bytes(&writer, blob->ciphertext, blob->plaintext_length);
    if (writer.failed) return false;
    *size_out = writer.position;
    return true;
}

static void disable_transport(void) {
    destroy_vault_keys();
    status.transport_ready = false;
    status.root_of_trust_established = false;
    status.pcr_policy_bound = false;
    status.persistent_hierarchy = false;
    if (response_mapping != 0 && !response_shares_command_mapping) (void)mmio_unmap(response_mapping);
    if (command_mapping != 0) (void)mmio_unmap(command_mapping);
    if (crb_mapping != 0) (void)mmio_unmap(crb_mapping);
    response_mapping = 0;
    command_mapping = 0;
    crb_mapping = 0;
    response_buffer = 0;
    command_buffer = 0;
    crb_registers = 0;
    response_shares_command_mapping = false;
    command_capacity = 0;
    response_capacity = 0;
}

bool tpm_init(void) {
    zero_bytes(&status, sizeof(status));
    crb_registers = 0;
    command_buffer = 0;
    response_buffer = 0;
    crb_mapping = 0;
    command_mapping = 0;
    response_mapping = 0;
    response_shares_command_mapping = false;
    command_capacity = 0;
    response_capacity = 0;
    vault_encryption_handle = 0;
    vault_hmac_handle = 0;
    root_attempted = false;

    if (!acpi_is_initialized() || !mmio_is_initialized()) return true;
    const struct acpi_sdt_header *header = acpi_find_table("TPM2");
    if (header == 0 || header->length < sizeof(struct acpi_tpm2_table)) return true;

    const struct acpi_tpm2_table *table = (const struct acpi_tpm2_table *)header;
    status.detected = true;
    status.start_method = table->start_method;
    status.control_area_physical = table->control_area_address;
    if (table->start_method != TPM2_START_METHOD_CRB || table->control_area_address < 0x40ull) return true;

    const uint64_t register_base = table->control_area_address - 0x40ull;
    crb_mapping = mmio_map(register_base, 0x80u);
    if (crb_mapping == 0) return true;
    crb_registers = (volatile uint8_t *)crb_mapping;

    mmio_write32(CRB_LOC_CTRL, CRB_LOC_REQUEST_ACCESS);
    if (!wait_register(CRB_LOC_STATE,
                       CRB_LOC_ASSIGNED | CRB_LOC_VALID,
                       CRB_LOC_ASSIGNED | CRB_LOC_VALID,
                       TPM_LOCALITY_TIMEOUT_MS)) {
        disable_transport();
        return true;
    }

    command_capacity = mmio_read32(CRB_CTRL_CMD_SIZE);
    response_capacity = mmio_read32(CRB_CTRL_RSP_SIZE);
    const uint64_t command_physical = (uint64_t)mmio_read32(CRB_CTRL_CMD_LOW) |
                                      ((uint64_t)mmio_read32(CRB_CTRL_CMD_HIGH) << 32);
    const uint64_t response_physical = (uint64_t)mmio_read32(CRB_CTRL_RSP_LOW) |
                                       ((uint64_t)mmio_read32(CRB_CTRL_RSP_HIGH) << 32);
    if (command_capacity < 64u || response_capacity < 64u ||
        command_capacity > 65536u || response_capacity > 65536u ||
        command_physical == 0 || response_physical == 0) {
        disable_transport();
        return true;
    }

    command_mapping = mmio_map(command_physical, command_capacity);
    if (command_mapping == 0) {
        disable_transport();
        return true;
    }
    command_buffer = (volatile uint8_t *)command_mapping;

    if (response_physical == command_physical && response_capacity <= command_capacity) {
        response_mapping = command_mapping;
        response_buffer = command_buffer;
        response_shares_command_mapping = true;
    } else {
        response_mapping = mmio_map(response_physical, response_capacity);
        if (response_mapping == 0) {
            disable_transport();
            return true;
        }
        response_buffer = (volatile uint8_t *)response_mapping;
    }

    status.transport_ready = true;
    uint8_t probe_random[8];
    if (!tpm_get_random(probe_random, sizeof(probe_random))) {
        zero_bytes(probe_random, sizeof(probe_random));
        disable_transport();
        return true;
    }
    zero_bytes(probe_random, sizeof(probe_random));
    return true;
}

void tpm_get_status(struct tpm_status *out) {
    if (out != 0) *out = status;
}

bool tpm_get_random(void *buffer, size_t size) {
    if (!status.transport_ready || buffer == 0) return false;
    uint8_t *out = (uint8_t *)buffer;
    size_t produced = 0;
    while (produced < size) {
        const size_t remaining = size - produced;
        const uint16_t request = (uint16_t)(remaining > 64u ? 64u : remaining);
        uint8_t command[16];
        uint8_t response[96];
        struct command_writer writer;
        begin_command(&writer, command, sizeof(command), TPM_ST_NO_SESSIONS, TPM_CC_GET_RANDOM);
        put_u16(&writer, request);
        if (!finish_command(&writer)) return false;
        size_t received = 0;
        if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
        if (status.last_response_code != TPM_RC_SUCCESS || received < 12) return false;
        const uint16_t returned = read_be16(&response[10]);
        if (returned == 0 || returned > request || received < 12u + returned) return false;
        for (size_t i = 0; i < returned; ++i) out[produced + i] = response[12u + i];
        produced += returned;
    }
    return true;
}

bool tpm_establish_root_of_trust(
    const uint8_t kernel_measurement[TPM_SHA256_DIGEST_SIZE],
    const uint8_t security_policy_measurement[TPM_SHA256_DIGEST_SIZE]) {
    if (!status.transport_ready || kernel_measurement == 0 || security_policy_measurement == 0) return false;
    if (root_attempted) return status.root_of_trust_established;
    root_attempted = true;

    if (!pcr_extend_sha256(TPM_PCR_TWILIGHT_KERNEL, kernel_measurement)) return false;
    if (!pcr_extend_sha256(TPM_PCR_TWILIGHT_POLICY, security_policy_measurement)) return false;

    uint8_t policy_digest[TPM_SHA256_DIGEST_SIZE];
    zero_bytes(policy_digest, sizeof(policy_digest));
    if (!current_root_policy_digest(policy_digest)) {
        zero_bytes(policy_digest, sizeof(policy_digest));
        return false;
    }

    uint32_t encryption_handle = 0;
    uint32_t hmac_handle = 0;
    bool persistent = create_policy_key_pair(TPM_RH_OWNER,
                                             policy_digest,
                                             &encryption_handle,
                                             &hmac_handle);
    if (!persistent) {
        encryption_handle = 0;
        hmac_handle = 0;
        if (!create_policy_key_pair(TPM_RH_NULL,
                                    policy_digest,
                                    &encryption_handle,
                                    &hmac_handle)) {
            zero_bytes(policy_digest, sizeof(policy_digest));
            return false;
        }
    }
    zero_bytes(policy_digest, sizeof(policy_digest));

    vault_encryption_handle = encryption_handle;
    vault_hmac_handle = hmac_handle;
    status.pcr_policy_bound = true;
    status.persistent_hierarchy = persistent;
    status.root_of_trust_established = true;
    status.vault_ready = true;
    status.integrity_failure = false;
    return true;
}

bool tpm_vault_protect(const void *plaintext,
                       size_t size,
                       struct tpm_protected_blob *out) {
    if (!status.vault_ready || plaintext == 0 || out == 0) return false;
    if (size == 0 || size > TPM_VAULT_MAX_PLAINTEXT) return false;
    zero_bytes(out, sizeof(*out));
    out->magic = TPM_VAULT_MAGIC;
    out->version = TPM_VAULT_VERSION;
    out->plaintext_length = (uint16_t)size;
    if (!tpm_get_random(out->iv, sizeof(out->iv))) {
        zero_bytes(out, sizeof(*out));
        return false;
    }
    if (!vault_crypt(false, out->iv, (const uint8_t *)plaintext, size, out->ciphertext)) {
        zero_bytes(out, sizeof(*out));
        return false;
    }
    uint8_t mac_input[512];
    size_t mac_size = 0;
    zero_bytes(mac_input, sizeof(mac_input));
    if (!build_vault_mac_input(out, mac_input, &mac_size) ||
        !vault_hmac(mac_input, mac_size, out->authentication_tag)) {
        zero_bytes(mac_input, sizeof(mac_input));
        zero_bytes(out, sizeof(*out));
        return false;
    }
    zero_bytes(mac_input, sizeof(mac_input));
    return true;
}

bool tpm_vault_unprotect(const struct tpm_protected_blob *blob,
                         void *plaintext,
                         size_t capacity,
                         size_t *size_out) {
    if (!status.vault_ready || blob == 0 || plaintext == 0) return false;
    if (blob->magic != TPM_VAULT_MAGIC || blob->version != TPM_VAULT_VERSION) return false;
    if (blob->plaintext_length == 0 || blob->plaintext_length > TPM_VAULT_MAX_PLAINTEXT) return false;
    if (blob->plaintext_length > capacity) return false;

    uint8_t mac_input[512];
    uint8_t expected_tag[TPM_SHA256_DIGEST_SIZE];
    size_t mac_size = 0;
    zero_bytes(mac_input, sizeof(mac_input));
    zero_bytes(expected_tag, sizeof(expected_tag));
    const bool authenticated = build_vault_mac_input(blob, mac_input, &mac_size) &&
                               vault_hmac(mac_input, mac_size, expected_tag) &&
                               bytes_equal(expected_tag, blob->authentication_tag, TPM_SHA256_DIGEST_SIZE);
    zero_bytes(mac_input, sizeof(mac_input));
    zero_bytes(expected_tag, sizeof(expected_tag));
    if (!authenticated) return false;
    if (!vault_crypt(true,
                     blob->iv,
                     blob->ciphertext,
                     blob->plaintext_length,
                     (uint8_t *)plaintext)) {
        return false;
    }
    if (size_out != 0) *size_out = blob->plaintext_length;
    return true;
}

bool tpm_vault_self_test(void) {
    if (!status.vault_ready || !status.root_of_trust_established) return false;
    uint8_t plaintext[32];
    uint8_t recovered[32];
    for (size_t i = 0; i < sizeof(plaintext); ++i) plaintext[i] = (uint8_t)(0x31u + i * 7u);
    zero_bytes(recovered, sizeof(recovered));

    struct tpm_protected_blob blob;
    bool success = tpm_vault_protect(plaintext, sizeof(plaintext), &blob);
    size_t recovered_size = 0;
    if (success) success = tpm_vault_unprotect(&blob, recovered, sizeof(recovered), &recovered_size);
    if (success) success = recovered_size == sizeof(plaintext) && bytes_equal(plaintext, recovered, sizeof(plaintext));

    if (success) {
        struct tpm_protected_blob tampered = blob;
        tampered.ciphertext[0] ^= 0x80u;
        zero_bytes(recovered, sizeof(recovered));
        recovered_size = 0;
        if (tpm_vault_unprotect(&tampered, recovered, sizeof(recovered), &recovered_size)) success = false;
        zero_bytes(&tampered, sizeof(tampered));
    }

    zero_bytes(&blob, sizeof(blob));
    zero_bytes(plaintext, sizeof(plaintext));
    zero_bytes(recovered, sizeof(recovered));

    if (!success || status.integrity_failure) {
        destroy_vault_keys();
        status.root_of_trust_established = false;
        status.pcr_policy_bound = false;
        status.persistent_hierarchy = false;
        return false;
    }
    return true;
}
