# ClusterFuzzLite build image (OSS-Fuzz base-builder).
# Builds the libFuzzer harnesses with the sanitizer the CI matrix selects
# (address / undefined). The actual compile logic lives in fuzz/build.sh so the
# local and CI builds stay identical.
FROM gcr.io/oss-fuzz-base/base-builder:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl unzip zip perl make && rm -rf /var/lib/apt/lists/*

# Spec 087: the backup harnesses link backup_crypto, which needs OpenSSL 3's API (EVP_PKEY_fromdata,
# OSSL_PARAM_BLD). base-builder is Ubuntu 20.04, whose libssl-dev is 1.1.1, and the runner image carries no
# libcrypto: so the version the extension's vcpkg build pins (vcpkg.json's override: 3.5.4, LTS) is built here,
# static and uninstrumented, from its source archive checked by the SHA512 vcpkg verified for it.
ARG OPENSSL_VERSION=3.5.4
ARG OPENSSL_SHA512=a741933f67f0c8e58e41eea2a16f6172c9a7e164f8f33bea3440561907c36191bde06dee3d94097af33f27bc66cad27fa861c9b734fa25bcbc6c1b24928fadec
RUN curl -fsSL -o /tmp/openssl.tar.gz \
        "https://github.com/openssl/openssl/archive/refs/tags/openssl-${OPENSSL_VERSION}.tar.gz" && \
    echo "${OPENSSL_SHA512}  /tmp/openssl.tar.gz" | sha512sum -c - && \
    mkdir /tmp/openssl && tar -xzf /tmp/openssl.tar.gz --strip-components=1 -C /tmp/openssl && \
    cd /tmp/openssl && \
    env -u CFLAGS -u CXXFLAGS -u LDFLAGS ./Configure no-shared no-tests no-docs no-apps \
        --prefix=/opt/openssl3 --libdir=lib && \
    make -j"$(nproc)" build_libs && make install_dev && \
    cd / && rm -rf /tmp/openssl /tmp/openssl.tar.gz
ENV OPENSSL_PREFIX=/opt/openssl3

COPY . $SRC/mssql-extension
WORKDIR $SRC/mssql-extension
COPY .clusterfuzzlite/build.sh $SRC/build.sh
