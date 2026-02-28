# syntax=docker/dockerfile:1.7

FROM --platform=linux/arm64 ros:humble AS base

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Taipei
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

ARG BASE_BUILD_JOBS=1
ENV CMAKE_BUILD_PARALLEL_LEVEL=${BASE_BUILD_JOBS}
ENV MAKEFLAGS=-j${BASE_BUILD_JOBS}

# ========== Base system, ROS tools, and build prerequisites ==========
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends software-properties-common; \
    add-apt-repository universe; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
    tzdata locales ca-certificates \
    sudo git curl wget vim \
    ccache \
    build-essential cmake ninja-build pkg-config \
    libboost-serialization-dev \
    python3 python3-pip python3-venv python3-rosdep \
    python3-colcon-common-extensions python3-argcomplete \
    udev usbutils v4l-utils \
    ros-humble-rosbag2 \
    ros-humble-tf2 ros-humble-tf2-ros ros-humble-tf2-tools \
    ros-humble-vision-msgs \
    ros-humble-cv-bridge ros-humble-image-transport \
    ros-humble-message-filters ros-humble-camera-info-manager \
    ros-humble-image-geometry \
    libgl1-mesa-dev libglu1-mesa-dev libepoxy-dev libglew-dev \
    libx11-dev libxi-dev libxrandr-dev libxcursor-dev libxinerama-dev libxxf86vm-dev; \
    echo "${TZ}" > /etc/timezone; \
    locale-gen en_US.UTF-8

# ========== C/C++ dev tools from Ubuntu 22.04 repos ==========
ARG LLVM_VERSION=15
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
    "clangd-${LLVM_VERSION}" \
    "clang-format-${LLVM_VERSION}" \
    "clang-tidy-${LLVM_VERSION}" \
    "clang-tools-${LLVM_VERSION}"; \
    update-alternatives --install /usr/bin/clangd clangd "/usr/bin/clangd-${LLVM_VERSION}" 150 \
    --slave /usr/bin/clang-format clang-format "/usr/bin/clang-format-${LLVM_VERSION}" \
    --slave /usr/bin/clang-tidy clang-tidy "/usr/bin/clang-tidy-${LLVM_VERSION}"; \
    update-alternatives --set clangd "/usr/bin/clangd-${LLVM_VERSION}"; \
    clangd --version; \
    clang-format --version; \
    clang-tidy --version

# ========== RealSense SDK (source build, pinned) ==========
ARG LIBREALSENSE_TAG=v2.56.5
ARG LIBREALSENSE_BUILD_JOBS=${BASE_BUILD_JOBS}
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    set -eux; \
    apt-get update; \
    apt-get remove -y 'librealsense2*' || true; \
    apt-get install -y --no-install-recommends \
    libusb-1.0-0-dev libssl-dev libudev-dev \
    libgtk-3-dev libglfw3-dev libopencv-dev; \
    workdir="/tmp/librealsense-src"; \
    rm -rf "${workdir}"; \
    git clone --depth=1 --branch "${LIBREALSENSE_TAG}" https://github.com/IntelRealSense/librealsense.git "${workdir}"; \
    cmake -S "${workdir}" -B "${workdir}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DBUILD_WITH_DDS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_GRAPHICAL_EXAMPLES=OFF \
    -DCHECK_FOR_UPDATES=OFF; \
    cmake --build "${workdir}/build" -j"${LIBREALSENSE_BUILD_JOBS}"; \
    cmake --install "${workdir}/build"; \
    ldconfig; \
    apt-mark hold 'librealsense2*' || true; \
    rm -rf "${workdir}"

# ========== ONNX Runtime (C++ prebuilt) ==========
ARG ORT_VERSION=1.23.0
ENV ONNXRUNTIME_ROOT=/opt/onnxruntime
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends tar libgomp1; \
    ort_arch="aarch64"; \
    url="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-${ort_arch}-${ORT_VERSION}.tgz"; \
    tmp_dir="/tmp/ort"; \
    rm -rf "${tmp_dir}"; \
    mkdir -p "${tmp_dir}"; \
    curl -L "${url}" -o "${tmp_dir}/ort.tgz"; \
    tar -xzf "${tmp_dir}/ort.tgz" -C "${tmp_dir}"; \
    dir="$(find "${tmp_dir}" -maxdepth 1 -type d -name "onnxruntime-linux-*-*${ORT_VERSION}*" | head -n1)"; \
    test -n "${dir}"; \
    rm -rf "${ONNXRUNTIME_ROOT}"; \
    mkdir -p "${ONNXRUNTIME_ROOT}"; \
    cp -a "${dir}/include" "${ONNXRUNTIME_ROOT}/"; \
    cp -a "${dir}/lib" "${ONNXRUNTIME_ROOT}/"; \
    echo "${ONNXRUNTIME_ROOT}/lib" > /etc/ld.so.conf.d/onnxruntime.conf; \
    ldconfig; \
    rm -rf "${tmp_dir}"

# ===== XDG_RUNTIME_DIR (Qt/GUI) =====
RUN printf '%s\n' \
    '# Ensure XDG_RUNTIME_DIR exists in containers (Qt/GUI)' \
    'if [ -z "${XDG_RUNTIME_DIR:-}" ]; then' \
    '  export XDG_RUNTIME_DIR="/tmp/runtime-${USER}"' \
    'fi' \
    'mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true' \
    'chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true' \
    > /etc/profile.d/xdg-runtime.sh \
    && chmod +x /etc/profile.d/xdg-runtime.sh

# ===== create non-root user =====
ARG USERNAME=rvl
ARG USER_UID=1000
ARG USER_GID=1000
RUN groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd -m -u ${USER_UID} -g ${USER_GID} -s /bin/bash ${USERNAME} \
    && usermod -aG sudo,video,plugdev,dialout ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/99-${USERNAME} \
    && chmod 0440 /etc/sudoers.d/99-${USERNAME}

# ===== install bash-it =====
RUN git clone --depth=1 https://github.com/Bash-it/bash-it.git /opt/bash-it \
    && chown -R ${USERNAME}:${USERNAME} /opt/bash-it

# ========== rosdep ==========
RUN rosdep init 2>/dev/null || true
RUN rosdep update || true

USER ${USERNAME}
ENV CONAN_HOME=/home/${USERNAME}/.conan2
RUN mkdir -p "${CONAN_HOME}" \
    && /opt/bash-it/install.sh --silent \
    && sed -i 's/^export BASH_IT_THEME=.*/export BASH_IT_THEME="powerline"/' ~/.bashrc \
    && echo "source /opt/ros/humble/setup.bash" >> ~/.bashrc \
    && echo '[ -f /etc/profile.d/xdg-runtime.sh ] && source /etc/profile.d/xdg-runtime.sh' >> ~/.bashrc \
    && echo 'export CONAN_HOME=${CONAN_HOME}' >> ~/.bashrc

WORKDIR /ws
CMD ["bash"]

FROM base AS vendor-builder

USER root
WORKDIR /tmp/vendor_ws

ARG VENDOR_BUILD_JOBS=1
ARG VENDOR_CACHE_BUST=static
ARG VENDOR_GIT_SHA=unknown
ENV CMAKE_BUILD_PARALLEL_LEVEL=${VENDOR_BUILD_JOBS}
ENV MAKEFLAGS=-j${VENDOR_BUILD_JOBS}

COPY colcon_ws/src/orbslam3_ros2_vendor ./src/orbslam3_ros2_vendor
COPY tools/patches/vendor-build-parallel.patch /tmp/vendor-build-parallel.patch
COPY tools/patches/pangolin-no-werror.patch /tmp/pangolin-no-werror.patch

RUN --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    set -eux; \
    echo "vendor cache bust: ${VENDOR_CACHE_BUST}"; \
    patch --batch --forward -p1 -d /tmp/vendor_ws/src/orbslam3_ros2_vendor \
      < /tmp/vendor-build-parallel.patch || true; \
    patch --batch --forward -p1 -d /tmp/vendor_ws/src/orbslam3_ros2_vendor \
      < /tmp/pangolin-no-werror.patch || true; \
    export CXXFLAGS="${CXXFLAGS:-} -Wno-error=type-limits"; \
    set +u; \
    source /opt/ros/humble/setup.bash; \
    set -u; \
    colcon build --merge-install \
      --install-base /opt/vendor \
      --build-base /tmp/vendor_ws/build \
      --packages-select orbslam3_ros2_vendor \
      --parallel-workers "${VENDOR_BUILD_JOBS}" \
      --cmake-args \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache; \
    { \
      echo "build_time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
      echo "vendor_rev=${VENDOR_GIT_SHA}"; \
    } > /opt/vendor/.vendor.stamp; \
    rm -rf /tmp/vendor_ws/build /tmp/vendor_ws/log

FROM base AS runtime

USER root
COPY --from=vendor-builder /opt/vendor /opt/vendor
COPY tools/container/entrypoint.sh /usr/local/bin/ros_entrypoint_ext.sh
COPY tools/container/vendor_check.py /usr/local/bin/vendor_check.py
RUN chmod +x /usr/local/bin/ros_entrypoint_ext.sh /usr/local/bin/vendor_check.py

ENV CMAKE_PREFIX_PATH=/opt/vendor:/opt/onnxruntime:${CMAKE_PREFIX_PATH}
ENV LD_LIBRARY_PATH=/opt/vendor/lib:/opt/onnxruntime/lib:${LD_LIBRARY_PATH}

ARG USERNAME=rvl
USER ${USERNAME}
RUN echo '[ -f /opt/vendor/local_setup.bash ] && source /opt/vendor/local_setup.bash' >> /home/${USERNAME}/.bashrc

ENTRYPOINT ["/usr/local/bin/ros_entrypoint_ext.sh"]
WORKDIR /ws
CMD ["bash"]
