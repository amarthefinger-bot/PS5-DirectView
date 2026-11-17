// Offline regression tests for the vendored Remote Play crypto (mbedtls backend),
// built with MSVC's /GS stack guard. Exercises the exact path that runs when you
// press "Pair with PS5" (registration payload + rpcrypt IV/encrypt), plus the
// session handshake crypto, without talking to a console.
// Build: cmake --build build --config Release --target CryptoTests
extern "C" {
#include <chiaki/common.h>
#include <chiaki/ecdh.h>
#include <chiaki/regist.h>
#include <chiaki/rpcrypt.h>
#include <chiaki/session.h>
}

#include <cstdio>
#include <cstring>

namespace {
int g_failures = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}
} // namespace

int main() {
    Check(chiaki_lib_init() == CHIAKI_ERR_SUCCESS, "chiaki_lib_init");

    // --- Registration payload for a PS5 (what crashed: rpcrypt IV HMAC overrun).
    {
        uint8_t ambassador[CHIAKI_RPCRYPT_KEY_SIZE];
        for (int i = 0; i < CHIAKI_RPCRYPT_KEY_SIZE; ++i) ambassador[i] = static_cast<uint8_t>(i * 7 + 1);
        const uint8_t account[CHIAKI_PSN_ACCOUNT_ID_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t payload[0x400]{};
        size_t size = sizeof(payload);
        ChiakiRPCrypt crypt{};
        const ChiakiErrorCode err = chiaki_regist_request_payload_format(
            CHIAKI_TARGET_PS5_1, ambassador, payload, &size, &crypt, nullptr, account, 12345678, nullptr);
        Check(err == CHIAKI_ERR_SUCCESS, "PS5 regist payload formats");
        Check(size > 0 && size <= sizeof(payload), "regist payload size sane");
    }

    // --- rpcrypt IV generation / round trip (PS5 auth keys).
    {
        uint8_t nonce[CHIAKI_RPCRYPT_KEY_SIZE], morning[CHIAKI_RPCRYPT_KEY_SIZE];
        for (int i = 0; i < CHIAKI_RPCRYPT_KEY_SIZE; ++i) { nonce[i] = static_cast<uint8_t>(i); morning[i] = static_cast<uint8_t>(255 - i); }
        ChiakiRPCrypt crypt{};
        chiaki_rpcrypt_init_auth(&crypt, CHIAKI_TARGET_PS5_1, nonce, morning);
        uint8_t iv[CHIAKI_RPCRYPT_KEY_SIZE + 16];
        std::memset(iv, 0xAB, sizeof(iv));
        Check(chiaki_rpcrypt_generate_iv(&crypt, iv, 42) == CHIAKI_ERR_SUCCESS, "generate_iv");
        bool tailUntouched = true;
        for (size_t i = CHIAKI_RPCRYPT_KEY_SIZE; i < sizeof(iv); ++i) tailUntouched &= iv[i] == 0xAB;
        Check(tailUntouched, "generate_iv writes exactly 16 bytes");
        const char plain[] = "PS5 DirectView rpcrypt round trip";
        uint8_t enc[sizeof(plain)], dec[sizeof(plain)];
        Check(chiaki_rpcrypt_encrypt(&crypt, 7, reinterpret_cast<const uint8_t*>(plain), enc, sizeof(plain)) == CHIAKI_ERR_SUCCESS, "encrypt");
        Check(std::memcmp(enc, plain, sizeof(plain)) != 0, "ciphertext differs");
        Check(chiaki_rpcrypt_decrypt(&crypt, 7, enc, dec, sizeof(plain)) == CHIAKI_ERR_SUCCESS, "decrypt");
        Check(std::memcmp(dec, plain, sizeof(plain)) == 0, "round trip");
    }

    // --- Session handshake ECDH (secp256k1) + HMAC signature.
    {
        ChiakiECDH a{}, b{};
        Check(chiaki_ecdh_init(&a) == CHIAKI_ERR_SUCCESS && chiaki_ecdh_init(&b) == CHIAKI_ERR_SUCCESS, "ecdh init");
        uint8_t hk[CHIAKI_HANDSHAKE_KEY_SIZE] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
        uint8_t pa[128], pb[128], sa[32], sb[32];
        size_t pas = sizeof(pa), pbs = sizeof(pb), sas = sizeof(sa), sbs = sizeof(sb);
        Check(chiaki_ecdh_get_local_pub_key(&a, pa, &pas, hk, sa, &sas) == CHIAKI_ERR_SUCCESS, "pub key a");
        Check(chiaki_ecdh_get_local_pub_key(&b, pb, &pbs, hk, sb, &sbs) == CHIAKI_ERR_SUCCESS, "pub key b");
        uint8_t za[CHIAKI_ECDH_SECRET_SIZE], zb[CHIAKI_ECDH_SECRET_SIZE];
        Check(chiaki_ecdh_derive_secret(&a, za, pb, pbs, hk, sb, sbs) == CHIAKI_ERR_SUCCESS, "derive a");
        Check(chiaki_ecdh_derive_secret(&b, zb, pa, pas, hk, sa, sas) == CHIAKI_ERR_SUCCESS, "derive b");
        Check(std::memcmp(za, zb, sizeof(za)) == 0, "shared secrets match");
        chiaki_ecdh_fini(&a);
        chiaki_ecdh_fini(&b);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
