#!/bin/bash
set -eu

out=GeneratedFirmwares.cpp
manifest=GeneratedFirmwaresMD5.txt
if [[ ${1:-} == clean ]]; then
    rm -f "$out" "$manifest"
    exit 0
fi

# Rename complete files on the same filesystem. A failed xxd must fail Prebuild.
stage=$(mktemp -d ./.firmware-build.XXXXXX)
trap 'rm -rf "$stage"' EXIT
firmwares=(./firmwares/*.zhx)
for firmware in "${firmwares[@]}"; do
    hash=$(md5 -q "$firmware")
    printf '%s %s\n' "${firmware##*/}" "$hash" >> "$stage/$manifest"
done
digest=$(md5 -q "$stage/$manifest")
header="// GeneratedFirmwares.cpp ($digest)"
if [[ -s "$out" && -f "$manifest" ]] && IFS= read -r previous < "$out" &&
    [[ "$previous" == "$header" ]] && cmp -s "$manifest" "$stage/$manifest"; then
    exit 0
fi

{
    echo "$header"
    for firmware in "${firmwares[@]}"; do
        name=${firmware##*/}
        echo "static const unsigned char ${name//./_}[] = {"
        xxd -i < "$firmware"
        echo "};"
    done
    echo "static const FirmwareEntry firmwares[] = {"
    for firmware in "${firmwares[@]}"; do
        name=${firmware##*/}
        symbol=${name//./_}
        printf '    { "%s", %s, sizeof(%s) },\n' "$name" "$symbol" "$symbol"
    done
    echo "    { NULL, NULL, 0 }"
    echo "};"
} > "$stage/$out"
mv -f "$stage/$out" "$out"
mv -f "$stage/$manifest" "$manifest"
