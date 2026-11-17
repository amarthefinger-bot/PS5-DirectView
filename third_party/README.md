# Vendored third-party code

Everything here is compiled into `PS5DirectView.exe` as static libraries
(`third_party/CMakeLists.txt`). Nothing is downloaded at build time.

| Directory | Upstream | Version | Licence | Used for |
|---|---|---|---|---|
| `chiaki/` | [streetpea/chiaki-ng](https://github.com/streetpea/chiaki-ng) `lib/` | commit `43728af2e639fecb8a7af1cd68c29820a40de973` (2026-11-03) | AGPL-3.0 (with OpenSSL exception), see `chiaki/COPYING`, `chiaki/LICENSES/` | PS5 discovery, wake, registration (pairing), Remote Play session and controller-state transport |
| `nanopb/` | [nanopb/nanopb](https://github.com/nanopb/nanopb) runtime (`pb_*.c/h`) | `160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f` (chiaki-ng submodule) | zlib | Takion protobuf messages |
| `chiaki/generated/takion.pb.{c,h}` | generated from `chiaki/generated/takion.proto` with the nanopb generator above | — | as chiaki-ng | (pre-generated so the build needs no Python/protoc) |
| `jerasure/` | [streetpea/jerasure](https://github.com/streetpea/jerasure) | `505ccb4bc69eee8d7ea40a6089a056b99671134f` | BSD-3-Clause | Remote Play FEC (linked by the transport; video itself is discarded) |
| `gf-complete/` | [streetpea/gf-complete](https://github.com/streetpea/gf-complete) | `fa54a4670a5705c84abf6c24b92b0cd479625478` + chiaki-ng `scripts/windows-vc/gf-complete.patch` | BSD-3-Clause | Galois-field maths for jerasure |
| `mbedtls/` | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) `library/` + `include/` | v2.28.10 | Apache-2.0 | AES/GCM, ECDH secp256k1, HMAC-SHA256, CTR-DRBG for the Remote Play crypto |

## What was left out, and why

chiaki-ng's `lib/` also implements **internet** Remote Play (PSN holepunch,
STUN/UPnP, RUDP, PSN certificate fetching). Those parts need curl, json-c,
libevent and miniupnpc, and DirectView only talks to a PS5 on the same LAN.
They are not compiled:

* `src/remote/holepunch.c`, `src/remote/rudp.c`, `src/remote/rudpsendbuffer.c`,
  `src/aia.c`, `src/aia_openssl.c` — replaced by fail-cleanly stubs in
  `shims/chiaki_lan_stubs.c`. chiaki-ng only calls them when a holepunch
  session is supplied, which DirectView never does.
* `src/ffmpegdecoder.c`, `src/pidecoder.c`, Opus — no Remote Play audio or
  video is ever decoded (`shims/include/chiaki/config.h`).
* `shims/include/curl/curl.h` only defines the opaque `CURL` type so
  `src/aia.h` (included by `common.c`) compiles.

## Local patches (`patches/`)

* `chiaki-msvc-no-vla.patch` — three C99 variable-length arrays (all inside
  internet-only RUDP branches) changed to `_alloca`; MSVC has no VLAs.
* `chiaki-win-discovery-connreset.patch` — disables `SIO_UDP_CONNRESET` on the
  discovery socket. Without it, the ICMP "port unreachable" a PS5 returns for
  the PS4 discovery port (987) makes Windows fail the next `recvfrom()` with
  `WSAECONNRESET`, which kills the discovery thread before the PS5's reply on
  port 9302 is read. Found while testing against a real PS5 (firmware 14.23).
* `chiaki-mbedtls-rpcrypt-hmac-overflow.patch` — upstream bug in the mbedtls
  branch of `chiaki_rpcrypt_generate_iv()`: a 16-byte buffer received the full
  32-byte SHA-256 HMAC, overrunning the stack. MSVC's `/GS` stack guard aborted
  the app (0xC0000409) as soon as pairing started. The buffer is now 32 bytes;
  output is unchanged (first 16 bytes). Covered by `tests/CryptoTests.cpp`.

## Updating chiaki-ng (e.g. after a PS5 firmware change)

1. Check out the new chiaki-ng commit and its `third-party/nanopb`,
   `jerasure`, `gf-complete` submodules.
2. Copy `lib/include/chiaki` → `chiaki/include/chiaki` (delete `config.h`,
   `ffmpegdecoder.h`, `pidecoder.h`) and `lib/src/*.c|*.h` → `chiaki/src/`
   (skip the files listed above).
3. Re-apply `patches/*.patch` (`git apply` / `patch -p1` from the chiaki-ng root).
4. Regenerate `takion.pb.c/h`: `protoc -o takion.pb takion.proto` then
   `python nanopb/generator/nanopb_generator.py takion.pb`.
5. If new internet-only symbols appear at link time, add stubs to
   `shims/chiaki_lan_stubs.c`.

## Licence note

chiaki-ng is AGPL-3.0. Because it is linked into the executable, a
distributed PS5 DirectView binary is subject to the AGPL's terms (provide the
complete corresponding source to recipients). Personal use is unaffected.
