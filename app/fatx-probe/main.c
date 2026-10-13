/*
 * FATX probe: asks the Xbox kernel's FATX driver which names and paths it accepts.
 *
 * No authoritative list of FATX naming rules exists, and game files can't be renamed
 * (a game opens them by name), so the rules core/fatxname enforces (M2) come from the
 * kernel itself. xemu runs the real kernel from your BIOS dump, so runs there and on
 * hardware both measure the console's actual behaviour.
 *
 * Every file operation here calls the kernel (Nt*) directly. nxdk's CreateFileA and
 * FindFirstFileA assert that paths are shorter than MAX_PATH (260), which would stop
 * the path-length test before the kernel's own limit is reached (and did, in the first
 * xemu run). Results are reported as NTSTATUS codes.
 *
 * Works under E:\xcat-fatx-probe (deleted before and after), writes the full report to
 * E:\xcat-fatx-probe.txt and prints the results on screen. See docs/testing.md.
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
#define PATH_BUF 1024
#define MAX_ENTRIES_TRIED 5000
#define NXDK_WINAPI_PATH_LIMIT 259 /* nxdk asserts strlen < MAX_PATH (260) */

static HANDLE report = INVALID_HANDLE_VALUE;

static void rep(const char *fmt, ...)
{
    char line[PATH_BUF + 128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0 && report != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(report, line, (DWORD)strlen(line), &written, NULL);
    }
}

/* --- kernel file operations (no path-length asserts) -------------------------- */

static void object_attributes(OBJECT_ATTRIBUTES *attr, ANSI_STRING *name, const char *path)
{
    RtlInitAnsiString(name, path);
    InitializeObjectAttributes(attr, name, OBJ_CASE_INSENSITIVE, ObDosDevicesDirectory(), NULL);
}

/* Creates a new, empty file or directory. Returns the NTSTATUS. */
static NTSTATUS k_create(const char *path, BOOL directory)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    object_attributes(&attr, &name, path);
    NTSTATUS st = NtCreateFile(
        &h, (directory ? FILE_LIST_DIRECTORY : GENERIC_WRITE) | SYNCHRONIZE, &attr, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
        (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(st)) {
        NtClose(h);
    }
    return st;
}

/* Opens an existing file (case-insensitive lookup). Returns the NTSTATUS. */
static NTSTATUS k_open(const char *path)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    object_attributes(&attr, &name, path);
    NTSTATUS st = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &attr, &iosb, FILE_SHARE_READ,
                             FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(st)) {
        NtClose(h);
    }
    return st;
}

static void k_delete(const char *path, BOOL directory)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    object_attributes(&attr, &name, path);
    NTSTATUS st = NtOpenFile(&h, DELETE | SYNCHRONIZE, &attr, &iosb,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);
    if (!NT_SUCCESS(st)) {
        return;
    }
    FILE_DISPOSITION_INFORMATION info = {TRUE};
    NtSetInformationFile(h, &iosb, &info, sizeof info, FileDispositionInformation);
    NtClose(h);
}

/* Deletes a directory tree of any depth. Re-reads the directory after each deletion, so
   the enumeration never has to survive changes. */
static void k_delete_tree(const char *dir)
{
    for (;;) {
        ANSI_STRING name;
        OBJECT_ATTRIBUTES attr;
        IO_STATUS_BLOCK iosb;
        HANDLE h;
        object_attributes(&attr, &name, dir);
        if (!NT_SUCCESS(NtOpenFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attr, &iosb,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT))) {
            return; /* doesn't exist */
        }
        struct {
            FILE_DIRECTORY_INFORMATION info;
            char name[256];
        } entry;
        BOOL found = FALSE;
        BOOL restart = TRUE;
        char child[PATH_BUF];
        BOOL child_is_dir = FALSE;
        /* Like nxdk's FindFirstFileA: an empty mask on the first query lists everything. */
        ANSI_STRING all = {0, 0, (PCHAR) ""};
        while (!found) {
            memset(&entry, 0, sizeof entry);
            NTSTATUS st =
                NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, &entry, sizeof entry,
                                     FileDirectoryInformation, restart ? &all : NULL, restart);
            restart = FALSE;
            if (!NT_SUCCESS(st)) {
                break; /* no more entries */
            }
            ULONG n = entry.info.FileNameLength;
            if ((n == 1 && entry.info.FileName[0] == '.') ||
                (n == 2 && entry.info.FileName[0] == '.' && entry.info.FileName[1] == '.')) {
                continue;
            }
            snprintf(child, sizeof child, "%s\\%.*s", dir, (int)n, entry.info.FileName);
            child_is_dir = (entry.info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            found = TRUE;
        }
        NtClose(h);
        if (!found) {
            break;
        }
        if (child_is_dir) {
            k_delete_tree(child);
        } else {
            k_delete(child, FALSE);
        }
    }
    k_delete(dir, TRUE);
}

static const char *verdict(NTSTATUS st)
{
    return NT_SUCCESS(st) ? "ok" : "FAIL";
}

/* --- probes ------------------------------------------------------------------- */

/* 1. Name length: which of 40..43 characters work for files and directories. */
static void probe_lengths(void)
{
    debugPrint("Name lengths:");
    rep("\n[lengths]\n");
    for (int len = 40; len <= 43; len++) {
        char name[64];
        memset(name, 'a', (size_t)len);
        name[len] = '\0';
        char path[PATH_BUF];
        name[0] = 'f';
        snprintf(path, sizeof path, ROOT "\\%s", name);
        NTSTATUS sf = k_create(path, FALSE);
        name[0] = 'd';
        snprintf(path, sizeof path, ROOT "\\%s", name);
        NTSTATUS sd = k_create(path, TRUE);
        rep("len %d: file %s (0x%08lX), dir %s (0x%08lX)\n", len, verdict(sf), (unsigned long)sf,
            verdict(sd), (unsigned long)sd);
        debugPrint(" %d=%s/%s", len, NT_SUCCESS(sf) ? "ok" : "no", NT_SUCCESS(sd) ? "ok" : "no");
    }
    debugPrint("  (file/dir)\n");
}

/* 2. Every byte 0x01..0xFF (except the separator '\') in the middle of a name. */
static void probe_chars(void)
{
    rep("\n[characters] byte: result (NTSTATUS)\n");
    int ok_count = 0;
    int rejected[256];
    int nrej = 0;
    for (int c = 1; c <= 0xFF; c++) {
        if (c == '\\') {
            rep("0x5C: skipped (path separator)\n");
            continue;
        }
        char path[PATH_BUF];
        snprintf(path, sizeof path, ROOT "\\c%02X%cz", c, (char)c);
        NTSTATUS st = k_create(path, FALSE);
        BOOL printable = c >= 0x20 && c < 0x7F;
        rep("0x%02X%s%c%s: %s (0x%08lX)\n", c, printable ? " '" : "", printable ? (char)c : ' ',
            printable ? "'" : "", verdict(st), (unsigned long)st);
        if (NT_SUCCESS(st)) {
            ok_count++;
        } else {
            rejected[nrej++] = c;
        }
    }
    rep("summary: %d accepted, %d rejected\n", ok_count, nrej);
    debugPrint("Characters: %d of 254 accepted. Rejected (hex):\n", ok_count);
    for (int i = 0; i < nrej; i++) {
        debugPrint("%s%02X", (i % 20 == 0) ? (i ? "\n " : " ") : " ", rejected[i]);
    }
    debugPrint("\n");
}

/* 3. Names that are special on other filesystems. */
static void probe_edge_names(void)
{
    rep("\n[edge names]\n");
    debugPrint("Edge names:");
    static const char *const names[] = {
        "trailingdot.", "trailingspace ", " leadingspace", "a..b", "...", "CON", "nul.txt", "x"};
    static const char *const labels[] = {"dot.", "space_", "_space", "a..b",
                                         "...",  "CON",    "nul",    "x"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof path, ROOT "\\%s", names[i]);
        NTSTATUS st = k_create(path, FALSE);
        rep("'%s': %s (0x%08lX)\n", names[i], verdict(st), (unsigned long)st);
        debugPrint(" %s=%s", labels[i], NT_SUCCESS(st) ? "ok" : "no");
    }
    debugPrint("\n");
}

/* 4. Case sensitivity of lookups and of creation. */
static void probe_case(void)
{
    rep("\n[case]\n");
    NTSTATUS created = k_create(ROOT "\\CaseProbe", FALSE);
    NTSTATUS found = k_open(ROOT "\\caseprobe");
    NTSTATUS dup = k_create(ROOT "\\CASEPROBE", FALSE);
    rep("create CaseProbe: 0x%08lX; open caseprobe: %s (0x%08lX); create CASEPROBE: %s "
        "(0x%08lX)\n",
        (unsigned long)created, verdict(found), (unsigned long)found, verdict(dup),
        (unsigned long)dup);
    debugPrint("Case: lookup %s, other-case duplicate %s\n",
               NT_SUCCESS(found) ? "insensitive" : "SENSITIVE",
               NT_SUCCESS(dup) ? "ALLOWED" : "rejected");
}

/* 5. Longest path (excluding "E:") for directories and files, as the kernel sees it. */
static void probe_path_length(void)
{
    rep("\n[path length] lengths exclude the drive (\"E:\"); nxdk's CreateFileA/FindFirstFileA "
        "allow at most %d characters including it\n",
        NXDK_WINAPI_PATH_LIMIT);
    char cur[PATH_BUF];
    snprintf(cur, sizeof cur, ROOT "\\p");
    if (!NT_SUCCESS(k_create(cur, TRUE))) {
        rep("could not create %s\n", cur);
        return;
    }
    static const char d40[] = "dddddddddddddddddddddddddddddddddddddddd";
    static const char f42[] = "ffffffffffffffffffffffffffffffffffffffffff";
    for (int k = 40; k >= 1;) {
        char next[PATH_BUF];
        int n = snprintf(next, sizeof next, "%s\\%.*s", cur, k, d40);
        if (n <= 0 || (size_t)n >= sizeof next - 64) {
            rep("stopped: probe buffer limit\n");
            break;
        }
        NTSTATUS st = k_create(next, TRUE);
        if (NT_SUCCESS(st)) {
            memcpy(cur, next, (size_t)n + 1);
        } else {
            rep("dir path of %d chars: FAIL (0x%08lX)\n", n - 2, (unsigned long)st);
            k--;
        }
    }
    int dir_len = (int)strlen(cur) - 2;
    int file_len = -1;
    NTSTATUS last = 0;
    for (int k = 42; k >= 1; k--) {
        char f[PATH_BUF];
        int n = snprintf(f, sizeof f, "%s\\%.*s", cur, k, f42);
        last = k_create(f, FALSE);
        if (NT_SUCCESS(last)) {
            file_len = n - 2;
            break;
        }
    }
    rep("longest directory path: %d; longest file path: %d (last failure 0x%08lX)\n", dir_len,
        file_len, (unsigned long)last);
    debugPrint("Longest path (excl. E:): dir %d, file %d\n", dir_len, file_len);
}

/* 6. How many entries one directory can hold. */
static void probe_entries(void)
{
    debugPrint("Entries per directory (up to %d):", MAX_ENTRIES_TRIED);
    rep("\n[entries per directory]\n");
    if (!NT_SUCCESS(k_create(ROOT "\\many", TRUE))) {
        rep("could not create directory\n");
        return;
    }
    int count = 0;
    NTSTATUS st = 0;
    for (; count < MAX_ENTRIES_TRIED; count++) {
        char path[PATH_BUF];
        snprintf(path, sizeof path, ROOT "\\many\\e%05d", count);
        st = k_create(path, FALSE);
        if (!NT_SUCCESS(st)) {
            break;
        }
        if (count % 1000 == 999) {
            debugPrint(" %d", count + 1);
        }
    }
    rep("created %d entries; %s (0x%08lX)\n", count,
        NT_SUCCESS(st) ? "stopped at the probe's limit" : "then FAIL", (unsigned long)st);
    debugPrint("\n  -> %d entries%s\n", count,
               NT_SUCCESS(st) ? " (no limit hit)" : ", then rejected");
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("xiso-cartographer FATX probe (v2)\n");
    if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
        debugPrint("Could not mount E:\n");
        for (;;) {
            Sleep(1000);
        }
    }
    debugPrint("Removing old probe files...\n");
    k_delete_tree(ROOT); /* also clears deep leftovers from an earlier, interrupted run */
    report =
        CreateFileA(REPORT, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    NTSTATUS root = k_create(ROOT, TRUE);
    if (report == INVALID_HANDLE_VALUE || !NT_SUCCESS(root)) {
        debugPrint("Could not create the report or %s (0x%08lX)\n", ROOT, (unsigned long)root);
        for (;;) {
            Sleep(1000);
        }
    }
    rep("xiso-cartographer FATX probe v2\nkernel %u.%u.%u.%u\n", XboxKrnlVersion.Major,
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
    k_delete_tree(ROOT);
    rep("\ndone in %lu ms\n", (unsigned long)(GetTickCount() - start));
    CloseHandle(report);
    debugPrint("Done. Full report: " REPORT "\n");
    for (;;) {
        Sleep(1000);
    }
}
