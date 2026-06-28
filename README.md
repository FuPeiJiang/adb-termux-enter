# adb-termux-enter

Enter an installed Termux app from `adb shell`.

The native helper switches to the app UID, app SELinux context, and a
Termux-like environment before executing Termux `login`, `bash`, or another
shell. The shell payload is a best-effort fallback: it can reconstruct the
environment and run as the app UID, but it cannot perform the same
setgroups/setuid/setcon sequence inside one process.

This is for rooted Android devices or emulators where `adb shell` can run
`su 0`. The native helper is useful when a shell-only approach is too sensitive
to wrapper/interpreter behavior.

## Files

- `adb-termux-enter.c`: native launcher. Defaults to package `com.termux` and user
  `0`, derives UID/MCS/package metadata, sets groups, uid/gid, SELinux context,
  environment, working directory, then execs Termux.
- `adb-termux-enter.sh`: inline `adb shell -t` payload that performs the same
  environment discovery and enters Termux through shell commands. It exports the
  derived target SELinux context in Termux env variables, but the real process
  context may remain the `su` domain.

## Minimal NDK Sysroot

This repo intentionally does not require the NDK compiler. The build below uses
Alpine's `clang` plus a small subset of the Android NDK r30 beta 1 sysroot and
Clang resource headers.

Extract the minimal files into an existing Android SDK. Set `API=24` for an
Android 24 build, or `API=36` for an Android 36 build. Only one API directory is
needed per target; run the command once for each API level you want to build.

```sh
API=${API:-36}
ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
cd "$ANDROID_SDK_ROOT"

pickarc cp \
  --include-glob 'android-ndk-r30-beta1/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/**' \
  --or-glob "android-ndk-r30-beta1/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/${API}/crtbegin_dynamic.o" \
  --or-glob "android-ndk-r30-beta1/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/${API}/crtend_android.o" \
  --or-glob "android-ndk-r30-beta1/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/${API}/libc.so" \
  --or-glob 'android-ndk-r30-beta1/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/*/include/**' \
  --strip-components 1 \
  'https://dl.google.com/android/repository/android-ndk-r30-beta1-linux.zip'
```

For this source, the link uses only:

- `sysroot/usr/lib/x86_64-linux-android/${API}/crtbegin_dynamic.o`
- `sysroot/usr/lib/x86_64-linux-android/${API}/crtend_android.o`
- `sysroot/usr/lib/x86_64-linux-android/${API}/libc.so`

## Build

Build an Android x86_64 binary with Alpine's `clang`:

```sh
cd /path/to/adb-termux-enter

API=${API:-36}
ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
NDK_LLVM="$ANDROID_SDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$NDK_LLVM/sysroot"
RES="$NDK_LLVM/lib/clang/21"

clang --no-default-config \
  --target=x86_64-linux-android${API} \
  --sysroot="$SYSROOT" \
  -resource-dir "$RES" \
  -nostdlib \
  -fuse-ld=lld \
  -Wl,-pie \
  -Wl,--dynamic-linker=/system/bin/linker64 \
  "$SYSROOT/usr/lib/x86_64-linux-android/${API}/crtbegin_dynamic.o" \
  adb-termux-enter.c \
  "$SYSROOT/usr/lib/x86_64-linux-android/${API}/libc.so" \
  "$SYSROOT/usr/lib/x86_64-linux-android/${API}/crtend_android.o" \
  -o "adb-termux-enter.x86_64.android${API}"
```

`-nostdlib` is deliberate. Without it, this host clang injects runtime link
inputs such as `-lssp_nonshared`, compiler-rt builtins, `libunwind`, and `libdl`.
Those are not needed for this program and are not present in the minimal sysroot.

Inspect the hidden clang link line with:

```sh
clang --target=x86_64-linux-android${API} --sysroot="$SYSROOT" adb-termux-enter.c -o /tmp/adb-termux-enter.probe -###
```

## GitHub Actions

The `build` workflow runs in `container: alpine:3.24`, installs `pickarc` with:

```sh
bun add -g github:FuPeiJiang/pickarc.js
```

It runs on tag pushes, pull requests, and manual dispatches. Pull requests and
manual runs upload temporary workflow artifacts for debugging. Tag pushes create
or update a GitHub release with these NDK r30 beta artifacts:

- `adb-termux-enter.x86_64.android24`
- `adb-termux-enter.x86_64.android36`
- `adb-termux-enter.aarch64.android24`
- `adb-termux-enter.aarch64.android36`
- `SHA256SUMS`

The release notes also include the SHA256 sums in a fenced text block.

Pick the artifact from the device ABI list, not only the primary ABI:

```sh
"$ADB" shell 'getprop ro.product.cpu.abilist'
```

An x86_64 emulator may also run aarch64 binaries if native bridge support is
enabled, for example when `ro.product.cpu.abilist` includes `arm64-v8a` and
`ro.dalvik.vm.native.bridge` is set to `libndk_translation.so`. If `arm64-v8a`
is not listed, use the x86_64 artifact on an x86_64 emulator.

## Install And Enter Termux

Set the adb path once:

```sh
ANDROID_SDK_ROOT=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
ADB=${ADB:-"$ANDROID_SDK_ROOT/platform-tools/adb"}
```

Push the helper:

```sh
API=${API:-36}
"$ADB" push "adb-termux-enter.x86_64.android${API}" /data/local/tmp/adb-termux-enter
"$ADB" shell 'chmod 755 /data/local/tmp/adb-termux-enter'
```

Enter Termux through its default `login`:

```sh
"$ADB" shell -t 'su 0 /data/local/tmp/adb-termux-enter'
```

Enter a login bash directly:

```sh
"$ADB" shell -t 'su 0 /data/local/tmp/adb-termux-enter bash'
```

Use a different package or Android user:

```sh
"$ADB" shell -t 'su 0 env PKG=com.termux USER_ID=0 /data/local/tmp/adb-termux-enter'
```

The helper currently recognizes these shell arguments:

- no argument: `PREFIX/bin/login`
- `bash`: `PREFIX/bin/bash -l`
- `nu`: `PREFIX/bin/nu -l`
- absolute path: that executable with `-l`

## Shell Payload

Run the shell fallback inline from a local terminal:

```sh
"$ADB" shell -t "$(cat adb-termux-enter.sh)"
```

In Nushell:

```nu
adb shell -t (cat adb-termux-enter.sh)
```

For non-interactive verification from a pipe, force a remote PTY with `-tt` if
needed:

```sh
printf '%s\n' \
  'printf "TERMUX__SE_PROCESS_CONTEXT=%s\n" "$TERMUX__SE_PROCESS_CONTEXT"' \
  'id' \
  'printf "Context:"; tr -d "\000" < /proc/self/attr/current; printf "\n"' \
  'perl -MConfig -e '\''print "\$^X=$^X\nreadlink=", readlink("/proc/self/exe"), "\nperlpath=$Config{perlpath}\n"'\''' \
  'exit' |
  "$ADB" shell -tt "$(cat adb-termux-enter.sh)"
```

For the shell payload, `Context:u:r:untrusted_app_27:...` is not required. The
important check for the Perl linker bug is that the environment contains:

```text
TERMUX__SE_PROCESS_CONTEXT=u:r:untrusted_app_27:s0:c216,c256,c512,c768
```

In Nushell syntax, that is:

```nu
$env.TERMUX__SE_PROCESS_CONTEXT = "u:r:untrusted_app_27:s0:c216,c256,c512,c768"
```

## Verification

From inside the entered Termux shell:

```sh
id
printf 'Context:'; tr -d '\000' < /proc/self/attr/current; printf '\n'
perl -MConfig -e 'print "\$^X=$^X\nreadlink=", readlink("/proc/self/exe"), "\nperlpath=$Config{perlpath}\n"'
```

Expected shape for the tested emulator:

```text
uid=10216(u0_a216) gid=10216(u0_a216) ... context=u:r:untrusted_app_27:s0:c216,c256,c512,c768
Context:u:r:untrusted_app_27:s0:c216,c256,c512,c768
$^X=/data/data/com.termux/files/usr/bin/perl
readlink=/data/data/com.termux/files/usr/bin/perl
perlpath=/data/data/com.termux/files/usr/bin/perl
```

The bug this avoids is Perl identifying the Android dynamic linker as its own
executable:

```text
$^X=/apex/com.android.runtime/bin/linker64
readlink=/apex/com.android.runtime/bin/linker64
```

`readlink("/proc/self/exe")` is the direct probe for the executable behind the
running Perl process. In the bad state, that symlink points at Android's runtime
linker and Perl reports `linker64` in `$^X`. Termux's exec/linker wrapper needs
`TERMUX__SE_PROCESS_CONTEXT` to recognize the process as a Termux app process;
without it, a linker-launched program can be reported as `linker64` instead of
the Termux executable.

## Notes

- The target Termux package must already be installed.
- The launcher should be invoked as root, normally with `su 0`.
- The SELinux context is derived from the app data directory MCS label and the
  package target SDK. Termux target SDK 28 maps to `untrusted_app_27`.
- For the Perl `linker64` bug specifically, the needed variable is
  `TERMUX__SE_PROCESS_CONTEXT`. Other Termux app metadata variables are emitted
  for compatibility with normal Termux sessions, including
  `TERMUX_APP__SE_FILE_CONTEXT`.
- This is not a general-purpose privilege tool. It is a small adb/root helper
  for entering an already-installed Termux app environment.

## License

0BSD. See `LICENSE`.
