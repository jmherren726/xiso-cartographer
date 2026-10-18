/* Tests for the whole-image hashes (xcat/hash.h, docs/formats/journal.md §5 and §7). */
#include "unity.h"
#include "xcat/hash.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void hex_of(const xc_image_hash *h, char md5[33], char sha1[41])
{
    uint8_t m[XC_MD5_BYTES];
    uint8_t s[XC_SHA1_BYTES];
    xc_image_hash_final(h, m, s);
    xc_hex_lower(m, sizeof m, md5);
    xc_hex_lower(s, sizeof s, sha1);
}

static void test_known_values(void)
{
    xc_image_hash *h = NULL;
    char md5[33];
    char sha1[41];
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_create(&h));
    hex_of(h, md5, sha1);
    TEST_ASSERT_EQUAL_STRING("d41d8cd98f00b204e9800998ecf8427e", md5);
    TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", sha1);
    xc_image_hash_update(h, "abc", 3);
    hex_of(h, md5, sha1);
    TEST_ASSERT_EQUAL_STRING("900150983cd24fb0d6963f7d28e17f72", md5);
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", sha1);
    /* final doesn't end the hash: more data continues it. */
    xc_image_hash_update(h, "dbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 53);
    TEST_ASSERT_EQUAL_UINT64(56, xc_image_hash_count(h));
    hex_of(h, md5, sha1);
    TEST_ASSERT_EQUAL_STRING("84983e441c3bd26ebaae4aa1f95129e5e54670f1", sha1);
    xc_image_hash_free(h);
    TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_image_hash_create(NULL));
}

/* Saving at any point and restoring into a fresh hash, then feeding the rest from the saved
 * offset, gives the same digests as one pass (journal.md §6.7). */
static void test_save_and_restore(void)
{
    static uint8_t data[1000];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7U + 3U);
    }
    xc_image_hash *whole = NULL;
    TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_create(&whole));
    xc_image_hash_update(whole, data, sizeof data);
    char want_md5[33];
    char want_sha1[41];
    hex_of(whole, want_md5, want_sha1);

    static const size_t cuts[] = {0, 1, 63, 64, 65, 127, 128, 500, 999, 1000};
    for (size_t c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
        xc_image_hash *a = NULL;
        xc_image_hash *b = NULL;
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_create(&a));
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_create(&b));
        xc_image_hash_update(a, data, cuts[c]);
        uint8_t m[XC_MD5_BYTES];
        uint8_t s[XC_SHA1_BYTES];
        uint64_t off = xc_image_hash_save(a, m, s);
        TEST_ASSERT_EQUAL_UINT64(cuts[c] / 64U * 64U, off);
        TEST_ASSERT_EQUAL_INT(XC_OK, xc_image_hash_restore(b, off, m, s));
        TEST_ASSERT_EQUAL_UINT64(off, xc_image_hash_count(b));
        xc_image_hash_update(b, data + off, sizeof data - (size_t)off);
        char md5[33];
        char sha1[41];
        hex_of(b, md5, sha1);
        TEST_ASSERT_EQUAL_STRING(want_md5, md5);
        TEST_ASSERT_EQUAL_STRING(want_sha1, sha1);
        TEST_ASSERT_EQUAL_INT(XC_ERR_INVALID_ARG, xc_image_hash_restore(b, 65, m, s));
        xc_image_hash_free(a);
        xc_image_hash_free(b);
    }
    xc_image_hash_free(whole);
}

static void test_hex(void)
{
    static const uint8_t b[4] = {0x00, 0x9F, 0xAB, 0xFF};
    char out[9];
    xc_hex_lower(b, sizeof b, out);
    TEST_ASSERT_EQUAL_STRING("009fabff", out);
    uint8_t back[4];
    TEST_ASSERT_TRUE(xc_hex_parse("009FabFF", back, sizeof back));
    TEST_ASSERT_EQUAL_MEMORY(b, back, sizeof b);
    TEST_ASSERT_FALSE(xc_hex_parse("009fab", back, sizeof back));
    TEST_ASSERT_FALSE(xc_hex_parse("009fabfff", back, sizeof back));
    TEST_ASSERT_FALSE(xc_hex_parse("009fabfg", back, sizeof back));
    TEST_ASSERT_FALSE(xc_hex_parse(NULL, back, sizeof back));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_values);
    RUN_TEST(test_save_and_restore);
    RUN_TEST(test_hex);
    return UNITY_END();
}
