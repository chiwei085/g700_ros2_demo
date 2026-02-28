#!/usr/bin/env bash
set -euo pipefail

if [ -r /home/rvl/.docker.xauth ]; then
  cp -f /home/rvl/.docker.xauth /home/rvl/.Xauthority
  chmod 0600 /home/rvl/.Xauthority
  export XAUTHORITY=/home/rvl/.Xauthority
fi

check_args=()
if [ "${VENDOR_CHECK_JSON:-0}" = "1" ]; then
  check_args+=(--json)
fi
if [ "${VENDOR_CHECK_STRICT:-0}" = "1" ]; then
  check_args+=(--assert-prebuilt-ready)
fi

if [ "${VENDOR_CHECK_STRICT:-0}" = "1" ]; then
  python3 /usr/local/bin/vendor_check.py "${check_args[@]}"
else
  python3 /usr/local/bin/vendor_check.py "${check_args[@]}" || {
    echo "[WARN] vendor_check.py failed (strict mode disabled)"
  }
fi

exec "$@"
