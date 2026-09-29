#ifndef __INCL_NEWHTTP_H
#define __INCL_NEWHTTP_H

#if defined(DESCRFILE_SUPPORT) || defined(NEWHTTPD)

struct dfile_struct {       /* hinoserm */  /************************************/
    FILE                    *fp;            /* File handle for file transfers.  */
    size_t                   size;          /* File size for file transfers.    */
    size_t                   sent;          /* File amount sent for file trans. */
    int                      pid;           /* Pid of process that sent the file*/
};                          /* hinoserm */  /************************************/

#endif /* DESCRFILE_SUPPORT */

#ifdef NEWHTTPD

#include <format>
#include <string>
#include <utility>

extern int tp_web_logfile_lvl;  /* the two levels http::log tests */
extern int tp_web_logwall_lvl;

extern int httpucount;
extern int httpfcount;

extern int  array_set_strkey_arrval(stk_array **arr, const char *key, stk_array *arr2);
extern int  queue_write(struct descriptor_data *d, const char *b, int n);

/* Queue formatted text to a descriptor, std::format syntax. The format
 * must be a compile-time string, which is the point: the vsprintf
 * version this replaces was handed built text as its format (MPI page
 * output, and error pages carrying the client's own Host header), so a
 * web client could send "%s%n" and run a format-string attack on the
 * server. Text that is data goes through queue_write, never here. */
template <typename... Args>
int
queue_text(struct descriptor_data *d, std::format_string<Args...> fmt,
           Args &&...args)
{
    std::string s = std::format(fmt, std::forward<Args>(args)...);

    return queue_write(d, s.data(), (int) s.size());
}

//struct descriptor_data;

struct ws_queue {
    std::string text;
    dbref orig;
    std::string tag;

    struct ws_queue* next;
};

struct http_method {
    const char *method;
    int         flags;
    //void       (*handler)(struct descriptor_data *d);
};

struct http_statstruct {
    int         code;
    const char *msg;
};

struct http_mimestruct {
    const char *ext;
    const char *type;
};

/*- End hinoserm new code -*/
class http {
    private:
        /* Variables */                    /************************************/
        struct descriptor_data *d;         /* Descriptor pointer.              */
        int flags;                         /* Various flags.                   */
        struct http_method *smethod;       /* The method, in struct form.      */
        char *rootdir;                     /* The propdir the data is in.      */
        dbref rootobj;                     /* The root object dbref number.    */
    
        /* Functions */
        char *split(char *s, int c);
        void sendheader(int statcode, const char *content_type, int content_length);
        void sendredirect(const char *url);
        void senderror(int statcode, const char *msg);
        int dohtmuf(const char *prop);
        int doproplist(dbref what, const char *prop, int statcode);
        int dofile(void);
        int dourl(void);
        void processheader(void);
        /* adopt X-Forwarded-For from a web_trusted_proxies peer; false
         * if the real client turns out to be a blocked site */
        bool apply_forwarded_for(void);
        void begin_websocket(void);
        void finish(void);
        void handler_get(void);
        void handler_head(void);
        void handler_post(void);
        const char *statlookup(int status);
        struct http_method *methodlookup(const std::string &method);
        int parsedest(void);
        stk_array *formarray(const char *data);
        const char *gethost(void);
        char *parsempi(dbref what, const char *yerf, char *buf);
        void listdir(const char *dir, DIR * df);
        int doprop(const char *prop);

        string ws_buffer;
        size_t ws_buf_plen = 0;
        bool f_fin = 0;
        unsigned char f_reserved = 0;
        unsigned char f_opcode = 0;
        bool f_masked = false;
        uint64_t f_len = 0;
        string f_mkey;
        string f_payload;
    public:
        /* Constructor/Destructor */
        http(struct descriptor_data *d);
       ~http(void);

        /* Variables */                    /************************************/
        std::map <string, string> fields;  /* Header fields.                   */
        string cgidata;                    /* Stuff after the '?' in the URI.  */
        string newdest;                    /* The URI after parsing.           */
        string method;                     /* The method, in string form.      */
        string dest;                       /* The destination URI.             */
        string ver;                        /* The HTTP version.                */
        bool header_complete;              /* Has the header been received?    */
        struct {                           /************************************/
            char *data;                    /* Pointer for message body data.   */
            int elen;                      /* Expected length of body data.    */
            int len;                       /* Current length of body data.     */
            int curr;                      /* Current char. Used by prims.     */
        } body;                            /* Body struct.                     */
        struct frame *fr;                  /* HTMuf active program frame.      */
                                           /************************************/
        bool websocket = false;
        struct ws_queue* ws_q;        /* Websocket output queue           */
        struct ws_queue* ws_q_tail;
        size_t ws_q_bytes = 0;        /* bytes waiting in ws_q            */

        /* Keepalive and activity. Liveness (any frame at all, pongs
         * included) and activity (a typed line) are separate on
         * purpose: a ping exchange proves the connection is alive but
         * says nothing about the player, so it must never unidle
         * them. */
        time_t ws_last_rx = 0;        /* last complete frame received     */
        time_t ws_last_ping = 0;      /* last keepalive ping sent         */
        bool ws_typed = false;        /* a typed line since last asked    */

        /* true once per typed line; the main loop's unidle handling
         * runs for a websocket only when this says so */
        bool take_typed_activity(void)
        {
            bool t = ws_typed;

            ws_typed = false;
            return t;
        }

        /* ping on schedule; drop a connection silent for 3 intervals */
        void ws_keepalive(time_t now);

        /* when ws_keepalive next has something to do (a ping due, or
         * the silence deadline), so the main loop's select can wake
         * for it instead of sleeping past it */
        time_t ws_next_due(void);

        /* RFC 6455 closing handshake. ws_close sends our Close frame
         * (status code, then an optional reason) exactly once; after
         * it nothing else is sent, and the peer's Close in reply
         * completes the handshake. ws_fail is for protocol violations:
         * it closes with the given code and drops the connection. */
        bool ws_close_sent = false;
        void ws_close(unsigned short code, const std::string &reason = "");
        void ws_fail(unsigned short code, const std::string &why);

        /* a fragmented message being reassembled: its opcode (1 text,
         * 2 binary; 0 when none is in progress) and the text so far */
        unsigned char ws_msg_op = 0;
        std::string ws_msg;

        /* one complete text message, reassembled if it was fragmented */
        void process_ws_message(const std::string &text);

       
        /* Functions */

        /* Web log, std::format syntax. The level test comes first so a
         * disabled level costs nothing, not even formatting: several
         * callers pass whole websocket payloads. */
        template <typename... Args>
        void log(int debuglvl, std::format_string<Args...> fmt,
                 Args &&...args)
        {
            if (debuglvl > tp_web_logwall_lvl && debuglvl > tp_web_logfile_lvl)
                return;
            logText(debuglvl, std::format(fmt, std::forward<Args>(args)...));
        }
        void logText(int debuglvl, const std::string &text);
        void process_input(const char *input, const size_t length);
        void process_ws_input(const char* input, size_t length);
        void process_ws_frame(const std::string& payload);
        int ws_process_output(void);
        void ws_add_to_queue(const std::string& in, dbref orig, std::string tag);
        /* opcode 1 is text, the only thing the output path sends; the
         * frame handler passes 8 (close) and 10 (pong) */
        void send_ws_frame(const std::string& payload,
                           unsigned char opcode = 1);
        void disconnect(void);

        /* --- websocket sideband (docs/WEBSOCKET.txt) --- */

        /* 1 to 64 characters of [A-Za-z0-9._-]. Names starting with
         * '.' or '_' are refused unless reserved is set: '_' names
         * belong to the server (_error), and '.' props are hidden. */
        static bool sideband_name_ok(const std::string &name,
                                     bool reserved = false);

        /* Queue one outbound sideband packet in order with text. On
         * refusal returns false and sets *err (7-bit, user-readable):
         * the packet would exceed json_max_len, or this connection's
         * unsent output is already over max_output + json_max_len. */
        bool queue_sideband_out(const std::string &cmd, const json &data,
                                dbref orig, std::string *err);

        /* Run the program registered for an inbound packet that has
         * waited its turn in the input queue ("<cmd>\0<data JSON>"). */
        void dispatch_sideband(const char *buf, int len);

        /* Tell the client its packet was not run. */
        void sideband_error(const std::string &cmd, const std::string &why);

        int processcontent(const char in);
        int sendfile(const char *filename);
        stk_array *makearray(void);
        const char *mimelookup(const char *ext) const;
};

/* The logfile. */
#define HTTP_LOG "logs/webserver"
#define HTTP_DIR "files/public_html"

/* The following flags are used in the http_methods[] table to tell */
/* which options the method supports, and is also used in the main  */
/* struct to tell what kind of page was found.                      */
#define HS_PROPLIST     0x1   /* Method supports proplists.         */
#define HS_REDIRECT     0x2   /* Method supports redirection.       */
#define HS_PLAYER       0x4   /* Method supports player webpages.   */
#define HS_HTMUF        0x8   /* Method supports HTMuf programs.    */
#define HS_VHOST       0x10   /* Method supports virtual hosts.     */
#define HS_FILE        0x20   /* Method supports server-side files. */
#define HS_BODY        0x40   /* Method requires a message body.    */
#define HS_MPI         0x80   /* Method supports MPI programs.      */
#define HS_HEADONLY   0x100   /* Method does not provide a body.    */

#endif /* NEWHTTPD */

/* For some reason, lots of things use these. */
extern std::string http_encode64(const std::string &in);
extern std::string http_decode64(const std::string &in);
#endif
