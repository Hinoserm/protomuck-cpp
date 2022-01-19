#include "config.h"

#ifdef NEWHTTPD

/*****************************************************************************/
/* ProtoMUCK Webserver v1.0 written by Hinoserm (foxsteve).                  */
/* Many thanks to The_Blob for helping with the HTTP protocol.               */
/*****************************************************************************/
/* TODO:                                                                     */
/*---------------------------------------------------------------------------*/
/* Finish proper mimetypes handling. I'd like to have it parse a file with   */
/*  the same format as /etc/mime.types and use that as a database of mime    */
/*  types based on file extensions.                                          */
/*---------------------------------------------------------------------------*/
/* Redesign the http_dourl() and related functions to be more like how the   */
/*  http_dofile() function works. (IE: proper index handling of HTMufs)      */
/*---------------------------------------------------------------------------*/
/* Eventually, I'd like to see MPI-in-props support, but not anytime soon.   */
/*---------------------------------------------------------------------------*/
/* Implement a-lot- more control via @tunes. This will likely be finished    */
/*  before I release the webserver, and is a top priority.                   */
/*---------------------------------------------------------------------------*/
/* URLs-in-props for redirections. This -will- be finished before I'm done.  */
/*---------------------------------------------------------------------------*/
/* The code needs some overall cleanup and proper commenting, and I'd like   */
/*  to write out a detailed document on it's use.                            */
/*---------------------------------------------------------------------------*/
/* Sorting for the directory listing stuff would be nice.                    */
/*****************************************************************************/

#include <sstream>
#include <iomanip>
#include <stdexcept>

#include "db.h"
#include "netresolve.h"
#include "interface.h"
#include "mufevent.h"
#include "externs.h"
#include "newhttp.h"
#include "strings.h"
#include "params.h"
#include "interp.h"
#include "match.h"
#include "props.h"
#include "tune.h"
#include "cgi.h"
#include "mpi.h"

#ifdef MCCP_ENABLED
void mccp_start(struct descriptor_data *d, int version);
extern int process_output(struct descriptor_data *d);
#endif

int httpucount = 0;             /* number of total HTTP users */
int httpfcount = 0;             /* number of total HTTP file transfers */

struct http_statstruct http_statcodes[] = {
    {100, "Continue"},
    {101, "Switching Protocols"},
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {306, "(Unused)"},
    {307, "Temporary Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Request Entity Too Large"},
    {414, "Request-URI Too Large"},
    {415, "Unsupported Media Type"},
    {416, "Requested Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {0, ""}
};

struct http_mimestruct http_mimetypes[] = {
    {"htm", "text/html"},
    {"html", "text/html"},
    {"shtml", "text/html"},
    {"txt", "text/plain"},
    {"text", "text/plain"},
    {"muf", "text/plain"},
    {"m", "text/plain"},
    {"gif", "image/gif"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"png", "image/png"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/x-gzip"},
    {"exe", "application/octet-stream"},
    {"bin", "application/octet-stream"},
    {"tar", "application/x-tar"},
    {"mp3", "audio/mpeg"},
    {"ra", "audio/x-realaudio"},
    {"wav", "audio/x-wav"},
    {"avi", "video/x-msvideo"},
    {"mpg", "video/mpeg"},
    {"mpeg", "video/mpeg"},
    {NULL, NULL}
};

const char *http_defaulthomes[] = {
    "index.html", "index.htm",
    "home.html", "home.htm",
    "", NULL
};

struct http_method http_methods[] = {
    {"GET",
     HS_PROPLIST | HS_REDIRECT | HS_PLAYER | HS_HTMUF | HS_VHOST | HS_FILE | HS_MPI | 0x200},
    {"HEAD",
     HS_PROPLIST | HS_REDIRECT | HS_PLAYER | HS_VHOST | HS_FILE | HS_MPI | HS_HEADONLY | 0x200},
    {"POST", HS_PLAYER | HS_HTMUF | HS_VHOST | HS_BODY | 0x200},
    {NULL, 0}
};

http::http(struct descriptor_data *d)
{
    //d->http = new http_struct;

    //if (d->type == CT_HTTP) {
    this->rootobj = NOTHING;
    this->rootdir = NULL;
    this->smethod = NULL;
    //this->cgidata = NULL; // This is a std::string now.
    //this->newdest = NULL; // This is a std::string now.
    //this->method = NULL;  // This is a std::string now.
    //this->dest = NULL;    // This is a std::string now.
    //this->ver = NULL;     // This is a std::string now.
    this->flags = 0;
    this->fr = NULL;
    this->header_complete = 0;

    this->body.data = NULL;
    this->body.elen = 0;
    this->body.len = 0;
    this->body.curr = 0;

    this->ws_q = NULL;
    this->ws_q_tail = NULL;
    this->websocket = false;

    this->d = d;

    if (tp_web_max_users && httpucount++ > tp_web_max_users)
        this->senderror(503, "Too many users connected to service.");
}

http::~http(void)
{
    delete[]this->rootdir;
    delete[]this->body.data;

    if (ws_q) {
        while (ws_q) {
            struct ws_queue* q = ws_q;
            ws_q = q->next;
            delete q;
        }
    }

    //d->http->fields.clear(); //Not required.

    httpucount--;
}

void
  http::log(int debuglvl, char *format, ...)
{
    char
      buf[BUFFER_LEN];
    char
      tbuf[40];

    va_list args;
    FILE *
        fp;

    time_t lt = current_systime;

    va_start(args, format);
    vsprintf(buf, format, args);
    va_end(args);

    /* Finish me! */
    if (debuglvl <= tp_web_logwall_lvl)
        wall_logwizards(buf);

    if (debuglvl <= tp_web_logfile_lvl) {
        *tbuf = '\0';
        if ((fp = fopen(HTTP_LOG, "a")) == NULL) {
            fprintf(stderr, "Unable to open %s!\n", HTTP_LOG);
            fprintf(stderr, "%.16s: [%d]: %s", ctime(&lt), d ? d->descriptor : -1, buf);
        } else {
            format_time(tbuf, 32, "%c\0", localtime(&lt));
            fprintf(fp, "%.32s: [%d]: %s", tbuf, d ? d->descriptor : -1, buf);
            fclose(fp);
        }
    }
}

/* queue_text():                                        */
/*   Works exactly like queue_write(), but can format   */
/*   like sprintf().                                    */
int
queue_text(struct descriptor_data *d, char *format, ...)
{
    va_list args;
    char
      buf[BUFFER_LEN];

    va_start(args, format);
    vsprintf(buf, format, args);
    va_end(args);

    return queue_write(d, buf, strlen(buf));
}

/* http_split():                                        */
/*   Splits string s at char c. Returns a pointer.      */
char *
http::split(char *s, int c)
{
    char *p;

    for (p = s; *p && (*p != c); p++) ;
    if (!*p)
        return NULL;

    *p++ = '\0';
    return p;
}

/* http_statlookup():                                   */
/*   Looks up the reason phrase for a status code from  */
/*   the http_statcodes[] table and returns it.         */
const char *
http::statlookup(int status)
{
    struct http_statstruct *s = http_statcodes;

    while (s->code && s->code != status)
        s++;

    return (s->msg);
}

/* http_methodlookup():                                 */
/*   Looks up data from the http_methods[] table and    */
/*   returns it.                                        */
struct http_method *
http::methodlookup(const std::string & method)
{
    struct http_method *m = http_methods;

    while (m->method && method.compare(m->method))
        m++;

    return (m);
}

/* http_mimelookup():                                   */
const char *
http::mimelookup(const char *ext) const
{
     struct http_mimestruct *m = http_mimetypes;

     while (m->ext && string_compare(m->ext, ext))
           m++;

       return (m->type);
 }
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        /* http_gethost():                                      *//*   Return the 'Host: ' data for a descr, or return    *//*   tp_servername if there's no field.                 */ const char *
   http::gethost(void)
{
    if (fields.count("Host")) {
        std::string & s = fields["Host"];

        if (s.length())
            return s.c_str();
    }

    return tp_servername;
}

/* http_sendheader():                                   */
/*   Create and send an HTTP header based on the info   */
/*   provided. If content_type isn't given, it defaults */
/*   to 'text/plain'. If content_length is below zero,  */
/*   it won't be given.                                 */
void
  http::sendheader(int statcode, const char *content_type, int content_length)
{
    char
      tbuf[BUFFER_LEN];

    time_t t = time(NULL) + get_tz_offset();

    format_time(tbuf, BUFFER_LEN, "%a, %d %b %Y %T GMT", localtime(&t));

    queue_text(d, "HTTP/1.1 %d %s\r\nDate: %s\r\n" "Server: ProtoMUCK/%s\r\n" "Connection: close\r\n", statcode, this->statlookup(statcode), tbuf, PROTOBASE);

    if (content_length >= 0)
        queue_text(d, "Content-Length: %d\r\n", content_length);

    if (content_type)
        queue_text(d, "Content-Type: %s\r\n", content_type);
    else
        queue_text(d, "Content-Type: text/plain\r\n");

    queue_text(d, "\r\n");
}

void
  http::sendredirect(const char *url)
{
    const char *
        statmsg = this->statlookup(301);
    char *
        host = alloc_string(this->gethost());

    time_t t = time(NULL) + get_tz_offset();
    char
      tbuf[50];
    char
      buf[BUFFER_LEN];
    char
      buf2[BUFFER_LEN];

    escape_url(buf2, (char *) url);
    this->split(host, ':');
    format_time(tbuf, 48, "%a, %d %b %Y %T GMT", localtime(&t));

    sprintf(buf, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
            "<html><head>\r\n  <title>%d %s</title>\r\n  </head><body>\r\n"
            "<h1>%s</h1>\r\n  <p>The document has moved <a href=\"%s\">here"
            "</a>.</p>\r\n  <hr />\r\n  <address>ProtoMUCK %s Server at %s"
            " Port %d</address>\r\n</body></html>\r\n", 301, statmsg, statmsg, buf2, PROTOBASE, host, this->d->cport);
    queue_text(this->d,
               "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: ProtoMUCK/%s\r\n"
               "Location: %s\r\nConnection: close\r\nContent-Type: text/h"
               "tml; charset=iso-8859-1\r\nContent-Length: %d\r\n\r\n", 301, statmsg, tbuf, PROTOBASE, buf2, strlen(buf));
    queue_text(this->d, buf);

    this->d->booted = 4;

    delete[]host;
}

/* http_senderror():                                    */
/*   Create and send an error page.                     */
void
  http::senderror(int statcode, const char *msg)
{
    const char *
        statmsg;
    char *
        host = alloc_string(this->gethost());
    char
      buf[BUFFER_LEN];

    statmsg = this->statlookup(statcode);

    this->log(2, "ERROR:   '%d, %s'\n", statcode, msg);
    this->split(host, ':');

    sprintf(buf, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
            "<html><head>\r\n  <title>%d %s</title>\r\n"
            "</head><body>\r\n  <h1>%s</h1>\r\n"
            "  <p>%s<br /></p>\r\n  <hr />\r\n"
            "  <address>ProtoMUCK %s Server at %s Port %d</address>\r\n" "</body></html>\r\n", statcode, statmsg, statmsg, msg, PROTOBASE, host, this->d->cport);

    this->sendheader(statcode, "text/html; charset=iso-8859-1", strlen(buf));
    queue_text(this->d, buf);

    delete[]host;

    this->body.elen = 0;
    this->body.len = 0;
    this->d->booted = 4;
    return;
}

/* smart_prop_getref():                                 */
/*   Get a dbref from a prop, smartly.                  */
dbref
smart_prop_getref(dbref what, const char *propname)
{
    const char *
        tmpchar = NULL;
    dbref
      ref = NOTHING;

    if (((ref = get_property_dbref(what, propname)) != NOTHING) || (tmpchar = get_property_class(what, propname))) {
        if (tmpchar) {
            if (*tmpchar == NUMBER_TOKEN && number(tmpchar + 1)) {
                ref = (dbref) atoi(++tmpchar);
            } else if (*tmpchar == REGISTERED_TOKEN) {
                ref = find_registered_obj(what, tmpchar);
            } else if (number(tmpchar)) {
                ref = (dbref) atoi(tmpchar);
            } else {
                ref = NOTHING;
            }
        } else {
            if (ref == AMBIGUOUS)
                ref = NOTHING;
        }
    }

    return ref;
}

char *
http::parsempi(dbref what, const char *yerf, char *buf)
{
    this->flags |= HS_MPI;

    if (yerf)
        return (do_parse_mesg(d->descriptor, OWNER(what), what, yerf, "WWW", buf, 0));
    else
        return "";
}

int
  http::parsedest(void)
{
    char
      buf[BUFFER_LEN];
    char
      buf2[BUFFER_LEN];
    char *
    cgi, *
        dir = NULL;
    char *
        p;                      // char *p, *q;
    const char *
        s;
    dbref
      ref = NOTHING;

    strcpy(buf, this->dest.c_str());
    cgi = this->split(buf, '?');

    for (p = buf; *p && *p == '/'; p++) ;
    unescape_url(p);

    if (tp_web_allow_vhosts && OkObj(tp_www_root)
        && (this->smethod->flags & HS_VHOST)) {
        char *
            host = alloc_string(this->gethost());

        this->split(host, ':');
        this->log(4, "VHOST:   '%s'\n", host);
        sprintf(buf2, "@vhosts/%s", host);
        if (is_propdir(tp_www_root, buf2)) {
            sprintf(buf2, "@vhosts/%s/rootObj", host);
            ref = smart_prop_getref(tp_www_root, buf2);

            sprintf(buf2, "@vhosts/%s/rootDir", host);
            s = get_property_class(tp_www_root, buf2);

            dir = alloc_string(s);
            if (OkObj(ref) || dir)
                this->flags |= HS_VHOST;
        }

        /* else { */
        /* I don't plan to support <player>.somename.com vhosts, */
        /* but if I did, this is where it'd go.                  */
        /* } */
        delete[]host;
    }

    /*
       if (tp_web_allow_players && *p == '~'
       && (d->http->smethod->flags & HS_PLAYER)
       && !(d->http->flags & HS_VHOST)) {
       p++;
       q = http_split(p, '/');
       http_log(d, 4, "PLAYER:  '%s'\n", p);

       ref = lookup_player(p);
       if (!OkObj(ref)) {
       http_senderror(d, 404, "Player not found.");
       return 1;
       }
       d->http->flags |= HS_PLAYER;
       p = q;
       }
     */

    if (!OkObj(ref))
        ref = tp_www_root;

    if (dir)
        this->rootdir = dir;
    else
        this->rootdir = string_dup("/_/www");

    if (cgi)
        this->cgidata = cgi;
    if (p)
        this->newdest = p;
    this->rootobj = ref;

    /* http_log(d, 3, "URL:     '%s'\n", d->http->newdest); */
    this->log(4, "ROOTOBJ: '%s'\n", unparse_object(1, this->rootobj));
    this->log(4, "ROOTDIR: '%s'\n", this->rootdir);
    if (this->cgidata.length())
        this->log(5, "CGIDATA: '%s'\n", this->cgidata.c_str());

    return 0;

}

stk_array *
http::formarray(const char *data)
{
    stk_array *nw, *val;
    struct inst temp1;
    char *buf, *cur, *next, *sep, *line, *end;
    int i, len, etype;

    nw = new_array_dictionary();

    size_t buflen = strlen(data) + 2;

    if (!(buf = new char[buflen] ()))
        return (nw);

    strcpy(buf, data);

    this->split(buf, '\r');
    this->split(buf, '\n');

    if (!buf[0]) {
        delete[]buf;
        return (nw);
    }

    for (cur = buf; cur; cur = next) {
        next = this->split(cur, '&');
        sep = this->split(cur, '=');
        if (!cur[0])
            continue;

        unescape_url(cur);
        this->log(6, "FIELD:   '%s' (%d)\n", cur, strlen(cur));

        val = new_array_packed(0, 0);
        if (!sep) {
            temp1.type = PROG_STRING;
            temp1.data.string = alloc_prog_string("");
            array_appenditem(&val, &temp1);
            CLEAR(&temp1);
            this->log(6, "LINE:    '' (0)\n");
            array_set_strkey_arrval(&nw, cur, val);
            continue;
        }
        unescape_url(sep);

        len = strlen(sep) + 1;
        line = end = sep;
        for (i = 0; i < len; i++) {

            switch (end[0]) {
                case '\0':
                case '\r':
                case '\n':
                    etype = end[0];
                    break;
                default:
                    etype = -1;
            }

            if (etype == -1) {
                end++;
                continue;
            }

            end[0] = '\0';

            if (etype == '\r' && end[1] == '\n') {
                end++;
                end[0] = '\0';
            }

            temp1.type = PROG_STRING;
            temp1.data.string = alloc_prog_string(line);
            array_appenditem(&val, &temp1);
            CLEAR(&temp1);
            this->log(6, "LINE:    '%s' (%d)\n", line, strlen(line));
            if (etype == '\0') {
                line = end;
                break;
            }
            line = ++end;
        }
        array_set_strkey_arrval(&nw, cur, val);
    }
    delete[]buf;
    return (nw);
}

stk_array *
http::makearray(void)
{
    stk_array *nw = new_array_dictionary();
    char *p = this->body.data;

    array_set_strkey_intval(&nw, "DESCR", d->descriptor);
    array_set_strkey_intval(&nw, "CONNECTED", d->connected);
    array_set_strkey_strval(&nw, "HOST", host_as_hex(d->hu->h->a));
    array_set_strkey_intval(&nw, "CONNECTED_AT", (int) d->connected_at);
    array_set_strkey_intval(&nw, "LAST_TIME", (int) d->last_time);
    array_set_strkey_intval(&nw, "COMMANDS", d->commands);
    array_set_strkey_intval(&nw, "PORT", d->cport);
    array_set_strkey_strval(&nw, "HOSTNAME", d->hu->h->name);
    array_set_strkey_strval(&nw, "USERNAME", d->hu->u->user);
    array_set_strkey_strval(&nw, "Method", this->method.c_str());
    array_set_strkey_strval(&nw, "TheDEST", this->dest.c_str());
    array_set_strkey_strval(&nw, "HTTPVer", this->ver.c_str());
    array_set_strkey_intval(&nw, "SID", d->descriptor);
    array_set_strkey_intval(&nw, "Flags", this->flags);

    if ((this->flags & HS_BODY) && this->body.len && p) {
        array_set_strkey_intval(&nw, "BODYLen", this->body.len);
        if (strlen(p) < BUFFER_LEN)
            array_set_strkey_strval(&nw, "BODY", p);
        array_set_strkey_arrval(&nw, "POSTData", this->formarray(p));
    }

    if (this->cgidata.length() < BUFFER_LEN)
        array_set_strkey_strval(&nw, "CGIParams", this->cgidata.c_str());

    if (this->cgidata.length())
        array_set_strkey_arrval(&nw, "CGIData", this->formarray(this->cgidata.c_str()));

    if (this->fields.size()) {
        std::map < std::string, std::string >::iterator iter;
        struct inst temp1;

        stk_array *nw2 = new_array_packed(0, 0);
        stk_array *nw3 = new_array_dictionary();

        for (iter = this->fields.begin(); iter != this->fields.end(); ++iter) {
            temp1.type = PROG_STRING;
            temp1.data.string = alloc_prog_string(iter->first.c_str());
            array_appenditem(&nw2, &temp1);
            array_set_strkey_strval(&nw3, iter->first.c_str(), iter->second.c_str());
            CLEAR(&temp1);
        }

        array_set_strkey_arrval(&nw, "HeaderFields", nw2);
        array_set_strkey_arrval(&nw, "HeaderData", nw3);
    }

    return nw;
}

int
  http::dohtmuf(const char *prop)
{
    char
      buf[BUFFER_LEN];
    const char *
        m = NULL;
    struct frame *
        tmpfr;
    dbref
      ref,
        player;

    //if (*prop && (Prop_Hidden(prop) || Prop_Private(prop)))
    //    return 0;

    if ((ref = smart_prop_getref(this->rootobj, prop)) < 0)
        return 0;

    if (!OkObj(ref)) {
        this->senderror(403, "Invalid program dbref.");
        return 1;
    } else if (Typeof(ref) != TYPE_PROGRAM) {
        this->senderror(415, "Dbref not of type program.");
        return 1;
    } else if (!tp_web_allow_playerhtmuf && (this->flags & HS_PLAYER)) {
        this->senderror(403, "Player HTMuf programs are currently disabled.");
        return 1;
    }

    player = OWNER(ref);
    /* Insanity check! */
    if (!OkObj(player))
        return 0;

    /* Permissions checks. Player and program must      */
    /* be web_htmuf_mlvl+, and program must be LINK_OK. */
    if (MLevel(player) < tp_web_htmuf_mlvl || MLevel(ref) < tp_web_htmuf_mlvl) {
        this->senderror(403, "Permission denied. (MLevel of program or player too low.)");
        return 1;
    } else if (!(FLAGS(ref) & LINK_OK)) {
        this->senderror(403, "Permission denied. (Program not set LINK_OK.)");
        return 1;
    } else {
        if (!DBFETCH(ref)->sp.program.start) {
            struct line *
                tmpline = DBFETCH(ref)->sp.program.first;

            DBFETCH(ref)->sp.program.first = read_program(ref);
            do_compile(d->descriptor, player, ref, 0);
            free_prog_text(DBFETCH(ref)->sp.program.first);
            DBSTORE(ref, sp.program.first, tmpline);
        }

        if (!DBFETCH(ref)->sp.program.siz) {
            this->senderror(500, "Program not compilable.");
            return 1;
        }
    }

    /* Get the type. */
    if ((m = get_property_class(ref, "/_type")))
        strcpy(buf, m);
    else
        strcpy(buf, "text/html");

    this->flags |= HS_HTMUF;

    /* Send the header. */
    if (string_compare(buf, "noheader"))
        this->sendheader(200, buf, -1);

    /* Do it! */
    sprintf(match_args, "%d|%s|%s|%s", d->descriptor, d->hu->h->name, d->hu->u->user, this->cgidata.c_str());
    strcpy(match_cmdname, "(WWW)");
    tmpfr = interp(d->descriptor, player, NOTHING, ref, this->rootobj, BACKGROUND, STD_HARDUID, 0);
    if (tmpfr) {
        struct inst temp1;
        stk_array *nw = new_array_dictionary();

        this->fr = tmpfr;

        array_set_strkey_intval(&nw, "caller_pid", tmpfr->pid);
        array_set_strkey_intval(&nw, "descr", tmpfr->descr);
        array_set_strkey_refval(&nw, "caller_prog", ref);
        array_set_strkey_refval(&nw, "trigger", tmpfr->trig);
        array_set_strkey_refval(&nw, "prog_uid", player);
        array_set_strkey_refval(&nw, "player", player);
        array_set_strkey_arrval(&nw, "data", this->makearray());

        temp1.type = PROG_ARRAY;
        temp1.data.array = nw;
        muf_event_add(tmpfr, "USER.SOCKINFO", &temp1, 0);
        CLEAR(&temp1);

        interp_loop(player, ref, tmpfr, 1);
    }
    d->booted = 4;
    return 1;
}

int
  http::doproplist(dbref what, const char *prop, int statcode)
{
    char
      buf[BUFFER_LEN];
    const char *
        m = NULL;
    int
      lines = 0;
    int
      i;

    if (!OkObj(what))
        return 0;

    //if (*prop && (Prop_Hidden(prop) || Prop_Private(prop)))
    //    return 0;

    /* Get lines count. */
    sprintf(buf, "%s#", prop);
    if ((lines = get_property_value(what, buf))
        || (m = get_property_class(what, buf))) {
        if (number(m))
            lines = atoi(m);
    }

    /* If 0 or below, exit. */
    sprintf(buf, "%s#/%d", prop, lines);
    if (lines <= 0 || !get_property_class(what, buf))
        return 0;

    /* Get the type. */
    sprintf(buf, "%s/_type", prop);
    if ((m = get_property_class(what, buf)))
        strcpy(buf, m);
    else
        strcpy(buf, "text/html");

    this->flags |= HS_PROPLIST;
    /* Send the header. */
    if (string_compare(buf, "noheader"))
        this->sendheader(statcode, buf, -1);

    /* Send the list. */
    for (i = 1; i <= lines; i++) {
        sprintf(buf, "%s#/%d", prop, i);
        if ((m = get_property_class(what, buf))) {
            if (tp_web_allow_mpi && *m == '&')
                m = this->parsempi(what, ++m, buf);

            queue_text(d, "%s\r\n", m);
        }
    }

    d->booted = 4;
    return i;
}

int
  http::sendfile(const char *filename)
{
    const char *
    p, *
        type;
    long
      i;

    if (tp_web_max_files && (httpfcount + 1) > tp_web_max_files) {
        this->senderror(503, "Too many file transfers.");
        return 1;
    }

    if ((i = descr_sendfile(d, -1, -1, filename, -1)) < 0)
        return 0;

    if (tp_web_max_filesize && i > (tp_web_max_filesize * 1024L)) {
        this->senderror(500, "Requested file too large.");
        return 1;
    }

    if ((p = strrchr(filename, '.'))) {
        if (!(type = this->mimelookup(++p)))
            type = "application/octet-stream";
    } else {
        type = "application/octet-stream";
    }

    httpfcount++;

    /* This should send out proper file dates. */

    this->sendheader(200, type, i);
    descr_sendfileblock(d);

    return 1;
}

int
fileexists(const char *filename)
{
    FILE *
        fp;

    if ((fp = fopen(filename, "r")))
        fclose(fp);

    return ((fp != NULL));
}

void
  http::listdir(const char *dir, DIR * df)
{
    char
      buf2[BUFFER_LEN];
    char
      buf3[BUFFER_LEN];
    char
      buf[BUFFER_LEN];
    char
      url[BUFFER_LEN];
    struct dirent *
        dp;
    struct stat
      fs;
    char
      tbuf[60];
    DIR *
        dh;
    int
      i;

    if (!tp_web_allow_dirlist) {
        this->senderror(403, "Directory listings are currently disabled.");
        return;
    }

    this->sendheader(200, "text/html; charset=iso-8859-1", -1);
    queue_text(d,
               "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\r\n"
               "<html><head>\r\n<title>Index of /%s</title>\r\n</head><body>"
               "\r\n<h1>Index of /%s</h1>\r\n<pre>   Name                    " "       Last modified           Size<hr />\r\n", this->newdest.c_str(), this->newdest.c_str());

    while ((dp = readdir(df))) {
        if (*(dp->d_name) != '.') {
            strcpy(buf, dp->d_name);
            sprintf(buf2, "%s/%s", dir, dp->d_name);
            if ((dh = opendir(buf2))) {
                closedir(dh);
                strcat(buf, "/");
                strcpy(buf2, "-");
                strcpy(tbuf, "");
            } else {
                sprintf(buf2, "%s/%s%s", HTTP_DIR, this->newdest.c_str(), buf);
                if (!stat(buf2, &fs)) {
                    sprintf(buf2, "%d", (int) fs.st_size);
                    format_time(tbuf, 40, "%d-%b-%Y %H:%M\0", localtime(&fs.st_mtime));
                } else {
                    continue;
                }
            }

            sprintf(url, "/%s%s", this->newdest.c_str(), buf);
            escape_url(buf3, url);
            i = 30 - strlen(buf);
            queue_text(d, "   <a href=\"%s\">%s</a>%-*s %-24s %s\r\n", buf3, buf, i < 0 ? 0 : i, "", tbuf, buf2);
        }
    }

    queue_text(d, "<hr /></pre>\r\n<address>ProtoMUCK %s Server at %s" " Port %d</address>\r\n</body></html>\r\n", PROTOBASE, this->gethost(), d->cport);
}

int
  http::dofile(void)
{
    char
      buf[BUFFER_LEN];
    const char **
        m = http_defaulthomes;
    int
      i = 0;
    DIR *
        df;

    if (this->newdest.find(".."))
        return 0;

    if ((i = this->newdest.length()) > BUFFER_LEN)
        return 0;

    const char *
        nd = this->newdest.c_str();

    if (!*nd || (i && nd[i - 1] == '/')) {
        for (; *m; m++) {
            if (**m) {
                sprintf(buf, "%s/%s%s", HTTP_DIR, nd, *m);
                if ((i = this->sendfile(buf)))
                    return i;
            }
        }
    }

    sprintf(buf, "%s/%s", HTTP_DIR, nd);

    if ((df = opendir(buf))) {
        if (i && nd[i - 1] != '/') {
            sprintf(buf, "http://%s/%s/", this->gethost(), nd);
            this->sendredirect(buf);
        } else {
            this->listdir(buf, df);
            d->booted = 4;
        }

        closedir(df);
        return 1;
    }

    return (this->sendfile(buf));
}

int
  http::doprop(const char *prop)
{
    char
      buf[BUFFER_LEN];
    const char *
    m, *
        s;

    if (!OkObj(this->rootobj))
        return 0;

    //if (*prop && (Prop_Hidden(prop) || Prop_Private(prop)))
    //    return 0;

    /* Get the propery value. */
    if (!(m = get_property_class(this->rootobj, prop)))
        return 0;

    if (!*m)
        return 0;

    if (tp_web_allow_mpi && *m == '&') {
        sprintf(buf, "%s/_type", prop);
        if ((s = get_property_class(this->rootobj, buf)))
            strcpy(buf, m);
        else
            strcpy(buf, "text/html");

        /* Send the header. */
        if (string_compare(buf, "noheader"))
            this->sendheader(200, buf, -1);

        queue_text(d, this->parsempi(this->rootobj, ++m, buf));
    } else {
        this->flags |= HS_REDIRECT;
        this->sendredirect(m);
    }

    d->booted = 4;
    return 1;
}

int
  http::dourl(void)
{
    char
      prop[BUFFER_LEN];
    int
      i;

    if (!OkObj(this->rootobj)) {
        this->senderror(404, "Page not found.");
        return -1;
    }

    sprintf(prop, "%s/%s", this->rootdir, this->newdest.c_str());
    i = strlen(prop);
    while ((i-- > 0) && (prop[i] == '/'))
        prop[i] = '\0';

    if ((this->flags & HS_PROPLIST)
        && this->doproplist(this->rootobj, prop, 200))
        return 1;

    if (tp_web_allow_htmuf && (this->smethod->flags & HS_HTMUF)
        && this->dohtmuf(prop))
        return 1;

    if (tp_web_allow_mpi && (this->smethod->flags & HS_MPI)
        && this->doprop(prop))
        return 1;

    if (tp_web_allow_files && (this->smethod->flags & HS_FILE)
        && this->dofile())
        return 1;

    /* If it's a propdir and nothing else was found, */
    /* send a 403 error. Eventually, this'll be used */
    /* to display a directory listing.               */
    if (is_propdir(this->rootobj, prop)) {
        this->senderror(403, "Access denied.");
        return -1;
    }

    this->senderror(404, "Page not found.");
    return 0;
}

/* http_processcontent():                               */
/*   Process message body stuff. Called in interface.c. */
int
  http::processcontent(const char in)
{
    if (d->booted || !this->body.elen)
        return 1;

    this->body.data[this->body.len] = in;
    this->body.len++;

    /* Update that idletime! */
    d->last_time = time(NULL);

    /* Finished? */
    if (this->body.len >= this->body.elen) {
        this->body.data[this->body.len] = '\0';
        this->finish();
        return 1;
    }

    return 0;
}

/* http_process_input():                                */
/*   Process input stuff. Called in interface.c.        */
void
  http::process_input(const char *input, const size_t length)
{
    char buf[BUFFER_LEN];
    char *p, *q;
    int  i;

    std::string tmp(input, length);
    //this->log(5, "INPUT:   '%s' (%d)\n", strToHex(tmp).c_str(), length);
    this->log(5, "INPUT:   '%s' (%d)\n", tmp.c_str(), length);

    if (this->fr && !this->fr->pid) {
        fprintf(stderr, "HTTP_INPUT tried to access bad program frame!\n");
        this->log(1, "HTTP_INPUT tried to access bad program frame!\n");
    } else if (this->fr) {
        struct frame *
            destfr = this->fr;

        if (destfr) {
            struct inst
              temp1;

            temp1.type = PROG_STRING;
            temp1.data.string = alloc_prog_string_exact(input, length, -2);
            muf_event_add(destfr, "HTTP.INPUT", &temp1, 0);
            CLEAR(&temp1);
        }
    }

    if (this->body.elen || d->booted || d->type != CT_HTTP)
        return;                 /* It's not ours. Handle it elsewhere. */

    d->last_time = time(NULL);
    d->commands++;

    strcpy(buf, input);
    i = strlen(buf);
    while (i-- > 0 && buf[i] && (buf[i] == '\n' || buf[i] == '\r'))
        buf[i] = '\0';

    if (!buf[0]) {
        /* Empty string means bare newline. */
        if (!this->smethod)
            this->processheader();
        return;
    }

    if (!this->method.length()) {
        p = this->split(buf, ' ');
        q = NULL;
        if (p) {
            q = this->split(p, ' ');
            for (; *p && *p == '/' && *(p + 1) == '/'; p++) ;
        }
        this->method = buf;
        this->dest = (p ? p : "");
        this->ver = (q ? q : "");

        /* Strip all but one / from beginning of dest. */

        this->log(1, "WWW: %d %s '%s' %s(%s)\n", d->descriptor, this->method.c_str(), this->dest.c_str(), d->hu->h->name, d->hu->u->user);
        this->log(4, "VER:     '%s'\n", this->ver.c_str());
    } else {
        p = this->split(buf, ':');
        if (!p)
            return;

        while (*p && isspace(*p))
            p++;

        if (!buf[0] || !*p)
            return;

        //if (this->fields.count(buf)) {
        std::string & s = this->fields[buf];
        if (s.length()) {
            s += ", ";
            s += p;
        } else
            s = p;
        //} else
        //    this->fields[buf] = p;

        this->log(4, "HDR: %s: %s", buf, this->fields[buf].c_str());
    }

    return;
}

/* http_processheader():                                */
/*   Called from process_input() when a bare newline is */
/*   received. Processes all the header information.    */
void
  http::processheader(void)
{
    this->log(9, "HTTP: BEGIN processheader()\r\n");

    if (!OkObj(tp_www_root)) {
        /* Bad webroot @tune. */
        this->senderror(503, "Service unavailable. (Bad webroot @tune)");
        return;
    }

    if (!this->method.length() || !this->dest.length() || !this->ver.length()) {
        /* No method? Bad request. */
        this->senderror(400, "A malformed request was sent to the server.");
        return;
    }

    if (this->ver.compare("HTTP/1.1") && this->ver.compare("HTTP/1.0")) {
        /* No point wasting time if it's not the right version. */
        this->senderror(505, "Only HTTP/1.1 is supported at this time.");
        return;
    }

    if (!this->dest.compare("/ws")) {
        this->log(4, "BEGIN WEBSOCKET", this->dest.c_str());
        if (!this->fields.count("Upgrade") || this->fields["Upgrade"].compare("WebSocket")) {
            this->senderror(400, "A malformed request was sent to the server.");
            return;
        }

        if (!this->fields.count("Sec-WebSocket-Key")) {
            this->senderror(400, "Missing Sec-WebSocket-Key.");
            return;
        }
     
        this->begin_websocket();
        
        return;
    }

    this->log(4, "DEST: %s", this->dest.c_str());


    this->smethod = this->methodlookup(this->method);

    if (!this->smethod || !this->smethod->method) {
        /* No method? No handler? No service. */
        this->senderror(501, "Method not implemented or not supported.");
        return;
    }

    if ((this->smethod->flags & HS_BODY)
        && this->fields.count("Content-Length")) {
        /* Handle message-body stuff. */
        std::string & f = this->fields["Content-Length"];

        if (number(f.c_str())) {
            this->flags |= HS_BODY;
            /* Content-Length can be zero, which is perfectly legal. */
            /* If it is, just continue through to http_finish(). */
            if ((this->body.elen = atoi(f.c_str()))) {
                if (this->body.elen < 0) { /* It -can- be 0, but not below zero. */
                    this->senderror(400, "A malformed request was sent to the server.");
                } else if (tp_web_max_filesize && this->body.elen > (tp_web_max_filesize * 1024L)) {
                    this->senderror(413, "Message body too large.");
                } else if (!(this->body.data = new char[this->body.elen + 2])) {
                    this->senderror(413, "Not enough memory.");
                }

                /* Musn't continue onto http_finish(). */
                return;
            }
        } else {
            /* Content-Length wasn't a number, which is illegal. Error out. */
            this->senderror(400, "A malformed request was sent to the server.");
            return;
        }
    } else if (this->smethod->flags & HS_BODY) {
        /* No Content-Length field, and method is set to require one. */
        this->senderror(411, "Method requires a Content-Length field.");
        return;
    }

    this->log(6, "Header Block Complete");
    this->header_complete = 1;

    /* Certain checks fall through to here. */
    this->finish();
}

/* http_finish():                                       */
/*   Called to begin a websocket connection.            */
void http::begin_websocket(void)
{
    this->log(9, "HTTP: begin_websocket()\r\n");

    char
        tbuf[BUFFER_LEN];

    time_t t = time(NULL) + get_tz_offset();

    format_time(tbuf, BUFFER_LEN, "%a, %d %b %Y %T GMT", localtime(&t));

    queue_text(d, "HTTP/1.1 101 Switching Protocols\r\nDate: %s\r\nServer: ProtoMUCK/%s\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n", tbuf, PROTOBASE);

    string return_key = this->fields["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char tmphash[20];

    this->log(9, "HTTP: Sec-WebSocket-Key: %s", this->fields["Sec-WebSocket-Key"].c_str());

    SHA1hash((unsigned char *)tmphash, return_key.c_str(), return_key.length());
    return_key.assign(tmphash, 20);
    return_key = http_encode64(return_key);

    queue_text(d, "Sec-WebSocket-Accept: %s\r\n", return_key.c_str());
    this->log(9, "HTTP SEND: Sec-WebSocket-Accept: %s", return_key.c_str());

    queue_text(d, "\r\n");

    this->websocket = true;


}

void http::process_ws_input(const char* input, size_t length)
{
    this->ws_buffer.append(input, length);

    if (ws_buf_plen == 0 && ws_buffer.length() >= 1) {
        f_fin = (ws_buffer.at(0) & 0x80) == 0x80;    // Final Packet Flag
        f_reserved = (ws_buffer.at(0) & 0x70) >> 4;  // Reserved Bits
        f_opcode = ws_buffer.at(0) & 0x0F;           // Opcode

        ws_buffer.erase(0, 1);
        ws_buf_plen++;
    }

    if (ws_buf_plen == 1 && ws_buffer.length() >= 1) {
        f_masked = (ws_buffer.at(0) & 0x80) == 0x80;
        f_len = ws_buffer.at(0) & 0x7F;

        ws_buffer.erase(0, 1);
        ws_buf_plen++;
    }

    if (ws_buf_plen == 2 && f_len >= 126 && ws_buffer.length() >= (f_len == 126 ? 2 : 8)) {
        if (f_len == 126) {
            f_len = (uint64_t)((unsigned char)ws_buffer.at(0) << 8) | (unsigned char)ws_buffer.at(1);
            ws_buffer.erase(0, 2);
        }
        else if (f_len == 127) {
            f_len = (uint64_t)((unsigned char)ws_buffer.at(0) << 56) | ((unsigned char)ws_buffer.at(1) << 48) | ((unsigned char)ws_buffer.at(2) << 40) | ((unsigned char)ws_buffer.at(3) << 32) | ((unsigned char)ws_buffer.at(4) << 24) | ((unsigned char)ws_buffer.at(5) << 16) | ((unsigned char)ws_buffer.at(6) << 8) | (unsigned char)ws_buffer.at(7);
            ws_buffer.erase(0, 8);
        }

        ws_buf_plen++;
    }
    else if (ws_buf_plen == 2 && f_len < 126) {
        ws_buf_plen++;
    }

    if (ws_buf_plen == 3 && ws_buffer.length() >= 4) {
        f_mkey = ws_buffer.substr(0, 4);
        ws_buffer.erase(0, 4);
        ws_buf_plen++;
    }

    if (ws_buf_plen == 4 && ws_buffer.length() >= f_len) {
        f_payload = ws_buffer.substr(0, f_len);
        ws_buffer.erase(0, f_len);

        process_ws_frame(f_payload);
        f_len = 0;
        ws_buf_plen = 0;
    }
}

void http::process_ws_frame(const std::string& payload)
{
    string tmp;

    tmp.reserve(payload.length());
    for (size_t i = 0; i < payload.length(); i++)
        tmp.push_back(payload.at(i) ^ f_mkey.at(i % 4));
    f_payload = tmp;
    
    //this->log(9, "WS_IN: RAW: '%s' (%d)\n", strToHex(raw).c_str(), raw.length());

    this->log(9, "WS_IN: f_fin: %s\n", (f_fin ? "true" : "false"));
    this->log(9, "WS_IN: f_opcode: %u\n", f_opcode);
    this->log(9, "WS_IN: f_masked: %s\n", (f_masked ? "true" : "false"));
    this->log(9, "WS_IN: f_len: %llu\n", (unsigned long long int)f_len);
    this->log(9, "WS_IN: f_mkey: '%s' (%d)\n", strToHex(f_mkey).c_str(), f_mkey.length());

    this->log(9, "WS_IN: f_payload: '%s' (%d)\n", f_payload.c_str(), f_payload.length());

    if (f_opcode == 1) {
        try {
            json j = json::parse(f_payload);
            string cmd = j["muck"]["command"].get<std::string>();

            save_command(d, cmd.c_str(), cmd.length(), -2);
        } catch (std::exception & e) {
            this->log(2, "process_ws_frame(): JSON Exception: %s\n", e.what());
            save_command(d, f_payload.c_str(), f_payload.length(), -2);
        }

    }
        
    //send_ws_frame(f_payload);
}

int
http::ws_process_output(void)
{
    std::string payload;

    while (ws_q) {
        struct ws_queue* qa = ws_q;

        payload += qa->text;
        ws_q = qa->next;

        delete qa;
    }

    if (!ws_q)
        ws_q_tail = NULL;

    json j = {
        {"muck", { {"text", ascii_to_utf8(payload)} } }
    };

    d->http->send_ws_frame(j.dump());

    return 0;
}

void http::ws_add_to_queue(const std::string& in, dbref orig = -1, std::string tag = "SYS")
{
    struct ws_queue* q = new struct ws_queue;
    
    q->next = NULL;
    q->text = in;
    q->orig = -1;
    q->tag = "SYS";

    if (!ws_q)
        ws_q = q;

    if (ws_q_tail)
        ws_q_tail->next = q;
       
    ws_q_tail = q;
}

void http::send_ws_frame(const std::string& payload)
{
    std::string out;

    out.reserve(payload.length() + 4);
    out.push_back((unsigned char)129);

    if (payload.length() < 126) {
        out.push_back(payload.length());
    } else if (payload.length() <= 65535) {
        out.push_back(126);
        out.push_back((payload.length() >> 8) & 0xFF);
        out.push_back((payload.length() >> 0) & 0xFF);
    } else {
        out.push_back(127);
        for (char x = 0; x < 8; x++) {
            out.push_back((payload.length() >> ((7 - x) * 8)) & 0xFF);
        }
    }

    out += payload;

    add_to_queue(&d->output, out.c_str(), out.length(), -2);
    d->output_size += out.length();
}

/* http_finish():                                       */
/*   Called from various other functions. It handles    */
/*   actually doing stuff, like calling method handlers.*/
void
  http::finish(void)
{
    if (this->body.len && this->body.len < MAX_COMMAND_LEN && this->body.data)
        this->log(4, "BODY:    '%s' (%d)\n", this->body.data, this->body.len);

    if (this->parsedest())
        return;

    if (this->smethod->method)
        this->dourl();

    return;
}

void
  http::disconnect(void)
{
    //struct frame *fr = NULL;
    char buf[1024];

    this->log(3, "WWW %d Disconnected\n", d->descriptor);

    if (this->fr && !this->fr->pid) {
        fprintf(stderr, "HTTP_DISCONNECT tried to access bad program frame!\n");
        this->log(1, "HTTP_DISCONNECT tried to access bad program frame!\n");
        return;
    }

    sprintf(buf, "HTTP.DISCONNECT.%d", d->descriptor);

    if (this->fr) {
        struct inst temp;

        this->log(3, "HTTP DBUG: Sending HTTP.DISCONNECT.%d to PID %d (frame %p)\n", d->descriptor, this->fr->pid, (void *) this->fr);

        temp.type = PROG_INTEGER;
        temp.data.number = (int) time(NULL);
        muf_event_add(this->fr, buf, &temp, 0);
        CLEAR(&temp);
    }
}

#endif /* NEWHTTPD */

#define CHAR64(c) (((unsigned char)c) > 127 ? -1 : index_64[(unsigned char)(c)])

static char
  basis_64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char
  index_64[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1
};

std::string http_encode64(const std::string & in)
{
    size_t inlen = in.size();
    size_t extra = inlen % 3;

    inlen -= extra;

    std::stringstream out;

    size_t p;
    unsigned char c[3];

    for (p = 0; p < inlen; p += 3) {
        std::string tmp = in.substr(p, 3);
        for (size_t i = 0; i < 3; i++)
            c[i] = static_cast < unsigned char >(tmp[i]);

        out << basis_64[c[0] >> 2];
        out << basis_64[((c[0] << 4) & 0x30) | (c[1] >> 4)];
        out << basis_64[((c[1] << 2) & 0x3C) | (c[2] >> 6)];
        out << basis_64[c[2] & 0x3F];
    }

    std::string readout = out.str();

    if (extra) {
        std::string tmp = in.substr(p, extra);
        for (size_t i = 0; i < extra; i++)
            c[i] = static_cast < unsigned char >(tmp[i]);

        out << basis_64[c[0] >> 2];
        switch (extra) {
            case 1:
                out << basis_64[((c[0] << 4) & 0x30)];
                out << "=";
                break;
            case 2:
                out << basis_64[((c[0] << 4) & 0x30) | (c[1] >> 4)];
                out << basis_64[((c[1] << 2) & 0x3C)];
                break;
        }
        out << "=";
    }
    return out.str();
}

std::string http_decode64(const std::string & in)
{
    size_t inlen = in.size();

    // This check used to be roughly here but according to spec it should not be:  if (in[0] == '+' && in[1] == ' ') in += 2;
    if (!inlen)
        return "";
    if (inlen < 4 || inlen % 4)
        throw std::runtime_error("Invalid string length.");

    inlen -= 4;

    std::stringstream out;

    int c[4];
    size_t p;

    for (p = 0; p < inlen; p += 4) {
        std::string tmp = in.substr(p, 4);
        for (size_t i = 0; i < 4; i++) {
            c[i] = CHAR64(tmp[i]);
            if (c[i] < 0) {
                std::stringstream err;
                err << "Invalid base64 digit ";
                if (tmp[i] < 0x20 || tmp[i] > 0x7E)
                    err << (int) tmp[i];
                else
                    err << "'" << tmp[i] << "'";
                err << " at position " << (p + i) << ".";
                throw std::runtime_error(err.str());
            }
        }
        out << static_cast < char >(((unsigned char) c[0] << 2) | ((unsigned char) c[1] >> 4));
        out << static_cast < char >((((unsigned char) c[1] << 4) & 0xF0) | ((unsigned char) c[2] >> 2));
        out << static_cast < char >((((unsigned char) c[2] << 6) & 0xC0) | (unsigned char)
                                    c[3]);
    }
    std::string tmp = in.substr(p, 4);
    for (size_t i = 0; i < 4; i++)
        c[i] = CHAR64(tmp[i]);

    int errpos = -1;

    // Check for Error Characters
    for (size_t i = 0; i < 4; i++) {
        if (c[i] < 0 && c[i] != -2) {
            errpos = i;
            break;
        }
    }
    // Check for Incorrect Equals Positions
    if (errpos == -1) {
        for (size_t i = 0; i < 2; i++)
            if (c[i] == -2) {
                errpos = i;
                break;
            }
    }
    // Check for Invalid Non-Equals After Equals
    if (errpos == -1 && c[2] == -2 && c[3] != -2)
        errpos = 3;

    // Report Errors
    if (errpos > -1) {
        char c = tmp[errpos];

        std::stringstream err;
        err << "Invalid base64 digit ";
        if (c < 0x20 || c > 0x7E) {
            err << static_cast < int >(c);
        } else {
            err << "'" << c << "'";
        }
        err << " at position " << (p + errpos) << ".";
        throw std::runtime_error(err.str());
    }

    out << (char) ((c[0] << 2) | (c[1] >> 4));
    if (c[2] != -2)
        out << (char) (((c[1] << 4) & 0xF0) | (c[2] >> 2));
    if (c[3] != -2)
        out << (char) (((c[2] << 6) & 0xC0) | c[3]);
    return out.str();
}
