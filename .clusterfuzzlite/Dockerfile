# ClusterFuzzLite build image (OSS-Fuzz base-builder).
# Builds the libFuzzer harnesses with the sanitizer the CI matrix selects
# (address / undefined). The actual compile logic lives in fuzz/build.sh so the
# local and CI builds stay identical.
FROM gcr.io/oss-fuzz-base/base-builder:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl unzip zip && rm -rf /var/lib/apt/lists/*

COPY . $SRC/mssql-extension
WORKDIR $SRC/mssql-extension
COPY .clusterfuzzlite/build.sh $SRC/build.sh
