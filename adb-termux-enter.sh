# SPDX-License-Identifier: 0BSD
#
# This file is meant to be passed inline:
#   adb shell -t "$(cat adb-termux-enter.sh)"
#
# adb runs the payload as "sh -c <payload>". Capture that payload into a file so
# su can re-run the same script as root, then skip this bootstrap in stage two.
if [ "${ADB_TERMUX_ENTER_ROOT:-}" != 1 ]; then
    awk 'BEGIN { RS = "\0" } NR > 2 { print; }' /proc/$$/cmdline > /data/local/tmp/adb-termux-enter.sh
    exec su 0 env ADB_TERMUX_ENTER_ROOT=1 sh /data/local/tmp/adb-termux-enter.sh
fi

set -eu

PKG="${PKG:-com.termux}"
USER_ID="${USER_ID:-0}"

LEGACY_DATA="/data/data/$PKG"
DATA_DIR="/data/user/$USER_ID/$PKG"
FILES_DIR="$LEGACY_DATA/files"
ROOTFS="$FILES_DIR"
HOME_DIR="$FILES_DIR/home"
PREFIX_DIR="$FILES_DIR/usr"
TMPDIR_DIR="$PREFIX_DIR/tmp"

OUT="/data/local/tmp/termux-env.generated.sh"

q() {
  # shell single-quote escape
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

emit() {
  [ -n "${2-}" ] || return 0
  printf 'export %s=%s\n' "$1" "$(q "$2")" >> "$OUT"
}

pm_dump="$(dumpsys package "$PKG" 2>/dev/null || true)"

apk_path="$(pm path "$PKG" 2>/dev/null | sed -n 's/^package://p' | head -n1)"
version_name="$(printf '%s\n' "$pm_dump" | sed -n 's/^[[:space:]]*versionName=//p' | head -n1)"
version_code="$(printf '%s\n' "$pm_dump" | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' | head -n1)"
target_sdk="$(printf '%s\n' "$pm_dump" | sed -n 's/.*targetSdk=\([0-9][0-9]*\).*/\1/p' | head -n1)"
seinfo="$(printf '%s\n' "$pm_dump" | sed -n 's/.*se[Ii]nfo=\([^ ]*\).*/\1/p' | head -n1)"
debuggable="$(printf '%s\n' "$pm_dump" | grep -q 'DEBUGGABLE' && echo true || echo false)"

# Prefer actual package dataDir if dumpsys exposes it.
dump_data_dir="$(printf '%s\n' "$pm_dump" | sed -n 's/^[[:space:]]*dataDir=//p' | head -n1)"
[ -n "$dump_data_dir" ] && DATA_DIR="$dump_data_dir"

uid="$(stat -c '%u' "$LEGACY_DATA" 2>/dev/null || true)"
[ -n "$uid" ] || uid="$(cmd package list packages -U "$PKG" 2>/dev/null | sed -n 's/.*uid:\([0-9][0-9]*\).*/\1/p' | head -n1)"

# File context gives us the app MCS level, e.g. s0:c216,c256,c512,c768.
file_ctx="$(ls -Zd "$FILES_DIR" 2>/dev/null | awk '{print $1}' || true)"
[ -n "$file_ctx" ] || file_ctx="$(ls -Zd "$LEGACY_DATA" 2>/dev/null | awk '{print $1}' || true)"
mcs_level="$(printf '%s\n' "$file_ctx" | cut -d: -f4-)"

# Derive the GUI-style app process domain from target SDK.
# For your Termux targetSdk 28 case, this is the important fix.
case "${target_sdk:-28}" in
  ''|*[!0-9]*) app_domain="untrusted_app_27" ;;
  *)
    if [ "$target_sdk" -le 25 ]; then
      app_domain="untrusted_app_25"
    elif [ "$target_sdk" -le 28 ]; then
      app_domain="untrusted_app_27"
    else
      app_domain="untrusted_app"
    fi
    ;;
esac

[ -n "$mcs_level" ] || mcs_level="s0"
process_ctx="u:r:${app_domain}:${mcs_level}"

# If dumpsys did not expose seinfo, synthesize the common form.
[ -n "$target_sdk" ] || target_sdk="$(getprop ro.build.version.sdk)"
[ -n "$seinfo" ] || seinfo="default:targetSdkVersion=${target_sdk}:complete"

# libtermux-exec name differs by Termux generation/fork.
ld_preload=""
if [ -f "$PREFIX_DIR/lib/libtermux-exec-ld-preload.so" ]; then
  ld_preload="$PREFIX_DIR/lib/libtermux-exec-ld-preload.so"
elif [ -f "$PREFIX_DIR/lib/libtermux-exec.so" ]; then
  ld_preload="$PREFIX_DIR/lib/libtermux-exec.so"
fi

: > "$OUT"

# Android-ish base env.
emit ANDROID_ART_ROOT "${ANDROID_ART_ROOT:-/apex/com.android.art}"
emit ANDROID_ASSETS "${ANDROID_ASSETS:-/system/app}"
emit ANDROID_DATA "${ANDROID_DATA:-/data}"
emit ANDROID_I18N_ROOT "${ANDROID_I18N_ROOT:-/apex/com.android.i18n}"
emit ANDROID_ROOT "${ANDROID_ROOT:-/system}"
emit ANDROID_STORAGE "${ANDROID_STORAGE:-/storage}"
emit ANDROID_TZDATA_ROOT "${ANDROID_TZDATA_ROOT:-/apex/com.android.tzdata}"
emit ANDROID__BUILD_VERSION_SDK "$(getprop ro.build.version.sdk)"
emit ASEC_MOUNTPOINT "${ASEC_MOUNTPOINT:-/mnt/asec}"
emit EXTERNAL_STORAGE "${EXTERNAL_STORAGE:-/sdcard}"

emit BOOTCLASSPATH "${BOOTCLASSPATH:-}"
emit DEX2OATBOOTCLASSPATH "${DEX2OATBOOTCLASSPATH:-}"
emit SYSTEMSERVERCLASSPATH "${SYSTEMSERVERCLASSPATH:-}"

# Generic terminal env.
emit COLORTERM "truecolor"
emit LANG "en_US.UTF-8"
emit TERM "xterm-256color"

# Termux paths.
emit HOME "$HOME_DIR"
emit PREFIX "$PREFIX_DIR"
emit TMPDIR "$TMPDIR_DIR"
emit TMP "$TMPDIR_DIR"
emit PATH "$PREFIX_DIR/bin"
emit SHELL "$PREFIX_DIR/bin/bash"

# Current/new Termux rootfs metadata. Emit several aliases because versions differ.
emit TERMUX__PREFIX "$PREFIX_DIR"
emit TERMUX__HOME "$HOME_DIR"
emit TERMUX__ROOTFS "$ROOTFS"
emit TERMUX__ROOTFS_DIR "$ROOTFS"
emit TERMUX__APPS_DIR "$DATA_DIR/termux/apps"
emit TERMUX__UID "$uid"
emit TERMUX__USER_ID "$USER_ID"
emit TERMUX__SE_PROCESS_CONTEXT "$process_ctx"

# App metadata. Emit aliases for old/new naming differences.
emit TERMUX_VERSION "$version_name"
emit TERMUX_APP__PACKAGE_NAME "$PKG"
emit TERMUX_APP__DATA_DIR "$DATA_DIR"
emit TERMUX_APP__LEGACY_DATA_DIR "$LEGACY_DATA"
emit TERMUX_APP__FILES_DIR "$FILES_DIR"

emit TERMUX_APP__VERSION_NAME "$version_name"
emit TERMUX_APP__APP_VERSION_NAME "$version_name"
emit TERMUX_APP__VERSION_CODE "$version_code"
emit TERMUX_APP__APP_VERSION_CODE "$version_code"

emit TERMUX_APP__APK_PATH "$apk_path"
emit TERMUX_APP__APK_FILE "$apk_path"

emit TERMUX_APP__UID "$uid"
emit TERMUX_APP__TARGET_SDK "$target_sdk"
emit TERMUX_APP__IS_DEBUGGABLE_BUILD "$debuggable"
emit TERMUX_APP__IS_INSTALLED_ON_EXTERNAL_STORAGE "false"

emit TERMUX_APP__SE_PROCESS_CONTEXT "$process_ctx"
emit TERMUX_APP__SE_FILE_CONTEXT "$file_ctx"
emit TERMUX_APP__SE_INFO "$seinfo"
emit TERMUX_APP__USER_ID "$USER_ID"

# Release is signature-derived in Termux app code. This rough heuristic is enough for env emulation.
if [ "$debuggable" = true ]; then
  emit TERMUX_APP__APK_RELEASE "GITHUB"
else
  emit TERMUX_APP__APK_RELEASE "UNKNOWN"
fi

# Session metadata: not essential; GUI Termux increments counters from prefs.
emit SHELL_CMD__PACKAGE_NAME "$PKG"
emit SHELL_CMD__RUNNER_NAME "terminal-session"
emit SHELL_CMD__SHELL_ID "0"
emit SHELL_CMD__APP_TERMINAL_SESSION_NUMBER_SINCE_APP_START "0"
emit SHELL_CMD__APP_TERMINAL_SESSION_NUMBER_SINCE_BOOT "0"

# login normally adds this, but add it now if you bypass login.
emit LD_PRELOAD "$ld_preload"

chmod 644 "$OUT"

echo "Wrote $OUT"
# cat "$OUT"

echo "$process_ctx"

exec su "$uid" /system/bin/sh -c '
  . "$1"
  cd "$HOME"
  exec "$PREFIX/bin/login"
' sh "$OUT"
# runcon "$process_ctx" /system/bin/sh -c '
#   exec "$PREFIX/bin/login"
# ' sh "$OUT"
