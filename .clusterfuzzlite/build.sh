#!/bin/bash -eu
# ClusterFuzzLite / OSS-Fuzz entry point. base-builder runs this at $SRC/build.sh.
# It honours $CC $CXX $CFLAGS $CXXFLAGS $LIB_FUZZING_ENGINE $OUT $SRC, so it just
# delegates to the single shared builder.
#
# ClusterFuzzLite passes no environment of its own into this build, so a run that fuzzes more than
# the default targets says so in a file of the checkout: cflite-pr.yml's weekly job writes
# .clusterfuzzlite/extra_targets (the backup reader's harnesses); it is never committed.
if [ -f "$SRC/mssql-extension/.clusterfuzzlite/extra_targets" ]; then
	TARGETS_EXTRA="$(cat "$SRC/mssql-extension/.clusterfuzzlite/extra_targets")"
	export TARGETS_EXTRA
fi
bash "$SRC/mssql-extension/fuzz/build.sh"
