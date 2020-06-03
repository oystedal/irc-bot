#! /usr/bin/env bash

pushd ./third_party/fmt
mkdir -p build
cd build
cmake -G Ninja ..
ninja
popd

pushd ./third_party/openssl
# Arch linux specific config + patch stolen from AUR
PATCH="ca-dir.patch"
if [ ! -e "$PATCH" ]; then
    wget -nc -O "$PATCH" https://aur.archlinux.org/cgit/aur.git/plain/ca-dir.patch?h=openssl-static
    patch -p0 -i "$PATCH"
fi
if [ ! -e "Makefile" ]; then
    ./Configure --openssldir=/etc/ssl linux-x86_64 no-shared no-ssl3-method
fi
make -j10
popd

pushd ./third_party/c-ares
mkdir -p build
cd build
cmake \
    -G Ninja \
    -DCARES_STATIC=ON \
    -DCARES_SHARED=ON \
    -DCARES_INSTALL=OFF \
    ..
ninja
popd

pushd ./third_party/curl
if [ ! -e "Makefile" ]; then
    ./buildconf
    ./configure \
        --enable-ares \
        --disable-shared \
        --without-nghttp3 \
        --without-nghttp2 \
        --without-librtmp \
        --without-libmetalink \
        --disable-ldap \
        --without-brotli \
        --without-libpsl \
        --without-libidn2
fi
make -j10
popd
