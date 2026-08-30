#include "copyright.h"
#include "config.h"
#include "db.h"
#include "externs.h"
#include "strutils.h"
#include "PasswordHash.h"

namespace MUCK {

bool PasswordHash::enabled = 0;
int PasswordHash::version = 0;

} /* namespace MUCK */

char *
MUCK::PasswordHash::valToTag(int type)
{
    switch (type) {
        case HTYPE_SHA1SALT:
            return "SHA1SALTED";
        case HTYPE_MD5:
            return "MD5";
        case HTYPE_NONE:
            return "NONE";
        case HTYPE_DISABLED:
            return "DISABLED";
        case HTYPE_PLAIN:
            return "PLAIN";
        case HTYPE_SHA1:
            return "SHA1";
        case HTYPE_MD5SALT:
            return "MD5SALTED";
        case HTYPE_INVALID:
            return NULL;
        default:
            return NULL;
    }
}

int
MUCK::PasswordHash::tagToVal(const char *tag)
{
    char buf[BUFFER_LEN];
    int i = 0;

    if (!tag)
        return HTYPE_INVALID;

    for (i = 0; (i < BUFFER_LEN - 1); i++) {
        if (tag[i] == '\0' || tag[i] == ':')
            break;
        buf[i] = (char) toupper((int) tag[i]);
    }

    buf[i++] = '\0';

    if (!strcmp(buf, "SHA1SALTED"))
        return HTYPE_SHA1SALT;
    if (!strcmp(buf, "MD5"))
        return HTYPE_MD5;
    if (!strcmp(buf, "NONE"))
        return HTYPE_NONE;
    if (!strcmp(buf, "DISABLED"))
        return HTYPE_DISABLED;
    if (!strcmp(buf, "PLAIN"))
        return HTYPE_PLAIN;
    if (!strcmp(buf, "SHA1"))
        return HTYPE_SHA1;
    if (!strcmp(buf, "MD5SALTED"))
        return HTYPE_MD5SALT;

    return HTYPE_INVALID;
}

int
MUCK::PasswordHash::hash(int type, char *out, const char *password, const char *saltin)
{
    char buf[BUFFER_LEN];
    char sbuf[17];
    char salt[9];
    int i = 0;

    if (!out)
        return 0;
    if (!password || !*password) {
        sprintf(out, "%s", valToTag(HTYPE_NONE));
        return 1;
    }
    /* The salt is EIGHT BINARY BYTES, not a C string: any of them can
     * legitimately be 0x00. Testing *saltin treated a stored salt
     * beginning with a zero byte as "no salt supplied" and minted a
     * fresh one, so the recomputed hash could never match and the
     * account was locked out for good. Only a null pointer means
     * "generate one". */
    if (!saltin) {
        for (i = 0; i < 8; i++)
            salt[i] = (unsigned char) (RANDOM() & 0xFF);
        salt[8] = '\0';
    } else {
        memcpy(salt, saltin, 8);
        salt[8] = '\0';
    }

    strtohex(sbuf, 17, salt, 8);

    switch (type) {
        case HTYPE_SHA1SALT:
            /* memcpy, not sprintf: "%.8s" stops at the first zero
             * byte in the salt while the hash below still reads all
             * eight, so a salt containing 0x00 hashed uninitialized
             * stack and produced a value that could not be
             * reproduced at verify time. */
            memcpy(buf, salt, 8);
            memcpy(buf + 8, password, strlen(password));
            SHA1hex(buf, buf, strlen(password) + 8);
            sprintf(out, "%s:%s:%s", valToTag(type), buf, sbuf);
            break;
        case HTYPE_MD5:
            MD5hex(buf, password, strlen(password));
            sprintf(out, "%s:%s", valToTag(type), buf);
            break;
        case HTYPE_NONE:
            sprintf(out, "%s", valToTag(type));
            break;
        case HTYPE_DISABLED:
            sprintf(out, "%s", valToTag(type));
            break;
        case HTYPE_PLAIN:
            sprintf(buf, "%s", password);
            sprintf(out, "%s:%s", valToTag(type), buf);
            break;
        case HTYPE_SHA1:
            SHA1hex(buf, password, strlen(password));
            sprintf(out, "%s:%s", valToTag(type), buf);
            break;
        case HTYPE_MD5SALT:
            /* same zero-byte hazard as HTYPE_SHA1SALT above */
            memcpy(buf, salt, 8);
            memcpy(buf + 8, password, strlen(password));
            MD5hex(buf, buf, strlen(password) + 8);
            sprintf(out, "%s:%s:%s", valToTag(type), buf, sbuf);
            break;
        case HTYPE_INVALID:
            *out = '\0';
            return 0;
        default:
            *out = '\0';
            return 0;
    }
    return 1;
}

int
MUCK::PasswordHash::split(const char *hashin, int *tagout, char *hashout, char *saltout)
{
    int i = 0, k = 0, mode = 0;
    int j[3];

    if (!hashin)
        return 0;

    if (hashin[i] == '\0')
        return 0;

    mode = 1;

    for (i = 0; (i < BUFFER_LEN - 1) && (mode < 4); i++) {
        if (hashin[i] == ':') {
            j[mode - 1] = i;
            mode++;
        }
        if (hashin[i] == '\0') {
            j[mode - 1] = i;
            break;
        }
    }

    switch (mode) {
        case 4:
            mode--;
        case 3:
            for (i = j[1] + 1, k = 0; i < j[2]; i++, k++) {
                if (saltout)
                    saltout[k] = hashin[i];
            }
            if (saltout)
                saltout[k++] = '\0';
        case 2:
            for (i = j[0] + 1, k = 0; i < j[1]; i++, k++) {
                if (hashout)
                    hashout[k] = hashin[i];
            }
            if (hashout)
                hashout[k++] = '\0';
        case 1:
            if (tagout)
                *tagout = tagToVal(hashin);
            break;
        default:
            return 0;
    }

    return mode;
}

int
MUCK::PasswordHash::compare(const char *hashed, const char *password)
{
    char buf[BUFFER_LEN];
    char hbuf[BUFFER_LEN];
    char sbuf[BUFFER_LEN];
    char salt[9];
    int res = 0, tag = 0, i = 0;

    sbuf[0] = '\0';
    salt[0] = '\0';

    if (!hashed)
        return 1;
    for (i = 0; hashed[i] != 0 && i < BUFFER_LEN - 1; i++)
        buf[i] = toupper(hashed[i]);
    buf[i] = '\0';
    res = split(buf, &tag, NULL, sbuf);
    if (res == 0)
        return 0;
    if (tag == HTYPE_DISABLED)
        return 0;
    if (tag == HTYPE_NONE)
        return 1;
    if (!password || !*password)
        return 0;
    if (res == 3) {
        hextostr(salt, 9, sbuf, 16);
        if (!hash(tag, hbuf, password, salt))
            return 0;
    } else {
        if (!hash(tag, hbuf, password, NULL))
            return 0;
    }
    return !strcmp(buf, hbuf);
}

int
MUCK::PasswordHash::oldConvert(char *out, const char *hash)
{
    char buf[BUFFER_LEN];

    if (!hash || !*hash) {
        sprintf(out, "%s", valToTag(HTYPE_NONE));
        return 1;
    }

    if (!base64tohex(buf, BUFFER_LEN, hash, strlen(hash)))
        return 0;

    sprintf(out, "%s:%s", valToTag(HTYPE_MD5), buf);
    return 1;
}
