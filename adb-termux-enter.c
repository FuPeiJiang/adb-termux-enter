// SPDX-License-Identifier: 0BSD

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define MAX_OUT 65536

static void die(const char *what) {
    fprintf(stderr, "%s failed: errno=%d %s\n", what, errno, strerror(errno));
    _exit(127);
}

static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static char *read_cmd(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return strdup("");

    char *buf = calloc(1, MAX_OUT);
    if (!buf) die("calloc");

    size_t used = 0;
    while (used + 1 < MAX_OUT) {
        size_t n = fread(buf + used, 1, MAX_OUT - used - 1, f);
        used += n;
        if (n == 0) break;
    }
    buf[used] = 0;
    pclose(f);
    return buf;
}

static char *first_line_cmd(const char *cmd) {
    char *s = read_cmd(cmd);
    char *nl = strchr(s, '\n');
    if (nl) *nl = 0;
    chomp(s);
    return s;
}

static char *find_line_value(const char *haystack, const char *prefix) {
    size_t plen = strlen(prefix);
    const char *p = haystack;

    while (*p) {
        while (*p == '\n' || *p == '\r') p++;

        const char *line = p;
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        while (len && (*line == ' ' || *line == '\t')) {
            line++;
            len--;
        }

        if (len >= plen && memcmp(line, prefix, plen) == 0) {
            char *out = strndup(line + plen, len - plen);
            if (!out) die("strndup");
            chomp(out);
            return out;
        }

        if (!end) break;
        p = end + 1;
    }

    return strdup("");
}

static char *find_after_token(const char *haystack, const char *token) {
    const char *p = strstr(haystack, token);
    if (!p) return strdup("");

    p += strlen(token);
    const char *end = p;
    while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t') {
        end++;
    }

    char *out = strndup(p, (size_t)(end - p));
    if (!out) die("strndup");
    chomp(out);
    return out;
}

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static char *get_xattr_context_or_empty(const char *path) {
    ssize_t n = getxattr(path, "security.selinux", NULL, 0);
    if (n <= 0) return strdup("");

    char *buf = calloc(1, (size_t)n + 1);
    if (!buf) die("calloc");

    ssize_t r = getxattr(path, "security.selinux", buf, (size_t)n);
    if (r <= 0) {
        free(buf);
        return strdup("");
    }

    buf[r] = 0;
    chomp(buf);
    return buf;
}

static char *fallback_ls_context(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -Zd '%s' 2>/dev/null | awk '{print $1}'", path);
    return first_line_cmd(cmd);
}

static char *file_context(const char *path) {
    char *ctx = get_xattr_context_or_empty(path);
    if (ctx[0]) return ctx;
    free(ctx);
    return fallback_ls_context(path);
}

static const char *mcs_from_context(const char *ctx) {
    // u:object_r:app_data_file:s0:c216,c256,c512,c768
    int colons = 0;
    for (const char *p = ctx; *p; p++) {
        if (*p == ':') {
            colons++;
            if (colons == 3) return p + 1;
        }
    }
    return "s0";
}

static void setcon_raw(const char *ctx) {
    int fd = open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
    if (fd < 0) die("open /proc/self/attr/current");

    ssize_t n = write(fd, ctx, strlen(ctx));
    if (n < 0 || (size_t)n != strlen(ctx)) die("write /proc/self/attr/current");

    if (close(fd) != 0) die("close /proc/self/attr/current");
}

static void emit_env(const char *k, const char *v) {
    if (!v || !*v) return;
    if (setenv(k, v, 1) != 0) die("setenv");
}

static char *path_join3(const char *a, const char *b, const char *c) {
    size_t n = strlen(a) + strlen(b) + strlen(c) + 3;
    char *out = malloc(n);
    if (!out) die("malloc");
    snprintf(out, n, "%s/%s/%s", a, b, c);
    return out;
}

int main(int argc, char **argv) {
    const char *pkg = getenv("PKG");
    if (!pkg || !*pkg) pkg = "com.termux";

    const char *user_id = getenv("USER_ID");
    if (!user_id || !*user_id) user_id = "0";

    char legacy_data[256];
    char data_dir[256];

    snprintf(legacy_data, sizeof(legacy_data), "/data/data/%s", pkg);
    snprintf(data_dir, sizeof(data_dir), "/data/user/%s/%s", user_id, pkg);

    char *files_dir = path_join3(legacy_data, "files", "");
    files_dir[strlen(files_dir) - 1] = 0;

    char *home_dir = path_join3(files_dir, "home", "");
    home_dir[strlen(home_dir) - 1] = 0;

    char *prefix_dir = path_join3(files_dir, "usr", "");
    prefix_dir[strlen(prefix_dir) - 1] = 0;

    char *tmpdir_dir = path_join3(prefix_dir, "tmp", "");
    tmpdir_dir[strlen(tmpdir_dir) - 1] = 0;

    char *login_path = path_join3(prefix_dir, "bin", "login");
    char *bash_path = path_join3(prefix_dir, "bin", "bash");
    char *nu_path = path_join3(prefix_dir, "bin", "nu");

    char dumpsys_cmd[256];
    snprintf(dumpsys_cmd, sizeof(dumpsys_cmd), "dumpsys package %s 2>/dev/null", pkg);
    char *pm_dump = read_cmd(dumpsys_cmd);

    char pm_path_cmd[256];
    snprintf(pm_path_cmd, sizeof(pm_path_cmd), "pm path %s 2>/dev/null | sed -n 's/^package://p' | head -n1", pkg);
    char *apk_path = first_line_cmd(pm_path_cmd);

    char *version_name = find_line_value(pm_dump, "versionName=");
    char *version_code = find_after_token(pm_dump, "versionCode=");
    char *target_sdk = find_after_token(pm_dump, "targetSdk=");
    char *seinfo = find_after_token(pm_dump, "seinfo=");
    if (!seinfo[0]) {
        free(seinfo);
        seinfo = find_after_token(pm_dump, "seInfo=");
    }

    char *dump_data_dir = find_line_value(pm_dump, "dataDir=");
    if (dump_data_dir[0]) {
        snprintf(data_dir, sizeof(data_dir), "%s", dump_data_dir);
    }

    int debuggable = contains(pm_dump, "DEBUGGABLE");

    struct stat st;
    if (stat(legacy_data, &st) != 0) die("stat /data/data/com.termux");
    uid_t uid = st.st_uid;

    char uid_str[32];
    snprintf(uid_str, sizeof(uid_str), "%ld", (long)uid);

    char *file_ctx = file_context(files_dir);
    if (!file_ctx[0]) {
        free(file_ctx);
        file_ctx = file_context(legacy_data);
    }

    const char *mcs = mcs_from_context(file_ctx);

    if (!target_sdk[0]) {
        free(target_sdk);
        target_sdk = first_line_cmd("getprop ro.build.version.sdk");
    }

    int sdk = target_sdk[0] ? atoi(target_sdk) : 28;
    const char *app_domain =
        sdk <= 25 ? "untrusted_app_25" :
        sdk <= 28 ? "untrusted_app_27" :
                    "untrusted_app";

    char process_ctx[256];
    snprintf(process_ctx, sizeof(process_ctx), "u:r:%s:%s", app_domain, mcs);

    if (!seinfo[0]) {
        free(seinfo);
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "default:targetSdkVersion=%s:complete", target_sdk);
        seinfo = strdup(tmp);
    }

    char *android_sdk = first_line_cmd("getprop ro.build.version.sdk");

    char *ld_preload = NULL;
    char *ld1 = path_join3(prefix_dir, "lib", "libtermux-exec-ld-preload.so");
    char *ld2 = path_join3(prefix_dir, "lib", "libtermux-exec.so");

    if (access(ld1, R_OK) == 0) {
        ld_preload = ld1;
    } else if (access(ld2, R_OK) == 0) {
        ld_preload = ld2;
    } else {
        ld_preload = "";
    }

    // Android-ish base env. Preserve inherited values when present.
    emit_env("ANDROID_ART_ROOT", getenv("ANDROID_ART_ROOT") ?: "/apex/com.android.art");
    emit_env("ANDROID_ASSETS", getenv("ANDROID_ASSETS") ?: "/system/app");
    emit_env("ANDROID_DATA", getenv("ANDROID_DATA") ?: "/data");
    emit_env("ANDROID_I18N_ROOT", getenv("ANDROID_I18N_ROOT") ?: "/apex/com.android.i18n");
    emit_env("ANDROID_ROOT", getenv("ANDROID_ROOT") ?: "/system");
    emit_env("ANDROID_STORAGE", getenv("ANDROID_STORAGE") ?: "/storage");
    emit_env("ANDROID_TZDATA_ROOT", getenv("ANDROID_TZDATA_ROOT") ?: "/apex/com.android.tzdata");
    emit_env("ANDROID__BUILD_VERSION_SDK", android_sdk);
    emit_env("ASEC_MOUNTPOINT", getenv("ASEC_MOUNTPOINT") ?: "/mnt/asec");
    emit_env("EXTERNAL_STORAGE", getenv("EXTERNAL_STORAGE") ?: "/sdcard");

    emit_env("BOOTCLASSPATH", getenv("BOOTCLASSPATH"));
    emit_env("DEX2OATBOOTCLASSPATH", getenv("DEX2OATBOOTCLASSPATH"));
    emit_env("SYSTEMSERVERCLASSPATH", getenv("SYSTEMSERVERCLASSPATH"));

    emit_env("COLORTERM", "truecolor");
    emit_env("LANG", "en_US.UTF-8");
    emit_env("TERM", getenv("TERM") ?: "xterm-256color");

    // Termux paths.
    emit_env("HOME", home_dir);
    emit_env("PREFIX", prefix_dir);
    emit_env("TMPDIR", tmpdir_dir);
    emit_env("TMP", tmpdir_dir);
    emit_env("PATH", prefix_dir); // temporarily overwritten below

    char path_env[512];
    snprintf(path_env, sizeof(path_env), "%s/bin", prefix_dir);
    emit_env("PATH", path_env);
    emit_env("SHELL", bash_path);

    // New/current Termux metadata aliases.
    emit_env("TERMUX__PREFIX", prefix_dir);
    emit_env("TERMUX__HOME", home_dir);
    emit_env("TERMUX__ROOTFS", files_dir);
    emit_env("TERMUX__ROOTFS_DIR", files_dir);

    char apps_dir[512];
    snprintf(apps_dir, sizeof(apps_dir), "%s/termux/apps", data_dir);
    emit_env("TERMUX__APPS_DIR", apps_dir);

    emit_env("TERMUX__UID", uid_str);
    emit_env("TERMUX__USER_ID", user_id);
    emit_env("TERMUX__SE_PROCESS_CONTEXT", process_ctx);

    // App metadata aliases.
    emit_env("TERMUX_VERSION", version_name);
    emit_env("TERMUX_APP__PACKAGE_NAME", pkg);
    emit_env("TERMUX_APP__DATA_DIR", data_dir);
    emit_env("TERMUX_APP__LEGACY_DATA_DIR", legacy_data);
    emit_env("TERMUX_APP__FILES_DIR", files_dir);

    emit_env("TERMUX_APP__VERSION_NAME", version_name);
    emit_env("TERMUX_APP__APP_VERSION_NAME", version_name);
    emit_env("TERMUX_APP__VERSION_CODE", version_code);
    emit_env("TERMUX_APP__APP_VERSION_CODE", version_code);

    emit_env("TERMUX_APP__APK_PATH", apk_path);
    emit_env("TERMUX_APP__APK_FILE", apk_path);

    emit_env("TERMUX_APP__UID", uid_str);
    emit_env("TERMUX_APP__TARGET_SDK", target_sdk);
    emit_env("TERMUX_APP__IS_DEBUGGABLE_BUILD", debuggable ? "true" : "false");
    emit_env("TERMUX_APP__IS_INSTALLED_ON_EXTERNAL_STORAGE", "false");

    emit_env("TERMUX_APP__SE_PROCESS_CONTEXT", process_ctx);
    emit_env("TERMUX_APP__SE_FILE_CONTEXT", file_ctx);
    emit_env("TERMUX_APP__SE_INFO", seinfo);
    emit_env("TERMUX_APP__USER_ID", user_id);

    emit_env("TERMUX_APP__APK_RELEASE", debuggable ? "GITHUB" : "UNKNOWN");

    // Session-ish metadata.
    emit_env("SHELL_CMD__PACKAGE_NAME", pkg);
    emit_env("SHELL_CMD__RUNNER_NAME", "terminal-session");
    emit_env("SHELL_CMD__SHELL_ID", "0");
    emit_env("SHELL_CMD__APP_TERMINAL_SESSION_NUMBER_SINCE_APP_START", "0");
    emit_env("SHELL_CMD__APP_TERMINAL_SESSION_NUMBER_SINCE_BOOT", "0");

    if (ld_preload && ld_preload[0]) {
        emit_env("LD_PRELOAD", ld_preload);
    }

    // Correct order proved by your probe:
    // setgroups -> setgid -> setuid -> setcon -> exec.
    gid_t groups[] = {
        uid,
        3003,
        9997,
        uid + 10000,
        uid + 40000
    };

    if (setgroups(sizeof(groups) / sizeof(groups[0]), groups) != 0)
        die("setgroups");

    if (setresgid(uid, uid, uid) != 0)
        die("setresgid");

    if (setresuid(uid, uid, uid) != 0)
        die("setresuid");

    setcon_raw(process_ctx);

    if (chdir(home_dir) != 0)
        die("chdir home");

    const char *shell = login_path;

    if (argc >= 2 && strcmp(argv[1], "nu") == 0) {
        shell = nu_path;
    } else if (argc >= 2 && strcmp(argv[1], "bash") == 0) {
        shell = bash_path;
    } else if (argc >= 2 && argv[1][0] == '/') {
        shell = argv[1];
    }

    char *const login_argv[] = {
        (char *)shell,
        NULL
    };

    char *const shell_login_argv[] = {
        (char *)shell,
        "-l",
        NULL
    };

    if (strcmp(shell, login_path) == 0) {
        execv(shell, login_argv);
    } else {
        execv(shell, shell_login_argv);
    }

    die("execv");
}
