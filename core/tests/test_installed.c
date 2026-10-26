/* Tests for what's already installed (xcat/installed.h, docs/formats/journal.md §7.1). */
#include "unity.h"
#include "xcat/installed.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static xc_err parse(const char *text, xc_marker *m)
{
    return xc_marker_parse(text, strlen(text), m);
}

/* --- §7.1: the marker ------------------------------------------------------------------ */

static void test_marker_as_written(void)
{
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_OK, parse("xiso-cartographer 0.8.0\n"
                                   "image=http://192.168.1.20:8080/Alpha%20Racer.xiso.iso\n"
                                   "files=12\n"
                                   "bytes=734003200\n"
                                   "verified=crc32,image-hash\n"
                                   "md5=0123456789abcdef0123456789abcdef\n"
                                   "sha1=0123456789abcdef0123456789abcdef01234567\n"
                                   "title_id=4D53000A\n"
                                   "title=Alpha Racer\n",
                                   &m));
    TEST_ASSERT_EQUAL_STRING("xiso-cartographer 0.8.0", m.program);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8080/Alpha%20Racer.xiso.iso", m.image);
    TEST_ASSERT_TRUE(m.has_files);
    TEST_ASSERT_EQUAL_UINT64(12, m.files);
    TEST_ASSERT_TRUE(m.has_bytes);
    TEST_ASSERT_EQUAL_UINT64(734003200ULL, m.bytes);
    TEST_ASSERT_EQUAL_STRING("crc32,image-hash", m.verified);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", m.md5);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef01234567", m.sha1);
    TEST_ASSERT_TRUE(m.has_title_id);
    TEST_ASSERT_EQUAL_HEX32(0x4D53000AU, m.title_id);
    TEST_ASSERT_EQUAL_STRING("Alpha Racer", m.title);
}

static void test_marker_without_optional_keys(void)
{
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_OK, parse("xiso-cartographer 0.5.0\nimage=E:\\xcat\\isos\\a.iso\n"
                                   "files=1\nbytes=0\nverified=crc32\n",
                                   &m));
    TEST_ASSERT_EQUAL_STRING("E:\\xcat\\isos\\a.iso", m.image);
    TEST_ASSERT_TRUE(m.has_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, m.bytes);
    TEST_ASSERT_EQUAL_STRING("", m.md5);
    TEST_ASSERT_FALSE(m.has_title_id);
    TEST_ASSERT_EQUAL_STRING("", m.title);
}

static void test_marker_crlf_unknown_keys_and_no_final_lf(void)
{
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_OK, parse("xiso-cartographer 9.0\r\nnew_key=whatever\r\nno equals "
                                   "sign\r\ntitle=Beta\r\nfiles=3",
                                   &m));
    TEST_ASSERT_EQUAL_STRING("xiso-cartographer 9.0", m.program);
    TEST_ASSERT_EQUAL_STRING("Beta", m.title);
    TEST_ASSERT_TRUE(m.has_files);
    TEST_ASSERT_EQUAL_UINT64(3, m.files);
}

static void test_marker_bad_values_are_absent(void)
{
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_OK, parse("xiso-cartographer 0.8.0\nfiles=-1\nbytes=99999999999999999999\n"
                                   "md5=xyz\nsha1=0123\ntitle_id=4D53\ntitle=Bad\x01Title\n"
                                   "image=\xC3\x28\n",
                                   &m));
    TEST_ASSERT_FALSE(m.has_files);
    TEST_ASSERT_FALSE(m.has_bytes);
    TEST_ASSERT_EQUAL_STRING("", m.md5);
    TEST_ASSERT_EQUAL_STRING("", m.sha1);
    TEST_ASSERT_FALSE(m.has_title_id);
    TEST_ASSERT_EQUAL_STRING("", m.title);
    TEST_ASSERT_EQUAL_STRING("", m.image);
}

static void test_marker_long_value_cut_at_utf8_boundary(void)
{
    char text[200] = "xiso-cartographer 1\ntitle=";
    size_t n = strlen(text);
    for (int i = 0; i < 60; i++) { /* "é" x 60: 120 bytes, more than title's 3 x 40 */
        text[n++] = '\xC3';
        text[n++] = '\xA9';
    }
    text[n++] = 'x';
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_OK, xc_marker_parse(text, n, &m));
    TEST_ASSERT_EQUAL(sizeof m.title - 1, strlen(m.title));
    TEST_ASSERT_EQUAL_UINT8(0xA9, (uint8_t)m.title[strlen(m.title) - 1]);
}

static void test_marker_rejected(void)
{
    xc_marker m;
    TEST_ASSERT_EQUAL(XC_ERR_CORRUPT, parse("", &m));
    TEST_ASSERT_EQUAL(XC_ERR_CORRUPT, parse("something else 1.0\nfiles=1\n", &m));
    TEST_ASSERT_EQUAL(XC_ERR_CORRUPT, parse("xiso-cartographerX\n", &m));
    TEST_ASSERT_EQUAL(XC_ERR_CORRUPT, xc_marker_parse("xiso-cartographer 1\0x", 21, &m));
    char *big = malloc(XC_MARKER_MAX_FILE + 1);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'a', XC_MARKER_MAX_FILE + 1);
    TEST_ASSERT_EQUAL(XC_ERR_LIMIT, xc_marker_parse(big, XC_MARKER_MAX_FILE + 1, &m));
    free(big);
    TEST_ASSERT_EQUAL(XC_OK, parse("xiso-cartographer", &m));
}

/* --- one folder ----------------------------------------------------------------------- */

typedef struct file {
    const char *path;
    const char *text; /* NULL: a directory, or a file that can't be read */
} file;

static const file *files;
static size_t file_count;

static const file *find(const char *path)
{
    for (size_t i = 0; i < file_count; i++) {
        if (strcmp(files[i].path, path) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

static bool fake_exists(void *ctx, const char *path)
{
    (void)ctx;
    return find(path) != NULL;
}

static xc_err fake_read_all(void *ctx, const char *path, size_t max_len, char **data, size_t *len)
{
    (void)ctx;
    const file *f = find(path);
    if (f == NULL || f->text == NULL) {
        return XC_ERR_IO;
    }
    *len = strlen(f->text);
    if (*len > max_len) {
        return XC_ERR_LIMIT;
    }
    *data = malloc(*len + 1);
    TEST_ASSERT_NOT_NULL(*data);
    memcpy(*data, f->text, *len + 1);
    return XC_OK;
}

static xc_install_io fake_io(void)
{
    xc_install_io io;
    memset(&io, 0, sizeof io);
    io.separator = '/';
    io.exists = fake_exists;
    io.read_all = fake_read_all;
    return io;
}

static const file disk[] = {
    {"E:/Games/Done", NULL},
    {"E:/Games/Done/.xcat-installed", "xiso-cartographer 0.8.0\nimage=http://h/Done.iso\n"},
    {"E:/Games/Half", NULL},
    {"E:/Games/Half/.xcat-journal", "# xcat-journal v1.4\n"},
    {"E:/Games/Odd", NULL},
    {"E:/Games/Odd/.xcat-installed", "not a marker\n"},
    {"E:/Games/Locked", NULL},
    {"E:/Games/Locked/.xcat-installed", NULL},
    {"E:/Games/Theirs", NULL},
    {"E:/Games/Theirs/default.xbe", "x"},
};

static void test_folder_states(void)
{
    files = disk;
    file_count = sizeof disk / sizeof disk[0];
    xc_install_io io = fake_io();
    xc_installed in;

    TEST_ASSERT_EQUAL(XC_OK, xc_installed_check(&io, "E:/Games", "Done", &in));
    TEST_ASSERT_EQUAL(XC_FOLDER_INSTALLED, in.state);
    TEST_ASSERT_TRUE(in.marker_ok);
    TEST_ASSERT_EQUAL_STRING("http://h/Done.iso", in.marker.image);

    TEST_ASSERT_EQUAL(XC_OK, xc_installed_check(&io, "E:/Games", "Half", &in));
    TEST_ASSERT_EQUAL(XC_FOLDER_UNFINISHED, in.state);

    TEST_ASSERT_EQUAL(XC_OK, xc_installed_check(&io, "E:/Games", "Odd", &in));
    TEST_ASSERT_EQUAL(XC_FOLDER_INSTALLED, in.state);
    TEST_ASSERT_FALSE(in.marker_ok);

    TEST_ASSERT_EQUAL(XC_OK, xc_installed_check(&io, "E:/Games", "Locked", &in));
    TEST_ASSERT_EQUAL(XC_FOLDER_INSTALLED, in.state);
    TEST_ASSERT_FALSE(in.marker_ok);

    TEST_ASSERT_EQUAL(XC_OK, xc_installed_check(&io, "E:/Games", "Theirs", &in));
    TEST_ASSERT_EQUAL(XC_FOLDER_OTHER, in.state);

    char longname[XC_INSTALL_MAX_PATH + 1];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    TEST_ASSERT_EQUAL(XC_ERR_LIMIT, xc_installed_check(&io, "E:/Games", longname, &in));
}

/* --- ADR 0008: the folder an install of a title gets ------------------------------------ */

static void test_folder_for_title(void)
{
    TEST_ASSERT_TRUE(xc_installed_folder_for("Alpha Racer (USA)", "Alpha Racer (USA)"));
    TEST_ASSERT_TRUE(xc_installed_folder_for("alpha racer (usa)", "Alpha Racer (USA)"));
    TEST_ASSERT_TRUE(xc_installed_folder_for("Alpha Racer (USA) (2)", "Alpha Racer (USA)"));
    TEST_ASSERT_TRUE(xc_installed_folder_for("Alpha Racer (USA) (99)", "Alpha Racer (USA)"));
    TEST_ASSERT_FALSE(xc_installed_folder_for("Alpha Racer (USA) (100)", "Alpha Racer (USA)"));
    TEST_ASSERT_FALSE(xc_installed_folder_for("Alpha Racer (USA) (1)", "Alpha Racer (USA)"));
    TEST_ASSERT_FALSE(xc_installed_folder_for("Alpha Racer", "Alpha Racer (USA)"));
    /* The FATX name, not the title: ": " becomes " - ". */
    TEST_ASSERT_TRUE(xc_installed_folder_for("Beta - The Sequel", "Beta: The Sequel"));
    /* Shortened to 42 bytes, and shortened again to fit the number. */
    const char *title = "A Very Long Game Title That Goes On And On Forever";
    char base[XC_FATX_MAX_NAME + 1];
    char numbered[XC_FATX_MAX_NAME + 1];
    (void)xc_fatx_title_name(title, base, XC_FATX_MAX_NAME);
    (void)xc_fatx_numbered_name(base, 7, numbered);
    TEST_ASSERT_TRUE(xc_installed_folder_for(base, title));
    TEST_ASSERT_TRUE(xc_installed_folder_for(numbered, title));
    TEST_ASSERT_FALSE(xc_installed_folder_for(title, title));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_marker_as_written);
    RUN_TEST(test_marker_without_optional_keys);
    RUN_TEST(test_marker_crlf_unknown_keys_and_no_final_lf);
    RUN_TEST(test_marker_bad_values_are_absent);
    RUN_TEST(test_marker_long_value_cut_at_utf8_boundary);
    RUN_TEST(test_marker_rejected);
    RUN_TEST(test_folder_states);
    RUN_TEST(test_folder_for_title);
    return UNITY_END();
}
