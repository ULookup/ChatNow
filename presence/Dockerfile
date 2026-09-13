# 声明基础镜像来源
ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90
FROM ${UBUNTU_IMAGE}
# 声明工作路径
WORKDIR /im
RUN mkdir -p /im/logs &&\
    mkdir -p /im/data &&\
    mkdir -p /im/conf &&\
    mkdir -p /im/bin
# 将可执行程序文件，拷贝进入镜像
COPY ./build/presence_server /im/bin
# 将可执行程序依赖，拷贝进入镜像
COPY ./depends/ /im/depends/
RUN apt-get update && apt-get install -y netcat-openbsd && rm -rf /var/lib/apt/lists/*
ENV LD_LIBRARY_PATH=/im/depends
# 设置容器的启动默认操作 --- 运行程序
CMD /im/bin/presence_server -flagfile=/im/conf/presence_server.conf
