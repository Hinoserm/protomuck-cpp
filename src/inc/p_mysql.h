extern void prim_sqlquery(PRIM_PROTOTYPE);
extern void prim_sqlconnect(PRIM_PROTOTYPE);
extern void prim_sqlclose(PRIM_PROTOTYPE);
extern void prim_sqlping(PRIM_PROTOTYPE);
extern void prim_sqlquery_t(PRIM_PROTOTYPE);

#define PRIMLIST_MYSQL { "SQLQUERY",     LARCH, 2, prim_sqlquery    }, \
                       { "SQLCONNECT",   LARCH, 5, prim_sqlconnect  }, \
                       { "SQLCLOSE",     LARCH, 1, prim_sqlclose    }, \
					   { "SQLQUERY_T",   LARCH, 2, prim_sqlquery_t  }, \
                       { "SQLPING",      LARCH, 1, prim_sqlping     }

#ifdef SQL_SUPPORT
#define PRIMS_MYSQL_CNT 5
#else
#define PRIMS_MYSQL_CNT 0
#endif
