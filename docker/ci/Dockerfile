ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90
FROM ${UBUNTU_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        autoconf \
        automake \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libboost-all-dev \
        libcpprest-dev \
        libcrypt-dev \
        libcurl4-openssl-dev \
        libev-dev \
        libfmt-dev \
        libgflags-dev \
        libgoogle-glog-dev \
        libgrpc++-dev \
        libgrpc-dev \
        libhiredis-dev \
        libjsoncpp-dev \
        libleveldb-dev \
        libmysqlclient-dev \
        libodb-boost-dev \
        libodb-dev \
        libodb-mysql-dev \
        libprotobuf-dev \
        libsnappy-dev \
        libspdlog-dev \
        libssl-dev \
        libtool \
        libunwind-dev \
        libxml2-dev \
        ninja-build \
        odb \
        pkg-config \
        protobuf-compiler \
        protobuf-compiler-grpc \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY docker/ci/dependencies.lock /tmp/dependencies.lock

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/brpc; \
    git -C /tmp/brpc remote add origin https://github.com/apache/brpc.git; \
    git -C /tmp/brpc fetch --depth 1 origin "$BRPC_REV"; \
    git -C /tmp/brpc checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/brpc rev-parse HEAD)" = "$BRPC_REV"; \
    cmake -S /tmp/brpc -B /tmp/brpc-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_BRPC_TOOLS=OFF \
        -DBUILD_UNIT_TESTS=OFF \
        -DWITH_GLOG=ON \
        -DWITH_SNAPPY=ON; \
    cmake --build /tmp/brpc-build --parallel "$(nproc)"; \
    cmake --install /tmp/brpc-build; \
    rm -rf /tmp/brpc /tmp/brpc-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/etcd-cpp-apiv3; \
    git -C /tmp/etcd-cpp-apiv3 remote add origin https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3.git; \
    git -C /tmp/etcd-cpp-apiv3 fetch --depth 1 origin "$ETCD_CPP_APIV3_REV"; \
    git -C /tmp/etcd-cpp-apiv3 checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/etcd-cpp-apiv3 rev-parse HEAD)" = "$ETCD_CPP_APIV3_REV"; \
    cmake -S /tmp/etcd-cpp-apiv3 -B /tmp/etcd-cpp-apiv3-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_ETCD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=ON \
        -DETCD_W_STRICT=OFF; \
    cmake --build /tmp/etcd-cpp-apiv3-build --parallel "$(nproc)"; \
    cmake --install /tmp/etcd-cpp-apiv3-build; \
    rm -rf /tmp/etcd-cpp-apiv3 /tmp/etcd-cpp-apiv3-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/redis-plus-plus; \
    git -C /tmp/redis-plus-plus remote add origin https://github.com/sewenew/redis-plus-plus.git; \
    git -C /tmp/redis-plus-plus fetch --depth 1 origin "$REDIS_PLUS_PLUS_REV"; \
    git -C /tmp/redis-plus-plus checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/redis-plus-plus rev-parse HEAD)" = "$REDIS_PLUS_PLUS_REV"; \
    cmake -S /tmp/redis-plus-plus -B /tmp/redis-plus-plus-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DREDIS_PLUS_PLUS_BUILD_STATIC=OFF \
        -DREDIS_PLUS_PLUS_BUILD_TEST=OFF; \
    cmake --build /tmp/redis-plus-plus-build --parallel "$(nproc)"; \
    cmake --install /tmp/redis-plus-plus-build; \
    rm -rf /tmp/redis-plus-plus /tmp/redis-plus-plus-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/cpr; \
    git -C /tmp/cpr remote add origin https://github.com/whoshuu/cpr.git; \
    git -C /tmp/cpr fetch --depth 1 origin "$CPR_REV"; \
    git -C /tmp/cpr checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/cpr rev-parse HEAD)" = "$CPR_REV"; \
    cmake -S /tmp/cpr -B /tmp/cpr-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_CPR_TESTS=OFF \
        -DBUILD_SHARED_LIBS=ON \
        -DUSE_SYSTEM_CURL=ON; \
    cmake --build /tmp/cpr-build --parallel "$(nproc)"; \
    cmake --install /tmp/cpr-build; \
    rm -rf /tmp/cpr /tmp/cpr-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/elasticlient; \
    git -C /tmp/elasticlient remote add origin https://github.com/seznam/elasticlient.git; \
    git -C /tmp/elasticlient fetch --depth 1 origin "$ELASTICLIENT_REV"; \
    git -C /tmp/elasticlient checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/elasticlient rev-parse HEAD)" = "$ELASTICLIENT_REV"; \
    cmake -S /tmp/elasticlient -B /tmp/elasticlient-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_ELASTICLIENT_EXAMPLE=OFF \
        -DBUILD_ELASTICLIENT_TESTS=OFF \
        -DBUILD_SHARED_LIBS=ON \
        -DUSE_SYSTEM_CPR=ON \
        -DUSE_SYSTEM_JSONCPP=ON; \
    cmake --build /tmp/elasticlient-build --parallel "$(nproc)"; \
    cmake --install /tmp/elasticlient-build; \
    rm -rf /tmp/elasticlient /tmp/elasticlient-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/amqp-cpp; \
    git -C /tmp/amqp-cpp remote add origin https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git; \
    git -C /tmp/amqp-cpp fetch --depth 1 origin "$AMQP_CPP_REV"; \
    git -C /tmp/amqp-cpp checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/amqp-cpp rev-parse HEAD)" = "$AMQP_CPP_REV"; \
    cmake -S /tmp/amqp-cpp -B /tmp/amqp-cpp-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DAMQP-CPP_BUILD_EXAMPLES=OFF \
        -DAMQP-CPP_BUILD_SHARED=ON; \
    cmake --build /tmp/amqp-cpp-build --parallel "$(nproc)"; \
    cmake --install /tmp/amqp-cpp-build; \
    rm -rf /tmp/amqp-cpp /tmp/amqp-cpp-build

RUN set -euo pipefail; \
    . /tmp/dependencies.lock; \
    git init /tmp/aws-sdk-cpp; \
    git -C /tmp/aws-sdk-cpp remote add origin https://github.com/aws/aws-sdk-cpp.git; \
    git -C /tmp/aws-sdk-cpp fetch --depth 1 origin "$AWS_SDK_CPP_REV"; \
    git -C /tmp/aws-sdk-cpp checkout --detach FETCH_HEAD; \
    test "$(git -C /tmp/aws-sdk-cpp rev-parse HEAD)" = "$AWS_SDK_CPP_REV"; \
    cmake -S /tmp/aws-sdk-cpp -B /tmp/aws-sdk-cpp-build -G Ninja \
        -DBUILD_ONLY=s3 \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DENABLE_TESTING=OFF; \
    cmake --build /tmp/aws-sdk-cpp-build --parallel "$(nproc)"; \
    cmake --install /tmp/aws-sdk-cpp-build; \
    rm -rf /tmp/aws-sdk-cpp /tmp/aws-sdk-cpp-build

RUN printf '%s\n' '/usr/local/lib' '/usr/local/lib64' > /etc/ld.so.conf.d/chatnow-local.conf \
    && ldconfig

WORKDIR /workspace
