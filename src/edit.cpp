#include "copyright.h"
#include "config.h"

#include "db.h"
#include "props.h"
#include "interface.h"
#include "externs.h"
#include "Modules.h"
#include "params.h"
#include "tune.h"
#include "match.h"
#include "strutils.h"
#include "MacroTable.h"

#define DOWNCASE(x) (tolower(x))

void editor(int descr, dbref player, const char *command);
void do_insert(dbref player, dbref program, int arg[], int argc);
void do_delete(dbref player, dbref program, int arg[], int argc);
void do_quit(dbref player, dbref program);
void do_cancel(dbref player, dbref program);
void do_list(dbref player, dbref program, int arg[], int argc, int commentit);
void insert(dbref player, const char *line);
void do_compile(int descr, dbref player, dbref program, int force_err_disp);
void free_line(struct line *l);
void free_prog_text(struct line *l);
void prog_clean(struct frame *fr);
void val_and_head(dbref player, int arg[], int argc);
void do_list_header(dbref player, dbref program);
void list_publics(int descr, dbref player, int arg[], int argc);
void do_list_publics(dbref player, dbref program);
void toggle_numbers(dbref player, int arg[], int argc);

/* Editor routines --- Also contains routines to handle input */

/* This routine determines if a player is editing or running an interactive
   command.  It does it by checking the frame pointer field of the player ---
   if the program counter is NULL, then the player is not running anything
   The reason we don't just check the pointer but check the pc too is because
   I plan to leave the frame always on to save the time required allocating
   space each time a program is run.
   */
void
interactive(int descr, dbref player, const char *command, int len, int wclen)
{
    if (FLAGS(player) & READMODE) {
        /*
         * process command, push onto stack, and return control to forth
         * program
         */
        handle_read_event(descr, player, command, NULL, len, wclen);
    } else {
        editor(descr, player, command);
    }
}

/* The editor itself --- this gets called each time every time to
 * parse a command.
 */

void
editor(int descr, dbref player, const char *command)
{
    dbref program;
    int arg[MAX_ARG + 1];
    char buf[BUFFER_LEN];
    const char *word[MAX_ARG + 1];
    int i, j;                   /* loop variables */

    program = MUCK::playerSession(player).currProg;

    /* check to see if we are insert mode */
    if (MUCK::playerSession(player).insertMode) {
        insert(player, command); /* insert it! */
        return;
    }
    /* parse the commands */
    for (i = 0; i <= MAX_ARG && *command; i++) {
        while (*command && isspace(*command))
            command++;
        j = 0;
        while (*command && !isspace(*command)) {
            buf[j] = *command;
            command++, j++;
        }

        buf[j] = '\0';
        word[i] = alloc_string(buf);
        if ((i == 1) && !string_compare(word[0], "def")) {
            while (*command && isspace(*command))
                command++;
            word[2] = alloc_string(command);
            if (MLevel(player) < LMAGE) {
                anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
            } else if (!word[2]) {
                anotify_nolisten(player, CFAIL "Invalid definition syntax.", 1);
            } else {
                if (MUCK::macros().insert(word[1], word[2], player)) {
                    anotify_nolisten(player, CSUCC "Entry created.", 1);
                } else {
                    anotify_nolisten(player, CINFO "That macro already exists.", 1);
                }
            }
            for (; i >= 0; i--)
                delete[]word[i];

            return;
        }
        arg[i] = atoi(buf);
        if (arg[i] < 0) {
            anotify_nolisten(player, CFAIL "Negative arguments not allowed.", 1);
            for (; i >= 0; i--)
                delete[]word[i];
            return;
        }
    }
    i--;
    while ((i >= 0) && !word[i])
        i--;
    if (i < 0) {
        return;
    } else {
        switch (word[i][0]) {
            case KILL_COMMAND:
                if (!Mage(player)) {
                    anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
                } else {
                    if (MUCK::macros().remove(word[0]))
                        anotify_nolisten(player, CSUCC "Macro entry deleted.", 1);
                    else
                        anotify_nolisten(player, CINFO "Macro to delete not found.", 1);
                }
                break;
            case SHOW_COMMAND:
                MUCK::macros().list(word, i, player, true);
                break;
            case SHORTSHOW_COMMAND:
                MUCK::macros().list(word, i, player, false);
                break;
            case INSERT_COMMAND:
                do_insert(player, program, arg, i);
                anotify_nolisten(player, CINFO "Entering insert mode.", 1);
                break;
            case DELETE_COMMAND:
                do_delete(player, program, arg, i);
                break;
            case QUIT_EDIT_COMMAND:
                do_quit(player, program);
                anotify_nolisten(player, CINFO "Editor exited.", 1);
                break;
            case CANCEL_EDIT_COMMAND:
                /* Just figured this was due. We'll see how it goes. -Akari */
                do_cancel(player, program);
                anotify_nolisten(player, SYSBLUE "Changes cancelled.", 1);
                break;
            case COMPILE_COMMAND:
                /* compile code belongs in compile.c, not in the editor */
                notify(player, "Compiling...");
                do_compile(descr, player, program, 1);
                anotify_nolisten(player, CSUCC "Compiler done.", 1);
                break;
            case LIST_COMMAND:
                do_list(player, program, arg, i, 0);
                break;
            case EDITOR_HELP_COMMAND:
                spit_file(player, EDITOR_HELP_FILE);
                break;
            case VIEW_COMMAND:
                val_and_head(player, arg, i);
                break;
            case UNASSEMBLE_COMMAND:
                disassemble(player, program);
                break;
            case NUMBER_COMMAND:
                toggle_numbers(player, arg, i);
                break;
            case PUBLICS_COMMAND:
                list_publics(descr, player, arg, i);
                break;
            default:
                anotify_nolisten(player, CFAIL "Illegal editor command.", 1);
                break;
        }
    }
    for (; i >= 0; i--)
        delete[]word[i];
}


/* puts program into insert mode */
void
do_insert(dbref player, dbref program, int arg[], int argc)
{
    MUCK::playerSession(player).insertMode++;
    /* DBDIRTY(player); */
    if (argc)
        DBSTORE(program, sp.program.curr_line, arg[0] - 1);
    /* set current line to something else */
}

/* deletes line n if one argument,
   lines arg1 -- arg2 if two arguments
   current line if no argument */
void
do_delete(dbref player, dbref program, int arg[], int argc)
{
    struct line *curr, *temp;
    char buf[BUFFER_LEN];
    int i;

    switch (argc) {
        case 0:
            arg[0] = DBFETCH(program)->sp.program.curr_line;
        case 1:
            arg[1] = arg[0];
        case 2:
            /* delete from line 1 to line 2 */
            /* first, check for conflict */
            if (arg[0] > arg[1]) {
                anotify_nolisten(player, CFAIL "Nonsensical arguments.", 1);
                return;
            }
            i = arg[0] - 1;
            for (curr = DBFETCH(program)->sp.program.first; curr && i; i--)
                curr = curr->next;
            if (curr) {
                DBFETCH(program)->sp.program.curr_line = arg[0];
                i = arg[1] - arg[0] + 1;
                /* delete n lines */
                while (i && curr) {
                    temp = curr;
                    if (curr->prev)
                        curr->prev->next = curr->next;
                    else
                        DBFETCH(program)->sp.program.first = curr->next;
                    if (curr->next)
                        curr->next->prev = curr->prev;
                    curr = curr->next;
                    free_line(temp);
                    i--;
                }
                sprintf(buf, CSUCC "%d lines deleted", arg[1] - arg[0] - i + 1);
                anotify_nolisten(player, buf, 1);
            } else
                anotify_nolisten(player, CINFO "No line to delete.", 1);
            break;
        default:
            anotify_nolisten(player, CINFO "Too many arguments.", 1);
            break;
    }
}




/* quit from edit mode.  Put player back into the regular game mode */
void
do_quit(dbref player, dbref program)
{
    log_status("PROGRAM SAVED: %s by %s(%d)\n", unparse_object(player, program), NAME(player), player);
    MUCK::programs().write(DBFETCH(program)->sp.program.first, program);

    if (tp_log_programs)
        MUCK::programs().logText(DBFETCH(program)->sp.program.first, player, program);

    free_prog_text(DBFETCH(program)->sp.program.first);
    DBFETCH(program)->sp.program.first = NULL;
    FLAGS(program) &= ~INTERNAL;
    FLAGS(player) &= ~INTERACTIVE;
    MUCK::playerSession(player).currProg = NOTHING;
    DBDIRTY(player);
    DBDIRTY(program);
}

/* quit from edit mode, but don't write the code out and uncompile -Akari */
void
do_cancel(dbref player, dbref program)
{
    uncompile_program(program);
    free_prog_text(DBFETCH(program)->sp.program.first);
    DBFETCH(program)->sp.program.first = NULL;
    FLAGS(program) &= ~INTERNAL;
    FLAGS(player) &= ~INTERACTIVE;
    MUCK::playerSession(player).currProg = NOTHING;
    DBDIRTY(player);

}


void
match_and_list(int descr, dbref player, const char *name, char *linespec, int editor)
{
    dbref thing;
    char *p;
    char *q;
    int range[2];
    int argc;
    int commentit = 0;
    int haveNumbers = 0;        /* 1 = numbers, -1 = no numbers, 0 = no pref */
    int tempFlags;
    struct match_data md;
    struct line *tmpline;

    if (Guest(player)) {
        anotify_fmt(player, CFAIL "%s", tp_noguest_mesg);
        return;
    }
    init_match(descr, player, name, TYPE_PROGRAM, &md);

    match_neighbor(&md);
    match_possession(&md);
    match_registered(&md);
    match_absolute(&md);
    if ((thing = noisy_match_result(&md)) == NOTHING)
        return;
    if (Typeof(thing) != TYPE_PROGRAM) {
        anotify_nolisten(player, CINFO "You can't list anything but a program.", 1);
        return;
    }
    if (!controls(player, thing) && !Viewable(thing)
        && !(POWERS(player) & POW_SEE_ALL)) {
        anotify_fmt(player, CFAIL "%s", tp_noperm_mesg);
        return;
    }
    while ((*linespec == '!' || *linespec == '#' || *linespec == '@')
           && *linespec) {
        if (*linespec == '!')
            commentit = (!commentit);
        if (*linespec == '#')
            haveNumbers = 1;
        if (*linespec == '@')
            haveNumbers = -1;
        (void) *linespec++;
    }
    tempFlags = FLAGS(player);
    if (haveNumbers == -1)      /* no numbers no matter what */
        FLAGS(player) &= ~INTERNAL;
    if (haveNumbers == 1)       /* force number displaying */
        FLAGS(player) |= INTERNAL;

    if (!*linespec) {
        range[0] = 1;
        range[1] = -1;
        argc = 2;
    } else {
        q = p = linespec;
        while (*p) {
            while (*p && !isspace(*p))
                *q++ = *p++;
            while (*p && isspace(*++p)) ;
        }
        *q = '\0';

        argc = 1;
        if (isdigit(*linespec)) {
            range[0] = atoi(linespec);
            while (*linespec && isdigit(*linespec))
                linespec++;
        } else {
            range[0] = 1;
        }
        if (*linespec) {
            argc = 2;
            while (*linespec && !isdigit(*linespec))
                linespec++;
            if (*linespec)
                range[1] = atoi(linespec);
            else
                range[1] = -1;
        }
    }
    tmpline = DBFETCH(thing)->sp.program.first;
    DBSTORE(thing, sp.program.first, MUCK::programs().read(thing));
    do_list(player, thing, range, argc, commentit);
    free_prog_text(DBFETCH(thing)->sp.program.first);
    DBSTORE(thing, sp.program.first, tmpline);
    if (haveNumbers)
        FLAGS(player) = tempFlags;
    return;
}


/* list --- if no argument, redisplay the current line
   if 1 argument, display that line
   if 2 arguments, display all in between   */
void
do_list(dbref player, dbref program, int oarg[], int argc, int commentit)
{
    struct line *curr;
    int i, count;
    int arg[2];
    char buf[BUFFER_LEN];

    if (oarg) {
        arg[0] = oarg[0];
        arg[1] = oarg[1];
    } else
        arg[0] = arg[1] = 0;
    switch (argc) {
        case 0:
            arg[0] = DBFETCH(program)->sp.program.curr_line;
        case 1:
            arg[1] = arg[0];
        case 2:
            if ((arg[0] > arg[1]) && (arg[1] != -1)) {
                if (commentit) {
                    anotify_nolisten(player, CFAIL "( Arguments don't make sense. )", 1);
                } else {
                    anotify_nolisten(player, CFAIL "Arguments don't make sense.", 1);
                }
                return;
            }
            i = arg[0] - 1;
            for (curr = DBFETCH(program)->sp.program.first; i && curr; i--)
                curr = curr->next;
            if (curr) {
                i = arg[1] - arg[0] + 1;
                /* display n lines */
                for (count = arg[0]; curr && (i || (arg[1] == -1)); i--) {
                    if (FLAGS(player) & INTERNAL)
                        sprintf(buf, "%3d: %s", count, DoNull(curr->this_line));
                    else
                        sprintf(buf, "%s", DoNull(curr->this_line));
                    notify_nolisten(player, buf, 1);
                    count++;
                    curr = curr->next;
                }
                if (count - arg[0] > 1) {
                    if (commentit) {
                        sprintf(buf, SYSBLUE "( %d lines displayed. )", count - arg[0]);
                    } else {
                        sprintf(buf, SYSBLUE "%d lines displayed.", count - arg[0]);
                    }
                    anotify_nolisten(player, buf, 1);
                }
            } else {
                if (commentit) {
                    anotify_nolisten(player, SYSBLUE "( Line not available for display. )", 1);
                } else {
                    anotify_nolisten(player, SYSBLUE "Line not available for display.", 1);
                }
            }
            break;
        default:
            if (commentit) {
                anotify_nolisten(player, CINFO "( Too many arguments. )", 1);
            } else {
                anotify_nolisten(player, CINFO "Too many arguments.", 1);
            }
            break;
    }
}

void
val_and_head(dbref player, int arg[], int argc)
{
    dbref program;

    if (argc != 1) {
        anotify_nolisten(player, CINFO "I don't understand which header you're trying to look at.", 1);
        return;
    }
    program = arg[0];
    if ((program < 0) || (program >= MUCK::database().top())
        || (Typeof(program) != TYPE_PROGRAM)) {
        anotify_nolisten(player, CINFO "That isn't a program.", 1);
        return;
    }
    if (!(controls(player, program) || Viewable(program))) {
        anotify_nolisten(player, CFAIL "That's not a public program.", 1);
        return;
    }
    do_list_header(player, program);
}

void
do_list_header(dbref player, dbref program)
{
    struct line *curr = MUCK::programs().read(program);

    while (curr && (((curr->this_line)[0] == '(') || (((curr->this_line)[0] == '/')
                                                      && ((curr->this_line)[1] == '*')))) {
        notify(player, curr->this_line);
        curr = curr->next;
    }
    if (!(FLAGS(program) & INTERNAL)) {
        free_prog_text(curr);
    }
    anotify_nolisten(player, CINFO "Done.", 1);
}

void
list_publics(int descr, dbref player, int arg[], int argc)
{
    dbref program;

    if (argc > 1) {
        anotify_nolisten(player, CINFO "I don't understand which program you want to list PUBLIC functions for.", 1);
        return;
    }
    program = (argc == 0) ? MUCK::playerSession(player).currProg : arg[0];
    if (Typeof(program) != TYPE_PROGRAM) {
        anotify_nolisten(player, CINFO "That isn't a program.", 1);
        return;
    }
    if (!(controls(player, program) || Viewable(program))) {
        anotify_nolisten(player, CFAIL "That's not a public program.", 1);
        return;
    }
    if (!(DBFETCH(program)->sp.program.code)) {
        if (program == MUCK::playerSession(player).currProg) {
            do_compile(descr, OWNER(program), program, 0);
        } else {
            struct line *tmpline;

            tmpline = DBFETCH(program)->sp.program.first;
            DBFETCH(program)->sp.program.first = (struct line *) MUCK::programs().read(program);
            do_compile(descr, OWNER(program), program, 0);
            free_prog_text(DBFETCH(program)->sp.program.first);
            DBSTORE(program, sp.program.first, tmpline);
        }
        if (!(DBFETCH(program)->sp.program.code)) {
            anotify_nolisten(player, CFAIL "Program not compilable.", 1);
            return;
        }
    }
    do_list_publics(player, program);
}

void
do_list_publics(dbref player, dbref program)
{
    struct publics *ptr;

    anotify_nolisten(player, CINFO "PUBLIC functions:", 1);
    for (ptr = DBFETCH(program)->sp.program.pubs; ptr; ptr = ptr->next)
        notify(player, ptr->subname);
}

void
toggle_numbers(dbref player, int arg[], int argc)
{
    if (argc) {
        switch (arg[0]) {
            case 0:
                FLAGS(player) &= ~INTERNAL;
                anotify_nolisten(player, CINFO "Line numbers off.", 1);
                break;

            default:
                FLAGS(player) |= INTERNAL;
                anotify_nolisten(player, CINFO "Line numbers on.", 1);
                break;
        }
    } else if (FLAGS(player) & INTERNAL) {
        FLAGS(player) &= ~INTERNAL;
        anotify_nolisten(player, CINFO "Line numbers off.", 1);
    } else {
        FLAGS(player) |= INTERNAL;
        anotify_nolisten(player, CINFO "Line numbers on.", 1);
    }
}



/* insert this line into program */
void
insert(dbref player, const char *line)
{
    dbref program;
    int i;
    struct line *curr;
    struct line *new_line;

    program = MUCK::playerSession(player).currProg;
    if (!string_compare(line, EXIT_INSERT)) {
        MUCK::playerSession(player).insertMode = 0; /* turn off insert mode */
#ifndef NO_EXITMSG
        anotify_nolisten(player, CSUCC "Exiting insert mode.", 1);
#endif
        return;
    }
    i = DBFETCH(program)->sp.program.curr_line - 1;
    for (curr = DBFETCH(program)->sp.program.first; curr && i && i + 1; i--)
        curr = curr->next;
    new_line = MUCK::programs().newLine();  /* initialize line */
    if (!*line) {
        new_line->this_line = alloc_string(" ");
    } else {
        new_line->this_line = alloc_string(line);
    }
    if (!DBFETCH(program)->sp.program.first) { /* nothing --- insert in front */
        DBFETCH(program)->sp.program.first = new_line;
        DBFETCH(program)->sp.program.curr_line = 2; /* insert at the end */
        /* DBDIRTY(program); */
        return;
    }
    if (!curr) {                /* insert at the end */
        i = 1;
        for (curr = DBFETCH(program)->sp.program.first; curr->next; curr = curr->next)
            i++;                /* count lines */
        DBFETCH(program)->sp.program.curr_line = i + 2;
        new_line->prev = curr;
        curr->next = new_line;
        /* DBDIRTY(program); */
        return;
    }
    if (!DBFETCH(program)->sp.program.curr_line) { /* insert at the
                                                    * beginning */
        DBFETCH(program)->sp.program.curr_line = 1; /* insert after this new
                                                     * line */
        new_line->next = DBFETCH(program)->sp.program.first;
        DBFETCH(program)->sp.program.first = new_line;
        /* DBDIRTY(program); */
        return;
    }
    /* inserting in the middle */
    DBFETCH(program)->sp.program.curr_line++;
    new_line->prev = curr;
    new_line->next = curr->next;
    if (new_line->next)
        new_line->next->prev = new_line;
    curr->next = new_line;
    /* DBDIRTY(program); */
}
