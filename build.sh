#!/bin/sh
# Compile the firmware with the Particle Device OS buildpack.
# --platform linux/amd64 runs the x86 image under Rosetta on Apple Silicon.
set -e
cd "$(dirname "$0")"

# Clear stale artifacts first. The buildpack copies its output with a `mv` that
# can fail while the overall run still exits 0, which used to leave a previous
# build's firmware.bin in place - and a stale binary flashed to the device is
# indistinguishable from a bad code change until you read the serial log.
rm -f target/firmware.bin target/memory-use.log

docker run --rm --platform linux/amd64 \
  -v "$PWD":/input \
  -v "$PWD/target":/output \
  -e PLATFORM_ID=6 \
  particle/buildpack-particle-firmware:2.3.1-photon

# Fail loudly rather than leaving the caller to flash something that is not there.
if [ ! -f target/firmware.bin ]; then
  echo "ERROR: build produced no target/firmware.bin" >&2
  exit 1
fi

echo "Built: target/firmware.bin ($(wc -c < target/firmware.bin | tr -d ' ') bytes)"
