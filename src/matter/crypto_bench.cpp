#include "crypto_bench.h"

#include <errno.h>
#include <psa/crypto.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

namespace {

constexpr size_t kDefaultIterations = 10;
constexpr size_t kMaxIterations     = 1000;

uint64_t cycles_to_us(uint64_t cycles)
{
    return k_cyc_to_us_floor64(cycles);
}

uint64_t now_cycles()
{
    return k_cycle_get_64();
}

void print_result(const shell *sh, const char *name, size_t iterations, uint64_t total_us)
{
    const uint64_t avg_us = iterations == 0 ? 0 : total_us / iterations;

    shell_print(sh, "%-24s total=%llu us avg=%llu us iter=%u",
                name,
                static_cast<unsigned long long>(total_us),
                static_cast<unsigned long long>(avg_us),
                static_cast<unsigned>(iterations));
}

int print_psa_error(const shell *sh, const char *op, psa_status_t status)
{
    shell_error(sh, "%s failed: %d", op, static_cast<int>(status));
    return -EIO;
}

int bench_hash_sha256(const shell *sh, size_t iterations)
{
    uint8_t input[512];
    uint8_t hash[32];

    memset(input, 0xA5, sizeof(input));

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        size_t hash_len = 0;
        psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), hash, sizeof(hash), &hash_len);
        if (status != PSA_SUCCESS) {
            return print_psa_error(sh, "psa_hash_compute(SHA-256)", status);
        }
    }

    print_result(sh, "SHA-256 512B", iterations, cycles_to_us(now_cycles() - start));
    return 0;
}

int bench_hmac_sha256(const shell *sh, size_t iterations)
{
    uint8_t key[32];
    uint8_t input[512];
    uint8_t mac[32];
    size_t mac_len = 0;

    memset(key, 0x11, sizeof(key));
    memset(input, 0x5A, sizeof(input));

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attrs, sizeof(key) * 8);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_status_t status = psa_import_key(&attrs, key, sizeof(key), &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        return print_psa_error(sh, "psa_import_key(HMAC)", status);
    }

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), input, sizeof(input), mac, sizeof(mac), &mac_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return print_psa_error(sh, "psa_mac_compute(HMAC-SHA256)", status);
        }
    }

    print_result(sh, "HMAC-SHA256 512B", iterations, cycles_to_us(now_cycles() - start));
    psa_destroy_key(key_id);
    return 0;
}

int bench_hkdf_sha256(const shell *sh, size_t iterations)
{
    uint8_t ikm[32];
    uint8_t salt[16];
    uint8_t info[16];
    uint8_t output[32];

    memset(ikm, 0x22, sizeof(ikm));
    memset(salt, 0x33, sizeof(salt));
    memset(info, 0x44, sizeof(info));

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
        psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
        if (status != PSA_SUCCESS) {
            return print_psa_error(sh, "psa_key_derivation_setup(HKDF)", status);
        }

        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, sizeof(salt));
        if (status == PSA_SUCCESS) {
            status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, sizeof(ikm));
        }
        if (status == PSA_SUCCESS) {
            status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, sizeof(info));
        }
        if (status == PSA_SUCCESS) {
            status = psa_key_derivation_output_bytes(&op, output, sizeof(output));
        }

        psa_key_derivation_abort(&op);

        if (status != PSA_SUCCESS) {
            return print_psa_error(sh, "psa_key_derivation_output_bytes(HKDF)", status);
        }
    }

    print_result(sh, "HKDF-SHA256 32B", iterations, cycles_to_us(now_cycles() - start));
    return 0;
}

int import_aes_key(const shell *sh, psa_key_id_t *key_id)
{
    uint8_t key[16];

    memset(key, 0x6B, sizeof(key));

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, sizeof(key) * 8);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CCM);

    psa_status_t status = psa_import_key(&attrs, key, sizeof(key), key_id);
    psa_reset_key_attributes(&attrs);

    if (status != PSA_SUCCESS) {
        return print_psa_error(sh, "psa_import_key(AES-128)", status);
    }

    return 0;
}

int bench_aes_ccm_encrypt(const shell *sh, size_t iterations)
{
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    int ret             = import_aes_key(sh, &key_id);
    if (ret != 0) {
        return ret;
    }

    uint8_t nonce[13];
    uint8_t aad[16];
    uint8_t plaintext[128];
    uint8_t ciphertext[sizeof(plaintext) + 16];
    size_t ciphertext_len = 0;

    memset(nonce, 0x01, sizeof(nonce));
    memset(aad, 0x02, sizeof(aad));
    memset(plaintext, 0x03, sizeof(plaintext));

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        nonce[sizeof(nonce) - 1] = static_cast<uint8_t>(i);
        psa_status_t status     = psa_aead_encrypt(key_id, PSA_ALG_CCM, nonce, sizeof(nonce), aad, sizeof(aad), plaintext,
                                                   sizeof(plaintext), ciphertext, sizeof(ciphertext), &ciphertext_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return print_psa_error(sh, "psa_aead_encrypt(AES-CCM)", status);
        }
    }

    print_result(sh, "AES-CCM enc 128B", iterations, cycles_to_us(now_cycles() - start));
    psa_destroy_key(key_id);
    return 0;
}

int bench_aes_ccm_decrypt(const shell *sh, size_t iterations)
{
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    int ret             = import_aes_key(sh, &key_id);
    if (ret != 0) {
        return ret;
    }

    uint8_t nonce[13];
    uint8_t aad[16];
    uint8_t plaintext[128];
    uint8_t ciphertext[sizeof(plaintext) + 16];
    uint8_t decrypted[sizeof(plaintext)];
    size_t ciphertext_len = 0;
    size_t decrypted_len  = 0;

    memset(nonce, 0x01, sizeof(nonce));
    memset(aad, 0x02, sizeof(aad));
    memset(plaintext, 0x03, sizeof(plaintext));

    psa_status_t status = psa_aead_encrypt(key_id, PSA_ALG_CCM, nonce, sizeof(nonce), aad, sizeof(aad), plaintext,
                                           sizeof(plaintext), ciphertext, sizeof(ciphertext), &ciphertext_len);
    if (status != PSA_SUCCESS) {
        psa_destroy_key(key_id);
        return print_psa_error(sh, "psa_aead_encrypt(AES-CCM decrypt setup)", status);
    }

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        status = psa_aead_decrypt(key_id, PSA_ALG_CCM, nonce, sizeof(nonce), aad, sizeof(aad), ciphertext, ciphertext_len,
                                  decrypted, sizeof(decrypted), &decrypted_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return print_psa_error(sh, "psa_aead_decrypt(AES-CCM)", status);
        }
    }

    print_result(sh, "AES-CCM dec 128B", iterations, cycles_to_us(now_cycles() - start));
    psa_destroy_key(key_id);
    return 0;
}

int make_p256_key(const shell *sh, psa_key_usage_t usage, psa_algorithm_t alg, psa_key_id_t *key_id)
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, usage);
    psa_set_key_algorithm(&attrs, alg);

    psa_status_t status = psa_generate_key(&attrs, key_id);
    psa_reset_key_attributes(&attrs);

    if (status != PSA_SUCCESS) {
        return print_psa_error(sh, "psa_generate_key(P-256)", status);
    }

    return 0;
}

int bench_p256_keygen(const shell *sh, size_t iterations)
{
    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        psa_key_id_t key_id = PSA_KEY_ID_NULL;
        int ret = make_p256_key(sh, PSA_KEY_USAGE_SIGN_HASH, PSA_ALG_ECDSA(PSA_ALG_SHA_256), &key_id);
        if (ret != 0) {
            return ret;
        }
        psa_destroy_key(key_id);
    }

    print_result(sh, "P-256 keygen", iterations, cycles_to_us(now_cycles() - start));
    return 0;
}

int bench_p256_ecdsa(const shell *sh, size_t iterations)
{
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    int ret = make_p256_key(sh, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH, PSA_ALG_ECDSA(PSA_ALG_SHA_256), &key_id);
    if (ret != 0) {
        return ret;
    }

    uint8_t hash[32];
    uint8_t sig[PSA_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;

    memset(hash, 0x9C, sizeof(hash));

    uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        hash[0] = static_cast<uint8_t>(i);
        psa_status_t status = psa_sign_hash(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig), &sig_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return print_psa_error(sh, "psa_sign_hash(ECDSA)", status);
        }
    }

    print_result(sh, "P-256 ECDSA sign", iterations, cycles_to_us(now_cycles() - start));

    start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        psa_status_t status = psa_verify_hash(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sig_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return print_psa_error(sh, "psa_verify_hash(ECDSA)", status);
        }
    }

    print_result(sh, "P-256 ECDSA verify", iterations, cycles_to_us(now_cycles() - start));
    psa_destroy_key(key_id);
    return 0;
}

int bench_p256_ecdh(const shell *sh, size_t iterations)
{
    psa_key_id_t local_key_id  = PSA_KEY_ID_NULL;
    psa_key_id_t remote_key_id = PSA_KEY_ID_NULL;

    int ret = make_p256_key(sh, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT, PSA_ALG_ECDH, &local_key_id);
    if (ret != 0) {
        return ret;
    }

    ret = make_p256_key(sh, PSA_KEY_USAGE_EXPORT, PSA_ALG_ECDH, &remote_key_id);
    if (ret != 0) {
        psa_destroy_key(local_key_id);
        return ret;
    }

    uint8_t remote_pub[PSA_EXPORT_PUBLIC_KEY_MAX_SIZE];
    size_t remote_pub_len = 0;
    psa_status_t status   = psa_export_public_key(remote_key_id, remote_pub, sizeof(remote_pub), &remote_pub_len);
    psa_destroy_key(remote_key_id);
    if (status != PSA_SUCCESS) {
        psa_destroy_key(local_key_id);
        return print_psa_error(sh, "psa_export_public_key(ECDH)", status);
    }

    uint8_t secret[PSA_RAW_KEY_AGREEMENT_OUTPUT_MAX_SIZE];
    size_t secret_len = 0;

    const uint64_t start = now_cycles();
    for (size_t i = 0; i < iterations; ++i) {
        status = psa_raw_key_agreement(PSA_ALG_ECDH, local_key_id, remote_pub, remote_pub_len, secret, sizeof(secret), &secret_len);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(local_key_id);
            return print_psa_error(sh, "psa_raw_key_agreement(ECDH)", status);
        }
    }

    print_result(sh, "P-256 ECDH", iterations, cycles_to_us(now_cycles() - start));
    psa_destroy_key(local_key_id);
    return 0;
}

} // namespace

int matter_crypto_bench_run(const shell *sh, size_t iterations)
{
    if (iterations == 0) {
        iterations = kDefaultIterations;
    } else if (iterations > kMaxIterations) {
        shell_error(sh, "iterations too high, max=%u", static_cast<unsigned>(kMaxIterations));
        return -EINVAL;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return print_psa_error(sh, "psa_crypto_init", status);
    }

    shell_print(sh, "Crypto benchmark: iterations=%u, cpu cycles/sec=%u",
                static_cast<unsigned>(iterations), static_cast<unsigned>(sys_clock_hw_cycles_per_sec()));

    int ret = bench_hash_sha256(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_hmac_sha256(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_hkdf_sha256(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_aes_ccm_encrypt(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_aes_ccm_decrypt(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_p256_keygen(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    ret = bench_p256_ecdh(sh, iterations);
    if (ret != 0) {
        return ret;
    }

    return bench_p256_ecdsa(sh, iterations);
}
