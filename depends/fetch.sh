#!/bin/sh
# Download the pinned dependency tarballs into depends/sources and check their SHA-256.
set -e
cd "$(dirname "$0")" && mkdir -p sources && cd sources
fetch() { [ -f "$2" ] || curl -fsSL -o "$2" "$1"; echo "$3  $2" | sha256sum -c -; }
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz openssl-3.5.9.tar.gz 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
fetch https://github.com/ngtcp2/ngtcp2/releases/download/v1.25.0/ngtcp2-1.25.0.tar.xz ngtcp2-1.25.0.tar.xz 2a34d2484ba17847a5d11965704e9dd0fac4c6d8efc75ffe1ec7de66d8c6b6fb
fetch https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz libsodium-1.0.20.tar.gz ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
fetch https://www.sqlite.org/2025/sqlite-autoconf-3500400.tar.gz sqlite-autoconf-3500400.tar.gz a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18
fetch https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz nghttp2-1.70.0.tar.xz e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
