/*
 * (c) 1990 Chelsea Dyerman, University of California, Berkeley (XCF)
 *
 * Portions of this header file are copyright of Regents of U of California.
 * This file is being distributed along with a copy of the Berkeley software
 * agreement. 
 *
 */

/*
 * Copyright (c) 1980 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 *
 *	@(#)strings.h	5.3 (Berkeley) 11/18/87
 */

/*
 * External function definitions
 * for routines described in string(3).
 */

/*
char	*strcat();
char	*strncat();
int	strcmp();
int	strncmp();
int	strcasecmp();
int	strncasecmp();
char	*strcpy();
char	*strncpy();
int	strlen();
char	*index();
char	*rindex();
*/

extern const char *strencrypt(const char *data, const char *key);
extern const char *strdecrypt(const char *data, const char *key);
extern void MD5hash(void *dest, const void *orig, int len);
extern void MD5hex(char *dest, const char *orig, int len);
extern void SHA1hash(unsigned char *dest, const char *orig, int len);
extern void SHA1hex(char *dest, const char *orig, int len);
extern int base64tohex(char *dest, int olen, const char *orig, int ilen);
extern int hextobase64(char *dest, int olen, const char *orig, int ilen);
extern int strtohex(char *dest, int olen, const char *orig, int ilen, bool uppercase = 1);
extern int hextostr(char *dest, int olen, const char *orig, int ilen);
extern size_t escapestr(char *obuf, const size_t olen, const char *ibuf, const size_t ilen, bool *truncated = NULL);

extern std::string strToHex(const std::string &in, bool uppercase = true);       
extern std::string hexToStr(const std::string &in);
extern std::string ascii_to_utf8(const std::string& in);