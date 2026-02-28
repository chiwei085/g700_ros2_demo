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

stamp_rev="$(awk -F= '/^vendor_rev=/{print $2}' /opt/vendor/.vendor.stamp 2>/dev/null | tail -n1)"
ws_vendor_repo="/ws/src/orbslam3_ros2_vendor"
if [ -n "${stamp_rev}" ] && [ "${stamp_rev}" != "unknown" ] && [ -d "${ws_vendor_repo}/.git" ]; then
  ws_rev="$(git -C "${ws_vendor_repo}" rev-parse HEAD 2>/dev/null || echo unknown)"
  if [ "${ws_rev}" != "unknown" ] && [ "${ws_rev}" != "${stamp_rev}" ]; then
    echo "[WARN] workspace vendor rev (${ws_rev}) differs from image vendor stamp (${stamp_rev})"
    echo "[WARN] run: ./build_container.py build-vendor"
  fi
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
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
  echo "${CMAKE_PREFIX_PATH}" | tr ':' '\n'
else
  echo "(empty)"
fi

if [ -n "${AMENT_PREFIX_PATH:-}" ]; then
  vendor_idx="$(echo "${AMENT_PREFIX_PATH}" | tr ':' '\n' | nl -ba | awk '$2=="/opt/vendor"{print $1; exit}')"
  ws_install_idx="$(echo "${AMENT_PREFIX_PATH}" | tr ':' '\n' | nl -ba | awk '$2=="/ws/install"{print $1; exit}')"
  if [ -n "${vendor_idx}" ] && [ -n "${ws_install_idx}" ] && [ "${ws_install_idx}" -lt "${vendor_idx}" ]; then
    echo "[WARN] /ws/install appears before /opt/vendor in AMENT_PREFIX_PATH"
  fi
fi
