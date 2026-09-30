#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $# != 2 ]]; then
  echo 'Usage: bash tests/run-host.sh PATCHED_KERNEL_SOURCE PATCHED_MESA_SOURCE' >&2
  exit 2
fi
kernel=$(realpath "$1")
mesa=$(realpath "$2")
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$mesa/include" -I"$mesa/src/amd/vulkan/winsys/amdgpu" \
  "$here/host/test_batch.c" -o "$work/test-batch"
"$work/test-batch"
python3 "$here/host/test_queue_order.py" "$mesa"
python3 "$here/host/test_destroy_scope.py" "$mesa"
python3 "$here/host/test_optin.py" "$kernel" "$mesa"
python3 "$here/host/test_native_validator.py"
