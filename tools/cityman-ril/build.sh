#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to an Android NDK directory}"
compiler="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
output_dir=${1:-"$source_dir/build"}
mkdir -p -- "$output_dir"
"$compiler" -Os -Wall -Wextra -Werror -fPIC -shared \
  "$source_dir/cityman-ds-target.c" -ldl \
  -Wl,-soname,libcityman_ds_target.so -o "$output_dir/libcityman_ds_target.so"
"$compiler" -Os -Wall -Wextra -Werror -fPIC -shared \
  "$source_dir/cityman-ril-init.c" -ldl -llog \
  -Wl,-soname,libcityman_ril_init.so -o "$output_dir/libcityman_ril_init.so"
"$compiler" -Os -Wall -Wextra -Werror \
  "$source_dir/cityman-uim-bootstrap.c" -ldl -llog \
  -o "$output_dir/cityman-uim-bootstrap"
"$compiler" -Os -Wall -Wextra -Werror \
  "$source_dir/qmi-client-query.c" -ldl -o "$output_dir/qmi-client-query"
cp -- "$source_dir/cityman-uim-bootstrap.rc" "$output_dir/"
sha256sum -- "$output_dir/libcityman_ds_target.so" \
  "$output_dir/libcityman_ril_init.so" \
  "$output_dir/cityman-uim-bootstrap" "$output_dir/qmi-client-query"
