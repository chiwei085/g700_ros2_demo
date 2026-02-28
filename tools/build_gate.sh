#!/usr/bin/env bash
set -euo pipefail

MODE="prebuilt"
PKGS="minimal"
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --pkgs)
      PKGS="${2:-}"
      shift 2
      ;;
    --clean)
      DO_CLEAN=1
      shift
      ;;
    *)
      echo "[ERROR] unknown argument: $1"
      exit 1
      ;;
  esac
done

if [[ "$MODE" != "prebuilt" && "$MODE" != "source" ]]; then
  echo "[ERROR] invalid --mode: $MODE"
  exit 1
fi

if [[ "$PKGS" != "minimal" && "$PKGS" != "full" ]]; then
  echo "[ERROR] invalid --pkgs: $PKGS"
  exit 1
fi

echo "[INFO] build gate start (mode=$MODE pkgs=$PKGS clean=$DO_CLEAN)"

cd /ws

clear_workspace_outputs() {
  for d in /ws/build /ws/install /ws/log; do
    if [[ -d "$d" ]]; then
      find "$d" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    fi
  done
}

if [[ "$DO_CLEAN" -eq 1 ]]; then
  echo "[INFO] cleaning /ws/build /ws/install /ws/log"
  clear_workspace_outputs
fi

echo "[INFO] sanity check: colcon list"
if ! colcon list; then
  echo "[ERROR] colcon list failed"
  exit 2
fi

echo "[INFO] sourcing /opt/ros/humble/setup.bash"
set +u
# shellcheck source=/dev/null
source /opt/ros/humble/setup.bash
set -u

ensure_ros_pkg() {
  local ros_pkg="$1"
  local apt_pkg="$2"
  if ros2 pkg prefix "$ros_pkg" >/dev/null 2>&1; then
    return 0
  fi
  echo "[WARN] missing ROS package '$ros_pkg'; installing '$apt_pkg'"
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends "$apt_pkg"
  set +u
  # shellcheck source=/dev/null
  source /opt/ros/humble/setup.bash
  set -u
}

ensure_ros_pkg diagnostic_updater ros-humble-diagnostic-updater

if [[ "$MODE" == "prebuilt" ]]; then
  if [[ -f /opt/vendor/local_setup.bash ]]; then
    echo "[INFO] sourcing /opt/vendor/local_setup.bash"
    set +u
    # shellcheck source=/dev/null
    source /opt/vendor/local_setup.bash
    set -u
  fi

  if [[ ! -f /opt/vendor/lib/libORB_SLAM3.so ]]; then
    echo "[ERROR] missing /opt/vendor/lib/libORB_SLAM3.so"
    exit 4
  fi

  echo "[INFO] checking vendor prefix via ros2 pkg prefix orbslam3_ros2_vendor"
  vendor_prefix="$(ros2 pkg prefix orbslam3_ros2_vendor 2>/dev/null || true)"
  if [[ -z "$vendor_prefix" || "$vendor_prefix" != *"/opt/vendor"* ]]; then
    echo "[ERROR] orbslam3_ros2_vendor prefix is not /opt/vendor (got: ${vendor_prefix:-<empty>})"
    echo "[ERROR] AMENT_PREFIX_PATH=${AMENT_PREFIX_PATH:-}"
    echo "[ERROR] CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:-}"
    exit 3
  fi
fi

if [[ "$MODE" == "source" ]]; then
  export CXXFLAGS="${CXXFLAGS:-} -Wno-error=type-limits"
  echo "[INFO] source mode CXXFLAGS=${CXXFLAGS}"
  if [[ -f /repo/tools/patches/pangolin-no-werror.patch ]]; then
    echo "[INFO] applying source patch: pangolin-no-werror.patch"
    patch --batch --forward -p1 -d /ws/src/orbslam3_ros2_vendor < /repo/tools/patches/pangolin-no-werror.patch || true
  fi
  if [[ -f /repo/tools/patches/vendor-build-parallel.patch ]]; then
    echo "[INFO] applying source patch: vendor-build-parallel.patch"
    patch --batch --forward -p1 -d /ws/src/orbslam3_ros2_vendor < /repo/tools/patches/vendor-build-parallel.patch || true
  fi
fi

build_cmd=(colcon build --merge-install --parallel-workers 1 --cmake-args -DCMAKE_BUILD_TYPE=Release)
if [[ "$MODE" == "prebuilt" ]]; then
  build_cmd+=(--packages-skip orbslam3_ros2_vendor)
fi

layout_file="/ws/install/.colcon_install_layout"
if [[ -f "$layout_file" ]]; then
  current_layout="$(cat "$layout_file" 2>/dev/null || true)"
  if [[ "$current_layout" != "merged" ]]; then
    echo "[WARN] install layout is '$current_layout' (expected 'merged'); cleaning workspace outputs"
    clear_workspace_outputs
  fi
fi

echo "[INFO] running build: ${build_cmd[*]}"
"${build_cmd[@]}"

echo "[INFO] post-build ldd check"
candidate_bin="$(find /ws/install -type f -executable \( -name '*orbslam3*' -o -name '*rgbd*' \) | head -n1 || true)"
if [[ -n "$candidate_bin" ]]; then
  echo "[INFO] ldd target: $candidate_bin"
  ldd "$candidate_bin" | grep -E 'libORB_SLAM3|not found' || true
else
  echo "[WARN] skip ldd check (no matching executable found under /ws/install)"
fi

echo "[INFO] workspace size summary"
du -sh /ws/build /ws/install /ws/log || true

echo "[INFO] build gate passed"
