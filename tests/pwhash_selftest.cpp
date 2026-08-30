/* Salted password hashing must round-trip a salt containing zero bytes.
 *
 * The salt is eight BINARY bytes, so roughly 3% of randomly generated
 * salts contain at least one 0x00. Two C-string assumptions used to
 * break exactly those: testing *saltin treated a stored salt starting
 * with 0x00 as "no salt supplied" and minted a fresh one, and
 * sprintf("%.8s%s") truncated at the first zero while the hash still
 * consumed eight salt bytes, digesting uninitialized stack. Either way
 * the value could not be reproduced at login and the account was
 * locked out with no diagnostic.
 *
 * Build (after a normal ninja build of the server):
 *   see tests/run_pwhash_tests.sh
 */
#include <cstdio>
#include <cstring>

#define HTYPE_SHA1SALT 5
#define HTYPE_MD5SALT  3
#define TBUF 16384

namespace MUCK {
struct PasswordHash {
    static int hash(int type, char *out, const char *password,
                    const char *saltin);
    static int compare(const char *hashed, const char *password);
    static bool enabled;
    static int version;
};
}

/* Stubs: the real objects reference these from paths this test never
 * runs. They exist only to satisfy the linker. */
struct t_hash_entry;
int add_hash_int(const char *, int, t_hash_entry **, unsigned int) { return 0; }
const char *envpropstr(int *, const char *) { return 0; }
const char *genderof(int, int) { return 0; }
const char *get_property_class(int, const char *) { return 0; }
struct t_hash_entry *find_hash(const char *, t_hash_entry **, unsigned int) { return 0; }
int kill_hash(t_hash_entry **, unsigned int, int) { return 0; }
int number(const char *) { return 0; }
const char *tp_sex_prop = "sex";
namespace MUCK {
class Database;
char g_database[4096];
const char *getName(int) { return 0; }
int getOwner(int) { return -1; }
int typeOf(int) { return 6; }
}

static int fails = 0;

static void
check(const char *what, bool ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

int
main(void)
{
    const char *pw = "correct horse battery staple";
    char desc[128];

    /* every position a zero byte can land in */
    for (int type = 0; type < 2; type++) {
        int ht = type ? HTYPE_MD5SALT : HTYPE_SHA1SALT;

        for (int pos = 0; pos < 8; pos++) {
            char salt[9], stored[TBUF];

            for (int i = 0; i < 8; i++)
                salt[i] = (char) (0x41 + i);
            salt[pos] = '\0';
            salt[8] = '\0';

            sprintf(desc, "%s salt with 0x00 at byte %d verifies",
                    ht == HTYPE_SHA1SALT ? "sha1" : "md5", pos);
            if (!MUCK::PasswordHash::hash(ht, stored, pw, salt)) {
                check(desc, false);
                continue;
            }
            check(desc, MUCK::PasswordHash::compare(stored, pw) != 0);
        }
    }

    /* an ordinary salt must still work, and a wrong password must
     * still be rejected: the fix must not weaken anything */
    {
        char salt[9], stored[TBUF];

        memcpy(salt, "ABCDEFGH", 9);
        MUCK::PasswordHash::hash(HTYPE_SHA1SALT, stored, pw, salt);
        check("ordinary salt verifies",
              MUCK::PasswordHash::compare(stored, pw) != 0);
        check("wrong password rejected",
              MUCK::PasswordHash::compare(stored, "not the password") == 0);
    }

    /* a caller-supplied salt must actually be USED, not silently
     * replaced: that replacement is what locks out the roughly one in
     * 256 accounts whose stored salt begins with a zero byte */
    {
        char salt[9], a[TBUF], b[TBUF];

        memset(salt, 0, sizeof(salt));
        salt[0] = '\0';
        memcpy(salt + 1, "ABCDEFG", 7);
        MUCK::PasswordHash::hash(HTYPE_SHA1SALT, a, pw, salt);
        MUCK::PasswordHash::hash(HTYPE_SHA1SALT, b, pw, salt);
        check("zero-leading salt is honored, not regenerated",
              strcmp(a, b) == 0);
    }

    printf("RESULT: %d failures\n", fails);
    return fails ? 1 : 0;
}
