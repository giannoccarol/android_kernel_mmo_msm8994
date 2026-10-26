#!/usr/bin/env bash
# Build a committed Android 13 camera kernel in a new, private output folder.
set -euo pipefail
repo=$(git -C "$(dirname -- "$0")" rev-parse --show-toplevel)
output=$(realpath -m "${1:?usage: build-a13-kernel.sh NEW_OUTPUT CROSS_COMPILE_PREFIX [JOBS]}")
compiler=${2:?supply the Android GCC 4.9 prefix}
jobs=${3:-8}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid job count' >&2; exit 1; }
[[ $(git -C "$repo" branch --show-current) == lineage-20-cityman-camera ]] || {
    echo 'Select lineage-20-cityman-camera first' >&2; exit 1;
}
[[ -z $(git -C "$repo" status --porcelain --untracked-files=normal) ]] || {
    echo 'Commit or save source changes before building' >&2; exit 1;
}
[[ ! -e "$output" ]] || { echo 'Output directory already exists' >&2; exit 1; }
"${compiler}gcc" --version >/dev/null
revision=$(git -C "$repo" rev-parse HEAD)
mkdir -p "$output/source" "$output/build"
printf '%s\n' "$revision" > "$output/source.commit"
git -C "$repo" archive "$revision" | tar -xf - -C "$output/source"
export ARCH=arm64 SUBARCH=arm64 CROSS_COMPILE="$compiler"
export KBUILD_BUILD_USER=lorenzo KBUILD_BUILD_HOST=cityman-builder KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP=$(git -C "$repo" show -s --format=%cD "$revision")
export SOURCE_DATE_EPOCH=$(git -C "$repo" show -s --format=%ct "$revision")
export LOCALVERSION="-camera-g${revision:0:12}"
"${compiler}gcc" --version > "$output/toolchain.txt"
make -C "$output/source" O="$output/build" mmo_defconfig > "$output/config.log" 2>&1
make -C "$output/source" O="$output/build" -j"$jobs" Image.gz-dtb 2>&1 | tee "$output/build.log"
cp "$output/build/.config" "$output/config"
cp "$output/build/arch/arm64/boot/Image.gz-dtb" "$output/Image.gz-dtb"
(
    cd "$output"
    sha256sum source.commit toolchain.txt config Image.gz-dtb > SHA256SUMS
)
echo "Built $revision in $output"
