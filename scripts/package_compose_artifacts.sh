#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=compose_artifact_core_abi.sh
. "$script_dir/compose_artifact_core_abi.sh"

services=(conversation gateway identity media message presence push relationship transmite)
build_root="${1:-build}"
artifact_root="${2:-compose-artifacts}"
ldd_command="${LDD:-ldd}"
sha256sum_command="${SHA256SUM:-sha256sum}"

rm -rf "$artifact_root"
mkdir -p "$artifact_root"

for service in "${services[@]}"; do
    binary="$build_root/$service/${service}_server"
    [[ -x "$binary" ]] || {
        echo "missing executable service binary: $binary" >&2
        exit 1
    }

    service_root="$artifact_root/$service"
    depends_dir="$service_root/depends"
    mkdir -p "$service_root/build" "$depends_dir"
    cp -p "$binary" "$service_root/build/${service}_server"

    ldd_output="$("$ldd_command" "$binary" 2>&1)" || {
        echo "ldd failed for $binary: $ldd_output" >&2
        exit 1
    }
    if grep -Fq "not found" <<<"$ldd_output"; then
        echo "unresolved shared library for $binary:" >&2
        echo "$ldd_output" >&2
        exit 1
    fi

    while IFS= read -r library; do
        [[ -n "$library" ]] || continue
        library_name="$(basename "$library")"
        if is_core_system_abi "$library_name"; then
            if ! is_system_library_path "$library"; then
                echo "core system ABI resolved outside pinned runtime paths for $binary: $library" >&2
                exit 1
            fi
            continue
        fi
        resolved_library="$(realpath "$library")" || {
            echo "unable to resolve shared library for $binary: $library" >&2
            exit 1
        }
        cp -L "$resolved_library" "$depends_dir/$library_name"
    done < <(awk '
        /=> \/[^ ]+/ { print $3; next }
        /^[[:space:]]*\/[^ ]+/ { print $1 }
    ' <<<"$ldd_output" | LC_ALL=C sort -u)
done

(
    cd "$artifact_root"
    find . \( -type f -o -type l \) ! -name MANIFEST.sha256 -print0 \
        | LC_ALL=C sort -z \
        | xargs -0 "$sha256sum_command" > MANIFEST.sha256
)
