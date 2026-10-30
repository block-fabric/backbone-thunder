# backbone-thunder

Drop-in replacement clients for the eCash sidechains of the LayerTwo family, starting with
**Thunder (slot 9)**: consensus-, P2P-, mainchain- and RPC-compatible with thunder-rust
v0.19.1 on eCash mainnet and v0.18.2 on betanet, in C++20. See `doc/DESIGN.md` (architecture, libraries, decisions),
`doc/thunder.md` (status, drop-in surface, reproduced quirks, deviations) and
`doc/differential-tests.md` (what to run against their binaries on the test machine).

Inspired by Thunder by LayerTwo Labs.

## Layout

The shared engine (codecs, crypto, store, QUIC, gRPC, enforcer client, JSON-RPC, test
harness, pinned dependencies) is the git submodule `core/` = `../backbone-core`. Clone with
`git clone --recurse-submodules`, or run `git submodule update --init` after cloning.

## Build (Linux)

```
core/depends/fetch.sh
cmake -S core/depends -B core/depends/work/linux       # OpenSSL 3.5.9 + ngtcp2 1.25.0
~/skydoge/tools/build.sh ~/skydoge/backbone-thunder core/depends/work/linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo   # system libsodium, sqlite3, nghttp2
~/skydoge/tools/build.sh ~/skydoge/backbone-thunder build
build/tests/bt_tests
```

## The app (thunder-app)

Our sidechain app on this node: the sidechain template's window, trusted dialog and interface kit, with pages for
this chain's data model; the node runs as the app's child, unchanged (consensus and the LayerTwo RPC byte for byte),
the app talking to its separate, authenticated app RPC. The same pages are a pack for the unified wallet. See
`doc/app.md` and `core/doc/app.md`.

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo>   # GTK 3, WebKitGTK 4.1, npm
~/skydoge/tools/build.sh ~/skydoge/backbone-thunder build-app
build-app/chains/thunder/thunder-app
```

## Cross-build (Windows, mingw-w64 posix)

```
cmake -S core/depends -B core/depends/work/mingw64 -DBT_HOST=mingw64 -DCMAKE_TOOLCHAIN_FILE=$PWD/core/cmake/mingw64.cmake
~/skydoge/tools/build.sh ~/skydoge/backbone-thunder core/depends/work/mingw64
PKG_CONFIG_LIBDIR=$PWD/core/depends/built/mingw64/lib/pkgconfig cmake -S . -B build-mingw64 \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/core/cmake/mingw64.cmake -DBT_DEPENDS_PREFIX=$PWD/core/depends/built/mingw64 \
  -DBT_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
~/skydoge/tools/build.sh ~/skydoge/backbone-thunder build-mingw64
```

Dependency tarballs are kept in `core/depends/sources` and checked against their SHA-256.

## Run

```
thunder --headless --mainchain-grpc-url http://localhost:50051                    # eCash mainnet (thunder-rust 0.19.1's rules), the default
thunder --headless --network betanet --mainchain-grpc-url http://localhost:50051  # betanet (0.18.2's rules)
thunder-cli getblockcount
```

The mainchain is Backbone (`backboned -grpc`) or an eCash node with bip300301_enforcer. See `doc/thunder.md`, "Mainnet".
