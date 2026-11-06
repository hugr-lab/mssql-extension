#!/usr/bin/env bash
# Spec 091: run tools/code_pages/bench_remote.sh on a set of Google Cloud machines, one per
# CPU the code-page kernels care about, in parallel, and delete them afterwards.
#
#   tools/code_pages/bench_gcp.sh <project> [results-dir] [machine ...]
#
# Each machine is a Spot VM (Ubuntu 24.04) with a run limit: GCP deletes it after
# MAX_RUN on its own, so a run that dies leaves nothing behind for long; the script also
# deletes every machine it created on exit. The working tree's code-page module, test and
# bench go over by scp (uncommitted changes included). Results land in results-dir, one
# file per machine. Machines: name=machine-type,zone,arch[,min-cpu-platform].
set -euo pipefail

PROJECT="${1:?usage: bench_gcp.sh <project> [results-dir] [machine ...]}"
RESULTS="${2:-bench-gcp-$(date +%Y%m%d-%H%M%S)}"
shift $(( $# >= 2 ? 2 : 1 ))
MAX_RUN="${MAX_RUN:-60m}"
ROOT="$(git rev-parse --show-toplevel)"

if [ $# -gt 0 ]; then
	MACHINES=("$@")
else
	MACHINES=(
		"zen3=n2d-standard-2,us-central1-a,amd64,AMD Milan"
		"zen4=c3d-standard-4,us-central1-a,amd64"
		"zen5=c4d-standard-2,us-central1-a,amd64"
		"sapphire-rapids=c3-standard-4,us-central1-a,amd64"
		"ice-lake=n2-standard-2,us-central1-a,amd64,Intel Ice Lake"
		"axion=c4a-standard-2,us-central1-a,arm64"
		"ampere=t2a-standard-2,us-central1-a,arm64"
	)
fi

# The key gcloud compute ssh uses, given to each machine at creation (instance metadata), so
# the parallel runs never race to add it to the project's metadata.
KEY="$HOME/.ssh/google_compute_engine"
if [ ! -f "$KEY" ]; then
	ssh-keygen -t rsa -b 3072 -f "$KEY" -N "" -q -C "$USER"
fi
SSH_KEYS="$USER:$(cat "$KEY.pub")"

mkdir -p "$RESULTS"
TAR="$(mktemp -d)/code-page-bench.tar.gz"
(
	cd "$ROOT"
	# What bench_remote.sh needs, from the working tree; the server data the test reads.
	# No macOS extended attributes in it (GNU tar on the machine warns about each).
	COPYFILE_DISABLE=1 tar --no-xattrs -czf "$TAR" src/include/codec/code_page src/codec/code_page test/cpp/codec/test_code_page.cpp \
		test/cpp/bench_code_page.cpp tools/code_pages/bench_remote.sh \
		$(git ls-files tools/code_pages/data)
)

CREATED=()
cleanup() {
	if [ ${#CREATED[@]} -gt 0 ]; then
		echo "deleting: ${CREATED[*]}"
		for entry in "${CREATED[@]}"; do
			gcloud compute instances delete "${entry%%@*}" --zone "${entry##*@}" --project "$PROJECT" --quiet \
				>/dev/null 2>&1 || true
		done
	fi
	echo "left in $PROJECT:"
	gcloud compute instances list --project "$PROJECT" --format="value(name,zone,status)" || true
}
trap cleanup EXIT

run_one() {
	local spec="$1" name type zone arch platform rest disk image vm log
	name="${spec%%=*}"
	rest="${spec#*=}"
	IFS=, read -r type zone arch platform <<<"$rest"
	vm="cp-bench-$name"
	log="$RESULTS/$name.txt"
	case "$type" in
	c4a-* | c4d-* | c4-*) disk=hyperdisk-balanced ;;
	*) disk=pd-balanced ;;
	esac
	image="ubuntu-2404-lts-$arch"
	{
		echo "### $name: $type in $zone${platform:+, min CPU platform $platform}"
		local args=(--project "$PROJECT" --zone "$zone" --machine-type "$type"
			--image-family "$image" --image-project ubuntu-os-cloud
			--boot-disk-size 20GB --boot-disk-type "$disk"
			--provisioning-model SPOT --instance-termination-action DELETE --max-run-duration "$MAX_RUN"
			--metadata "ssh-keys=$SSH_KEYS" --quiet)
		if [ -n "$platform" ]; then
			args+=(--min-cpu-platform "$platform")
		fi
		gcloud compute instances create "$vm" "${args[@]}" 2>&1 | tail -2
		for _ in $(seq 1 30); do
			if gcloud compute ssh "$vm" --zone "$zone" --project "$PROJECT" --quiet --command true >/dev/null 2>&1; then
				break
			fi
			sleep 10
		done
		gcloud compute scp "$TAR" "$vm:~/" --zone "$zone" --project "$PROJECT" --quiet >/dev/null
		gcloud compute ssh "$vm" --zone "$zone" --project "$PROJECT" --quiet --command \
			"mkdir -p src && tar -xzf code-page-bench.tar.gz -C src && BENCH_ALIGN_OFFSETS='${BENCH_ALIGN_OFFSETS:-0}' bash src/tools/code_pages/bench_remote.sh src" 2>&1
		gcloud compute instances delete "$vm" --zone "$zone" --project "$PROJECT" --quiet >/dev/null 2>&1 || true
	} >"$log" 2>&1
	echo "done: $name -> $log"
}

pids=()
for spec in "${MACHINES[@]}"; do
	name="${spec%%=*}"
	rest="${spec#*=}"
	IFS=, read -r _ zone _ _ <<<"$rest"
	CREATED+=("cp-bench-$name@$zone")
	run_one "$spec" &
	pids+=($!)
done
for pid in "${pids[@]}"; do
	wait "$pid" || true
done
echo "results in $RESULTS"
