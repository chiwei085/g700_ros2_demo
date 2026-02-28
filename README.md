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
