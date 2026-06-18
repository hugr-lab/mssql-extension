#!/bin/bash -eu
# ClusterFuzzLite / OSS-Fuzz entry point. base-builder runs this at $SRC/build.sh.
# It honours $CC $CXX $CFLAGS $CXXFLAGS $LIB_FUZZING_ENGINE $OUT $SRC, so it just
# delegates to the single shared builder.
bash "$SRC/mssql-extension/fuzz/build.sh"
