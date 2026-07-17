ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90
FROM ${UBUNTU_IMAGE}
ENV LD_LIBRARY_PATH=/im/depends
WORKDIR /im
RUN mkdir -p /im/logs && mkdir -p /im/data && mkdir -p /im/conf && mkdir -p /im/bin
COPY ./build/conversation_server /im/bin
COPY ./depends/ /im/depends/
RUN apt-get update && apt-get install -y netcat-openbsd && rm -rf /var/lib/apt/lists/*
CMD /im/bin/conversation_server -flagfile=/im/conf/conversation_server.conf
