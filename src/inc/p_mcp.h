#ifndef _P_MCP_H
#define _P_MCP_H

#ifdef MCP_SUPPORT

extern void prim_mcp_register(PRIM_PROTOTYPE);
extern void prim_mcp_register_event(PRIM_PROTOTYPE);
extern void prim_mcp_bind(PRIM_PROTOTYPE);
extern void prim_mcp_supports(PRIM_PROTOTYPE);
extern void prim_mcp_send(PRIM_PROTOTYPE);

extern void prim_gui_available(PRIM_PROTOTYPE);
extern void prim_gui_dlog_create(PRIM_PROTOTYPE);
extern void prim_gui_dlog_show(PRIM_PROTOTYPE);
extern void prim_gui_dlog_close(PRIM_PROTOTYPE);
extern void prim_gui_value_set(PRIM_PROTOTYPE);
extern void prim_gui_values_get(PRIM_PROTOTYPE);
extern void prim_gui_ctrl_create(PRIM_PROTOTYPE);
extern void prim_gui_ctrl_command(PRIM_PROTOTYPE);
extern void prim_gui_value_get(PRIM_PROTOTYPE);

#define PRIMLIST_MCP { "MCP_REGISTER",	     LM3, 3, prim_mcp_register       }, \
		     { "MCP_BIND",           LM3, 3, prim_mcp_bind           }, \
                     { "MCP_SUPPORTS",       LM3, 2, prim_mcp_supports       }, \
                     { "MCP_SEND",           LM3, 4, prim_mcp_send           }, \
                     { "MCP_REGISTER_EVENT", LM3, 3, prim_mcp_register_event }, \
                     { "GUI_AVAILABLE",      LM3, 1, prim_gui_available      }, \
                     { "GUI_DLOG_SHOW",      LM3, 1, prim_gui_dlog_show      }, \
                     { "GUI_DLOG_CLOSE",     LM3, 1, prim_gui_dlog_close     }, \
                     { "GUI_VALUES_GET",     LM3, 1, prim_gui_values_get     }, \
                     { "GUI_VALUE_SET",      LM3, 3, prim_gui_value_set      }, \
                     { "GUI_CTRL_CREATE",    LM3, 4, prim_gui_ctrl_create    }, \
                     { "GUI_DLOG_CREATE",    LM3, 4, prim_gui_dlog_create    }, \
                     { "GUI_CTRL_COMMAND",   LM3, 4, prim_gui_ctrl_command   }, \
                     { "GUI_VALUE_GET",      LM3, 2, prim_gui_value_get      }

#define PRIMS_MCP_CNT 14

#else

#define PRIMS_MCP_CNT 0

#endif

#endif /* _P_MCP_H */


