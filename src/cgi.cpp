#include "config.h"
#include "cgi.h"
#include <cstdlib>
#include <string.h>

#ifdef BUFFER_LEN
#undef BUFFER_LEN
#endif
#define BUFFER_LEN 4096

/** Convert a two-char hex string into the char it represents **/
char
x2c(char *what)
{
    char digit;

    digit = (what[0] >= 'A' ? ((what[0] & 0xdf) - 'A') + 10 : (what[0] - '0'));
    digit *= 16;
    digit += (what[1] >= 'A' ? ((what[1] & 0xdf) - 'A') + 10 : (what[1] - '0'));
    return (digit);
}

/** Reduce any %xx escape sequences to the characters they represent **/
void
unescape_url(char *url)
{
    int i, j;
    char *p;

    for (p = url; *p; p++)
        if (*p == '+')
            *p = ' ';

    for (i = 0, j = 0; url[j]; ++i, ++j) {
        if ((url[i] = url[j]) == '%') {
            url[i] = x2c(&url[j + 1]);
            if (!url[j + 1] || !url[j + 2])
                break;
            j += 2;
        }
    }
    url[i] = '\0';
}

char *
getparam(char *string)
{
    char *buf = strdup(string);
    char *cgi = buf;

    while ((*cgi != '=') && (*cgi != '\0'))
        cgi++;
    *cgi = '\0';
    return (buf);
}

char *
getvalue(char *string)
{
    char *buf = strdup(string);
    char *cgi = buf;
    char *result = NULL;

    while ((*cgi != '=') && (*cgi != '\0'))
        cgi++;
    cgi++;

    result = strdup(cgi);
    delete[]buf;
    return (result);
}

char *
getcgivar(char *cgistring, char *param)
{
    int i;

    char *result = NULL;
    char cgiinput[BUFFER_LEN];
    static char result2[BUFFER_LEN];
    char *nvpair;

    strcpy(cgiinput, cgistring);

    /** Change all plusses back to spaces **/
    for (i = 0; cgiinput[i]; i++)
        if (cgiinput[i] == '+')
            cgiinput[i] = ' ';

    nvpair = strtok(cgiinput, "&");
    while (nvpair) {
        char *test = getparam(nvpair);

        if (!strcasecmp(test, param)) {
            result = getvalue(nvpair);
            delete[]test;
            break;
        }
        delete[]test;
        nvpair = strtok(NULL, "&");
    }

    unescape_url(result);
    strcpy(result2, result);
    delete[]result;
    return (result2);
}

/* An escape is three bytes for one, so this wrote past any buffer the
 * input merely fit in: the result is a std::string now. */
std::string
escape_url(const char *in)
{
    std::string out;
    char hex[4];
    int isvalid_cgichar(char);

    for (; *in != '\0'; in++) {
        if (*in == SPACE)
            out += PLUS;
        else if (isvalid_cgichar(*in))
            out += *in;
        else {
            /* A signed char of 0x80 or above printed as %FFFFFFxx, and
             * only its first three bytes were ever kept: such a byte
             * escapes to "%FF", as it always has. */
            unsigned int v = (unsigned int) *in;

            snprintf(hex, sizeof(hex), "%%%.2X", v > 0xFF ? 0xFF : v);
            out += hex;
        }
    }

    return out;
}

int
isvalid_cgichar(char c)
{
    if (isalnum((unsigned char) c) || (c == UNDERSCORE) || (c == HYPHEN) || (c == ':') ||
/*      (c == '\\') || */
        (c == '/') || (c == '.') ||
/*      (c == PLUS) ||
 *      (c == TAB) || 
 *      (c == ATSIGN) ||
 */ (c == PERCENT))
        return (1);

    else
        return (0);
}
