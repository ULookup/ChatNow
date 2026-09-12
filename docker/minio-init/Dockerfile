FROM quay.io/minio/mc:RELEASE.2024-10-08T09-37-26Z AS mc

FROM busybox:1.36.1-musl

COPY --from=mc /usr/bin/mc /usr/bin/mc
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/bin/sh", "/usr/local/bin/entrypoint.sh"]
