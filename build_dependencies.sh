#! /usr/bin/env bash

pushd ./third_party/fmt
mkdir -p build
cd build
cmake -G Ninja ..
ninja
popd

pushd ./third_party/openssl
# Arch linux specific config + patch:
PATCH="ca-dir.patch"
# wget -nc -O "$PATCH" https://aur.archlinux.org/cgit/aur.git/plain/ca-dir.patch?h=openssl-static
# patch -p0 -i "$PATCH"
# ./Configure --openssldir=/etc/ssl linux-x86_64 no-shared no-ssl3-method
# make -j10
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
./buildconf
./configure --disable-shared --without-nghttp3 --without-nghttp2 --without-librtmp --without-libmetalink --disable-ldap --without-brotli --without-libpsl --without-libidn2 --enable-ares
make -j10
popd
