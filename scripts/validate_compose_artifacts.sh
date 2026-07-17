#!/usr/bin/env bash
set -euo pipefail

services=(conversation gateway identity media message presence push relationship transmite)
artifact_root="${1:-compose-artifacts}"
if [[ ! -d "$artifact_root" ]]; then
    echo "missing artifact root directory: $artifact_root" >&2
    exit 1
fi
artifact_root="$(cd "$artifact_root" && pwd -P)"
manifest="$artifact_root/MANIFEST.sha256"
ldd_command="${LDD:-ldd}"
sha256sum_command="${SHA256SUM:-sha256sum}"

if [[ ! -f "$manifest" ]]; then
    echo "missing artifact manifest: $manifest" >&2
    exit 1
fi

actual_manifest="$(mktemp)"
trap 'rm -f "$actual_manifest"' EXIT
(
    cd "$artifact_root"
    find . \( -type f -o -type l \) ! -name MANIFEST.sha256 -print0 \
        | LC_ALL=C sort -z \
        | xargs -0 "$sha256sum_command" > "$actual_manifest"
    if ! cmp -s MANIFEST.sha256 "$actual_manifest"; then
        echo "manifest mismatch or unlisted artifact file" >&2
        exit 1
    fi
    # sha256sum --check is retained as an explicit integrity gate.
    "$sha256sum_command" --check MANIFEST.sha256
)

for service in "${services[@]}"; do
    binary="$artifact_root/$service/build/${service}_server"
    depends_dir="$artifact_root/$service/depends"
    [[ -x "$binary" ]] || {
        echo "missing executable service binary: $binary" >&2
        exit 1
    }
    if [[ ! -d "$depends_dir" ]]; then
        echo "missing shared-library directory: $depends_dir" >&2
        exit 1
    fi
    depends_dir_real="$(cd "$depends_dir" && pwd -P)"

    while IFS= read -r packaged_library; do
        library_name="$(basename "$packaged_library")"
        case "$library_name" in
            ld-linux*.so.*|ld-musl-*.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libgcc_s.so.*|libstdc++.so.*|libanl.so.*|libBrokenLocale.so.*|libcrypt.so.*|libnss_*.so.*|libresolv.so.*|libutil.so.*)
                echo "packaged runtime loader or system ABI library for $binary: $packaged_library" >&2
                exit 1
                ;;
        esac
    done < <(find "$depends_dir" -mindepth 1 -maxdepth 1 \( -type f -o -type l \) -print | LC_ALL=C sort)

    ldd_output="$(env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="$depends_dir" "$ldd_command" "$binary" 2>&1)" || {
        echo "isolated ldd failed for $binary: $ldd_output" >&2
        exit 1
    }
    if grep -Fq "not found" <<<"$ldd_output"; then
        echo "unresolved shared library for $binary:" >&2
        echo "$ldd_output" >&2
        exit 1
    fi

    while IFS=$'\t' read -r entry_kind library; do
        [[ -n "$library" ]] || continue
        library_name="$(basename "$library")"
        if [[ "$entry_kind" == "loader" ]]; then
            case "$library_name" in
                ld-linux*.so.*|ld-musl-*.so.*) ;;
                *)
                    echo "unexpected system library outside packaged closure for $binary: $library" >&2
                    exit 1
                    ;;
            esac
        fi

        if [[ ! -f "$library" ]]; then
            echo "shared library is outside packaged closure for $binary: $library" >&2
            exit 1
        fi
        resolved_library="$(realpath "$library")"
        case "$resolved_library" in
            "$depends_dir_real"/*) ;;
            /lib/*|/lib64/*|/usr/lib/*) ;;
            *)
                echo "shared library is outside packaged closure for $binary: $library" >&2
                exit 1
                ;;
        esac
    done < <(awk '
        /=> \/[^ ]+/ { print "dependency\t" $3; next }
        /^[[:space:]]*\/[^ ]+/ { print "loader\t" $1 }
    ' <<<"$ldd_output" | LC_ALL=C sort -u)
done
