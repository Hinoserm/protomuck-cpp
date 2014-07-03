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

extern int httpucount;
extern int httpfcount;

extern int  array_set_strkey_arrval(stk_array **arr, const char *key, stk_array *arr2);
extern int  queue_text(struct descriptor_data *d, char *format, ...);

//struct descriptor_data;

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
        /* Functions */
        void log(int debuglvl, char *format, ...);
        void process_input(const char *input);
        void disconnect(void);
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
extern int http_decode64(const char *in, unsigned inlen, char *out);
extern int http_encode64(const char *_in, unsigned inlen, char *_out);
#endif
