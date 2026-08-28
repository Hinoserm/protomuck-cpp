#ifndef MUCK_PASSWORDHASH_H
#define MUCK_PASSWORDHASH_H

/* Player password hashing: tagged hash strings of the form $tag$salt$hash,
 * versioned so old databases with plain, MD5, or untagged hashes can be
 * read and upgraded. Extracted from Database.cpp.
 */

#define HVER_MASK       0x00F0
#define HVER_SHIFT      4
#define HVER_NONE       0x0 /* Tagging Disabled (Equivalent to old DB_MD5PASSES behavior, used when DB_NEWPASSES is ment to be off) */
#define HVER_RAWMD5     0x0 /* Tagging Disabled (Equivalent to old DB_MD5PASSES behavior, used when DB_NEWPASSES is ment to be on) */
#define HVER_SHA1SALT   0x1 /* Tagging Enabled, SHA-1+Salt is default algo, can read Plain, MD5, MD5+Salt, SHA1, SHA1+Salt */
#define HVER_RESERVED2  0x2 /* Reserved for future hashing algo upgrades */
#define HVER_RESERVED3  0x3 /* Reserved for future hashing algo upgrades */
#define HVER_RESERVED4  0x4 /* Reserved for future hashing algo upgrades */
#define HVER_RESERVED5  0x5 /* Reserved for future hashing algo upgrades */
#define HVER_RESERVED6  0x6 /* Reserved for future hashing algo upgrades */
#define HVER_RESERVED7  0x7 /* Reserved for future hashing algo upgrades */
                           /* If more slots are needed, consider making a new database format update with a global DB parameter */
                           /* Alternately, there's a flag reserved for future expansion if need be which will double the capacity */
#define HVER_CURRENT   HVER_SHA1SALT /* Our current version */

#define HTYPE_DISABLED  -2 /* Reserved for diabled passwords (disallow login with any password) */
#define HTYPE_INVALID   -1 /* Returned for errors */
#define HTYPE_NONE      0  /* Reserved for blank passwords (allow logins with any password) */
#define HTYPE_PLAIN     1  /* Reserved for plaintext passwords */
#define HTYPE_MD5       2  /* Reserved for MD5 hashed passwords */
#define HTYPE_MD5SALT   3  /* Reserved for Salted MD5 hashed passwords */
#define HTYPE_SHA1      4  /* Reserved for SHA1 hashed passwords */
#define HTYPE_SHA1SALT  5  /* Reserved for Salted SHA1 hashed passwords */
#define HTYPE_CURRENT   HTYPE_SHA1SALT /* Our current best */

namespace MUCK {

class PasswordHash {
  public:
    /* Master switches, historically db_hash_passwords and db_hash_ver:
     * set from the command line and the database header at load. */
    static bool enabled;
    static int version;

    static char *valToTag(int type);
    static int tagToVal(const char *tag);
    static int hash(int type, char *out, const char *password, const char *saltin);
    static int split(const char *hashin, int *tagout, char *hashout, char *saltout);
    static int oldConvert(char *out, const char *hash);
    static int compare(const char *hashed, const char *password);
};

} /* namespace MUCK */

#endif /* MUCK_PASSWORDHASH_H */
