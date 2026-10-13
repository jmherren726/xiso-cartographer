/*
 * FATX probe: asks the Xbox kernel's FATX driver which names and paths it accepts.
 *
 * No authoritative list of FATX naming rules exists, and game files can't be renamed
 * (a game opens them by name), so the rules core/fatxname enforces (M2) come from the
 * kernel itself. nxdk's CreateFileA passes paths to the kernel unchanged, and xemu runs
 * the real kernel from your BIOS dump, so this measures the console's actual behaviour.
 *
 * Works under E:\xcat-fatx-probe (deleted before and after), writes the full report to
 * E:\xcat-fatx-probe.txt and prints a summary. See docs/testing.md for how to run it.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define ROOT "E:\\xcat-fatx-probe"
#define REPORT "E:\\xcat-fatx-probe.txt"
#define PATH_MAX_BUF 600
#define MAX_ENTRIES_TRIED 5000

static HANDLE report = INVALID_HANDLE_VALUE;

/* Appends a line to the report file. */
static void rep(const char *fmt, ...)
{
    char line[PATH_MAX_BUF + 128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0 && report != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(report, line, (DWORD)strlen(line), &written, NULL);
    }
}

/* Creates an empty file. Returns 0 on success, otherwise the Win32 error. */
static DWORD try_file(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e != 0 ? e : 1;
    }
    CloseHandle(h);
    return 0;
}

static DWORD try_dir(const char *path)
{
    if (CreateDirectoryA(path, NULL)) {
        return 0;
    }
    DWORD e = GetLastError();
    return e != 0 ? e : 1;
}

static void delete_tree(const char *dir)
{
    char pattern[PATH_MAX_BUF];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
                continue;
            }
            char child[PATH_MAX_BUF];
            snprintf(child, sizeof child, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(child);
            } else {
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* 1. Name length: which of 40..43 characters work for files and directories. */
static void probe_lengths(void)
{
    debugPrint("Name lengths...\n");
    rep("\n[lengths]\n");
    for (int len = 40; len <= 43; len++) {
        char name[64];
        memset(name, 'a', (size_t)len);
        name[len] = '\0';
        name[0] = 'f';
        char path[PATH_MAX_BUF];
        snprintf(path, sizeof path, ROOT "\\%s", name);
        DWORD ef = try_file(path);
        name[0] = 'd';
        snprintf(path, sizeof path, ROOT "\\%s", name);
        DWORD ed = try_dir(path);
        rep("len %d: file %s (%lu), dir %s (%lu)\n", len, ef ? "FAIL" : "ok", (unsigned long)ef,
            ed ? "FAIL" : "ok", (unsigned long)ed);
        debugPrint("  %d chars: file %s, dir %s\n", len, ef ? "rejected" : "ok",
                   ed ? "rejected" : "ok");
    }
}

/* 2. Every byte 0x01..0xFF (except the separator '\') in the middle of a name. */
static void probe_chars(void)
{
    debugPrint("Characters...\n");
    rep("\n[characters] byte: result (error)\n");
    char rejected[1024] = "";
    size_t rlen = 0;
    int ok_count = 0;
    for (int c = 1; c <= 0xFF; c++) {
        if (c == '\\') {
            rep("0x5C: skipped (path separator)\n");
            continue;
        }
        char path[PATH_MAX_BUF];
        snprintf(path, sizeof path, ROOT "\\c%02X%cz", c, (char)c);
        DWORD e = try_file(path);
        rep("0x%02X%s%c%s: %s (%lu)\n", c, (c >= 0x20 && c < 0x7F) ? " '" : "",
            (c >= 0x20 && c < 0x7F) ? (char)c : ' ', (c >= 0x20 && c < 0x7F) ? "'" : "",
            e ? "FAIL" : "ok", (unsigned long)e);
        if (e) {
            if (rlen + 4 < sizeof rejected) {
                rlen += (size_t)snprintf(rejected + rlen, sizeof rejected - rlen, "%02X ", c);
            }
        } else {
            ok_count++;
        }
    }
    rep("summary: %d accepted, rejected bytes: %s\n", ok_count, rejected);
    debugPrint("  %d of 254 bytes accepted\n", ok_count);
    debugPrint("  rejected (hex): %.90s\n", rejected);
}

/* 3. Names that are special on other filesystems. */
static void probe_edge_names(void)
{
    debugPrint("Edge-case names...\n");
    rep("\n[edge names]\n");
    static const char *const names[] = {"trailingdot.",
                                        "trailingspace ",
                                        " leadingspace",
                                        "a..b",
                                        "...",
                                        "noext",
                                        "CON",
                                        "nul.txt",
                                        "x"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[PATH_MAX_BUF];
        snprintf(path, sizeof path, ROOT "\\%s", names[i]);
        DWORD e = try_file(path);
        rep("'%s': %s (%lu)\n", names[i], e ? "FAIL" : "ok", (unsigned long)e);
    }
}

/* 4. Case sensitivity of lookups and of creation. */
static void probe_case(void)
{
    debugPrint("Case sensitivity...\n");
    rep("\n[case]\n");
    DWORD e = try_file(ROOT "\\CaseProbe");
    HANDLE h = CreateFileA(ROOT "\\caseprobe", GENERIC_READ, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL found = h != INVALID_HANDLE_VALUE;
    if (found) {
        CloseHandle(h);
    }
    DWORD dup = try_file(ROOT "\\CASEPROBE");
    rep("create CaseProbe: %lu; open caseprobe: %s; create CASEPROBE: %s (%lu)\n", (unsigned long)e,
        found ? "found" : "not found", dup ? "FAIL" : "ok", (unsigned long)dup);
    debugPrint("  lookup %s, duplicate in other case %s\n",
               found ? "case-insensitive" : "case-sensitive", dup ? "rejected" : "allowed");
}

/* 5. Longest path (excluding "E:") for directories and files. */
static void probe_path_length(void)
{
    debugPrint("Path length...\n");
    rep("\n[path length] lengths exclude the drive (\"E:\")\n");
    char cur[PATH_MAX_BUF];
    snprintf(cur, sizeof cur, ROOT "\\p");
    if (try_dir(cur) != 0) {
        rep("could not create %s\n", cur);
        return;
    }
    /* Grow with 40-character directories, then refine with shorter names. */
    for (int k = 40; k >= 1;) {
        char next[PATH_MAX_BUF];
        int n = snprintf(next, sizeof next, "%s\\%.*s", cur, k,
                         "dddddddddddddddddddddddddddddddddddddddddd");
        if (n <= 0 || (size_t)n >= sizeof next) {
            break;
        }
        DWORD e = try_dir(next);
        if (e == 0) {
            memcpy(cur, next, (size_t)n + 1);
        } else {
            rep("dir path of %d chars: FAIL (%lu)\n", n - 2, (unsigned long)e);
            k--;
        }
    }
    int dir_len = (int)strlen(cur) - 2;
    int file_len = -1;
    for (int k = 42; k >= 1; k--) {
        char f[PATH_MAX_BUF];
        int n =
            snprintf(f, sizeof f, "%s\\%.*s", cur, k, "ffffffffffffffffffffffffffffffffffffffffff");
        if (n > 0 && (size_t)n < sizeof f && try_file(f) == 0) {
            file_len = n - 2;
            break;
        }
    }
    rep("longest directory path: %d; longest file path: %d\n", dir_len, file_len);
    debugPrint("  longest dir path %d, file path %d (excl. drive)\n", dir_len, file_len);
}

/* 6. How many entries one directory can hold. */
static void probe_entries(void)
{
    debugPrint("Entries per directory (up to %d)...\n", MAX_ENTRIES_TRIED);
    rep("\n[entries per directory]\n");
    if (try_dir(ROOT "\\many") != 0) {
        rep("could not create directory\n");
        return;
    }
    int count = 0;
    DWORD e = 0;
    for (; count < MAX_ENTRIES_TRIED; count++) {
        char path[PATH_MAX_BUF];
        snprintf(path, sizeof path, ROOT "\\many\\e%05d", count);
        e = try_file(path);
        if (e != 0) {
            break;
        }
        if (count % 500 == 499) {
            debugPrint("  %d...\n", count + 1);
        }
    }
    rep("created %d entries; %s (%lu)\n", count, e ? "then FAIL" : "stopped at the probe's limit",
        (unsigned long)e);
    debugPrint("  %d entries%s\n", count, e ? " (then rejected)" : " (no limit hit)");
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer FATX probe\n\n");
    if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
        debugPrint("Could not mount E:\n");
        for (;;) {
            Sleep(1000);
        }
    }
    delete_tree(ROOT);
    report =
        CreateFileA(REPORT, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (report == INVALID_HANDLE_VALUE || try_dir(ROOT) != 0) {
        debugPrint("Could not create the report or %s (error %lu)\n", ROOT,
                   (unsigned long)GetLastError());
        for (;;) {
            Sleep(1000);
        }
    }
    rep("xiso-cartographer FATX probe\nkernel %u.%u.%u.%u\n", XboxKrnlVersion.Major,
        XboxKrnlVersion.Minor, XboxKrnlVersion.Build, XboxKrnlVersion.Qfe);
    debugPrint("Kernel %u.%u.%u.%u\n", XboxKrnlVersion.Major, XboxKrnlVersion.Minor,
               XboxKrnlVersion.Build, XboxKrnlVersion.Qfe);

    DWORD start = GetTickCount();
    probe_lengths();
    probe_chars();
    probe_edge_names();
    probe_case();
    probe_path_length();
    probe_entries();

    debugPrint("Cleaning up...\n");
    delete_tree(ROOT);
    rep("\ndone in %lu ms\n", (unsigned long)(GetTickCount() - start));
    CloseHandle(report);
    debugPrint("\nDone. Full report: " REPORT "\n");
    for (;;) {
        Sleep(1000);
    }
}
