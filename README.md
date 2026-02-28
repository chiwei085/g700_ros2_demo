# ros2_demo

ROS 2 Humble container workspace (arm64) with:
- `orbslam3_ros2` and `realsense-ros` as git submodules
- Docker BuildKit optimized image build
- Conan-managed non-ROS dependencies for `orbslam3_ros2`
- Local tooling executed via `uv`

## Prerequisites

- Docker + Docker Compose v2
- `uv` installed locally
- Linux host with `/dev/bus/usb` access (for RealSense)

## One-Time Setup

1. Sync Python tool environment:

```bash
uv sync
```

2. Initialize submodules (shallow):

```bash
git submodule update --init --recursive --depth 1
```

## Local Container Commands (via uv)

All local helper commands should be run with `uv run`.
Prefer `uv run ./build_container.py ...` so Python dependencies are resolved by `uv` and host missing-package errors are avoided.

1. Build image:

```bash
uv run python build_container.py build
```

2. Start / recreate container:

```bash
uv run python build_container.py up -d --build --force-recreate
```

3. Open shell in running container:

```bash
uv run python build_container.py exec
```

4. Stop and remove container:

```bash
uv run python build_container.py down
```

`replug` flow is intentionally removed. If device/runtime state changes, just recreate the container with `--force-recreate`.

## Dev Toolchain In Container

Container image includes LLVM tools from Ubuntu 22.04 official repositories:
- `clangd-15`
- `clang-format-15`
- `clang-tidy-15`
- `clang-tools-15` (for utilities such as `run-clang-tidy` and `clang-apply-replacements`)

`update-alternatives` is configured during build so default commands resolve to version 15:
- `clangd`
- `clang-format`
- `clang-tidy`

Check versions inside container:

```bash
clangd --version
clang-format --version
clang-tidy --version
```

## RealSense Override

`build_container.py` supports `--realsense auto|on|off` (default `auto`):
- `auto`: generate/use override only when Intel camera nodes are detected
- `on`: always generate/use override
- `off`: disable override

Debug merged compose files:

```bash
uv run python build_container.py --realsense auto --print-files config
```

Generated file:
- `compose.realsense.override.yml` (git-ignored)

## Build Gates

Run workspace build gates from host via the wrapper:

```bash
./build_container.py --vendor-mode prebuilt exec -- bash -lc "/repo/tools/build_gate.sh --mode prebuilt --pkgs minimal"
./build_container.py --vendor-mode source exec -- bash -lc "/repo/tools/build_gate.sh --mode source --pkgs full"
```

## Demo Quickstart (RealSense + ORB-SLAM3 + YOLO)

Run the demo inside container shells (for example, first open one shell with `uv run ./build_container.py exec`, then use two terminals).

### A) Terminal A — Start RealSense (RGB-D)

```bash
ros2 launch realsense2_camera rs_launch.py \
  camera_name:=camera \
  enable_sync:=true \
  align_depth.enable:=true \
  enable_gyro:=false \
  enable_accel:=false
```

### B) Terminal B — Start SLAM + YOLO (with GUI)

```bash
ros2 launch yolo_ros realsense_d435i_rgbd_yolo_slam.launch.py \
  start_realsense:=false \
  wait_for_topics:=true \
  enable_yolo_infer:=true \
  yolo_show_window:=true \
  yolo_target_fps:=2.0 \
  yolo_ort_threads:=1 \
  yolo_conf_thres:=0.25 \
  yolo_iou_thres:=0.45
```

### C) Expected outputs (what you should see)

- YOLO HighGUI window opens and shows camera frames with boxes.
- ORB-SLAM3 logs: "topic gate passed" and "New Map created ..."
- Useful topics: `/orb_slam3/camera_pose`, `/tf`

### D) Performance knobs (CPU-friendly)

- `yolo_target_fps`: set to `1.0` if CPU is tight
- `yolo_ort_threads`: keep `1` when SLAM is running
- `yolo_show_window`: set `false` for headless runs
- `draw_labels`: set `false` to reduce overlay drawing overhead

### E) Cleanup (avoid duplicate nodes)

Run inside the container:

```bash
pkill -f "ros2 launch" || true
pkill -f "realsense2_camera" || true
pkill -f "orb_slam3" || true
pkill -f "yolo_infer" || true
pkill -f "yolo_dummy" || true
pkill -f "ros_rgbd" || true
pkill -f "run_and_tee.py" || true
```

If you also started source profile processes, stop them before running this demo.

## Conan and Cache Strategy

`compose.yml` mounts project-local caches:
- `./.cache/conan2 -> /home/rvl/.conan2`
- `./.cache/ccache -> /home/rvl/.cache/ccache`

Both are git-ignored.

Inside container (`/ws`) build `orbslam3_ros2`:

```bash
conan install src/orbslam3_ros2 \
  -pr:h src/orbslam3_ros2/conan/profiles/myprofile \
  -pr:b src/orbslam3_ros2/conan/profiles/myprofile \
  -of build/conan \
  -b missing

source /opt/ros/humble/setup.bash
colcon build --packages-select orbslam3_ros2 \
  --cmake-args \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/build/conan/conan_toolchain.cmake \
    -DCMAKE_PREFIX_PATH=$PWD/build/conan \
    -DCMAKE_BUILD_TYPE=Release
```

## Repository Notes

- `Dockerfile`: base image, RealSense SDK, Conan tooling
- `compose.yml`: runtime mounts and cache persistence
- `build_container.py`: compose wrapper (BuildKit env + optional RealSense override)
- `tools/gen_realsense_override.py`: Intel node detection and override generation

## Cache Design Notes

- APT installs use BuildKit cache mounts:
  - `/var/cache/apt`
  - `/var/lib/apt/lists`
- LLVM dev tools are installed in a dedicated Docker layer to avoid invalidating the larger base-package layer.
- Local persistent runtime caches remain in project mounts:
  - `./.cache/conan2`
  - `./.cache/ccache`
