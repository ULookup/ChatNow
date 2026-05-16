FROM ubuntu:24.04
WORKDIR /im
RUN mkdir -p /im/logs && mkdir -p /im/data && mkdir -p /im/conf && mkdir -p /im/bin
COPY ./build/conversation_server /im/bin
COPY ./depends/* /lib/x86_64-linux-gnu/
COPY ./nc /bin
CMD /im/bin/conversation_server -flagfile=/im/conf/conversation_server.conf
