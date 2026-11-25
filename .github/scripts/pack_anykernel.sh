#!/usr/bin/env bash
# Build SELinux-labeled EROFS vendor_dlkm.img and package unified 3-in-1 AnyKernel3 zip
# (vendor_dlkm + boot Image + vendor_boot ramdisk modules; init_boot is never touched).
set -euo pipefail

: "${WS:?WS is required}"
: "${KERNEL_OUT:?KERNEL_OUT is required}"
: "${PARTITIONS_DIR:?PARTITIONS_DIR is required}"
: "${DIST_DIR:?DIST_DIR is required}"
: "${DEVICE:?DEVICE is required}"
: "${KERNEL_VER:?KERNEL_VER is required}"
: "${ANYKERNEL_REPO:?ANYKERNEL_REPO is required}"
: "${LTO_MODE:?LTO_MODE is required}"
: "${KSU_VARIANT:?KSU_VARIANT is required}"
: "${BUILD_DATE:?BUILD_DATE is required}"
: "${SHORT_SHA:?SHORT_SHA is required}"
: "${EXPORT_RAW:=false}"

STAGE_IMG_DIR="${WS}/out/stage_images"
EROFS_SRC="${WS}/out/erofs_vendor_dlkm"
FC_FILE="${WS}/out/vendor_dlkm_file_contexts"
AK3_DIR="${WS}/AnyKernel3"

mkdir -p "${STAGE_IMG_DIR}" "${DIST_DIR}"
cp "${KERNEL_OUT}/arch/arm64/boot/Image" "${STAGE_IMG_DIR}/Image"
cp "${KERNEL_OUT}/.config" "${DIST_DIR}/defconfig_used"

# 1. Build SELinux-labeled EROFS vendor_dlkm.img (LZ4HC compressed)
rm -rf "${EROFS_SRC}"
mkdir -p "${EROFS_SRC}/lib/modules" "${EROFS_SRC}/etc"
cp -a "${PARTITIONS_DIR}/vendor_dlkm/lib/modules/${KERNEL_VER}/." "${EROFS_SRC}/lib/modules/"
chmod 755 "${EROFS_SRC}" "${EROFS_SRC}/lib" "${EROFS_SRC}/lib/modules" "${EROFS_SRC}/etc"
chmod 644 "${EROFS_SRC}/lib/modules/"*

case "${DEVICE}" in
  socrates) BRAND="Redmi";  MODEL="22127RK46C" ;;
  vermeer)  BRAND="Redmi";  MODEL="23113RKC6C" ;;
  fuxi)     BRAND="Xiaomi"; MODEL="2211133C" ;;
  nuwa)     BRAND="Xiaomi"; MODEL="2210132C" ;;
  *)        BRAND="Xiaomi"; MODEL="${DEVICE}" ;;
esac

cat <<EOF > "${EROFS_SRC}/etc/build.prop"
ro.product.vendor_dlkm.brand=${BRAND}
ro.product.vendor_dlkm.device=${DEVICE}
ro.product.vendor_dlkm.manufacturer=Xiaomi
ro.product.vendor_dlkm.model=${MODEL}
ro.product.vendor_dlkm.name=${DEVICE}
EOF
touch "${EROFS_SRC}/etc/fs_config_dirs" "${EROFS_SRC}/etc/fs_config_files"
chmod 644 "${EROFS_SRC}/etc/"*

cat <<'EOF' > "${FC_FILE}"
/vendor_dlkm(/.*)?                  u:object_r:vendor_file:s0
/vendor_dlkm/etc(/.*)?              u:object_r:vendor_configs_file:s0
EOF

mkfs.erofs -zlz4hc,9 \
  --mount-point=/vendor_dlkm \
  --file-contexts="${FC_FILE}" \
  --force-uid=0 --force-gid=0 \
  -T 1230768000 \
  "${STAGE_IMG_DIR}/vendor_dlkm.img" \
  "${EROFS_SRC}"

DLKM_SIZE=$(wc -c < "${STAGE_IMG_DIR}/vendor_dlkm.img")
echo "vendor_dlkm.img size: ${DLKM_SIZE} bytes" | tee -a "${DIST_DIR}/module_report.txt"

# 2. Pack unified 3-in-1 AnyKernel3 zip
rm -rf "${AK3_DIR}"
git clone --depth=1 "${ANYKERNEL_REPO}" "${AK3_DIR}"
cd "${AK3_DIR}"
rm -rf .git README.md
cp "${STAGE_IMG_DIR}/Image" ./Image
cp "${STAGE_IMG_DIR}/vendor_dlkm.img" ./vendor_dlkm.img
mkdir -p ./ak3_modules/first_stage ./ak3_modules/recovery
cp -a "${PARTITIONS_DIR}/first_stage/lib/modules/${KERNEL_VER}/." ./ak3_modules/first_stage/
cp -a "${PARTITIONS_DIR}/recovery/lib/modules/${KERNEL_VER}/." ./ak3_modules/recovery/

cat <<EOF > anykernel.sh
### AnyKernel3 Unified 3-in-1 Partition Flash Script
## Flashes: vendor_dlkm (EROFS image) + boot (Image) + vendor_boot (first_stage/recovery modules)

properties() { '
kernel.string=Xiaomi SM8550 Full Kernel (${DEVICE}) - ${KERNEL_VER}
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=${DEVICE}
device.name2=
device.name3=
device.name4=
device.name5=
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties
EOF

cat <<'EOF' >> anykernel.sh

### Initialize AnyKernel3 (device check & slot detection)
BLOCK=boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

. tools/ak3-core.sh;

### Phase 1/3: Flash vendor_dlkm.img FIRST (so any failure aborts before touching boot/vendor_boot)
ui_print " " "Phase 1/3: Flashing vendor_dlkm.img...";
DLKM_NAME="vendor_dlkm${SLOT}";

find_dlkm_block() {
  local p f;
  for p in /dev/block/mapper /dev/block/by-name /dev/block/bootdevice/by-name; do
    for f in "$DLKM_NAME" vendor_dlkm; do
      if [ -e "$p/$f" ]; then
        echo "$p/$f";
        return 0;
      fi;
    done;
  done;
  if [ -x /system/bin/dmctl ]; then
    p=$(/system/bin/dmctl getpath "$DLKM_NAME" 2>/dev/null || true);
    if [ -n "$p" ] && [ -e "$p" ]; then
      echo "$p";
      return 0;
    fi;
  fi;
  return 1;
}

DLKM_BLK=$(find_dlkm_block || true);
if [ -z "$DLKM_BLK" ] && [ -x "$BIN/lptools_static" ]; then
  "$BIN/lptools_static" map "$DLKM_NAME" 2>/dev/null || true;
  DLKM_BLK=$(find_dlkm_block || true);
fi;
if [ -z "$DLKM_BLK" ] && [ -x /system/bin/dmctl ]; then
  /system/bin/dmctl create-from-super /dev/block/by-name/super "$DLKM_NAME" 2>/dev/null || true;
  DLKM_BLK=$(find_dlkm_block || true);
fi;
if [ -z "$DLKM_BLK" ] && [ -x /system/bin/lpdump ] && [ -x /system/bin/dmctl ]; then
  parse_lpdump() {
    "$BIN/busybox" awk -v target="$DLKM_NAME" '
      $1 == "Name:" { in_part = ($2 == target) }
      in_part && $2 == ".." && $4 == "linear" && $5 == "super" {
        cnt = $3 - $1 + 1;
        printf "linear %s %s /dev/block/by-name/super %s ", $1, cnt, $6;
      }
    ';
  }
  DM_TARGETS=$(/system/bin/lpdump 2>/dev/null | parse_lpdump);
  if [ -z "$DM_TARGETS" ]; then
    case "$SLOT" in
      _a) DM_TARGETS=$(/system/bin/lpdump --slot=0 2>/dev/null | parse_lpdump) ;;
      _b) DM_TARGETS=$(/system/bin/lpdump --slot=1 2>/dev/null | parse_lpdump) ;;
    esac;
  fi;
  if [ -n "$DM_TARGETS" ]; then
    ui_print " " "  -> Mapping ${DLKM_NAME} directly from super metadata...";
    /system/bin/dmctl create "$DLKM_NAME" $DM_TARGETS 2>/dev/null || true;
    sleep 1;
    DLKM_BLK=$(find_dlkm_block || true);
  fi;
fi;

[ -n "$DLKM_BLK" ] || abort "vendor_dlkm partition (${DLKM_NAME}) could not be found or mapped! Boot/vendor_boot untouched. Aborting...";

IMG_SZ=$(wc -c < "$AKHOME/vendor_dlkm.img");
PART_SZ=$(blockdev --getsize64 "$DLKM_BLK" 2>/dev/null || /system/bin/blockdev --getsize64 "$DLKM_BLK" 2>/dev/null || wc -c < "$DLKM_BLK");
ui_print " " "  -> Target: ${DLKM_BLK} (img=${IMG_SZ}B, part=${PART_SZ}B)";

if [ "$IMG_SZ" -le "$PART_SZ" ]; then
  # Image fits inside existing partition: write in-place without risky lptools resize/unmap
  if [ -x "$BIN/httools_static" ] && [ ! "$NO_VBMETA_PARTITION_PATCH" ]; then
    AVB_PART=$("$BIN/httools_static" avb vendor_dlkm 2>/dev/null || true);
    AVB_FLAGS=$("$BIN/httools_static" disable-flags 2>/dev/null || true);
    if [ -n "$AVB_PART" ] && [ "$AVB_FLAGS" = "enabled" ]; then
      ui_print " " "  -> dm-verity detected! Patching ${AVB_PART}...";
      AVB_BLK="";
      for p in /dev/block/mapper /dev/block/by-name /dev/block/bootdevice/by-name; do
        for f in "${AVB_PART}${SLOT}" "$AVB_PART"; do
          if [ -e "$p/$f" ]; then AVB_BLK="$p/$f"; break 2; fi;
        done;
      done;
      [ -n "$AVB_BLK" ] || abort "Could not locate ${AVB_PART} for AVB patch! Aborting...";
      (cd "$BIN" && ./httools_static patch vendor_dlkm "$AKHOME/vendor_dlkm.img" "$AVB_BLK") || abort "Failed to patch AVB on ${AVB_PART}! Aborting...";
    fi;
  fi;

  if grep -q ' /vendor_dlkm ' /proc/mounts 2>/dev/null; then
    "$BIN/httools_static" umount vendor_dlkm 2>/dev/null || umount /vendor_dlkm 2>/dev/null || umount -l /vendor_dlkm 2>/dev/null || true;
  fi;

  IS_RO=$(blockdev --getro "$DLKM_BLK" 2>/dev/null || /system/bin/blockdev --getro "$DLKM_BLK" 2>/dev/null || echo 1);
  blockdev --setrw "$DLKM_BLK" 2>/dev/null || /system/bin/blockdev --setrw "$DLKM_BLK" 2>/dev/null || true;
  dd if="$AKHOME/vendor_dlkm.img" of="$DLKM_BLK" bs=4096 conv=fsync || abort "Flashing vendor_dlkm.img failed! Aborting...";

  SRC_SHA=$(sha256sum "$AKHOME/vendor_dlkm.img" 2>/dev/null | awk '{print $1}');
  DST_SHA=$(head -c "$IMG_SZ" "$DLKM_BLK" 2>/dev/null | sha256sum 2>/dev/null | awk '{print $1}');
  if [ -n "$SRC_SHA" ] && [ -n "$DST_SHA" ] && [ "$SRC_SHA" != "$DST_SHA" ]; then
    abort "vendor_dlkm SHA256 verification failed! Aborting...";
  fi;
  ui_print " " "  -> vendor_dlkm flashed & verified (SHA256 OK).";

  if [ "$IS_RO" != "0" ]; then
    blockdev --setro "$DLKM_BLK" 2>/dev/null || /system/bin/blockdev --setro "$DLKM_BLK" 2>/dev/null || true;
  fi;
  rm -f "$AKHOME/vendor_dlkm.img";
  touch "$AKHOME/vendor_dlkm_flashed";
else
  # Only fall back to ak3 dynamic resize if image actually exceeds current partition size
  flash_generic vendor_dlkm;
fi;

### Phase 2/3: Flash GKI boot partition (Image only; ramdisk lives in init_boot)
ui_print " " "Phase 2/3: Flashing boot Image...";
split_boot;
flash_boot;

# Remove Image before vendor_boot phase so flash_boot will not inject kernel into vendor_boot
rm -f "$AKHOME/Image";

### Phase 3/3: Update vendor_boot ramdisk modules & repack
BLOCK=vendor_boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

ui_print " " "Phase 3/3: Updating vendor_boot ramdisk modules...";
reset_ak;
dump_boot;

UPDATED_ANY=0;
for moddir in "$RAMDISK/lib/modules" "$VENDORRD"/*/lib/modules; do
  [ -d "$moddir" ] || continue;
  UPDATED_ANY=1;
  MOD_CNT=$(wc -l < "$moddir/modules.load" 2>/dev/null || echo 0);
  case "$moddir" in
    *recovery*) SRC_DIR="$AKHOME/ak3_modules/recovery" ;;
    *)
      if [ "$MOD_CNT" -gt 200 ] 2>/dev/null; then
        SRC_DIR="$AKHOME/ak3_modules/recovery";
      else
        SRC_DIR="$AKHOME/ak3_modules/first_stage";
      fi;
      ;;
  esac;
  ui_print " " "  -> Updating $(basename "$(dirname "$(dirname "$moddir")")")/lib/modules ($(basename "$SRC_DIR"))";
  rm -f "$moddir"/*.ko "$moddir"/modules.*;
  cp -f "$SRC_DIR"/* "$moddir"/;
  set_perm_recursive 0 0 755 644 "$moddir";
done;
[ "$UPDATED_ANY" = 1 ] || abort "No lib/modules found inside vendor_boot! Aborting...";

ui_print " " "  -> Repacking & flashing vendor_boot...";
write_boot;
EOF

sh -n anykernel.sh
chmod 644 anykernel.sh

KSU_TAG=""
if [ "${KSU_VARIANT}" != "none" ]; then
  KSU_TAG="-${KSU_VARIANT}"
fi
BASE_TAG="sm8550-${DEVICE}-${LTO_MODE}${KSU_TAG}-${BUILD_DATE}-${SHORT_SHA}"
AK3_ZIP="AnyKernel3-full-${BASE_TAG}.zip"
zip -r9 "${DIST_DIR}/${AK3_ZIP}" ./* -x "*.zip"

if [ "${EXPORT_RAW}" = "true" ]; then
  cp "${STAGE_IMG_DIR}/Image" "${DIST_DIR}/Image"
  cp "${STAGE_IMG_DIR}/vendor_dlkm.img" "${DIST_DIR}/vendor_dlkm.img"
fi

(cd "${DIST_DIR}" && sha256sum ./* > /tmp/SHA256SUMS.txt)
sed -i 's| \./| |' /tmp/SHA256SUMS.txt
mv /tmp/SHA256SUMS.txt "${DIST_DIR}/SHA256SUMS.txt"
ls -lh "${DIST_DIR}"

AK3_SIZE_H=$(du -h "${DIST_DIR}/${AK3_ZIP}" | awk '{print $1}')
AK3_SHA256=$(awk -v f="${AK3_ZIP}" '$2 == f {print $1}' "${DIST_DIR}/SHA256SUMS.txt")

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  {
    echo "ak3_zip=${AK3_ZIP}"
    echo "ak3_size_h=${AK3_SIZE_H}"
    echo "ak3_sha256=${AK3_SHA256}"
  } >> "${GITHUB_OUTPUT}"
fi
