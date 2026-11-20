#!/bin/bash

if [ "${TARGET_BUILD_DIR}" = "" ]; then
  echo "This must not be run outside of Xcode"
  exit 1
fi

if [[ ${1:-} == clean && ! -d "$TARGET_BUILD_DIR" ]]; then
  exit 0
fi
cd "$TARGET_BUILD_DIR" || exit 1
archive="BrcmPatchRAM-${CURRENT_PROJECT_VERSION}-$(echo "$CONFIGURATION" | tr /a-z/ /A-Z/).zip"
if [[ ${1:-} == clean ]]; then
  rm -f "$archive"
  exit $?
fi
if [[ -n ${1:-} ]]; then
  exit 0
fi

for kext in BrcmBluetoothInjector.kext BrcmBluetoothInjectorLegacy.kext; do
  rm -rf "$kext"
  cp -a "$PROJECT_DIR/$kext" "$kext" || exit 1
done

dist=()
if [ -d "$DWARF_DSYM_FILE_NAME" ]; then dist+=("$DWARF_DSYM_FILE_NAME"); fi

for kext in *.kext; do
  dist+=("$kext")
done

zip -qry -FS "${archive}" "${dist[@]}"
