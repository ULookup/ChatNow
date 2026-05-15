#!/usr/bin/env bash

# install_aws_sdk_linux.sh
# ---------------------------------------------------------------------------
# 在 Linux 构建环境上安装 aws-sdk-cpp（仅 S3 module），用于 P4 媒体子系统。
# ---------------------------------------------------------------------------
# 用法：
#   sudo bash scripts/install_aws_sdk_linux.sh
#
# 行为：
#   - 在 /tmp/aws-sdk-cpp-build 下 clone v1.11.420（如已存在则跳过）
#   - cmake 仅构建 s3 component + 共享库 + Release
#   - sudo make install 到 /usr/local
#   - 运行 ldconfig 让 -laws-cpp-sdk-s3 / -laws-cpp-sdk-core 立即可链
#
# 卸载：
#   - sudo rm /usr/local/lib/libaws-cpp-sdk-{s3,core}.so*
#   - sudo rm -r /usr/local/include/aws
#   - sudo rm -r /usr/local/lib/cmake/AWSSDK /usr/local/lib/cmake/aws-cpp-sdk-*
#
# 仅在 Linux 上运行；mac 用户应在容器/远端机执行。
# ---------------------------------------------------------------------------

set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[install_aws_sdk_linux] 仅支持 Linux；当前: $(uname -s)" >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "[install_aws_sdk_linux] 需要 cmake，请先安装" >&2
    exit 1
fi

BUILD_ROOT="${AWS_SDK_BUILD_ROOT:-/tmp/aws-sdk-cpp-build}"
TAG="${AWS_SDK_TAG:-1.11.420}"
PREFIX="${AWS_SDK_PREFIX:-/usr/local}"

mkdir -p "$BUILD_ROOT"
cd "$BUILD_ROOT"

if [[ ! -d aws-sdk-cpp ]]; then
    echo "[install_aws_sdk_linux] 克隆 aws-sdk-cpp $TAG ..."
    git clone --recurse-submodules --depth 1 -b "$TAG" \
        https://github.com/aws/aws-sdk-cpp.git aws-sdk-cpp
fi

mkdir -p aws-sdk-cpp/build
cd aws-sdk-cpp/build

if [[ ! -f Makefile ]]; then
    echo "[install_aws_sdk_linux] cmake 配置 ..."
    cmake .. \
        -DBUILD_ONLY="s3" \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
fi

echo "[install_aws_sdk_linux] 编译 ..."
make -j"$(nproc)"

echo "[install_aws_sdk_linux] 安装到 $PREFIX ..."
make install

echo "[install_aws_sdk_linux] 刷新 ld 缓存 ..."
ldconfig

echo "[install_aws_sdk_linux] 完成。验证："
ldconfig -p | grep -E "libaws-cpp-sdk-(s3|core)" || true
echo "[install_aws_sdk_linux] OK"
