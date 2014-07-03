/* This file contains the helper functions for the MySQL support */
/* As well as the actual prims themselves */


#include "copyright.h"
#include "config.h"
#include "params.h"
#ifdef SQL_SUPPORT
#include <mysql/mysql.h>
#include <mysql/mysql_version.h>
#include "db.h"
#include "tune.h"
#include "props.h"
#include "inst.h"
#include "externs.h"
#include "match.h"
#include "interface.h"
#include "strings.h"
#include "interp.h"
#include "p_mysql.h"
#include "mufevent.h"

#ifdef THREADED_SQL_SUPPORT

struct mysql_frames *mysql_frlist = NULL;
pthread_mutex_t mysql_frlist_mutex = PTHREAD_MUTEX_INITIALIZER;

void
mysql_register_frame(struct frame *fr, int id)
{
    struct mysql_frames *mfr = new mysql_frames;
    mfr->fr = fr;
	mfr->id = id;
	mfr->prev = NULL;
	mfr->next = NULL;

	pthread_mutex_lock(&mysql_frlist_mutex);
   	if (mysql_frlist)
		mysql_frlist->prev = mfr;
	mfr->next = mysql_frlist;
	mysql_frlist = mfr;
	pthread_mutex_unlock(&mysql_frlist_mutex);
}

void
mysql_unregister_frame(struct frame *fr)
{
	struct mysql_frames *ptr, *tmp;

	pthread_mutex_lock(&mysql_frlist_mutex);
    ptr = mysql_frlist;
    while (ptr) {
		tmp = ptr->next;
		if (ptr->fr == fr) {
			if (ptr->next)
				ptr->next->prev = ptr->prev;
			if (ptr->prev)
				ptr->prev->next = ptr->next;
			if (ptr == mysql_frlist)
				mysql_frlist = ptr->next;
			delete ptr;
        }
        ptr = tmp;
    }

	pthread_mutex_unlock(&mysql_frlist_mutex);
}

struct frame *
mysql_locate_frame(int id)
{
	struct mysql_frames *ptr;

	pthread_mutex_lock(&mysql_frlist_mutex);
    ptr = mysql_frlist;
    while (ptr) {
		if (ptr->id == id) {
			if (ptr->next)
				ptr->next->prev = ptr->prev;
			if (ptr->prev)
				ptr->prev->next = ptr->next;
			if (ptr == mysql_frlist)
				mysql_frlist = ptr->next;
			break;
        }
        ptr = ptr->next;
    }

	pthread_mutex_unlock(&mysql_frlist_mutex);

	if (ptr) {
		struct frame *fr = ptr->fr;
		delete ptr;
		return fr;
	} else
		return NULL;
}

struct mysql_conn *mysql_cpool = NULL;
pthread_mutex_t mysql_cpool_mutex = PTHREAD_MUTEX_INITIALIZER;

void
mysql_cpool_free(struct mysql_conn *ptr)
{
	if (ptr->mysql)
	   mysql_close(ptr->mysql);
    
	//delete ptr->mysql;
	delete[] ptr->hostname;
	delete[] ptr->username;
	delete[] ptr->password;
	delete[] ptr->database;
	delete ptr;
}

void
mysql_cpool_remember(struct mysql_conn *ptr)
{
	pthread_mutex_lock(&mysql_cpool_mutex);
   	if (mysql_cpool)
		mysql_cpool->prev = ptr;
	ptr->next = mysql_cpool;
	mysql_cpool = ptr;
	pthread_mutex_unlock(&mysql_cpool_mutex);
}

struct mysql_conn *
mysql_cpool_get(const char *hostname, const char *username, const char *password, const char *database, unsigned int timeout)
{
	struct mysql_conn *ptr;

	//log_status("MYSQL_CPOOL_GET(%s, %s, %s, %i): Start\r\n", hostname, username, database, timeout); 

	pthread_mutex_lock(&mysql_cpool_mutex);

    ptr = mysql_cpool;
    while (ptr) {
		if (!strcmp(ptr->hostname, hostname) && !strcmp(ptr->username, username) && !strcmp(ptr->password, password) && !strcmp(ptr->database, database) ) {
			if (ptr->next)
				ptr->next->prev = ptr->prev;
			if (ptr->prev)
				ptr->prev->next = ptr->next;
			if (ptr == mysql_cpool)
				mysql_cpool = ptr->next;
			break;
        }
        ptr = ptr->next;
    }

    pthread_mutex_unlock(&mysql_cpool_mutex);

	if (ptr && mysql_ping(ptr->mysql)) {
		//log_status("MYSQL_CPOOL_GET(%s, %s, %s, %i): Stale, starting over\r\n", hostname, username, database, timeout); 
		mysql_cpool_free(ptr);
		ptr = mysql_cpool_get(hostname, username, password, database, timeout); /* Maybe there's a good one still in the pool. */
	}
		
    if (!ptr) {
		MYSQL *result;
		my_bool sqlreconnect = 1;
		unsigned int *timeoutPtr = &timeout;

		//log_status("MYSQL_CPOOL_GET(%s, %s, %s, %i): New\r\n", hostname, username, database, timeout); 

		ptr = new mysql_conn; 

		if (!(ptr->mysql = mysql_init(NULL))) {
			delete ptr;
			return NULL;
		}

		/* set the timeout and try to connect */
		mysql_options(ptr->mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *) timeoutPtr);
		mysql_options(ptr->mysql, MYSQL_OPT_RECONNECT, &sqlreconnect);
		result = mysql_real_connect(ptr->mysql, hostname, username, password, database, 0, NULL, 0);

		if (!result) {/* connection failed. Push error int and mesg */
			//strcpy(errbuf, mysql_error(tempsql));
			//errNum = mysql_errno(tempsql);

       		//log_status("MYSQL_CPOOL_GET(%s, %s, %s, %i): Error %s\r\n", hostname, username, database, timeout, mysql_error(ptr->mysql)); 
            mysql_close(ptr->mysql);
			//delete ptr->mysql;
			delete ptr;
			return NULL;
		}

		ptr->hostname = alloc_string(hostname);
		ptr->username = alloc_string(username);
		ptr->password = alloc_string(password);
		ptr->database = alloc_string(database);
    }

	ptr->lastused = current_systime;
	ptr->next = NULL;
	ptr->prev = NULL;

	//log_status("MYSQL_CPOOL_GET(%s, %s, %s, %i): Done\r\n", hostname, username, database, timeout); 

	return ptr;
}

int mysql_thread_ids = 1;
extern int thread_fd;


struct tsql_data {
    struct mysql_conn *myconn;
	char *query;
	int pid;
	int id;
};

#ifdef WIN_VC
DWORD WINAPI mysql_query_thread(void *ptr)
#else
void *mysql_query_thread(void *ptr)
#endif
{
	struct tsql_data *tr = (struct tsql_data *)ptr;
    //struct mysql_conn *myc = tr->myconn;
	stk_array *arr, *arr2;
	struct inst temp1, temp2;
	struct frame *fr;
	MYSQL_RES *res = NULL;
    MYSQL_ROW row = NULL;
    char tmp[BUFFER_LEN];
    MYSQL_FIELD *fields = NULL;
    int num_rows, num_fields, counter, all_rows;
    unsigned long i = 0;
    stk_array *nw;
    stk_array *fieldsList;

    if (mysql_query(tr->myconn->mysql, tr->query)) { /* Query failed */
        errno = -1;
		temp1.type = PROG_STRING;
		temp1.data.string = alloc_prog_string(mysql_error(tr->myconn->mysql));
    } else {
        arr = new_array_packed(0, 0);
		res = mysql_store_result(tr->myconn->mysql);
		num_rows = 0;
		if (res) {                  /* there IS a result */
			all_rows = mysql_num_rows(res);
			num_rows = all_rows > tp_mysql_result_limit ? tp_mysql_result_limit : all_rows;
			num_fields = mysql_num_fields(res);
			fields = mysql_fetch_fields(res);
			counter = 0;
			fieldsList = new_array_packed(num_fields, 0);
			for (i = 0; i < num_fields; ++i)
				array_set_intkey_strval(&fieldsList, i, fields[i].name);
			while ((row = mysql_fetch_row(res))) { /* fetch all rows, push limit */
				nw = new_array_dictionary();
				if (counter++ < num_rows) {
					for (i = 0; i < num_fields; ++i) {
						/* Alynna: To make this safe, we must normalize the string to
									16383 characters and a terminating \0 */
									/* Why? -hinoserm april 17 2014 */
						if (row[i]) {                        
							strncpy(tmp, row[i], 16380);
							tmp[16380]='\0';
							array_set_strkey_strval(&nw, fields[i].name, tmp);
						}
					}
					temp2.type = PROG_ARRAY;
					temp2.data.array = nw;

					array_appenditem(&arr, &temp2);
                    CLEAR(&temp2);
				} else
					break;          /* The limit has been reached, exit the while loop */
			}
			/* delete tmp; */
			mysql_free_result(res);
		} else {                    /* no result */
			num_rows = 0;
			fieldsList = new_array_packed(0,0);
		}
		arr2 = new_array_dictionary();
		array_set_strkey_arrval(&arr2, "results", arr);
		array_set_strkey_arrval(&arr2, "fields", fieldsList);

		temp1.type = PROG_ARRAY;
		temp1.data.array = arr2;
	}

	fr = mysql_locate_frame(tr->id);
	if (fr) {
		char buf[500];
		sprintf(buf, "MYSQL.%i", tr->id);
		muf_event_add(fr, buf, &temp1, 0);
	}
        CLEAR(&temp1);
	
	mysql_cpool_remember(tr->myconn);
	uint64_t tid = tr->id;
	write(thread_fd, &tid, sizeof(uint64_t));
	delete[] tr->query;
    delete tr;
#ifdef WIN_VC
# ifndef __cplusplus
	/* Supposedly, when using C++ and WINAPI, you just return from the thread function. */
	ExitThread(0);
# endif
#else
    pthread_exit(NULL);
#endif
	return NULL;
}

/* Prim for sending queries to a SQL connection using threads and MUF events.*/
void
prim_sqlquery_t(PRIM_PROTOTYPE)
{
	struct mysql_conn *myconn;
	struct tsql_data *tr;
	int tid;
#ifndef WIN_VC
    int result;
    pthread_t thread1;
    pthread_attr_t attr;
#endif

    if (oper[0].type != PROG_STRING)
        abort_interp("String argument expected for query. (2)");
    if (!oper[0].data.string)
        abort_interp("The query cannot be an empty string. (2)");
    if (oper[1].type != PROG_MYSQL)
        abort_interp("MySQL connection expected. (1)");
    //if (!oper[1].data.mysql->connected)
    //    abort_interp("This MySQL connection is closed.");

    myconn = mysql_cpool_get(oper[1].data.mysql->hostname, oper[1].data.mysql->username, oper[1].data.mysql->password, oper[1].data.mysql->database, oper[1].data.mysql->timeout);
	if (!myconn) {
		abort_interp("MySQL connection error.");
	}
    tr = new tsql_data;
	tr->myconn = myconn;
	tr->query = alloc_string(oper[0].data.string->data);
	tr->id = tid = mysql_thread_ids++;
    mysql_register_frame(fr, tid);
	tr->pid = fr->pid;

#ifdef WIN_VC
    CreateThread(NULL, 0, mysql_query_thread, (void*)tr, 0, NULL);
#else
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);   
    result = pthread_create(&thread1, &attr, mysql_query_thread, (void*) tr);
    pthread_attr_destroy(&attr);
#endif

	if (result)
		abort_interp("Unable to create thread");

	PushInt(tid);
    return;
}

#else /* THREADED_SQL_SUPPORT */
void
prim_sqlquery_t(PRIM_PROTOTYPE)
{
	abort_interp("Not supported");
}

#endif /* THREADED_SQL_SUPPORT */


/* Prim for connecting to a SQL database */
void
prim_sqlconnect(PRIM_PROTOTYPE)
{
    char password[BUFFER_LEN];
    char hostname[BUFFER_LEN];
    char username[BUFFER_LEN];
    char database[BUFFER_LEN];
	my_bool sqlreconnect = 1;
    struct inst *newsql;
    unsigned int timeout, notConnected, *timeoutPtr;
    MYSQL *result, *tempsql;

    if (oper[0].type != PROG_INTEGER)
        abort_interp("Timeout must be an integer.");
    if (oper[0].data.number < 1)
        oper[0].data.number = 1; /* avoiding 0 or negative timeouts */
    if (oper[4].type != PROG_STRING || oper[3].type != PROG_STRING ||
        oper[2].type != PROG_STRING || oper[1].type != PROG_STRING)
        abort_interp("Login arguments must be strings.");
    if (!oper[3].data.string)
        abort_interp("Username cannot be an empty string.");
    /* copy data over */
    if (oper[4].data.string)     /* null host is assumed to be localhost */
        strcpy(hostname, oper[4].data.string->data);
    else
        strcpy(hostname, "");
    strcpy(username, oper[3].data.string->data);
    if (oper[2].data.string)     /* null passwords are possible. */
        strcpy(password, oper[2].data.string->data);
    else
        strcpy(password, "");
    if (oper[1].data.string)     /* null databases are possible. */
        strcpy(database, oper[1].data.string->data);
    else
        strcpy(database, "");
    timeout = oper[0].data.number;
    timeoutPtr = &timeout;      /* mysql_options wants a pointer to an int */

    /* Making this error abort because it's a rare critical error 
     * that the user can't do anything about most of the time. */
    if (!(tempsql = mysql_init(NULL)))
        abort_interp("Unable to initialize MySQL connection.");

    /* set the timeout and try to connect */
    mysql_options(tempsql, MYSQL_OPT_CONNECT_TIMEOUT, (char *) timeoutPtr);
	mysql_options(tempsql, MYSQL_OPT_RECONNECT, &sqlreconnect);
    result =
        mysql_real_connect(tempsql, hostname, username, password, database, 0,
                           NULL, 0);

    if (result) {               /* connection successful */
        notConnected = 0;
        newsql = new inst;
        newsql->type = PROG_MYSQL;
        newsql->data.mysql = new muf_sql;
        newsql->data.mysql->mysql_conn = tempsql;
        newsql->data.mysql->connected = 1;
        newsql->data.mysql->timeout = oper[0].data.number;
        newsql->data.mysql->links = 1; /* 1 instance so far */
		newsql->data.mysql->hostname = alloc_string(hostname);
		newsql->data.mysql->username = alloc_string(username);
		newsql->data.mysql->password = alloc_string(password);
		newsql->data.mysql->database = alloc_string(database);
        copyinst(newsql, &arg[(*top)++]);
        CLEAR(newsql);
        PushInt(notConnected);  /* Pushing a 0 for success, unlike normal */
    } else {                    /* connection failed. Push error int and mesg */
        int errNum;
        char errbuf[BUFFER_LEN];

        strcpy(errbuf, mysql_error(tempsql));
        errNum = mysql_errno(tempsql);
        PushString(errbuf);
        PushInt(errNum);
        delete tempsql;
    }
}

/* Prim for sending queries to a SQL connection */
void
prim_sqlquery(PRIM_PROTOTYPE)
{
    MYSQL *tempsql = NULL;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row = NULL;
    char tmp[BUFFER_LEN];
    MYSQL_FIELD *fields = NULL;
    int num_rows, num_fields, counter, all_rows;
    char query[BUFFER_LEN];
    unsigned long i = 0;
    stk_array *nw;
    stk_array *fieldsList;
    char errbuf[BUFFER_LEN];

    if (oper[0].type != PROG_STRING)
        abort_interp("String argument expected for query. (2)");
    if (!oper[0].data.string)
        abort_interp("The query cannot be an empty string. (2)");
    if (oper[1].type != PROG_MYSQL)
        abort_interp("MySQL connection expected. (1)");
    if (!oper[1].data.mysql->connected)
        abort_interp("This MySQL connection is closed.");

    strcpy(query, oper[0].data.string->data);
    tempsql = oper[1].data.mysql->mysql_conn;
    if (mysql_query(tempsql, query)) { /* Query failed */
        errno = -1;

        strcpy(errbuf, mysql_error(tempsql));
        PushString(errbuf);
        PushInt(errno);
        return;                 /* Push error string, and -1, and return */
    }
    res = mysql_store_result(tempsql);
    num_rows = 0;
    if (res) {                  /* there IS a result */
        /* tmp = new char[BUFFER_LEN]; */
        all_rows = mysql_num_rows(res);
        num_rows = all_rows > tp_mysql_result_limit ?
            tp_mysql_result_limit : all_rows;
        CHECKOFLOW(num_rows + 2);
        num_fields = mysql_num_fields(res);
        fields = mysql_fetch_fields(res);
        counter = 0;
        fieldsList = new_array_packed(num_fields, 0);
        for (i = 0; i < num_fields; ++i)
            array_set_intkey_strval(&fieldsList, i, fields[i].name);
        while ((row = mysql_fetch_row(res))) { /* fetch all rows, push limit */
            nw = new_array_dictionary();
            if (counter++ < num_rows) {
                for (i = 0; i < num_fields; ++i) {
                    /* Alynna: To make this safe, we must normalize the string to
                                16383 characters and a terminating \0 */
                    if (row[i]) {                        
                        strncpy(tmp, row[i], 16380);
			tmp[16380]='\0';
                        array_set_strkey_strval(&nw, fields[i].name, tmp);
		    }
                }
                PushArrayRaw(nw);
            } else
                break;          /* The limit has been reached, exit the while loop */
        }
        /* delete tmp; */
        mysql_free_result(res);
    } else {                    /* no result */
        num_rows = 0;
        fieldsList = new_array_packed(0,0);
     }
    PushInt(num_rows);
    PushArrayRaw(fieldsList);
    return;
}

/* Simply  close the connection. This is also done automatically 
 * if the MYSQL gets cleared without already having been closed.
 */
void
prim_sqlclose(PRIM_PROTOTYPE)
{
    if (oper[0].type != PROG_MYSQL)
        abort_interp("MySQL connection expected.");
    if (oper[0].data.mysql->connected) {
        mysql_close(oper[0].data.mysql->mysql_conn);
        oper[0].data.mysql->connected = 0;
        oper[0].data.mysql->mysql_conn = 0;
    }
}

void
prim_sqlping(PRIM_PROTOTYPE)
{
    int result = 0;

    if (oper[0].type != PROG_MYSQL)
        abort_interp("MySQL connection expected.");

    if (oper[0].data.mysql->connected) {
        result = mysql_ping(oper[0].data.mysql->mysql_conn);
        if (!result) {
            result = 1;
        } else {
            mysql_close(oper[0].data.mysql->mysql_conn);
            oper[0].data.mysql->mysql_conn = 0;
            oper[0].data.mysql->connected = 0;
        }
    }
    PushInt(result);
}

#endif
