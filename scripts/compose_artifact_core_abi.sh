#!/usr/bin/env bash

# Libraries guaranteed by the pinned Ubuntu base image. Everything else must
# travel in the service-private artifact closure.
is_core_system_abi() {
    case "$(basename "$1")" in
        ld-linux*.so.*|ld-musl-*.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libgcc_s.so.*|libstdc++.so.*|libanl.so.*|libBrokenLocale.so.*|libcrypt.so.*|libnss_*.so.*|libresolv.so.*|libutil.so.*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_system_library_path() {
    case "$1" in
        /lib/*|/lib64/*|/usr/lib/*) return 0 ;;
        *) return 1 ;;
    esac
}
