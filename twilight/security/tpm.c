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

#define TPM_RH_NULL 0x40000007u
#define TPM_RS_PW   0x40000009u

#define TPM_ALG_AES       0x0006u
#define TPM_ALG_SHA256    0x000bu
#define TPM_ALG_NULL      0x0010u
#define TPM_ALG_SYMCIPHER 0x0025u
#define TPM_ALG_CFB       0x0043u

#define TPM_CC_CREATE_PRIMARY   0x00000131u
#define TPM_CC_ENCRYPT_DECRYPT  0x00000164u
#define TPM_CC_FLUSH_CONTEXT    0x00000165u
#define TPM_CC_GET_RANDOM       0x0000017bu
#define TPM_CC_ENCRYPT_DECRYPT2 0x00000193u

#define TPM_PRIMARY_ATTRIBUTES 0x00060072u
#define TPM_VAULT_MAGIC 0x54564c54u /* "TLVT" little-endian in memory */
#define TPM_VAULT_VERSION 1u

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
static uint32_t vault_key_handle;

static void zero_bytes(void *pointer, size_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
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
    if (!wait_register(CRB_CTRL_START,
                       CRB_CTRL_START_INVOKE,
                       0,
                       TPM_COMMAND_TIMEOUT_MS)) {
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
    put_u32(writer, 0); /* commandSize patched by finish_command() */
    put_u32(writer, command_code);
}

static bool finish_command(struct command_writer *writer) {
    if (writer->failed || writer->position > UINT32_MAX || writer->position < 10) return false;
    write_be32_at(writer->buffer, 2, (uint32_t)writer->position);
    return true;
}

static void put_empty_password_session(struct command_writer *writer) {
    put_u32(writer, 9u);       /* authorizationSize */
    put_u32(writer, TPM_RS_PW);
    put_u16(writer, 0);        /* nonce size */
    put_u8(writer, 0);         /* session attributes */
    put_u16(writer, 0);        /* HMAC/password size */
}

static bool tpm_flush_context(uint32_t handle) {
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

static bool create_vault_key(uint32_t *handle_out) {
    uint8_t command[128];
    uint8_t response[1024];
    struct command_writer writer;

    begin_command(&writer, command, sizeof(command), TPM_ST_SESSIONS, TPM_CC_CREATE_PRIMARY);
    put_u32(&writer, TPM_RH_NULL);
    put_empty_password_session(&writer);

    /* TPM2B_SENSITIVE_CREATE: empty userAuth and empty sensitive data. */
    put_u16(&writer, 4);
    put_u16(&writer, 0);
    put_u16(&writer, 0);

    /*
     * TPM2B_PUBLIC / TPMT_PUBLIC for an AES-128-CFB symmetric object.
     * fixedTPM|fixedParent|sensitiveDataOrigin|userWithAuth|decrypt|signEncrypt
     * keeps the generated key material inside the TPM.
     */
    put_u16(&writer, 18);
    put_u16(&writer, TPM_ALG_SYMCIPHER);
    put_u16(&writer, TPM_ALG_SHA256);
    put_u32(&writer, TPM_PRIMARY_ATTRIBUTES);
    put_u16(&writer, 0); /* authPolicy */
    put_u16(&writer, TPM_ALG_AES);
    put_u16(&writer, 128);
    put_u16(&writer, TPM_ALG_CFB);
    put_u16(&writer, 0); /* unique.sym */

    put_u16(&writer, 0); /* outsideInfo */
    put_u32(&writer, 0); /* creationPCR.count */

    if (!finish_command(&writer)) return false;

    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS || received < 14) return false;

    const uint32_t handle = read_be32(&response[10]);
    if ((handle & 0xff000000u) != 0x80000000u) return false;
    *handle_out = handle;
    return true;
}

static bool encrypt_decrypt_command(uint32_t command_code,
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
    put_u32(&writer, vault_key_handle);
    put_empty_password_session(&writer);

    if (command_code == TPM_CC_ENCRYPT_DECRYPT2) {
        put_u16(&writer, (uint16_t)size);
        put_bytes(&writer, input, size);
        put_u8(&writer, decrypt ? 1u : 0u);
        put_u16(&writer, TPM_ALG_CFB);
        put_u16(&writer, 16);
        put_bytes(&writer, iv, 16);
    } else {
        put_u8(&writer, decrypt ? 1u : 0u);
        put_u16(&writer, TPM_ALG_CFB);
        put_u16(&writer, 16);
        put_bytes(&writer, iv, 16);
        put_u16(&writer, (uint16_t)size);
        put_bytes(&writer, input, size);
    }

    if (!finish_command(&writer)) return false;

    size_t received = 0;
    if (!crb_execute(command, writer.position, response, sizeof(response), &received)) return false;
    if (status.last_response_code != TPM_RC_SUCCESS) return false;

    const uint16_t tag = read_be16(response);
    size_t parameters = 10;
    if (tag == TPM_ST_SESSIONS) {
        if (received < 16) return false;
        const uint32_t parameter_size = read_be32(&response[10]);
        parameters = 14;
        if (parameter_size > received - parameters) return false;
    }

    if (received < parameters + 2) return false;
    const uint16_t output_size = read_be16(&response[parameters]);
    if (output_size != size || received < parameters + 2u + output_size) return false;
    for (size_t i = 0; i < size; ++i) output[i] = response[parameters + 2u + i];
    return true;
}

static bool vault_crypt(bool decrypt,
                        const uint8_t iv[16],
                        const uint8_t *input,
                        size_t size,
                        uint8_t *output) {
    if (!status.vault_ready || vault_key_handle == 0) return false;

    if (encrypt_decrypt_command(TPM_CC_ENCRYPT_DECRYPT2,
                                decrypt,
                                iv,
                                input,
                                size,
                                output)) {
        return true;
    }

    /* EncryptDecrypt2 is optional on older TPM 2.0 implementations. */
    return encrypt_decrypt_command(TPM_CC_ENCRYPT_DECRYPT,
                                   decrypt,
                                   iv,
                                   input,
                                   size,
                                   output);
}

static void disable_transport(void) {
    status.transport_ready = false;
    status.vault_ready = false;
    vault_key_handle = 0;

    if (response_mapping != 0 && !response_shares_command_mapping) {
        (void)mmio_unmap(response_mapping);
    }
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
    vault_key_handle = 0;

    if (!acpi_is_initialized() || !mmio_is_initialized()) return true;

    const struct acpi_sdt_header *header = acpi_find_table("TPM2");
    if (header == 0) return true;
    if (header->length < sizeof(struct acpi_tpm2_table)) return true;

    const struct acpi_tpm2_table *table = (const struct acpi_tpm2_table *)header;
    status.detected = true;
    status.start_method = table->start_method;
    status.control_area_physical = table->control_area_address;

    /* TIS (6) and CRB+ACPI-start (8) are detected but intentionally not
     * claimed until their transports/AML start method are implemented. */
    if (table->start_method != TPM2_START_METHOD_CRB || table->control_area_address < 0x40ull) {
        return true;
    }

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
    const uint64_t command_physical =
        (uint64_t)mmio_read32(CRB_CTRL_CMD_LOW) |
        ((uint64_t)mmio_read32(CRB_CTRL_CMD_HIGH) << 32);
    const uint64_t response_physical =
        (uint64_t)mmio_read32(CRB_CTRL_RSP_LOW) |
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

    uint32_t handle = 0;
    if (create_vault_key(&handle)) {
        vault_key_handle = handle;
        status.vault_ready = true;
    }

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

    if (!vault_crypt(false,
                     out->iv,
                     (const uint8_t *)plaintext,
                     size,
                     out->ciphertext)) {
        zero_bytes(out, sizeof(*out));
        return false;
    }

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
    if (!status.vault_ready) return false;

    uint8_t plaintext[32];
    uint8_t recovered[32];
    for (size_t i = 0; i < sizeof(plaintext); ++i) plaintext[i] = (uint8_t)(0x31u + i * 7u);
    zero_bytes(recovered, sizeof(recovered));

    struct tpm_protected_blob blob;
    bool success = tpm_vault_protect(plaintext, sizeof(plaintext), &blob);
    size_t recovered_size = 0;
    if (success) {
        success = tpm_vault_unprotect(&blob,
                                      recovered,
                                      sizeof(recovered),
                                      &recovered_size);
    }
    if (success) success = recovered_size == sizeof(plaintext) &&
                           bytes_equal(plaintext, recovered, sizeof(plaintext));

    zero_bytes(&blob, sizeof(blob));
    zero_bytes(plaintext, sizeof(plaintext));
    zero_bytes(recovered, sizeof(recovered));

    if (!success && vault_key_handle != 0) {
        (void)tpm_flush_context(vault_key_handle);
        vault_key_handle = 0;
        status.vault_ready = false;
    }
    return success;
}
