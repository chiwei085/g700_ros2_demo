#!/usr/bin/env bash
set +e

if [ ! -f /opt/vendor/lib/libORB_SLAM3.so ]; then
  echo "[WARN] vendor missing: /opt/vendor/lib/libORB_SLAM3.so"
else
  echo "[INFO] vendor library found: /opt/vendor/lib/libORB_SLAM3.so"
fi

if [ -f /opt/vendor/.vendor.stamp ]; then
  echo "[INFO] vendor stamp:"
  cat /opt/vendor/.vendor.stamp
else
  echo "[WARN] vendor stamp missing: /opt/vendor/.vendor.stamp"
fi

if command -v ldconfig >/dev/null 2>&1; then
  realsense_paths="$(ldconfig -p 2>/dev/null | grep librealsense2 | sed -E 's/.*=>[[:space:]]*//' | sort -u)"
  if [ -n "${realsense_paths}" ]; then
    bad_paths="$(echo "${realsense_paths}" | grep -v '^/usr/local/lib' || true)"
    if [ -n "${bad_paths}" ]; then
      echo "[WARN] librealsense may not be from /usr/local"
      echo "${realsense_paths}"
    else
      echo "[INFO] librealsense is resolved from /usr/local"
    fi
  else
    echo "[WARN] librealsense entries not found in ldconfig"
  fi
else
  echo "[WARN] ldconfig unavailable; skip librealsense source check"
fi

echo "[INFO] CMAKE_PREFIX_PATH order:"
if [ -n "${CMAKE_PREFIX_PATH}" ]; then
  echo "${CMAKE_PREFIX_PATH}" | tr ':' '\n'
else
  echo "(empty)"
fi
