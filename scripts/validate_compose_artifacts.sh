#!/usr/bin/env bash
set -euo pipefail

services=(conversation gateway identity media message presence push relationship transmite)
artifact_root="${1:-compose-artifacts}"
manifest="$artifact_root/MANIFEST.sha256"

if [[ ! -f "$manifest" ]]; then
    echo "missing artifact manifest: $manifest" >&2
    exit 1
fi

(
    cd "$artifact_root"
    sha256sum --check MANIFEST.sha256
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

    ldd_output="$(env -i PATH=/usr/bin:/bin LD_LIBRARY_PATH="$depends_dir" ldd "$binary" 2>&1)" || {
        echo "isolated ldd failed for $binary: $ldd_output" >&2
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
        if [[ ! -f "$depends_dir/$library_name" ]]; then
            echo "shared library is outside packaged closure for $binary: $library" >&2
            exit 1
        fi
    done < <(awk '
        /=> \/[^ ]+/ { print $3; next }
        /^[[:space:]]*\/[^ ]+/ { print $1 }
    ' <<<"$ldd_output" | LC_ALL=C sort -u)
done
