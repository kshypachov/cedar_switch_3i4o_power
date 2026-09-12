#!/bin/sh
# Run the simulated test tier in the Linux container.
#
# Usage:  tests/ci/run-sim-tests.sh [extra twister args...]
#
# Requires Colima (or any Docker) to be running:
#   colima start --cpu 4 --memory 8 --disk 60 --mount /Volumes/Programming:w
#
# The whole west workspace is mounted, not just the application, because
# twister needs ZEPHYR_BASE and the modules next to it.
#
# native_sim/native/64 rather than plain native_sim: the container runs on
# arm64 with a 64-bit-only userspace, where the 32-bit default cannot link.
set -eu

APP_DIR=$(cd "$(dirname "$0")/../.." && pwd)
WS_DIR=$(cd "$APP_DIR/.." && pwd)
APP_REL=$(basename "$APP_DIR")
IMAGE=cedar-sim-tests:1

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "Building $IMAGE (first run only)..."
	docker build -t "$IMAGE" "$APP_DIR/tests/ci"
fi

exec docker run --rm \
	-v "$WS_DIR":/ws \
	-w /ws \
	-e ZEPHYR_BASE=/ws/zephyr \
	"$IMAGE" \
	python3 /ws/zephyr/scripts/twister \
		-T "/ws/$APP_REL/tests" \
		-p native_sim/native/64 \
		--outdir /tmp/twister-out \
		"$@"
