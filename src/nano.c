/* $Id$ */
/**************************************************************************
 *   nano.c                                                               *
 *                                                                        *
 *   Copyright (C) 1999-2004 Chris Allegretta                             *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2, or (at your option)  *
 *   any later version.                                                   *
 *                                                                        *
 *   This program is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 *   GNU General Public License for more details.                         *
 *                                                                        *
 *   You should have received a copy of the GNU General Public License    *
 *   along with this program; if not, write to the Free Software          *
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.            *
 *                                                                        *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <assert.h>
#include "proto.h"
#include "nano.h"

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifdef HAVE_TERMIO_H
#include <termio.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifndef NANO_SMALL
#include <setjmp.h>
#endif

#ifndef DISABLE_WRAPJUSTIFY
static ssize_t fill = 0;	/* Fill - where to wrap lines,
				   basically */
#endif
#ifndef DISABLE_WRAPPING
static bool same_line_wrap = FALSE;	/* Whether wrapped text should
					   be prepended to the next
					   line */
#endif

static struct termios oldterm;	/* The user's original term settings */
static struct sigaction act;	/* For all our fun signal handlers */

#ifndef NANO_SMALL
static sigjmp_buf jmpbuf;	/* Used to return to mainloop after
				   SIGWINCH */
static int pid;			/* The PID of the newly forked process
				 * in open_pipe().  It must be global
				 * because the signal handler needs
				 * it. */
#endif

#ifndef DISABLE_JUSTIFY
static filestruct *jusbottom = NULL;
	/* Pointer to end of justify buffer. */
#endif

/* What we do when we're all set to exit. */
void finish(void)
{
    if (!ISSET(NO_HELP))
	blank_bottombars();
    else
	blank_statusbar();

    wrefresh(bottomwin);
    endwin();

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

#if !defined(NANO_SMALL) && defined(ENABLE_NANORC)
    if (!ISSET(NO_RCFILE) && ISSET(HISTORYLOG))
	save_history();
#endif

#ifdef DEBUG
    thanks_for_all_the_fish();
#endif

    exit(0);
}

/* Die (gracefully?). */
void die(const char *msg, ...)
{
    va_list ap;

    endwin();
    curses_ended = TRUE;

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);

    /* save the currently loaded file if it's been modified */
    if (ISSET(MODIFIED))
	die_save_file(filename);

#ifdef ENABLE_MULTIBUFFER
    /* then save all of the other modified loaded files, if any */
    if (open_files != NULL) {
	openfilestruct *tmp;

	tmp = open_files;

	while (open_files->prev != NULL)
	    open_files = open_files->prev;

	while (open_files->next != NULL) {

	    /* if we already saved the file above (i.e, if it was the
	       currently loaded file), don't save it again */
	    if (tmp != open_files) {
		/* make sure open_files->fileage and fileage, and
		   open_files->filebot and filebot, are in sync; they
		   might not be if lines have been cut from the top or
		   bottom of the file */
		fileage = open_files->fileage;
		filebot = open_files->filebot;
		/* save the file if it's been modified */
		if (open_files->file_flags & MODIFIED)
		    die_save_file(open_files->filename);
	    }
	    open_files = open_files->next;
	}
    }
#endif

    exit(1); /* We have a problem: exit w/ errorlevel(1) */
}

void die_save_file(const char *die_filename)
{
    char *ret;
    bool failed = TRUE;

    /* If we're using restricted mode, don't write any emergency backup
     * files, since that would allow reading from or writing to files
     * not specified on the command line. */
    if (ISSET(RESTRICTED))
	return;

    /* If we can't save, we have REAL bad problems, but we might as well
       TRY. */
    if (die_filename[0] == '\0')
	die_filename = "nano";

    ret = get_next_filename(die_filename);
    if (ret[0] != '\0')
	failed = (write_file(ret, TRUE, FALSE, TRUE) == -1);

    if (!failed)
	fprintf(stderr, _("\nBuffer written to %s\n"), ret);
    else
	fprintf(stderr, _("\nBuffer not written to %s (too many backup files?)\n"), ret);

    free(ret);
}

/* Die with an error message that the screen was too small if, well, the
 * screen is too small. */
void die_too_small(void)
{
    die(_("Window size is too small for nano...\n"));
}

void print_view_warning(void)
{
    statusbar(_("Key illegal in VIEW mode"));
}

/* Initialize global variables -- no better way for now.  If
 * save_cutbuffer is TRUE, don't set cutbuffer to NULL. */
void global_init(bool save_cutbuffer)
{
    current_x = 0;
    current_y = 0;

    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS || COLS < MIN_EDITOR_COLS)
	die_too_small();

    fileage = NULL;
    if (!save_cutbuffer)
	cutbuffer = NULL;
    current = NULL;
    edittop = NULL;
    totlines = 0;
    totsize = 0;
    placewewant = 0;

#ifndef DISABLE_WRAPJUSTIFY
    fill = wrap_at;
    if (fill <= 0)
	fill += COLS;
    if (fill < 0)
	fill = 0;
#endif

    hblank = charalloc(COLS + 1);
    memset(hblank, ' ', COLS);
    hblank[COLS] = '\0';
}

void window_init(void)
{
    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS)
	die_too_small();

    if (topwin != NULL)
	delwin(topwin);
    if (edit != NULL)
	delwin(edit);
    if (bottomwin != NULL)
	delwin(bottomwin);

    /* Set up the windows. */
    topwin = newwin(2, COLS, 0, 0);
    edit = newwin(editwinrows, COLS, 2, 0);
    bottomwin = newwin(3 - no_help(), COLS, LINES - 3 + no_help(), 0);

    /* Turn the keypad back on. */
    keypad(edit, TRUE);
    keypad(bottomwin, TRUE);
}

#ifndef DISABLE_MOUSE
void mouse_init(void)
{
    if (ISSET(USE_MOUSE)) {
	mousemask(BUTTON1_RELEASED, NULL);
	mouseinterval(50);
    } else
	mousemask(0, NULL);
}
#endif

#ifndef DISABLE_HELP
/* This function allocates help_text, and stores the help string in it. 
 * help_text should be NULL initially. */
void help_init(void)
{
    size_t allocsize = 1;	/* Space needed for help_text. */
    const char *htx;		/* Untranslated help message. */
    char *ptr;
    const shortcut *s;
#ifndef NANO_SMALL
    const toggle *t;
#endif

    /* First, set up the initial help text for the current function. */
    if (currshortcut == whereis_list || currshortcut == replace_list
	     || currshortcut == replace_list_2)
	htx = N_("Search Command Help Text\n\n "
		"Enter the words or characters you would like to search "
		"for, then hit Enter.  If there is a match for the text you "
		"entered, the screen will be updated to the location of the "
		"nearest match for the search string.\n\n The previous "
		"search string will be shown in brackets after the search "
		"prompt.  Hitting Enter without entering any text will "
		"perform the previous search.  If you have selected text "
		"with the mark and then search to replace, only matches in "
		"the selected text will be replaced.\n\n The following "
		"function keys are available in Search mode:\n\n");
    else if (currshortcut == gotoline_list)
	htx = N_("Go To Line Help Text\n\n "
		"Enter the line number that you wish to go to and hit "
		"Enter.  If there are fewer lines of text than the "
		"number you entered, you will be brought to the last line "
		"of the file.\n\n The following function keys are "
		"available in Go To Line mode:\n\n");
    else if (currshortcut == insertfile_list)
	htx = N_("Insert File Help Text\n\n "
		"Type in the name of a file to be inserted into the current "
		"file buffer at the current cursor location.\n\n "
		"If you have compiled nano with multiple file buffer "
		"support, and enable multiple buffers with the -F "
		"or --multibuffer command line flags, the Meta-F toggle, or "
		"a nanorc file, inserting a file will cause it to be "
		"loaded into a separate buffer (use Meta-< and > to switch "
		"between file buffers). If you need another blank buffer, "
		"do not enter any filename, or type in a nonexistent "
		"filename at the prompt and press Enter.\n\n The following "
		"function keys are available in Insert File mode:\n\n");
    else if (currshortcut == writefile_list)
	htx = N_("Write File Help Text\n\n "
		"Type the name that you wish to save the current file "
		"as and hit Enter to save the file.\n\n If you have "
		"selected text with the mark, you will be prompted to "
		"save only the selected portion to a separate file.  To "
		"reduce the chance of overwriting the current file with "
		"just a portion of it, the current filename is not the "
		"default in this mode.\n\n The following function keys "
		"are available in Write File mode:\n\n");
#ifndef DISABLE_BROWSER
    else if (currshortcut == browser_list)
	htx = N_("File Browser Help Text\n\n "
		"The file browser is used to visually browse the "
		"directory structure to select a file for reading "
		"or writing.  You may use the arrow keys or Page Up/"
		"Down to browse through the files, and S or Enter to "
		"choose the selected file or enter the selected "
		"directory.  To move up one level, select the directory "
		"called \"..\" at the top of the file list.\n\n The "
		"following function keys are available in the file "
		"browser:\n\n");
    else if (currshortcut == gotodir_list)
	htx = N_("Browser Go To Directory Help Text\n\n "
		"Enter the name of the directory you would like to "
		"browse to.\n\n If tab completion has not been disabled, "
		"you can use the Tab key to (attempt to) automatically "
		"complete the directory name.\n\n The following function "
		"keys are available in Browser Go To Directory mode:\n\n");
#endif
#ifndef DISABLE_SPELLER
    else if (currshortcut == spell_list)
	htx = N_("Spell Check Help Text\n\n "
		"The spell checker checks the spelling of all text "
		"in the current file.  When an unknown word is "
		"encountered, it is highlighted and a replacement can "
		"be edited.  It will then prompt to replace every "
		"instance of the given misspelled word in the "
		"current file, or, if you have selected text with the "
		"mark, in the selected text.\n\n The following other "
		"functions are available in Spell Check mode:\n\n");
#endif
#ifndef NANO_SMALL
    else if (currshortcut == extcmd_list)
	htx = N_("External Command Help Text\n\n "
		"This menu allows you to insert the output of a command "
		"run by the shell into the current buffer (or a new "
		"buffer in multibuffer mode). If you need another blank "
		"buffer, do not enter any command.\n\n The following keys "
		"are available in this mode:\n\n");
#endif
    else
	/* Default to the main help list. */
	htx = N_(" nano help text\n\n "
	  "The nano editor is designed to emulate the functionality and "
	  "ease-of-use of the UW Pico text editor.  There are four main "
	  "sections of the editor: The top line shows the program "
	  "version, the current filename being edited, and whether "
	  "or not the file has been modified.  Next is the main editor "
	  "window showing the file being edited.  The status line is "
	  "the third line from the bottom and shows important messages. "
	  "The bottom two lines show the most commonly used shortcuts "
	  "in the editor.\n\n "
	  "The notation for shortcuts is as follows: Control-key "
	  "sequences are notated with a caret (^) symbol and can be "
	  "entered either by using the Control (Ctrl) key or pressing the "
	  "Esc key twice.  Escape-key sequences are notated with the Meta "
	  "(M) symbol and can be entered using either the Esc, Alt or "
	  "Meta key depending on your keyboard setup.  Also, pressing Esc "
	  "twice and then typing a three-digit number from 000 to 255 "
	  "will enter the character with the corresponding ASCII code.  "
	  "The following keystrokes are available in the main editor "
	  "window.  Alternative keys are shown in parentheses:\n\n");

    htx = _(htx);

    allocsize += strlen(htx);

    /* The space needed for the shortcut lists, at most COLS characters,
     * plus '\n'. */
    allocsize += (COLS < 21 ? 21 : COLS + 1) *
	length_of_list(currshortcut);

#ifndef NANO_SMALL
    /* If we're on the main list, we also count the toggle help text.
     * Each line has "M-%c\t\t\t", which fills 24 columns, plus a space,
     * plus translated text, plus '\n'. */
    if (currshortcut == main_list) {
	size_t endislen = strlen(_("enable/disable"));

	for (t = toggles; t != NULL; t = t->next)
	    allocsize += 8 + strlen(t->desc) + endislen;
    }
#endif

    /* help_text has been freed and set to NULL unless the user resized
     * while in the help screen. */
    free(help_text);

    /* Allocate space for the help text. */
    help_text = charalloc(allocsize);

    /* Now add the text we want. */
    strcpy(help_text, htx);
    ptr = help_text + strlen(help_text);

    /* Now add our shortcut info.  Assume that each shortcut has, at the
     * very least, an equivalent control key, an equivalent primary meta
     * key sequence, or both.  Also assume that the meta key values are
     * not control characters.  We can display a maximum of 3 shortcut
     * entries. */
    for (s = currshortcut; s != NULL; s = s->next) {
	int entries = 0;

	/* Control key. */
	if (s->ctrlval != NANO_NO_KEY) {
	    entries++;
#ifndef NANO_SMALL
	    if (s->ctrlval == NANO_HISTORY_KEY)
		ptr += sprintf(ptr, "%.7s", _("Up"));
	    else
#endif
	    if (s->ctrlval == NANO_CONTROL_SPACE)
		ptr += sprintf(ptr, "^%.6s", _("Space"));
	    else if (s->ctrlval == NANO_CONTROL_8)
		ptr += sprintf(ptr, "^?");
	    else
		ptr += sprintf(ptr, "^%c", s->ctrlval + 64);
	    *(ptr++) = '\t';
	}

	/* Function key. */
	if (s->funcval != NANO_NO_KEY) {
	    entries++;
	    /* If this is the first entry, put it in the middle. */
	    if (entries == 1) {
		entries++;
		*(ptr++) = '\t';
	    }
	    ptr += sprintf(ptr, "(F%d)", s->funcval - KEY_F0);
	    *(ptr++) = '\t';
	}

	/* Primary meta key sequence. */
	if (s->metaval != NANO_NO_KEY) {
	    entries++;
	    /* If this is the last entry, put it at the end. */
	    if (entries == 2 && s->miscval == NANO_NO_KEY) {
		entries++;
		*(ptr++) = '\t';
	    }
	    /* If the primary meta key sequence is the first entry,
	     * don't put parentheses around it. */
	    if (entries == 1 && s->metaval == NANO_ALT_SPACE)
		ptr += sprintf(ptr, "M-%.5s", _("Space"));
	    else
		ptr += sprintf(ptr, entries == 1 ? "M-%c" : "(M-%c)",
			toupper(s->metaval));
	    *(ptr++) = '\t';
	}

	/* Miscellaneous meta key sequence. */
	if (entries < 3 && s->miscval != NANO_NO_KEY) {
	    entries++;
	    /* If this is the last entry, put it at the end. */
	    if (entries == 2) {
		entries++;
		*(ptr++) = '\t';
	    }
	    ptr += sprintf(ptr, "(M-%c)", toupper(s->miscval));
	    *(ptr++) = '\t';
	}

	/* Make sure all the help text starts at the same place. */
	while (entries < 3) {
	    entries++;
	    *(ptr++) = '\t';
	}

	assert(s->help != NULL);
	ptr += sprintf(ptr, "%.*s\n", COLS > 24 ? COLS - 24 : 0, s->help);
    }

#ifndef NANO_SMALL
    /* And the toggles... */
    if (currshortcut == main_list) {
	for (t = toggles; t != NULL; t = t->next) {
	    assert(t->desc != NULL);
	    ptr += sprintf(ptr, "M-%c\t\t\t%s %s\n", toupper(t->val),
		t->desc, _("enable/disable"));
	}
    }
#endif /* !NANO_SMALL */

    /* If all went well, we didn't overwrite the allocated space for
     * help_text. */
    assert(strlen(help_text) < allocsize);
}
#endif

/* Create a new filestruct node.  Note that we specifically do not set
 * prevnode->next equal to the new line. */
filestruct *make_new_node(filestruct *prevnode)
{
    filestruct *newnode = (filestruct *)nmalloc(sizeof(filestruct));

    newnode->data = NULL;
    newnode->prev = prevnode;
    newnode->next = NULL;
    newnode->lineno = prevnode != NULL ? prevnode->lineno + 1 : 1;

    return newnode;
}

/* Make a copy of a node to a pointer (space will be malloc()ed). */
filestruct *copy_node(const filestruct *src)
{
    filestruct *dst = (filestruct *)nmalloc(sizeof(filestruct));

    assert(src != NULL);

    dst->data = charalloc(strlen(src->data) + 1);
    dst->next = src->next;
    dst->prev = src->prev;
    strcpy(dst->data, src->data);
    dst->lineno = src->lineno;

    return dst;
}

/* Splice a node into an existing filestruct. */
void splice_node(filestruct *begin, filestruct *newnode, filestruct
	*end)
{
    if (newnode != NULL) {
	newnode->next = end;
	newnode->prev = begin;
    }
    if (begin != NULL)
	begin->next = newnode;
    if (end != NULL)
	end->prev = newnode;
}

/* Unlink a node from the rest of the filestruct. */
void unlink_node(const filestruct *fileptr)
{
    assert(fileptr != NULL);

    if (fileptr->prev != NULL)
	fileptr->prev->next = fileptr->next;

    if (fileptr->next != NULL)
	fileptr->next->prev = fileptr->prev;
}

/* Delete a node from the filestruct. */
void delete_node(filestruct *fileptr)
{
    if (fileptr != NULL) {
	if (fileptr->data != NULL)
	    free(fileptr->data);
	free(fileptr);
    }
}

/* Okay, now let's duplicate a whole struct! */
filestruct *copy_filestruct(const filestruct *src)
{
    filestruct *head;	/* copy of src, top of the copied list */
    filestruct *prev;	/* temp that traverses the list */

    assert(src != NULL);

    prev = copy_node(src);
    prev->prev = NULL;
    head = prev;
    src = src->next;
    while (src != NULL) {
	prev->next = copy_node(src);
	prev->next->prev = prev;
	prev = prev->next;

	src = src->next;
    }

    prev->next = NULL;
    return head;
}

/* Frees a filestruct. */
void free_filestruct(filestruct *src)
{
    if (src != NULL) {
	while (src->next != NULL) {
	    src = src->next;
	    delete_node(src->prev);
#ifdef DEBUG
	    fprintf(stderr, "%s: free'd a node, YAY!\n", "delete_node()");
#endif
	}
	delete_node(src);
#ifdef DEBUG
	fprintf(stderr, "%s: free'd last node.\n", "delete_node()");
#endif
    }
}

/* Partition a filestruct so it begins at (top, top_x) and ends at (bot,
 * bot_x). */
partition *partition_filestruct(filestruct *top, size_t top_x,
	filestruct *bot, size_t bot_x)
{
    partition *p;
    assert(top != NULL && bot != NULL && fileage != NULL && filebot != NULL);

    /* Initialize the partition. */
    p = (partition *)nmalloc(sizeof(partition));

    /* If the top and bottom of the partition are different from the top
     * and bottom of the filestruct, save the latter and then set them
     * to top and bot. */
    if (top != fileage) {
	p->fileage = fileage;
	fileage = top;
    } else
	p->fileage = NULL;
    if (bot != filebot) {
	p->filebot = filebot;
	filebot = bot;
    } else
	p->filebot = NULL;

    /* Save the line above the top of the partition, detach the top of
     * the partition from it, and save the text before top_x in
     * top_data. */
    p->top_prev = top->prev;
    top->prev = NULL;
    p->top_data = mallocstrncpy(NULL, top->data, top_x + 1);
    p->top_data[top_x] = '\0';

    /* Save the line below the bottom of the partition, detach the
     * bottom of the partition from it, and save the text after bot_x in
     * bot_data. */
    p->bot_next = bot->next;
    bot->next = NULL;
    p->bot_data = mallocstrcpy(NULL, bot->data + bot_x);

    /* Remove all text after bot_x at the bottom of the partition. */
    null_at(&bot->data, bot_x);

    /* Remove all text before top_x at the top of the partition. */
    charmove(top->data, top->data + top_x, strlen(top->data) -
	top_x + 1);
    align(&top->data);

    /* Return the partition. */
    return p;
}

/* Unpartition a filestruct so it begins at (fileage, 0) and ends at
 * (filebot, strlen(filebot)) again. */
void unpartition_filestruct(partition **p)
{
    char *tmp;
    assert(p != NULL && fileage != NULL && filebot != NULL);

    /* Reattach the line above the top of the partition, and restore the
     * text before top_x from top_data.  Free top_data when we're done
     * with it. */
    tmp = mallocstrcpy(NULL, fileage->data);
    fileage->prev = (*p)->top_prev;
    if (fileage->prev != NULL)
	fileage->prev->next = fileage;
    fileage->data = charealloc(fileage->data, strlen((*p)->top_data) +
	strlen(fileage->data) + 1);
    strcpy(fileage->data, (*p)->top_data);
    free((*p)->top_data);
    strcat(fileage->data, tmp);
    free(tmp);

    /* Reattach the line below the bottom of the partition, and restore
     * the text after bot_x from bot_data.  Free bot_data when we're
     * done with it. */
    filebot->next = (*p)->bot_next;
    if (filebot->next != NULL)
	filebot->next->prev = filebot;
    filebot->data = charealloc(filebot->data, strlen(filebot->data) +
	strlen((*p)->bot_data) + 1);
    strcat(filebot->data, (*p)->bot_data);
    free((*p)->bot_data);

    /* Restore the top and bottom of the filestruct, if they were
     * different from the top and bottom of the partition. */
    if ((*p)->fileage != NULL)
	fileage = (*p)->fileage;
    if ((*p)->filebot != NULL)
	filebot = (*p)->filebot;

    /* Uninitialize the partition. */
    free(*p);
    *p = NULL;
}

/* Move all the text between (top, top_x) and (bot, bot_x) in the
 * current filestruct to a filestruct beginning with file_top and ending
 * with file_bot.  If no text is between (top, top_x) and (bot, bot_x),
 * don't do anything. */
void move_to_filestruct(filestruct **file_top, filestruct **file_bot,
	filestruct *top, size_t top_x, filestruct *bot, size_t bot_x)
{
    filestruct *top_save;
    long part_totsize;
    bool at_edittop;
#ifndef NANO_SMALL
    bool mark_inside = FALSE;
#endif

    assert(file_top != NULL && file_bot != NULL && top != NULL && bot != NULL);

    /* If (top, top_x)-(bot, bot_x) doesn't cover any text, get out. */
    if (top == bot && top_x == bot_x)
	return;

    /* Partition the filestruct so that it contains only the text from
     * (top, top_x) to (bot, bot_x), keep track of whether the top of
     * the partition is the top of the edit window, and keep track of
     * whether the mark begins inside the partition. */
    filepart = partition_filestruct(top, top_x, bot, bot_x);
    at_edittop = (fileage == edittop);
#ifndef NANO_SMALL
    if (ISSET(MARK_ISSET))
	mark_inside = (mark_beginbuf->lineno >= fileage->lineno &&
		mark_beginbuf->lineno <= filebot->lineno &&
		(mark_beginbuf != fileage || mark_beginx >= top_x) &&
		(mark_beginbuf != filebot || mark_beginx <= bot_x));
#endif

    /* Get the number of characters in the text, and subtract it from
     * totsize. */
    get_totals(top, bot, NULL, &part_totsize);
    totsize -= part_totsize;

    if (*file_top == NULL) {
	/* If file_top is empty, just move all the text directly into
	 * it.  This is equivalent to tacking the text in top onto the
	 * (lack of) text at the end of file_top. */
	*file_top = fileage;
	*file_bot = filebot;
    } else {
	/* Otherwise, tack the text in top onto the text at the end of
	 * file_bot. */
	(*file_bot)->data = charealloc((*file_bot)->data,
		strlen((*file_bot)->data) + strlen(fileage->data) + 1);
	strcat((*file_bot)->data, fileage->data);

	/* Attach the line after top to the line after file_bot.  Then,
	 * if there's more than one line after top, move file_bot down
	 * to bot. */
	(*file_bot)->next = fileage->next;
	if ((*file_bot)->next != NULL) {
	    (*file_bot)->next->prev = *file_bot;
	    *file_bot = filebot;
	}
    }

    /* Since the text has now been saved, remove it from the filestruct.
     * If the top of the partition was the top of the edit window, set
     * edittop to where the text used to start.  If the mark began
     * inside the partition, set the beginning of the mark to where the
     * text used to start. */
    fileage = (filestruct *)nmalloc(sizeof(filestruct));
    fileage->data = mallocstrcpy(NULL, "");
    filebot = fileage;
    if (at_edittop)
	edittop = fileage;
#ifndef NANO_SMALL
    if (mark_inside) {
	mark_beginbuf = fileage;
	mark_beginx = top_x;
    }
#endif

    /* Restore the current line and cursor position. */
    current = fileage;
    current_x = top_x;

    top_save = fileage;

    /* Unpartition the filestruct so that it contains all the text
     * again, minus the saved text. */
    unpartition_filestruct(&filepart);

    /* Renumber starting with the beginning line of the old
     * partition. */
    renumber(top_save);

    if (filebot->data[0] != '\0')
	new_magicline();

    /* Set totlines to the new number of lines in the file. */
    totlines = filebot->lineno;
}

/* Copy all the text from the filestruct beginning with file_top and
 * ending with file_bot to the current filestruct at the current cursor
 * position. */
void copy_from_filestruct(filestruct *file_top, filestruct *file_bot)
{
    filestruct *top_save;
    int part_totlines;
    long part_totsize;
    bool at_edittop;

    assert(file_top != NULL && file_bot != NULL);

    /* Partition the filestruct so that it contains no text, and keep
     * track of whether the top of the partition is the top of the edit
     * window. */
    filepart = partition_filestruct(current, current_x, current,
	current_x);
    at_edittop = (fileage == edittop);

    /* Put the top and bottom of the filestruct at copies of file_top
     * and file_bot. */
    fileage = copy_filestruct(file_top);
    filebot = fileage;
    while (filebot->next != NULL)
	filebot = filebot->next;

    /* Restore the current line and cursor position. */
    current = filebot;
    current_x = strlen(filebot->data);
    if (fileage == filebot)
	current_x += strlen(filepart->top_data);

    /* Get the number of lines and the number of characters in the saved
     * text, and add the latter to totsize. */
    get_totals(fileage, filebot, &part_totlines, &part_totsize);
    totsize += part_totsize;

    /* If the top of the partition was the top of the edit window, set
     * edittop to where the saved text now starts, and update the
     * current y-coordinate to account for the number of lines it
     * has, less one since the first line will be tacked onto the
     * current line. */
    if (at_edittop)
	edittop = fileage;
    current_y += part_totlines - 1;

    top_save = fileage;

    /* Unpartition the filestruct so that it contains all the text
     * again, minus the saved text. */
    unpartition_filestruct(&filepart);

    /* Renumber starting with the beginning line of the old
     * partition. */
    renumber(top_save);

    if (filebot->data[0] != '\0')
	new_magicline();

    /* Set totlines to the new number of lines in the file. */
    totlines = filebot->lineno;
}

void renumber_all(void)
{
    filestruct *temp;
    int i = 1;

    assert(fileage == NULL || fileage != fileage->next);
    for (temp = fileage; temp != NULL; temp = temp->next)
	temp->lineno = i++;
}

void renumber(filestruct *fileptr)
{
    if (fileptr == NULL || fileptr->prev == NULL || fileptr == fileage)
	renumber_all();
    else {
	int lineno = fileptr->prev->lineno;

	assert(fileptr != fileptr->next);
	for (; fileptr != NULL; fileptr = fileptr->next)
	    fileptr->lineno = ++lineno;
    }
}

/* Print one usage string to the screen.  This cuts down on duplicate
 * strings to translate and leaves out the parts that shouldn't be
 * translatable (the flag names). */
void print1opt(const char *shortflag, const char *longflag, const char
	*desc)
{
    printf(" %s\t", shortflag);
    if (strlen(shortflag) < 8)
	printf("\t");

#ifdef HAVE_GETOPT_LONG
    printf("%s\t", longflag);
    if (strlen(longflag) < 8)
	printf("\t\t");
    else if (strlen(longflag) < 16)
	printf("\t");
#endif

    printf("%s\n", _(desc));
}

void usage(void)
{
#ifdef HAVE_GETOPT_LONG
    printf(_("Usage: nano [+LINE] [GNU long option] [option] [file]\n\n"));
    printf(_("Option\t\tLong option\t\tMeaning\n"));
#else
    printf(_("Usage: nano [+LINE] [option] [file]\n\n"));
    printf(_("Option\t\tMeaning\n"));
#endif

    print1opt("-h, -?", "--help", N_("Show this message"));
    print1opt(_("+LINE"), "", N_("Start at line number LINE"));
#ifndef NANO_SMALL
    print1opt("-A", "--smarthome", N_("Enable smart home key"));
    print1opt("-B", "--backup", N_("Backup existing files on save"));
    print1opt(_("-E [dir]"), _("--backupdir=[dir]"), N_("Directory for writing backup files"));
#endif
#ifdef ENABLE_MULTIBUFFER
    print1opt("-F", "--multibuffer", N_("Enable multiple file buffers"));
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
    print1opt("-H", "--historylog", N_("Log & read search/replace string history"));
#endif
    print1opt("-I", "--ignorercfiles", N_("Don't look at nanorc files"));
#endif
#ifndef NANO_SMALL
    print1opt("-N", "--noconvert", N_("Don't convert files from DOS/Mac format"));
#endif
#ifndef DISABLE_JUSTIFY
    print1opt(_("-Q [str]"), _("--quotestr=[str]"), N_("Quoting string, default \"> \""));
#endif
#ifdef HAVE_REGEX_H
    print1opt("-R", "--regexp", N_("Do regular expression searches"));
#endif
#ifndef NANO_SMALL
    print1opt("-S", "--smooth", N_("Smooth scrolling"));
#endif
    print1opt(_("-T [#cols]"), _("--tabsize=[#cols]"), N_("Set width of a tab in cols to #cols"));
    print1opt("-V", "--version", N_("Print version information and exit"));
#ifdef ENABLE_COLOR
    print1opt(_("-Y [str]"), _("--syntax [str]"), N_("Syntax definition to use"));
#endif
    print1opt("-Z", "--restricted", N_("Restricted mode"));
    print1opt("-c", "--const", N_("Constantly show cursor position"));
#ifndef NANO_SMALL
    print1opt("-d", "--rebinddelete", N_("Fix Backspace/Delete confusion problem"));
    print1opt("-i", "--autoindent", N_("Automatically indent new lines"));
    print1opt("-k", "--cut", N_("Cut from cursor to end of line"));
#endif
    print1opt("-l", "--nofollow", N_("Don't follow symbolic links, overwrite"));
#ifndef DISABLE_MOUSE
    print1opt("-m", "--mouse", N_("Enable mouse"));
#endif
#ifndef DISABLE_OPERATINGDIR
    print1opt(_("-o [dir]"), _("--operatingdir=[dir]"), N_("Set operating directory"));
#endif
    print1opt("-p", "--preserve", N_("Preserve XON (^Q) and XOFF (^S) keys"));
#ifndef DISABLE_WRAPJUSTIFY
    print1opt(_("-r [#cols]"), _("--fill=[#cols]"), N_("Set fill cols to (wrap lines at) #cols"));
#endif
#ifndef DISABLE_SPELLER
    print1opt(_("-s [prog]"), _("--speller=[prog]"), N_("Enable alternate speller"));
#endif
    print1opt("-t", "--tempfile", N_("Auto save on exit, don't prompt"));
    print1opt("-v", "--view", N_("View (read only) mode"));
#ifndef DISABLE_WRAPPING
    print1opt("-w", "--nowrap", N_("Don't wrap long lines"));
#endif
    print1opt("-x", "--nohelp", N_("Don't show help window"));
    print1opt("-z", "--suspend", N_("Enable suspend"));

    /* This is a special case. */
    printf(" %s\t\t\t%s\n","-a, -b, -e, -f, -g, -j", _("(ignored, for Pico compatibility)"));

    exit(0);
}

void version(void)
{
    printf(_(" GNU nano version %s (compiled %s, %s)\n"),
	   VERSION, __TIME__, __DATE__);
    printf(_
	   (" Email: nano@nano-editor.org	Web: http://www.nano-editor.org/"));
    printf(_("\n Compiled options:"));

#ifndef ENABLE_NLS
    printf(" --disable-nls");
#endif
#ifdef DEBUG
    printf(" --enable-debug");
#endif
#ifdef NANO_EXTRA
    printf(" --enable-extra");
#endif
#ifdef NANO_SMALL
    printf(" --enable-tiny");
#else
#ifdef DISABLE_BROWSER
    printf(" --disable-browser");
#endif
#ifdef DISABLE_HELP
    printf(" --disable-help");
#endif
#ifdef DISABLE_JUSTIFY
    printf(" --disable-justify");
#endif
#ifdef DISABLE_MOUSE
    printf(" --disable-mouse");
#endif
#ifdef DISABLE_OPERATINGDIR
    printf(" --disable-operatingdir");
#endif
#ifdef DISABLE_SPELLER
    printf(" --disable-speller");
#endif
#ifdef DISABLE_TABCOMP
    printf(" --disable-tabcomp");
#endif
#endif /* NANO_SMALL */
#ifdef DISABLE_WRAPPING
    printf(" --disable-wrapping");
#endif
#ifdef DISABLE_ROOTWRAP
    printf(" --disable-wrapping-as-root");
#endif
#ifdef ENABLE_COLOR
    printf(" --enable-color");
#endif
#ifdef ENABLE_MULTIBUFFER
    printf(" --enable-multibuffer");
#endif
#ifdef ENABLE_NANORC
    printf(" --enable-nanorc");
#endif
#ifdef USE_SLANG
    printf(" --with-slang");
#endif
    printf("\n");
}

int no_help(void)
{
    return ISSET(NO_HELP) ? 2 : 0;
}

void nano_disabled_msg(void)
{
    statusbar(_("Sorry, support for this function has been disabled"));
}

#ifndef NANO_SMALL
RETSIGTYPE cancel_fork(int signal)
{
    if (kill(pid, SIGKILL) == -1)
	nperror("kill");
}

/* Return TRUE on success. */
bool open_pipe(const char *command)
{
    int fd[2];
    FILE *f;
    struct sigaction oldaction, newaction;
			/* Original and temporary handlers for
			 * SIGINT. */
    bool sig_failed = FALSE;
    /* sig_failed means that sigaction() failed without changing the
     * signal handlers.
     *
     * We use this variable since it is important to put things back
     * when we finish, even if we get errors. */

    /* Make our pipes. */

    if (pipe(fd) == -1) {
	statusbar(_("Could not pipe"));
	return FALSE;
    }

    /* Fork a child. */

    if ((pid = fork()) == 0) {
	close(fd[0]);
	dup2(fd[1], fileno(stdout));
	dup2(fd[1], fileno(stderr));
	/* If execl() returns at all, there was an error. */

	execl("/bin/sh", "sh", "-c", command, 0);
	exit(0);
    }

    /* Else continue as parent. */

    close(fd[1]);

    if (pid == -1) {
	close(fd[0]);
	statusbar(_("Could not fork"));
	return FALSE;
    }

    /* Before we start reading the forked command's output, we set
     * things up so that ^C will cancel the new process. */

    /* Enable interpretation of the special control keys so that we get
     * SIGINT when Ctrl-C is pressed. */
    enable_signals();

    if (sigaction(SIGINT, NULL, &newaction) == -1) {
	sig_failed = TRUE;
	nperror("sigaction");
    } else {
	newaction.sa_handler = cancel_fork;
	if (sigaction(SIGINT, &newaction, &oldaction) == -1) {
	    sig_failed = TRUE;
	    nperror("sigaction");
	}
    }
    /* Note that now oldaction is the previous SIGINT signal handler,
     * to be restored later. */

    f = fdopen(fd[0], "rb");
    if (f == NULL)
	nperror("fdopen");

    read_file(f, "stdin");
    /* If multibuffer mode is on, we could be here in view mode.  If so,
     * don't set the modification flag. */
    if (!ISSET(VIEW_MODE))
	set_modified();

    if (wait(NULL) == -1)
	nperror("wait");

    if (!sig_failed && sigaction(SIGINT, &oldaction, NULL) == -1)
	nperror("sigaction");

    /* Disable interpretation of the special control keys so that we can
     * use Ctrl-C for other things. */
    disable_signals();

    return TRUE;
}
#endif /* !NANO_SMALL */

/* The user typed a character; add it to the edit buffer. */
void do_char(char ch)
{
    size_t current_len = strlen(current->data);
#if !defined(DISABLE_WRAPPING) || defined(ENABLE_COLOR)
    bool do_refresh = FALSE;
	/* Do we have to call edit_refresh(), or can we get away with
	 * update_line()? */
#endif

    if (ch == '\0')		/* Null to newline, if needed. */
	ch = '\n';
    else if (ch == '\n') {	/* Newline to Enter, if needed. */
	do_enter();
	return;
    }

    assert(current != NULL && current->data != NULL);

    /* When a character is inserted on the current magicline, it means
     * we need a new one! */
    if (filebot == current)
	new_magicline();

    /* More dangerousness fun =) */
    current->data = charealloc(current->data, current_len + 2);
    assert(current_x <= current_len);
    charmove(&current->data[current_x + 1], &current->data[current_x],
	current_len - current_x + 1);
    current->data[current_x] = ch;
    totsize++;
    set_modified();

#ifndef NANO_SMALL
    /* Note that current_x has not yet been incremented. */
    if (current == mark_beginbuf && current_x < mark_beginx)
	mark_beginx++;
#endif

    do_right(FALSE);

#ifndef DISABLE_WRAPPING
    /* If we're wrapping text, we need to call edit_refresh(). */
    if (!ISSET(NO_WRAP) && ch != '\t')
	do_refresh = do_wrap(current);
#endif

#ifdef ENABLE_COLOR
    /* If color syntaxes are turned on, we need to call
     * edit_refresh(). */
    if (ISSET(COLOR_SYNTAX))
	do_refresh = TRUE;
#endif

#if !defined(DISABLE_WRAPPING) || defined(ENABLE_COLOR)
    if (do_refresh)
	edit_refresh();
    else
#endif
	update_line(current, current_x);
}

void do_verbatim_input(void)
{
    int *v_kbinput = NULL;	/* Used to hold verbatim input. */
    size_t v_len;		/* Length of verbatim input. */
    size_t i;

    statusbar(_("Verbatim input"));

    v_kbinput = get_verbatim_kbinput(edit, ERR, v_kbinput, &v_len, TRUE);

    /* Turn on DISABLE_CURPOS while inserting character(s) and turn it
     * off afterwards, so that if constant cursor position display is
     * on, it will be updated properly. */
    SET(DISABLE_CURPOS);
    for (i = 0; i < v_len; i++)
	do_char((char)v_kbinput[i]);
    UNSET(DISABLE_CURPOS);

    free(v_kbinput);
}

void do_backspace(void)
{
    if (current != fileage || current_x > 0) {
	do_left(FALSE);
	do_delete();
    }
}

void do_delete(void)
{
    bool do_refresh = FALSE;
	/* Do we have to call edit_refresh(), or can we get away with
	 * update_line()? */

    assert(current != NULL && current->data != NULL && current_x <=
	strlen(current->data));

    placewewant = xplustabs();

    if (current->data[current_x] != '\0') {
	size_t linelen = strlen(current->data + current_x);

	assert(current_x < strlen(current->data));

	/* Let's get dangerous. */
	charmove(&current->data[current_x], &current->data[current_x + 1],
		linelen);

	null_at(&current->data, linelen + current_x - 1);
#ifndef NANO_SMALL
	if (current_x < mark_beginx && mark_beginbuf == current)
	    mark_beginx--;
#endif
    } else if (current != filebot && (current->next != filebot ||
	current->data[0] == '\0')) {
	/* We can delete the line before filebot only if it is blank: it
	 * becomes the new magicline then. */
	filestruct *foo = current->next;

	assert(current_x == strlen(current->data));

	/* If we're deleting at the end of a line, we need to call
	 * edit_refresh(). */
	if (current->data[current_x] == '\0')
	    do_refresh = TRUE;

	current->data = charealloc(current->data, current_x +
		strlen(foo->data) + 1);
	strcpy(current->data + current_x, foo->data);
#ifndef NANO_SMALL
	if (mark_beginbuf == current->next) {
	    mark_beginx += current_x;
	    mark_beginbuf = current;
	}
#endif
	if (filebot == foo)
	    filebot = current;

	unlink_node(foo);
	delete_node(foo);
	renumber(current);
	totlines--;
#ifndef DISABLE_WRAPPING
	wrap_reset();
#endif
    } else
	return;

    totsize--;
    set_modified();

#ifdef ENABLE_COLOR
    /* If color syntaxes are turned on, we need to call
     * edit_refresh(). */
    if (ISSET(COLOR_SYNTAX))
	do_refresh = TRUE;
#endif

    if (do_refresh)
	edit_refresh();
    else
	update_line(current, current_x);
}

void do_tab(void)
{
    do_char('\t');
}

/* Someone hits return *gasp!* */
void do_enter(void)
{
    filestruct *newnode = make_new_node(current);
    size_t extra = 0;

    assert(current != NULL && current->data != NULL);

#ifndef NANO_SMALL
    /* Do auto-indenting, like the neolithic Turbo Pascal editor. */
    if (ISSET(AUTOINDENT)) {
	/* If we are breaking the line in the indentation, the new
	 * indentation should have only current_x characters, and
	 * current_x should not change. */
	extra = indent_length(current->data);
	if (extra > current_x)
	    extra = current_x;
	totsize += extra;
    }
#endif
    newnode->data = charalloc(strlen(current->data + current_x) +
	extra + 1);
    strcpy(&newnode->data[extra], current->data + current_x);
#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT))
	strncpy(newnode->data, current->data, extra);
#endif
    null_at(&current->data, current_x);
#ifndef NANO_SMALL
    if (current == mark_beginbuf && current_x < mark_beginx) {
	mark_beginbuf = newnode;
	mark_beginx += extra - current_x;
    }
#endif
    current_x = extra;

    if (current == filebot)
	filebot = newnode;
    splice_node(current, newnode, current->next);

    totsize++;
    renumber(current);
    current = newnode;

    edit_refresh();

    totlines++;
    set_modified();
    placewewant = xplustabs();
}

#ifndef NANO_SMALL
void do_next_word(void)
{
    size_t old_pww = placewewant;
    const filestruct *old_current = current;
    assert(current != NULL && current->data != NULL);

    /* Skip letters in this word first. */
    while (current->data[current_x] != '\0' &&
	isalnum(current->data[current_x]))
	current_x++;

    for (; current != NULL; current = current->next) {
	while (current->data[current_x] != '\0' &&
		!isalnum(current->data[current_x]))
	    current_x++;

	if (current->data[current_x] != '\0')
	    break;

	current_x = 0;
    }
    if (current == NULL)
	current = filebot;

    placewewant = xplustabs();

    /* Update the screen. */
    edit_redraw(old_current, old_pww);
}

/* The same thing for backwards. */
void do_prev_word(void)
{
    size_t old_pww = placewewant;
    const filestruct *old_current = current;
    assert(current != NULL && current->data != NULL);

    current_x++;

    /* Skip letters in this word first. */
    while (current_x > 0 && isalnum(current->data[current_x - 1]))
	current_x--;

    for (; current != NULL; current = current->prev) {
	while (current_x > 0 && !isalnum(current->data[current_x - 1]))
	    current_x--;

	if (current_x > 0)
	    break;

	if (current->prev != NULL)
	    current_x = strlen(current->prev->data) + 1;
    }

    current_x--;

    if (current == NULL) {
	current = fileage;
	current_x = 0;
    } else {
	while (current_x > 0 && isalnum(current->data[current_x - 1]))
	    current_x--;
    }

    placewewant = xplustabs();

    /* Update the screen. */
    edit_redraw(old_current, old_pww);
}

void do_mark(void)
{
    TOGGLE(MARK_ISSET);
    if (ISSET(MARK_ISSET)) {
	statusbar(_("Mark Set"));
	mark_beginbuf = current;
	mark_beginx = current_x;
    } else {
	statusbar(_("Mark UNset"));
	edit_refresh();
    }
}
#endif /* !NANO_SMALL */

#ifndef DISABLE_WRAPPING
void wrap_reset(void)
{
    same_line_wrap = FALSE;
}
#endif

#ifndef DISABLE_WRAPPING
/* We wrap the given line.  Precondition: we assume the cursor has been
 * moved forward since the last typed character.  Return value: whether
 * we wrapped. */
bool do_wrap(filestruct *inptr)
{
    size_t len = strlen(inptr->data);
	/* Length of the line we wrap. */
    size_t i = 0;
	/* Generic loop variable. */
    ssize_t wrap_loc = -1;
	/* Index of inptr->data where we wrap. */
    ssize_t word_back = -1;
#ifndef NANO_SMALL
    const char *indentation = NULL;
	/* Indentation to prepend to the new line. */
    size_t indent_len = 0;	/* strlen(indentation) */
#endif
    const char *after_break;	/* Text after the wrap point. */
    size_t after_break_len;	/* strlen(after_break) */
    bool wrapping = FALSE;	/* Do we prepend to the next line? */
    const char *wrap_line = NULL;
	/* The next line, minus indentation. */
    size_t wrap_line_len = 0;	/* strlen(wrap_line) */
    char *newline = NULL;	/* The line we create. */
    size_t new_line_len = 0;	/* Eventual length of newline. */

/* There are three steps.  First, we decide where to wrap.  Then, we
 * create the new wrap line.  Finally, we clean up. */

/* Step 1, finding where to wrap.  We are going to add a new line
 * after a whitespace character.  In this step, we set wrap_loc as the
 * location of this replacement.
 *
 * Where should we break the line?  We need the last legal wrap point
 * such that the last word before it ended at or before fill.  If there
 * is no such point, we settle for the first legal wrap point.
 *
 * A legal wrap point is a whitespace character that is not followed by
 * whitespace.
 *
 * If there is no legal wrap point or we found the last character of the
 * line, we should return without wrapping.
 *
 * Note that the initial indentation does not count as a legal wrap
 * point if we are going to auto-indent!
 *
 * Note that the code below could be optimized, by not calling
 * strnlenpt() so often. */

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT))
	i = indent_length(inptr->data);
#endif
    wrap_line = inptr->data + i;
    for (; i < len; i++, wrap_line++) {
	/* Record where the last word ended. */
	if (!isblank(*wrap_line))
	    word_back = i;
	/* If we have found a legal wrap point and the current word
	 * extends too far, then we stop. */
	if (wrap_loc != -1 && strnlenpt(inptr->data, word_back + 1) >
		fill)
	    break;
	/* We record the latest legal wrap point. */
	if (word_back != i && !isblank(wrap_line[1]))
	    wrap_loc = i;
    }
    if (i == len)
	return FALSE;

    /* Step 2, making the new wrap line.  It will consist of indentation
     * + after_break + " " + wrap_line (although indentation and
     * wrap_line are conditional on flags and #defines). */

    /* after_break is the text that will be moved to the next line. */
    after_break = inptr->data + wrap_loc + 1;
    after_break_len = len - wrap_loc - 1;
    assert(after_break_len == strlen(after_break));

    /* new_line_len will later be increased by the lengths of indentation
     * and wrap_line. */
    new_line_len = after_break_len;

    /* We prepend the wrapped text to the next line, if the flag is set,
     * and there is a next line, and prepending would not make the line
     * too long. */
    if (same_line_wrap && inptr->next) {
	wrap_line = inptr->next->data;
	wrap_line_len = strlen(wrap_line);

	/* +1 for the space between after_break and wrap_line. */
	if ((new_line_len + 1 + wrap_line_len) <= fill) {
	    wrapping = TRUE;
	    new_line_len += (1 + wrap_line_len);
	}
    }

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT)) {
	/* Indentation comes from the next line if wrapping, else from
	 * this line. */
	indentation = (wrapping ? wrap_line : inptr->data);
	indent_len = indent_length(indentation);
	if (wrapping)
	    /* The wrap_line text should not duplicate indentation.
	     * Note in this case we need not increase new_line_len. */
	    wrap_line += indent_len;
	else
	    new_line_len += indent_len;
    }
#endif

    /* Now we allocate the new line and copy into it. */
    newline = charalloc(new_line_len + 1);  /* +1 for \0 */
    new_line_len = 0;
    *newline = '\0';

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT)) {
	strncpy(newline, indentation, indent_len);
	newline[indent_len] = '\0';
	new_line_len = indent_len;
    }
#endif
    strcat(newline, after_break);
    new_line_len += after_break_len;
    /* We end the old line after wrap_loc.  Note that this does not eat
     * the space. */
    null_at(&inptr->data, wrap_loc + 1);
    totsize++;
    if (wrapping) {
	/* In this case, totsize increases by 1 since we add a space
	 * between after_break and wrap_line.  If the line already ends
	 * in a tab or a space, we don't add a space and decrement
	 * totsize to account for that. */
	if (!isblank(newline[new_line_len - 1]))
	    strcat(newline, " ");
	else
	    totsize--;
	strcat(newline, wrap_line);
	free(inptr->next->data);
	inptr->next->data = newline;
    } else {
	filestruct *temp = (filestruct *)nmalloc(sizeof(filestruct));

	/* In this case, the file size changes by +1 for the new line,
	 * and +indent_len for the new indentation. */
#ifndef NANO_SMALL
	totsize += indent_len;
#endif
	totlines++;
	temp->data = newline;
	temp->prev = inptr;
	temp->next = inptr->next;
	temp->prev->next = temp;
	/* If temp->next is NULL, then temp is the last line of the
	 * file, so we must set filebot. */
	if (temp->next != NULL)
	    temp->next->prev = temp;
	else
	    filebot = temp;
    }

    /* Step 3, clean up.  Here we reposition the cursor and mark, and do
     * some other sundry things. */

    /* Later wraps of this line will be prepended to the next line. */
    same_line_wrap = TRUE;

    /* Each line knows its line number.  We recalculate these if we
     * inserted a new line. */
    if (!wrapping)
	renumber(inptr);

    /* If the cursor was after the break point, we must move it. */
    if (current_x > wrap_loc) {
	current = current->next;
	current_x -=
#ifndef NANO_SMALL
		-indent_len +
#endif
		wrap_loc + 1;
	wrap_reset();
	placewewant = xplustabs();
    }

#ifndef NANO_SMALL
    /* If the mark was on this line after the wrap point, we move it
     * down.  If it was on the next line and we wrapped, we move it
     * right. */
    if (mark_beginbuf == inptr && mark_beginx > wrap_loc) {
	mark_beginbuf = inptr->next;
	mark_beginx -= wrap_loc - indent_len + 1;
    } else if (wrapping && mark_beginbuf == inptr->next)
	mark_beginx += after_break_len;
#endif /* !NANO_SMALL */

    return TRUE;
}
#endif /* !DISABLE_WRAPPING */

#ifndef DISABLE_SPELLER
/* A word is misspelled in the file.  Let the user replace it.  We
 * return FALSE if the user cancels. */
bool do_int_spell_fix(const char *word)
{
    char *save_search, *save_replace;
    size_t current_x_save = current_x, pww_save = placewewant;
    filestruct *edittop_save = edittop, *current_save = current;
	/* Save where we are. */
    bool canceled = FALSE;
	/* The return value. */
    bool case_sens_set = ISSET(CASE_SENSITIVE);
#ifndef NANO_SMALL
    bool reverse_search_set = ISSET(REVERSE_SEARCH);
#endif
#ifdef HAVE_REGEX_H
    bool regexp_set = ISSET(USE_REGEXP);
#endif
#ifndef NANO_SMALL
    bool old_mark_set = ISSET(MARK_ISSET);
    bool added_magicline = FALSE;
	/* Whether we added a magicline after filebot. */
    bool right_side_up = FALSE;
	/* TRUE if (mark_beginbuf, mark_beginx) is the top of the mark,
	 * FALSE if (current, current_x) is. */
    filestruct *top, *bot;
    size_t top_x, bot_x;
#endif

    /* Make sure spell-check is case sensitive. */
    SET(CASE_SENSITIVE);

#ifndef NANO_SMALL
    /* Make sure spell-check goes forward only. */
    UNSET(REVERSE_SEARCH);
#endif
#ifdef HAVE_REGEX_H
    /* Make sure spell-check doesn't use regular expressions. */
    UNSET(USE_REGEXP);
#endif

    /* Save the current search/replace strings. */
    search_init_globals();
    save_search = last_search;
    save_replace = last_replace;

    /* Set the search/replace strings to the misspelled word. */
    last_search = mallocstrcpy(NULL, word);
    last_replace = mallocstrcpy(NULL, word);

#ifndef NANO_SMALL
    if (old_mark_set) {
	/* If the mark is on, partition the filestruct so that it
	 * contains only the marked text, keep track of whether the text
	 * will have a magicline added when we're done correcting
	 * misspelled words, and turn the mark off. */
	mark_order((const filestruct **)&top, &top_x,
	    (const filestruct **)&bot, &bot_x, &right_side_up);
	filepart = partition_filestruct(top, top_x, bot, bot_x);
	added_magicline = (filebot->data[0] != '\0');
	UNSET(MARK_ISSET);
    }
#endif

    /* Start from the top of the file. */
    edittop = fileage;
    current = fileage;
    current_x = (size_t)-1;
    placewewant = 0;

    /* Find the first whole-word occurrence of word. */
    findnextstr_wrap_reset();
    while (findnextstr(TRUE, TRUE, FALSE, fileage, 0, word, NULL)) {
	if (is_whole_word(current_x, current->data, word)) {
	    edit_refresh();

	    do_replace_highlight(TRUE, word);

	    /* Allow all instances of the word to be corrected. */
	    canceled = (statusq(FALSE, spell_list, word,
#ifndef NANO_SMALL
			NULL,
#endif
			 _("Edit a replacement")) == -1);

	    do_replace_highlight(FALSE, word);

	    if (!canceled && strcmp(word, answer) != 0) {
		current_x--;
		do_replace_loop(word, current, &current_x, TRUE,
			&canceled);
	    }

	    break;
	}
    }

#ifndef NANO_SMALL
    if (old_mark_set) {
	/* If the mark was on and we added a magicline, remove it
	 * now. */
	if (added_magicline)
	    remove_magicline();

	/* Put the beginning and the end of the mark at the beginning
	 * and the end of the spell-checked text. */
	if (fileage == filebot)
	    bot_x += top_x;
	if (right_side_up) {
	    mark_beginx = top_x;
	    current_x_save = bot_x;
	} else {
	    current_x_save = top_x;
	    mark_beginx = bot_x;
	}

	/* Unpartition the filestruct so that it contains all the text
	 * again, and turn the mark back on. */
	unpartition_filestruct(&filepart);
	SET(MARK_ISSET);
    }
#endif

    /* Restore the search/replace strings. */
    free(last_search);
    last_search = save_search;
    free(last_replace);
    last_replace = save_replace;

    /* Restore where we were. */
    edittop = edittop_save;
    current = current_save;
    current_x = current_x_save;
    placewewant = pww_save;

    /* Restore case sensitivity setting. */
    if (!case_sens_set)
	UNSET(CASE_SENSITIVE);

#ifndef NANO_SMALL
    /* Restore search/replace direction. */
    if (reverse_search_set)
	SET(REVERSE_SEARCH);
#endif
#ifdef HAVE_REGEX_H
    /* Restore regular expression usage setting. */
    if (regexp_set)
	SET(USE_REGEXP);
#endif

    return !canceled;
}

/* Integrated spell checking using 'spell' program.  Return value: NULL
 * for normal termination, otherwise the error string. */
const char *do_int_speller(const char *tempfile_name)
{
    char *read_buff, *read_buff_ptr, *read_buff_word;
    size_t pipe_buff_size, read_buff_size, read_buff_read, bytesread;
    int spell_fd[2], sort_fd[2], uniq_fd[2], tempfile_fd = -1;
    pid_t pid_spell, pid_sort, pid_uniq;
    int spell_status, sort_status, uniq_status;

    /* Create all three pipes up front. */
    if (pipe(spell_fd) == -1 || pipe(sort_fd) == -1 || pipe(uniq_fd) == -1)
	return _("Could not create pipe");

    statusbar(_("Creating misspelled word list, please wait..."));

    /* A new process to run spell in. */
    if ((pid_spell = fork()) == 0) {

	/* Child continues (i.e, future spell process). */

	close(spell_fd[0]);

	/* Replace the standard input with the temp file. */
	if ((tempfile_fd = open(tempfile_name, O_RDONLY)) == -1)
	    goto close_pipes_and_exit;

	if (dup2(tempfile_fd, STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(tempfile_fd);

	/* Send spell's standard output to the pipe. */
	if (dup2(spell_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(spell_fd[1]);

	/* Start spell program; we are using PATH. */
	execlp("spell", "spell", NULL);

	/* Should not be reached, if spell is found. */
	exit(1);
    }

    /* Parent continues here. */
    close(spell_fd[1]);

    /* A new process to run sort in. */
    if ((pid_sort = fork()) == 0) {

	/* Child continues (i.e, future spell process).  Replace the
	 * standard input with the standard output of the old pipe. */
	if (dup2(spell_fd[0], STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(spell_fd[0]);

	/* Send sort's standard output to the new pipe. */
	if (dup2(sort_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(sort_fd[1]);

	/* Start sort program.  Use -f to remove mixed case without
	 * having to have ANOTHER pipe for tr.  If this isn't portable,
	 * let me know. */
	execlp("sort", "sort", "-f", NULL);

	/* Should not be reached, if sort is found. */
	exit(1);
    }

    close(spell_fd[0]);
    close(sort_fd[1]);

    /* A new process to run uniq in. */
    if ((pid_uniq = fork()) == 0) {

	/* Child continues (i.e, future uniq process).  Replace the
	 * standard input with the standard output of the old pipe. */
	if (dup2(sort_fd[0], STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(sort_fd[0]);

	/* Send uniq's standard output to the new pipe. */
	if (dup2(uniq_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(uniq_fd[1]);

	/* Start uniq program; we are using PATH. */
	execlp("uniq", "uniq", NULL);

	/* Should not be reached, if uniq is found. */
	exit(1);
    }

    close(sort_fd[0]);
    close(uniq_fd[1]);

    /* Child process was not forked successfully. */
    if (pid_spell < 0 || pid_sort < 0 || pid_uniq < 0) {
	close(uniq_fd[0]);
	return _("Could not fork");
    }

    /* Get system pipe buffer size. */
    if ((pipe_buff_size = fpathconf(uniq_fd[0], _PC_PIPE_BUF)) < 1) {
	close(uniq_fd[0]);
	return _("Could not get size of pipe buffer");
    }

    /* Read in the returned spelling errors. */
    read_buff_read = 0;
    read_buff_size = pipe_buff_size + 1;
    read_buff = read_buff_ptr = charalloc(read_buff_size);

    while ((bytesread = read(uniq_fd[0], read_buff_ptr, pipe_buff_size)) > 0) {
	read_buff_read += bytesread;
	read_buff_size += pipe_buff_size;
	read_buff = read_buff_ptr = charealloc(read_buff, read_buff_size);
	read_buff_ptr += read_buff_read;

    }

    *read_buff_ptr = (char)NULL;
    close(uniq_fd[0]);

    /* Process the spelling errors. */
    read_buff_word = read_buff_ptr = read_buff;

    while (*read_buff_ptr != '\0') {

	if ((*read_buff_ptr == '\n') || (*read_buff_ptr == '\r')) {
	    *read_buff_ptr = (char)NULL;
	    if (read_buff_word != read_buff_ptr) {
		if (!do_int_spell_fix(read_buff_word)) {
		    read_buff_word = read_buff_ptr;
		    break;
		}
	    }
	    read_buff_word = read_buff_ptr + 1;
	}
	read_buff_ptr++;
    }

    /* Special case where last word doesn't end with \n or \r. */
    if (read_buff_word != read_buff_ptr)
	do_int_spell_fix(read_buff_word);

    free(read_buff);
    replace_abort();
    edit_refresh();

    /* Process end of spell process. */
    waitpid(pid_spell, &spell_status, 0);
    waitpid(pid_sort, &sort_status, 0);
    waitpid(pid_uniq, &uniq_status, 0);

    if (WIFEXITED(spell_status) == 0 || WEXITSTATUS(spell_status))
	return _("Error invoking \"spell\"");

    if (WIFEXITED(sort_status)  == 0 || WEXITSTATUS(sort_status))
	return _("Error invoking \"sort -f\"");

    if (WIFEXITED(uniq_status) == 0 || WEXITSTATUS(uniq_status))
	return _("Error invoking \"uniq\"");

    /* Otherwise... */
    return NULL;

  close_pipes_and_exit:

    /* Don't leak any handles. */
    close(tempfile_fd);
    close(spell_fd[0]);
    close(spell_fd[1]);
    close(sort_fd[0]);
    close(sort_fd[1]);
    close(uniq_fd[0]);
    close(uniq_fd[1]);
    exit(1);
}

/* External spell checking.  Return value: NULL for normal termination,
 * otherwise the error string. */
const char *do_alt_speller(char *tempfile_name)
{
    int alt_spell_status, lineno_save = current->lineno;
    size_t current_x_save = current_x, pww_save = placewewant;
    int current_y_save = current_y;
    pid_t pid_spell;
    char *ptr;
    static int arglen = 3;
    static char **spellargs = NULL;
    FILE *f;
#ifndef NANO_SMALL
    bool old_mark_set = ISSET(MARK_ISSET);
    bool added_magicline = FALSE;
	/* Whether we added a magicline after filebot. */
    bool right_side_up = FALSE;
	/* TRUE if (mark_beginbuf, mark_beginx) is the top of the mark,
	 * FALSE if (current, current_x) is. */
    filestruct *top, *bot;
    size_t top_x, bot_x;
    int mbb_lineno_save = 0;
	/* We're going to close the current file, and open the output of
	 * the alternate spell command.  The line that mark_beginbuf
	 * points to will be freed, so we save the line number and
	 * restore afterwards. */
    long old_totsize = totsize;
	/* Our saved value of totsize, used when we spell-check a marked
	 * selection. */

    if (old_mark_set) {
	/* If the mark is on, save the number of the line it starts on,
	 * and then turn the mark off. */
	mbb_lineno_save = mark_beginbuf->lineno;
	UNSET(MARK_ISSET);
    }
#endif

    endwin();

    /* Set up an argument list to pass execvp(). */
    if (spellargs == NULL) {
	spellargs = (char **)nmalloc(arglen * sizeof(char *));

	spellargs[0] = strtok(alt_speller, " ");
	while ((ptr = strtok(NULL, " ")) != NULL) {
	    arglen++;
	    spellargs = (char **)nrealloc(spellargs, arglen * sizeof(char *));
	    spellargs[arglen - 3] = ptr;
	}
	spellargs[arglen - 1] = NULL;
    }
    spellargs[arglen - 2] = tempfile_name;

    /* Start a new process for the alternate speller. */
    if ((pid_spell = fork()) == 0) {
	/* Start alternate spell program; we are using PATH. */
	execvp(spellargs[0], spellargs);

	/* Should not be reached, if alternate speller is found!!! */
	exit(1);
    }

    /* Could not fork?? */
    if (pid_spell < 0)
	return _("Could not fork");

    /* Wait for alternate speller to complete. */
    wait(&alt_spell_status);

    if (!WIFEXITED(alt_spell_status) || WEXITSTATUS(alt_spell_status) != 0) {
	char *altspell_error = NULL;
	char *invoke_error = _("Could not invoke \"%s\"");
	int msglen = strlen(invoke_error) + strlen(alt_speller) + 2;

	altspell_error = charalloc(msglen);
	snprintf(altspell_error, msglen, invoke_error, alt_speller);
	return altspell_error;
    }

    refresh();

    /* Restore the terminal to its previous state. */
    terminal_init();

#ifndef NANO_SMALL
    if (old_mark_set) {
	long part_totsize;

	/* If the mark was on, partition the filestruct so that it
	 * contains only the marked text, and keep track of whether the
	 * temp file (which should contain the spell-checked marked
	 * text) will have a magicline added when it's reloaded. */
	mark_order((const filestruct **)&top, &top_x,
		(const filestruct **)&bot, &bot_x, &right_side_up);
	filepart = partition_filestruct(top, top_x, bot, bot_x);
	added_magicline = (filebot->data[0] != '\0');

	/* Get the number of characters in the marked text, and subtract
	 * it from the saved value of totsize.  Note that we don't need
	 * to save totlines. */
	get_totals(top, bot, NULL, &part_totsize);
	old_totsize -= part_totsize;
    }
#endif

    /* Reinitialize the filestruct. */
    free_filestruct(fileage);
    global_init(TRUE);

    /* Reload the temp file.  Do what load_buffer() would do, except for
     * making a new buffer for the temp file if multibuffer support is
     * available. */
    open_file(tempfile_name, FALSE, &f);
    read_file(f, tempfile_name);
    current = fileage;

#ifndef NANO_SMALL
    if (old_mark_set) {
	filestruct *top_save = fileage;

	/* If the mark was on and we added a magicline, remove it
	 * now. */
	if (added_magicline)
	    remove_magicline();

	/* Put the beginning and the end of the mark at the beginning
	 * and the end of the spell-checked text. */
	if (fileage == filebot)
	    bot_x += top_x;
	if (right_side_up) {
	    mark_beginx = top_x;
	    current_x_save = bot_x;
	} else {
	    current_x_save = top_x;
	    mark_beginx = bot_x;
	}

	/* Unpartition the filestruct so that it contains all the text
	 * again.  Note that we've replaced the marked text originally
	 * in the partition with the spell-checked marked text in the
	 * temp file. */
	unpartition_filestruct(&filepart);

	/* Renumber starting with the beginning line of the old
	 * partition.  Also set totlines to the new number of lines in
	 * the file, add the number of characters in the spell-checked
	 * marked text to the saved value of totsize, and then make that
	 * saved value the actual value. */
	renumber(top_save);
	totlines = filebot->lineno;
	old_totsize += totsize;
	totsize = old_totsize;

	/* Assign mark_beginbuf to the line where the mark began
	 * before. */
	do_gotopos(mbb_lineno_save, mark_beginx, current_y_save, 0);
	mark_beginbuf = current;

	/* Assign mark_beginx to the location in mark_beginbuf where the
	 * mark began before, adjusted for any shortening of the
	 * line. */
	mark_beginx = current_x;

	/* Turn the mark back on. */
	SET(MARK_ISSET);
    }
#endif

    /* Go back to the old position, mark the file as modified, and make
     * sure that the titlebar is refreshed. */
    do_gotopos(lineno_save, current_x_save, current_y_save, pww_save);
    set_modified();
    clearok(topwin, FALSE);
    titlebar(NULL);

    return NULL;
}

void do_spell(void)
{
    int i;
    char *temp = safe_tempnam(0, "nano.");
    const char *spell_msg;

    if (temp == NULL) {
	statusbar(_("Could not create temp file: %s"), strerror(errno));
	return;
    }

#ifndef NANO_SMALL
    if (ISSET(MARK_ISSET))
	i = write_marked(temp, TRUE, FALSE);
    else
#endif
	i = write_file(temp, TRUE, FALSE, FALSE);

    if (i == -1) {
	statusbar(_("Error writing temp file: %s"), strerror(errno));
	free(temp);
	return;
    }

#ifdef ENABLE_MULTIBUFFER
    /* Update the current open_files entry before spell-checking, in
     * case any problems occur. */
    add_open_file(TRUE);
#endif

    spell_msg = alt_speller != NULL ? do_alt_speller(temp) :
	do_int_speller(temp);
    unlink(temp);
    free(temp);

    if (spell_msg != NULL)
	statusbar(_("Spell checking failed: %s: %s"), spell_msg,
		strerror(errno));
    else
	statusbar(_("Finished checking spelling"));
}
#endif /* !DISABLE_SPELLER */

#if !defined(NANO_SMALL) || !defined(DISABLE_JUSTIFY)
/* The "indentation" of a line is the whitespace between the quote part
 * and the non-whitespace of the line. */
size_t indent_length(const char *line)
{
    size_t len = 0;

    assert(line != NULL);
    while (isblank(*line)) {
	line++;
	len++;
    }
    return len;
}
#endif /* !NANO_SMALL || !DISABLE_JUSTIFY */

#ifndef DISABLE_JUSTIFY
/* justify_format() replaces Tab by Space and multiple spaces by 1
 * (except it maintains 2 after a non-repeated character in punct
 * followed by a character in brackets).  Note that the terminating \0
 * counts as a space.
 *
 * justify_format() might make line->data shorter, and change the actual
 * pointer with null_at().
 *
 * justify_format() will not look at the first skip characters of line.
 * skip should be at most strlen(line->data).  The character at
 * line[skip + 1] must not be whitespace. */
void justify_format(filestruct *line, size_t skip)
{
    char *back, *front;

    /* These four asserts are assumptions about the input data. */
    assert(line != NULL);
    assert(line->data != NULL);
    assert(skip < strlen(line->data));
    assert(!isblank(line->data[skip]));

    back = line->data + skip;
    for (front = back; ; front++) {
	bool remove_space = FALSE;
	    /* Do we want to remove this space? */

	if (*front == '\t')
	    *front = ' ';

	/* These tests are safe since line->data + skip is not a
	 * space. */
	if ((*front == '\0' || *front == ' ') && *(front - 1) == ' ') {
	    const char *bob = back - 2;

	    remove_space = TRUE;
	    for (; bob >= line->data + skip; bob--) {
		if (strchr(punct, *bob) != NULL) {
		    /* If this character is in punct, don't remove the
		     * space unless this character and the character
		     * before it are the same. */
		    remove_space = (bob > line->data + skip &&
			*bob == *(bob - 1));
		    break;
		}
		if (strchr(brackets, *bob) == NULL)
		    break;
	    }
	}

	if (remove_space) {
	    /* Now *front is a space we want to remove.  We do that by
	     * simply failing to assign it to *back. */
#ifndef NANO_SMALL
	    if (mark_beginbuf == line && back - line->data < mark_beginx)
		mark_beginx--;
#endif
	    if (*front == '\0')
		*(back - 1) = '\0';
	} else {
	    *back = *front;
	    back++;
	}
	if (*front == '\0')
	    break;
    }

    back--;
    assert(*back == '\0' && *front == '\0');

    /* Now back is the new end of line->data. */
    if (back != front) {
	totsize -= front - back;
	null_at(&line->data, back - line->data);
#ifndef NANO_SMALL
	if (mark_beginbuf == line && back - line->data < mark_beginx)
	    mark_beginx = back - line->data;
#endif
    }
}

/* The "quote part" of a line is the largest initial substring matching
 * the quote string.  This function returns the length of the quote part
 * of the given line.
 *
 * Note that if !HAVE_REGEX_H then we match concatenated copies of
 * quotestr. */
size_t quote_length(const char *line)
{
#ifdef HAVE_REGEX_H
    regmatch_t matches;
    int rc = regexec(&quotereg, line, 1, &matches, 0);

    if (rc == REG_NOMATCH || matches.rm_so == (regoff_t)-1)
	return 0;
    /* matches.rm_so should be 0, since the quote string should start
     * with the caret ^. */
    return matches.rm_eo;
#else	/* !HAVE_REGEX_H */
    size_t qdepth = 0;

    /* Compute quote depth level. */
    while (strncmp(line + qdepth, quotestr, quotelen) == 0)
	qdepth += quotelen;
    return qdepth;
#endif	/* !HAVE_REGEX_H */
}

/* a_line and b_line are lines of text.  The quotation part of a_line is
 * the first a_quote characters.  Check that the quotation part of
 * b_line is the same. */
bool quotes_match(const char *a_line, size_t a_quote, const char
	*b_line)
{
    /* Here is the assumption about a_quote: */
    assert(a_quote == quote_length(a_line));
    return a_quote == quote_length(b_line) &&
	strncmp(a_line, b_line, a_quote) == 0;
}

/* We assume a_line and b_line have no quote part.  Then, we return
 * whether b_line could follow a_line in a paragraph. */
bool indents_match(const char *a_line, size_t a_indent, const char
	*b_line, size_t b_indent)
{
    assert(a_indent == indent_length(a_line));
    assert(b_indent == indent_length(b_line));

    return b_indent <= a_indent &&
	strncmp(a_line, b_line, b_indent) == 0;
}

/* Is foo the beginning of a paragraph?
 *
 *   A line of text consists of a "quote part", followed by an
 *   "indentation part", followed by text.  The functions quote_length()
 *   and indent_length() calculate these parts.
 *
 *   A line is "part of a paragraph" if it has a part not in the quote
 *   part or the indentation.
 *
 *   A line is "the beginning of a paragraph" if it is part of a
 *   paragraph and
 *	1) it is the top line of the file, or
 *	2) the line above it is not part of a paragraph, or
 *	3) the line above it does not have precisely the same quote
 *	   part, or
 *	4) the indentation of this line is not an initial substring of
 *	   the indentation of the previous line, or
 *	5) this line has no quote part and some indentation, and
 *	   AUTOINDENT is not set.
 *   The reason for number 5) is that if AUTOINDENT is not set, then an
 *   indented line is expected to start a paragraph, like in books.
 *   Thus, nano can justify an indented paragraph only if AUTOINDENT is
 *   turned on. */
bool begpar(const filestruct *const foo)
{
    size_t quote_len;
    size_t indent_len;
    size_t temp_id_len;

    /* Case 1). */
    if (foo->prev == NULL)
	return TRUE;

    quote_len = quote_length(foo->data);
    indent_len = indent_length(foo->data + quote_len);

    /* Not part of a paragraph. */
    if (foo->data[quote_len + indent_len] == '\0')
	return FALSE;

    /* Case 3). */
    if (!quotes_match(foo->data, quote_len, foo->prev->data))
	return TRUE;

    temp_id_len = indent_length(foo->prev->data + quote_len);

    /* Case 2) or 5) or 4). */
    if (foo->prev->data[quote_len + temp_id_len] == '\0' ||
	(quote_len == 0 && indent_len > 0
#ifndef NANO_SMALL
	&& !ISSET(AUTOINDENT)
#endif
	) || !indents_match(foo->prev->data + quote_len, temp_id_len,
	foo->data + quote_len, indent_len))
	return TRUE;

    return FALSE;
}

/* We find the last beginning-of-paragraph line before the current
 * line. */
void do_para_begin(void)
{
    const filestruct *old_current = current;
    const size_t old_pww = placewewant;

    current_x = 0;
    placewewant = 0;

    if (current->prev != NULL) {
	do {
	    current = current->prev;
	    current_y--;
	} while (!begpar(current));
    }

    edit_redraw(old_current, old_pww);
}

bool inpar(const char *str)
{
    size_t quote_len = quote_length(str);

    return str[quote_len + indent_length(str + quote_len)] != '\0';
}

/* A line is the last line of a paragraph if it is in a paragraph, and
 * the next line isn't, or is the beginning of a paragraph.  We move
 * down to the end of a paragraph, then one line farther. */
void do_para_end(void)
{
    const filestruct *const old_current = current;
    const size_t old_pww = placewewant;

    current_x = 0;
    placewewant = 0;

    while (current->next != NULL && !inpar(current->data))
	current = current->next;

    while (current->next != NULL && inpar(current->next->data) &&
	    !begpar(current->next)) {
	current = current->next;
	current_y++;
    }

    if (current->next != NULL)
	current = current->next;

    edit_redraw(old_current, old_pww);
}

/* Put the next par_len lines, starting with first_line, into the
 * justify buffer, leaving copies of those lines in place.  Assume there
 * are enough lines after first_line.  Return the new copy of
 * first_line. */
filestruct *backup_lines(filestruct *first_line, size_t par_len, size_t
	quote_len)
{
    filestruct *top = first_line;
	/* The top of the paragraph we're backing up. */
    filestruct *bot = first_line;
	/* The bottom of the paragraph we're backing up. */
    size_t i;
	/* Generic loop variable. */
    size_t current_x_save = current_x;
    int fl_lineno_save = first_line->lineno;
    int edittop_lineno_save = edittop->lineno;
    int current_lineno_save = current->lineno;
#ifndef NANO_SMALL
    bool old_mark_set = ISSET(MARK_ISSET);
    int mbb_lineno_save = 0;

    if (old_mark_set)
	mbb_lineno_save = mark_beginbuf->lineno;
#endif

    /* Move bot down par_len lines to the newline after the last line of
     * the paragraph. */
    for (i = par_len; i > 0; i--)
	bot = bot->next;

    /* Move the paragraph from the main filestruct to the justify
     * buffer. */
    move_to_filestruct(&jusbuffer, &jusbottom, top, 0, bot, 0);

    /* Copy the paragraph from the justify buffer to the main
     * filestruct. */
    copy_from_filestruct(jusbuffer, jusbottom);

    /* Move upward from the last line of the paragraph to the first
     * line, putting first_line, edittop, current, and mark_beginbuf at
     * the same lines in the copied paragraph that they had in the
     * original paragraph. */
    top = current->prev;
    for (i = par_len; i > 0; i--) {
	if (top->lineno == fl_lineno_save)
	    first_line = top;
	if (top->lineno == edittop_lineno_save)
	    edittop = top;
	if (top->lineno == current_lineno_save)
	    current = top;
#ifndef NANO_SMALL
	if (old_mark_set && top->lineno == mbb_lineno_save)
	    mark_beginbuf = top;
#endif
	top = top->prev;
    }

    /* Put current_x at the same place in the copied paragraph that it
     * had in the original paragraph. */
    current_x = current_x_save;

    set_modified();

    return first_line;
}

/* Is it possible to break line at or before goal? */
bool breakable(const char *line, ssize_t goal)
{
    for (; *line != '\0' && goal >= 0; line++) {
	if (isblank(*line))
	    return TRUE;

	if (is_cntrl_char(*line))
	    goal -= 2;
	else
	    goal -= 1;
    }
    /* If goal is not negative, the whole line (one word) was short
     * enough. */
    return goal >= 0;
}

/* We are trying to break a chunk off line.  We find the last space such
 * that the display length to there is at most goal + 1.  If there is no
 * such space, and force is TRUE, then we find the first space.  Anyway,
 * we then take the last space in that group of spaces.  The terminating
 * '\0' counts as a space. */
int break_line(const char *line, ssize_t goal, bool force)
{
    ssize_t space_loc = -1;
	/* Current tentative return value.  Index of the last space we
	 * found with short enough display width.  */
    ssize_t cur_loc = 0;
	/* Current index in line. */

    assert(line != NULL);
    for (; *line != '\0' && goal >= 0; line++, cur_loc++) {
	if (*line == ' ')
	    space_loc = cur_loc;
	assert(*line != '\t');

	if (is_cntrl_char(*line))
	    goal -= 2;
	else
	    goal--;
    }
    if (goal >= 0)
	/* In fact, the whole line displays shorter than goal. */
	return cur_loc;
    if (space_loc == -1) {
	/* No space found short enough. */
	if (force)
	    for (; *line != '\0'; line++, cur_loc++)
		if (*line == ' ' && *(line + 1) != ' ' && *(line + 1) != '\0')
		    return cur_loc;
	return -1;
    }
    /* Perhaps the character after space_loc is a space.  But because
     * of justify_format(), there can be only two adjacent. */
    if (*(line - cur_loc + space_loc + 1) == ' ' ||
	*(line - cur_loc + space_loc + 1) == '\0')
	space_loc++;
    return space_loc;
}

/* Find the beginning of the current paragraph if we're in one, or the
 * beginning of the next paragraph if we're not.  Afterwards, save the
 * quote length and paragraph length in *quote and *par.  Return FALSE
 * if we found a paragraph, or TRUE if there was an error or we didn't
 * find a paragraph.
 *
 * See the comment at begpar() for more about when a line is the
 * beginning of a paragraph. */
bool do_para_search(size_t *const quote, size_t *const par)
{
    size_t quote_len;
	/* Length of the initial quotation of the paragraph we
	 * search. */
    size_t par_len;
	/* Number of lines in that paragraph. */
    size_t indent_len;
	/* Generic indentation length. */
    filestruct *line;
	/* Generic line of text. */

#ifdef HAVE_REGEX_H
    if (quoterc != 0) {
	statusbar(_("Bad quote string %s: %s"), quotestr, quoteerr);
	return TRUE;
    }
#endif

    /* Here is an assumption that is always true anyway. */
    assert(current != NULL);

    current_x = 0;

    quote_len = quote_length(current->data);
    indent_len = indent_length(current->data + quote_len);

    /* Here we find the first line of the paragraph to search.  If the
     * current line is in a paragraph, then we move back to the first
     * line of the paragraph.  Otherwise, we move to the first line that
     * is in a paragraph. */
    if (current->data[quote_len + indent_len] != '\0') {
	/* This line is part of a paragraph.  So we must search back to
	 * the first line of this paragraph.  First we check items 1)
	 * and 3) above. */
	while (current->prev != NULL &&	quotes_match(current->data,
		quote_len, current->prev->data)) {
	    size_t temp_id_len =
		indent_length(current->prev->data + quote_len);
		/* The indentation length of the previous line. */

	    /* Is this line the beginning of a paragraph, according to
	     * items 2), 5), or 4) above?  If so, stop. */
	    if (current->prev->data[quote_len + temp_id_len] == '\0' ||
		(quote_len == 0 && indent_len > 0
#ifndef NANO_SMALL
		&& !ISSET(AUTOINDENT)
#endif
		) || !indents_match(current->prev->data + quote_len,
		temp_id_len, current->data + quote_len, indent_len))
		break;
	    indent_len = temp_id_len;
	    current = current->prev;
	    current_y--;
	}
    } else {
	/* This line is not part of a paragraph.  Move down until we get
	 * to a non "blank" line. */
	do {
	    /* There is no next paragraph, so nothing to move to. */
	    if (current->next == NULL) {
		placewewant = 0;
		return TRUE;
	    }
	    current = current->next;
	    current_y++;
	    quote_len = quote_length(current->data);
	    indent_len = indent_length(current->data + quote_len);
	} while (current->data[quote_len + indent_len] == '\0');
    }

    /* Now current is the first line of the paragraph, and quote_len is
     * the quotation length of that line. */

    /* Next step, compute par_len, the number of lines in this
     * paragraph. */
    line = current;
    par_len = 1;
    indent_len = indent_length(line->data + quote_len);

    while (line->next != NULL &&
	    quotes_match(current->data, quote_len, line->next->data)) {
	size_t temp_id_len = indent_length(line->next->data + quote_len);

	if (!indents_match(line->data + quote_len, indent_len,
		line->next->data + quote_len, temp_id_len) ||
		line->next->data[quote_len + temp_id_len] == '\0' ||
		(quote_len == 0 && temp_id_len > 0
#ifndef NANO_SMALL
		&& !ISSET(AUTOINDENT)
#endif
		))
	    break;
	indent_len = temp_id_len;
	line = line->next;
	par_len++;
    }

    /* Now par_len is the number of lines in this paragraph.  We should
     * never call quotes_match() or quote_length() again. */

    /* Save the values of quote_len and par_len. */
    assert(quote != NULL && par != NULL);
    *quote = quote_len;
    *par = par_len;

    return FALSE;
}

/* If full_justify is TRUE, justify the entire file.  Otherwise, justify
 * the current paragraph. */
void do_justify(bool full_justify)
{
    filestruct *first_par_line = NULL;
	/* Will be the first line of the resulting justified paragraph.
	 * For restoring after unjustify. */
    filestruct *last_par_line;
	/* Will be the line containing the newline after the last line
	 * of the result.  Also for restoring after unjustify. */
    bool allow_respacing;
	/* Whether we should change the spacing at the end of a line
	 * after justifying it.  This should be TRUE whenever we move
	 * to the next line after justifying the current line. */

    /* We save these global variables to be restored if the user
     * unjustifies.  Note that we don't need to save totlines. */
    size_t current_x_save = current_x;
    int current_y_save = current_y;
    long flags_save = flags, totsize_save = totsize;
    filestruct *edittop_save = edittop, *current_save = current;
#ifndef NANO_SMALL
    filestruct *mark_beginbuf_save = mark_beginbuf;
    size_t mark_beginx_save = mark_beginx;
#endif
    int kbinput;
    bool meta_key, func_key;

    /* If we're justifying the entire file, start at the beginning. */
    if (full_justify)
	current = fileage;

    last_par_line = current;

    while (TRUE) {
	size_t quote_len;
	    /* Length of the initial quotation of the paragraph we
	     * justify. */
	size_t par_len;
	    /* Number of lines in that paragraph. */

	/* Find the first line of the paragraph to be justified.  That
	 * is the start of this paragraph if we're in one, or the start
	 * of the next otherwise.  Save the quote length and paragraph
	 * length (number of lines).  Don't refresh the screen yet
	 * (since we'll do that after we justify).  If the search failed
	 * and we're justifying the whole file, move the last line of
	 * the text we're justifying to just before the magicline, which
	 * is where it'll be anyway if we've searched the entire file,
	 * and break out of the loop; otherwise, refresh the screen and
	 * get out. */
	if (do_para_search(&quote_len, &par_len)) {
	    if (full_justify) {
		last_par_line = filebot;
		break;
	    } else {
		edit_refresh();
		return;
	    }
	}

	/* Next step, we loop through the lines of this paragraph,
	 * justifying each one individually. */
	for (; par_len > 0; current_y++, par_len--) {
	    size_t indent_len;	/* Generic indentation length. */
	    size_t line_len;
	    size_t display_len;
		/* The width of current in screen columns. */
	    ssize_t break_pos;
		/* Where we will break the line. */

	    /* We'll be moving to the next line after justifying the
	     * current line in almost all cases, so allow changing the
	     * spacing at the ends of justified lines by default. */
	    allow_respacing = TRUE;

	    indent_len = quote_len + indent_length(current->data +
		quote_len);

	    /* If we haven't already done it, copy the original
	     * paragraph to the justify buffer. */
	    if (first_par_line == NULL)
		first_par_line = backup_lines(current, full_justify ?
			filebot->lineno - current->lineno : par_len,
			quote_len);

	    /* Now we call justify_format() on the current line of the
	     * paragraph, which will remove excess spaces from it and
	     * change tabs to spaces. */
	    justify_format(current, quote_len +
		indent_length(current->data + quote_len));

	    line_len = strlen(current->data);
	    display_len = strlenpt(current->data);

	    if (display_len > fill) {
		/* The line is too long.  Try to wrap it to the next. */
	        break_pos = break_line(current->data + indent_len,
			fill - strnlenpt(current->data, indent_len), TRUE);
		if (break_pos == -1 || break_pos + indent_len == line_len)
		    /* We can't break the line, or don't need to, so
		     * just go on to the next. */
		    goto continue_loc;
		break_pos += indent_len;
		assert(break_pos < line_len);
		if (par_len == 1) {
		    /* There is no next line in this paragraph.  We make
		     * a new line and copy text after break_pos into
		     * it. */
		    splice_node(current, make_new_node(current), current->next);
		    /* In a non-quoted paragraph, we copy the indent
		     * only if AUTOINDENT is turned on. */
		    if (quote_len == 0
#ifndef NANO_SMALL
			&& !ISSET(AUTOINDENT)
#endif
			)
			    indent_len = 0;
		    current->next->data = charalloc(indent_len + line_len -
			break_pos);
		    strncpy(current->next->data, current->data, indent_len);
		    strcpy(current->next->data + indent_len,
			current->data + break_pos + 1);
		    assert(strlen(current->next->data) ==
			indent_len + line_len - break_pos - 1);
		    totlines++;
		    totsize += indent_len;
		    par_len++;
		} else {
		    size_t next_line_len = strlen(current->next->data);

		    indent_len = quote_len +
			indent_length(current->next->data + quote_len);
		    current->next->data = charealloc(current->next->data,
			next_line_len + line_len - break_pos + 1);

		    charmove(current->next->data + indent_len + line_len -
			break_pos, current->next->data + indent_len,
			next_line_len - indent_len + 1);
		    strcpy(current->next->data + indent_len,
			current->data + break_pos + 1);
		    current->next->data[indent_len + line_len -
			break_pos - 1] = ' ';
#ifndef NANO_SMALL
		    if (mark_beginbuf == current->next) {
			if (mark_beginx < indent_len)
			    mark_beginx = indent_len;
			mark_beginx += line_len - break_pos;
		    }
#endif
		}
#ifndef NANO_SMALL
		if (mark_beginbuf == current && mark_beginx > break_pos) {
		    mark_beginbuf = current->next;
		    mark_beginx -= break_pos + 1 - indent_len;
		}
#endif
		null_at(&current->data, break_pos);

		/* Go to the next line. */
		current = current->next;
	    } else if (display_len < fill && par_len > 1) {
		size_t next_line_len;

		indent_len = quote_len +
			indent_length(current->next->data + quote_len);
		/* If we can't pull a word from the next line up to this
		 * one, just go on. */
		if (!breakable(current->next->data + indent_len,
			fill - display_len - 1))
		    goto continue_loc;

		break_pos = break_line(current->next->data + indent_len,
			fill - display_len - 1, FALSE);
		assert(break_pos != -1);

		current->data = charealloc(current->data,
			line_len + break_pos + 2);
		current->data[line_len] = ' ';
		strncpy(current->data + line_len + 1,
			current->next->data + indent_len, break_pos);
		current->data[line_len + break_pos + 1] = '\0';
#ifndef NANO_SMALL
		if (mark_beginbuf == current->next) {
		    if (mark_beginx < indent_len + break_pos) {
			mark_beginbuf = current;
			if (mark_beginx <= indent_len)
			    mark_beginx = line_len + 1;
			else
			    mark_beginx = line_len + 1 + mark_beginx -
				indent_len;
		    } else
			mark_beginx -= break_pos + 1;
		}
#endif
		next_line_len = strlen(current->next->data);
		if (indent_len + break_pos == next_line_len) {
		    filestruct *line = current->next;

		    /* Don't destroy edittop! */
		    if (line == edittop)
			edittop = current;

		    unlink_node(line);
		    delete_node(line);
		    totlines--;
		    totsize -= indent_len;
		    current_y--;

		    /* Don't go to the next line.  Accordingly, don't
		     * allow changing the spacing at the end of the
		     * previous justified line, so that we don't end up
		     * doing it more than once on the same line. */
		    allow_respacing = FALSE;
		} else {
		    charmove(current->next->data + indent_len,
			current->next->data + indent_len + break_pos + 1,
			next_line_len - break_pos - indent_len);
		    null_at(&current->next->data, next_line_len - break_pos);

		    /* Go to the next line. */
		    current = current->next;
		}
	    } else
  continue_loc:
		/* Go to the next line. */
		current = current->next;

	    /* We've moved to the next line after justifying the
	     * current line.  If the justified line was not the last
	     * line of the paragraph, add a space to the end of it to
	     * replace the one removed or left out by justify_format().
	     * If it was the last line of the paragraph, and
	     * justify_format() left a space on the end of it, remove
	     * the space. */
	    if (allow_respacing) {
		size_t prev_line_len = strlen(current->prev->data);

		if (par_len > 1) {
		    current->prev->data = charealloc(current->prev->data,
			prev_line_len + 2);
		    current->prev->data[prev_line_len] = ' ';
		    current->prev->data[prev_line_len + 1] = '\0';
		    totsize++;
		} else if (par_len == 1 &&
			current->prev->data[prev_line_len - 1] == ' ') {
		    current->prev->data = charealloc(current->prev->data,
			prev_line_len);
		    current->prev->data[prev_line_len - 1] = '\0';
		    totsize--;
		}
	    }
	}

	/* We've just justified a paragraph. If we're not justifying the
	 * entire file, break out of the loop.  Otherwise, continue the
	 * loop so that we justify all the paragraphs in the file. */
	if (!full_justify)
	    break;

    } /* while (TRUE) */

    /* We are now done justifying the paragraph or the file, so clean
     * up.  totlines, totsize, and current_y have been maintained above.
     * Set last_par_line to the new end of the paragraph, update
     * fileage, and renumber() since edit_refresh() needs the line
     * numbers to be right (but only do the last two if we actually
     * justified something). */
    last_par_line = current;
    if (first_par_line != NULL) {
	if (first_par_line->prev == NULL)
	    fileage = first_par_line;
	renumber(first_par_line);
    }

    edit_refresh();

    statusbar(_("Can now UnJustify!"));

    /* Display the shortcut list with UnJustify. */
    shortcut_init(TRUE);
    display_main_list();

    /* Now get a keystroke and see if it's unjustify; if not, unget the
     * keystroke and return. */
    kbinput = get_edit_input(&meta_key, &func_key);

    if (!meta_key && !func_key && kbinput == NANO_UNJUSTIFY_KEY) {
	/* Restore the justify we just did (ungrateful user!). */
	current = current_save;
	current_x = current_x_save;
	current_y = current_y_save;
	edittop = edittop_save;

	/* Splice the justify buffer back into the file, but only if we
	 * actually justified something. */
	if (first_par_line != NULL) {
	    filestruct *bot_save;

	    /* Partition the filestruct so that it contains only the
	     * text of the justified paragraph. */
	    filepart = partition_filestruct(first_par_line, 0,
		last_par_line, 0);

	    /* Remove the text of the justified paragraph, and
	     * put the text in the justify buffer in its place. */
	    free_filestruct(fileage);
	    fileage = jusbuffer;
	    filebot = jusbottom;

	    bot_save = filebot;

	    /* Unpartition the filestruct so that it contains all the
	     * text again.  Note that the justified paragraph has been
	     * replaced with the unjustified paragraph. */
	    unpartition_filestruct(&filepart);

	     /* Renumber starting with the ending line of the old
	      * partition. */
	    if (bot_save->next != NULL)
		renumber(bot_save->next);

	    /* Restore global variables from before the justify. */
	    totsize = totsize_save;
	    totlines = filebot->lineno;
#ifndef NANO_SMALL
	    mark_beginbuf = mark_beginbuf_save;
	    mark_beginx = mark_beginx_save;
#endif
	    flags = flags_save;

	    /* Clear the justify buffer. */
	    jusbuffer = NULL;

	    if (!ISSET(MODIFIED))
		titlebar(NULL);
	    edit_refresh();
	}
    } else {
	/* Blow away the justify buffer.*/
	free_filestruct(jusbuffer);
	jusbuffer = NULL;
    }

    blank_statusbar();

    /* Display the shortcut list with UnCut. */
    shortcut_init(FALSE);
    display_main_list();
}

void do_justify_void(void)
{
    do_justify(FALSE);
}

void do_full_justify(void)
{
    do_justify(TRUE);
}
#endif /* !DISABLE_JUSTIFY */

void do_exit(void)
{
    int i;

    if (!ISSET(MODIFIED))
	i = 0;		/* Pretend the user chose not to save. */
    else if (ISSET(TEMP_FILE))
	i = 1;
    else
	i = do_yesno(FALSE,
		_("Save modified buffer (ANSWERING \"No\" WILL DESTROY CHANGES) ? "));

#ifdef DEBUG
    dump_buffer(fileage);
#endif

    if (i == 0 || (i == 1 && do_writeout(TRUE) > 0)) {
#ifdef ENABLE_MULTIBUFFER
	/* Exit only if there are no more open buffers. */
	if (!close_open_file())
#endif
	    finish();
    } else if (i != 1)
	statusbar(_("Cancelled"));

    display_main_list();
}

void signal_init(void)
{
    /* Trap SIGINT and SIGQUIT because we want them to do useful
     * things. */
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = SIG_IGN;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    /* Trap SIGHUP and SIGTERM because we want to write the file out. */
    act.sa_handler = handle_hupterm;
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

#ifndef NANO_SMALL
    /* Trap SIGWINCH because we want to handle window resizes. */
    act.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &act, NULL);
    allow_pending_sigwinch(FALSE);
#endif

    /* Trap normal suspend (^Z) so we can handle it ourselves. */
    if (!ISSET(SUSPEND)) {
	act.sa_handler = SIG_IGN;
	sigaction(SIGTSTP, &act, NULL);
    } else {
	/* Block all other signals in the suspend and continue handlers.
	 * If we don't do this, other stuff interrupts them! */
	sigfillset(&act.sa_mask);

	act.sa_handler = do_suspend;
	sigaction(SIGTSTP, &act, NULL);

	act.sa_handler = do_cont;
	sigaction(SIGCONT, &act, NULL);
    }
}

/* Handler for SIGHUP (hangup) and SIGTERM (terminate). */
RETSIGTYPE handle_hupterm(int signal)
{
    die(_("Received SIGHUP or SIGTERM\n"));
}

/* Handler for SIGTSTP (suspend). */
RETSIGTYPE do_suspend(int signal)
{
    endwin();
    printf("\n\n\n\n\n%s\n", _("Use \"fg\" to return to nano"));
    fflush(stdout);

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

    /* Trap SIGHUP and SIGTERM so we can properly deal with them while
     * suspended. */
    act.sa_handler = handle_hupterm;
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    /* Do what mutt does: send ourselves a SIGSTOP. */
    kill(0, SIGSTOP);
}

/* Handler for SIGCONT (continue after suspend). */
RETSIGTYPE do_cont(int signal)
{
#ifndef NANO_SMALL
    /* Perhaps the user resized the window while we slept.  Handle it
     * and update the screen in the process. */
    handle_sigwinch(0);
#else
    /* Just update the screen. */
    doupdate();
#endif
}

#ifndef NANO_SMALL
void handle_sigwinch(int s)
{
    const char *tty = ttyname(0);
    int fd;
    int result = 0;
    struct winsize win;

    if (tty == NULL)
	return;
    fd = open(tty, O_RDWR);
    if (fd == -1)
	return;
    result = ioctl(fd, TIOCGWINSZ, &win);
    close(fd);
    if (result == -1)
	return;

    /* Could check whether the COLS or LINES changed, and return
     * otherwise.  EXCEPT, that COLS and LINES are ncurses global
     * variables, and in some cases ncurses has already updated them. 
     * But not in all cases, argh. */
    COLS = win.ws_col;
    LINES = win.ws_row;
    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS || COLS < MIN_EDITOR_COLS)
	die_too_small();

#ifndef DISABLE_WRAPJUSTIFY
    fill = wrap_at;
    if (fill <= 0)
	fill += COLS;
    if (fill < 0)
	fill = 0;
#endif

    hblank = charealloc(hblank, COLS + 1);
    memset(hblank, ' ', COLS);
    hblank[COLS] = '\0';

    /* If we've partitioned the filestruct, unpartition it now. */
    if (filepart != NULL)
	unpartition_filestruct(&filepart);

#ifndef DISABLE_JUSTIFY
    /* If the justify buffer isn't empty, blow it away and display the
     * shortcut list with UnCut. */
    if (jusbuffer != NULL) {
	free_filestruct(jusbuffer);
	jusbuffer = NULL;
	shortcut_init(FALSE);
    }
#endif

#ifdef USE_SLANG
    /* Slang curses emulation brain damage, part 1: If we just do what
     * curses does here, it'll only work properly if the resize made the
     * window smaller.  Do what mutt does: Leave and immediately reenter
     * Slang screen management mode. */
    SLsmg_reset_smg();
    SLsmg_init_smg();
#else
    /* Do the equivalent of what Minimum Profit does: Leave and
     * immediately reenter curses mode. */
    endwin();
    refresh();
#endif

    /* Restore the terminal to its previous state. */
    terminal_init();

    /* Do the equivalent of what both mutt and Minimum Profit do:
     * Reinitialize all the windows based on the new screen
     * dimensions. */
    window_init();

    /* Redraw the contents of the windows that need it. */
    blank_statusbar();
    display_main_list();
    total_refresh();

    /* Turn cursor back on for sure. */
    curs_set(1);

    /* Reset all the input routines that rely on character sequences. */
    reset_kbinput();

    /* Jump back to the main loop. */
    siglongjmp(jmpbuf, 1);
}

void allow_pending_sigwinch(bool allow)
{
    sigset_t winch;
    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);
    if (allow)
	sigprocmask(SIG_UNBLOCK, &winch, NULL);
    else
	sigprocmask(SIG_BLOCK, &winch, NULL);
}
#endif /* !NANO_SMALL */

#ifndef NANO_SMALL
void do_toggle(const toggle *which)
{
    bool enabled;

    /* Even easier! */
    TOGGLE(which->flag);

    switch (which->val) {
    case TOGGLE_SUSPEND_KEY:
	signal_init();
	break;
#ifndef DISABLE_MOUSE
    case TOGGLE_MOUSE_KEY:
	mouse_init();
	break;
#endif
    case TOGGLE_NOHELP_KEY:
	blank_statusbar();
	blank_bottombars();
	wrefresh(bottomwin);
	window_init();
	edit_refresh();
	display_main_list();
	break;
#ifdef ENABLE_COLOR
    case TOGGLE_SYNTAX_KEY:
	edit_refresh();
	break;
#endif
#ifdef ENABLE_NANORC
    case TOGGLE_WHITESPACE_KEY:
	edit_refresh();
	break;
#endif
    }

    /* We are assuming here that shortcut_init() above didn't free and
     * reallocate the toggles. */
    enabled = ISSET(which->flag);
    if (which->val == TOGGLE_NOHELP_KEY || which->val == TOGGLE_WRAP_KEY)
	enabled = !enabled;
    statusbar("%s %s", which->desc,
		enabled ? _("enabled") : _("disabled"));
}
#endif /* !NANO_SMALL */

void disable_extended_input(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_lflag &= ~IEXTEN;
    tcsetattr(0, TCSANOW, &term);
}

void disable_signals(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_lflag &= ~ISIG;
    tcsetattr(0, TCSANOW, &term);
}

#ifndef NANO_SMALL
void enable_signals(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_lflag |= ISIG;
    tcsetattr(0, TCSANOW, &term);
}
#endif

void disable_flow_control(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_iflag &= ~(IXON|IXOFF);
    tcsetattr(0, TCSANOW, &term);
}

void enable_flow_control(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_iflag |= (IXON|IXOFF);
    tcsetattr(0, TCSANOW, &term);
}

/* Set up the terminal state.  Put the terminal in cbreak mode (read one
 * character at a time and interpret the special control keys), disable
 * translation of carriage return (^M) into newline (^J) so that we can
 * tell the difference between the Enter key and Ctrl-J, and disable
 * echoing of characters as they're typed.  Finally, disable extended
 * input processing, disable interpretation of the special control keys,
 * and if we're not in preserve mode, disable interpretation of the flow
 * control characters too. */
void terminal_init(void)
{
    cbreak();
    nonl();
    noecho();
    disable_extended_input();
    disable_signals();
    if (!ISSET(PRESERVE))
	disable_flow_control();
}

int main(int argc, char **argv)
{
    int optchr;
    int startline = 0;
	/* Line to try and start at. */
#ifndef DISABLE_WRAPJUSTIFY
    bool fill_flag_used = FALSE;
	/* Was the fill option used? */
#endif
#ifdef ENABLE_MULTIBUFFER
    bool old_multibuffer;
	/* The old value of the multibuffer option, restored after we
	 * load all files on the command line. */
#endif
    int kbinput;
	/* Input from keyboard. */
    bool meta_key, func_key;
#ifdef HAVE_GETOPT_LONG
    const struct option long_options[] = {
	{"help", 0, 0, 'h'},
#ifdef ENABLE_MULTIBUFFER
	{"multibuffer", 0, 0, 'F'},
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
	{"historylog", 0, 0, 'H'},
#endif
	{"ignorercfiles", 0, 0, 'I'},
#endif
#ifndef DISABLE_JUSTIFY
	{"quotestr", 1, 0, 'Q'},
#endif
#ifdef HAVE_REGEX_H
	{"regexp", 0, 0, 'R'},
#endif
	{"tabsize", 1, 0, 'T'},
	{"version", 0, 0, 'V'},
#ifdef ENABLE_COLOR
	{"syntax", 1, 0, 'Y'},
#endif
	{"const", 0, 0, 'c'},
	{"rebinddelete", 0, 0, 'd'},
	{"nofollow", 0, 0, 'l'},
#ifndef DISABLE_MOUSE
	{"mouse", 0, 0, 'm'},
#endif
#ifndef DISABLE_OPERATINGDIR
	{"operatingdir", 1, 0, 'o'},
#endif
	{"preserve", 0, 0, 'p'},
#ifndef DISABLE_WRAPJUSTIFY
	{"fill", 1, 0, 'r'},
#endif
#ifndef DISABLE_SPELLER
	{"speller", 1, 0, 's'},
#endif
	{"tempfile", 0, 0, 't'},
	{"view", 0, 0, 'v'},
#ifndef DISABLE_WRAPPING
	{"nowrap", 0, 0, 'w'},
#endif
	{"nohelp", 0, 0, 'x'},
	{"suspend", 0, 0, 'z'},
#ifndef NANO_SMALL
	{"smarthome", 0, 0, 'A'},
	{"backup", 0, 0, 'B'},
	{"backupdir", 1, 0, 'E'},
	{"noconvert", 0, 0, 'N'},
	{"smooth", 0, 0, 'S'},
	{"restricted", 0, 0, 'Z'},
	{"autoindent", 0, 0, 'i'},
	{"cut", 0, 0, 'k'},
#endif
	{0, 0, 0, 0}
    };
#endif

    setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

#if !defined(ENABLE_NANORC) && defined(DISABLE_ROOTWRAP) && !defined(DISABLE_WRAPPING)
    /* If we don't have rcfile support, we're root, and
     * --disable-wrapping-as-root is used, turn wrapping off. */
    if (geteuid() == NANO_ROOT_UID)
	SET(NO_WRAP);
#endif

    while ((optchr =
#ifdef HAVE_GETOPT_LONG
	getopt_long(argc, argv, "h?ABE:FHINQ:RST:VY:Zabcdefgijklmo:pr:s:tvwxz", long_options, NULL)
#else
	getopt(argc, argv, "h?ABE:FHINQ:RST:VY:Zabcdefgijklmo:pr:s:tvwxz")
#endif
		) != -1) {

	switch (optchr) {
	    case 'a':
	    case 'b':
	    case 'e':
	    case 'f':
	    case 'g':
	    case 'j':
		/* Pico compatibility flags. */
		break;
#ifndef NANO_SMALL
	    case 'A':
		SET(SMART_HOME);
		break;
	    case 'B':
		SET(BACKUP_FILE);
		break;
	    case 'E':
		backup_dir = mallocstrcpy(backup_dir, optarg);
		break;
#endif
#ifdef ENABLE_MULTIBUFFER
	    case 'F':
		SET(MULTIBUFFER);
		break;
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
	    case 'H':
		SET(HISTORYLOG);
		break;
#endif
	    case 'I':
		SET(NO_RCFILE);
		break;
#endif
#ifndef NANO_SMALL
	    case 'N':
		SET(NO_CONVERT);
		break;
#endif
#ifndef DISABLE_JUSTIFY
	    case 'Q':
		quotestr = mallocstrcpy(quotestr, optarg);
		break;
#endif
#ifdef HAVE_REGEX_H
	    case 'R':
		SET(USE_REGEXP);
		break;
#endif
#ifndef NANO_SMALL
	    case 'S':
		SET(SMOOTHSCROLL);
		break;
#endif
	    case 'T':
		if (!parse_num(optarg, &tabsize) || tabsize <= 0) {
		    fprintf(stderr, _("Requested tab size %s invalid"), optarg);
		    fprintf(stderr, "\n");
		    exit(1);
		}
		break;
	    case 'V':
		version();
		exit(0);
#ifdef ENABLE_COLOR
	    case 'Y':
		syntaxstr = mallocstrcpy(syntaxstr, optarg);
		break;
#endif
	    case 'Z':
		SET(RESTRICTED);
		break;
	    case 'c':
		SET(CONSTUPDATE);
		break;
	    case 'd':
		SET(REBIND_DELETE);
		break;
#ifndef NANO_SMALL
	    case 'i':
		SET(AUTOINDENT);
		break;
	    case 'k':
		SET(CUT_TO_END);
		break;
#endif
	    case 'l':
		SET(NOFOLLOW_SYMLINKS);
		break;
#ifndef DISABLE_MOUSE
	    case 'm':
		SET(USE_MOUSE);
		break;
#endif
#ifndef DISABLE_OPERATINGDIR
	    case 'o':
		operating_dir = mallocstrcpy(operating_dir, optarg);
		break;
#endif
	    case 'p':
		SET(PRESERVE);
		break;
#ifndef DISABLE_WRAPJUSTIFY
	    case 'r':
		if (!parse_num(optarg, &wrap_at)) {
		    fprintf(stderr, _("Requested fill size %s invalid"), optarg);
		    fprintf(stderr, "\n");
		    exit(1);
		}
		fill_flag_used = TRUE;
		break;
#endif
#ifndef DISABLE_SPELLER
	    case 's':
		alt_speller = mallocstrcpy(alt_speller, optarg);
		break;
#endif
	    case 't':
		SET(TEMP_FILE);
		break;
	    case 'v':
		SET(VIEW_MODE);
		break;
#ifndef DISABLE_WRAPPING
	    case 'w':
		SET(NO_WRAP);
		break;
#endif
	    case 'x':
		SET(NO_HELP);
		break;
	    case 'z':
		SET(SUSPEND);
		break;
	    default:
		usage();
	}
    }

    /* If the executable filename starts with 'r', we use restricted
     * mode. */
    if (*(tail(argv[0])) == 'r')
	SET(RESTRICTED);

    /* If we're using restricted mode, disable suspending, backups, and
     * reading rcfiles, since they all would allow reading from or
     * writing to files not specified on the command line. */
    if (ISSET(RESTRICTED)) {
	UNSET(SUSPEND);
	UNSET(BACKUP_FILE);
	SET(NO_RCFILE);
    }

/* We've read through the command line options.  Now back up the flags
 * and values that are set, and read the rcfile(s).  If the values
 * haven't changed afterward, restore the backed-up values. */
#ifdef ENABLE_NANORC
    if (!ISSET(NO_RCFILE)) {
#ifndef DISABLE_OPERATINGDIR
	char *operating_dir_cpy = operating_dir;
#endif
#ifndef DISABLE_WRAPJUSTIFY
	ssize_t wrap_at_cpy = wrap_at;
#endif
#ifndef NANO_SMALL
	char *backup_dir_cpy = backup_dir;
#endif
#ifndef DISABLE_JUSTIFY
	char *quotestr_cpy = quotestr;
#endif
#ifndef DISABLE_SPELLER
	char *alt_speller_cpy = alt_speller;
#endif
	ssize_t tabsize_cpy = tabsize;
	long flags_cpy = flags;

#ifndef DISABLE_OPERATINGDIR
	operating_dir = NULL;
#endif
#ifndef NANO_SMALL
	backup_dir = NULL;
#endif
#ifndef DISABLE_JUSTIFY
	quotestr = NULL;
#endif
#ifndef DISABLE_SPELLER
	alt_speller = NULL;
#endif

	do_rcfile();

#ifndef DISABLE_OPERATINGDIR
	if (operating_dir_cpy != NULL) {
	    free(operating_dir);
	    operating_dir = operating_dir_cpy;
	}
#endif
#ifndef DISABLE_WRAPJUSTIFY
	if (fill_flag_used)
	    wrap_at = wrap_at_cpy;
#endif
#ifndef NANO_SMALL
	if (backup_dir_cpy != NULL) {
	    free(backup_dir);
	    backup_dir = backup_dir_cpy;
	}
#endif	
#ifndef DISABLE_JUSTIFY
	if (quotestr_cpy != NULL) {
	    free(quotestr);
	    quotestr = quotestr_cpy;
	}
#endif
#ifndef DISABLE_SPELLER
	if (alt_speller_cpy != NULL) {
	    free(alt_speller);
	    alt_speller = alt_speller_cpy;
	}
#endif
	if (tabsize_cpy != -1)
	    tabsize = tabsize_cpy;
	flags |= flags_cpy;
    }
#if defined(DISABLE_ROOTWRAP) && !defined(DISABLE_WRAPPING)
    else if (geteuid() == NANO_ROOT_UID)
	SET(NO_WRAP);
#endif
#endif /* ENABLE_NANORC */

#ifndef NANO_SMALL
    history_init();
#ifdef ENABLE_NANORC
    if (!ISSET(NO_RCFILE) && ISSET(HISTORYLOG))
	load_history();
#endif
#endif

#ifndef NANO_SMALL
    /* Set up the backup directory (unless we're using restricted mode,
     * in which case backups are disabled, since they would allow
     * reading from or writing to files not specified on the command
     * line).  This entails making sure it exists and is a directory, so
     * that backup files will be saved there. */
    if (!ISSET(RESTRICTED))
	init_backup_dir();
#endif

#ifndef DISABLE_OPERATINGDIR
    /* Set up the operating directory.  This entails chdir()ing there,
     * so that file reads and writes will be based there. */
    init_operating_dir();
#endif

#ifndef DISABLE_JUSTIFY
    if (punct == NULL)
	punct = mallocstrcpy(punct, ".?!");

    if (brackets == NULL)
	brackets = mallocstrcpy(brackets, "'\")}]>");

    if (quotestr == NULL)
	quotestr = mallocstrcpy(NULL,
#ifdef HAVE_REGEX_H
		"^([ \t]*[|>:}#])+"
#else
		"> "
#endif
		);
#ifdef HAVE_REGEX_H
    quoterc = regcomp(&quotereg, quotestr, REG_EXTENDED);

    if (quoterc == 0) {
	/* We no longer need quotestr, just quotereg. */
	free(quotestr);
	quotestr = NULL;
    } else {
	size_t size = regerror(quoterc, &quotereg, NULL, 0);

	quoteerr = charalloc(size);
	regerror(quoterc, &quotereg, quoteerr, size);
    }
#else
    quotelen = strlen(quotestr);
#endif /* !HAVE_REGEX_H */
#endif /* !DISABLE_JUSTIFY */

#ifndef DISABLE_SPELLER
    /* If we don't have an alternative spell checker after reading the
     * command line and/or rcfile(s), check $SPELL for one, as Pico
     * does (unless we're using restricted mode, in which case spell
     * checking is disabled, since it would allow reading from or
     * writing to files not specified on the command line). */
    if (!ISSET(RESTRICTED) && alt_speller == NULL) {
	char *spellenv = getenv("SPELL");
	if (spellenv != NULL)
	    alt_speller = mallocstrcpy(NULL, spellenv);
    }
#endif

#if !defined(NANO_SMALL) && defined(ENABLE_NANORC)
    /* If whitespace wasn't specified, set its default value. */
    if (whitespace == NULL)
	whitespace = mallocstrcpy(NULL, "  ");
#endif

    /* If tabsize wasn't specified, set its default value. */
    if (tabsize == -1)
	tabsize = WIDTH_OF_TAB;

    /* Back up the old terminal settings so that they can be restored. */
    tcgetattr(0, &oldterm);

    /* Curses initialization stuff: Start curses and set up the
     * terminal state. */
    initscr();
    terminal_init();

    /* Set up the global variables and the shortcuts. */
    global_init(FALSE);
    shortcut_init(FALSE);

    /* Set up the signal handlers. */
    signal_init();

#ifdef DEBUG
    fprintf(stderr, "Main: set up windows\n");
#endif

    window_init();
#ifndef DISABLE_MOUSE
    mouse_init();
#endif

#ifdef DEBUG
    fprintf(stderr, "Main: open file\n");
#endif

    /* If there's a +LINE flag here, it is the first non-option
     * argument, and it is followed by at least one other argument, the
     * filename it applies to. */
    if (0 < optind && optind < argc - 1 && argv[optind][0] == '+') {
	startline = atoi(&argv[optind][1]);
	optind++;
    }

#ifdef ENABLE_MULTIBUFFER
    old_multibuffer = ISSET(MULTIBUFFER);
    SET(MULTIBUFFER);

    /* Read all the files after the first one on the command line into
     * new buffers. */
    {
	int i = optind + 1, iline = 0;
	for (; i < argc; i++) {
	    /* If there's a +LINE flag here, it is followed by at least
	     * one other argument, the filename it applies to. */
	    if (i < argc - 1 && argv[i][0] == '+' && iline == 0) {
		iline = atoi(&argv[i][1]);
	    } else {
		load_buffer(argv[i]);
		if (iline > 0) {
		    do_gotoline(iline, FALSE);
		    iline = 0;
		}
	    }
	}
    }
#endif

    /* Read the first file on the command line into either the current
     * buffer or a new buffer, depending on whether multibuffer mode is
     * enabled. */
    if (optind < argc)
	load_buffer(argv[optind]);

    /* We didn't open any files if all the command line arguments were
     * invalid files like directories or if there were no command line
     * arguments given.  In this case, we have to load a blank buffer.
     * Also, we unset view mode to allow editing. */
    if (filename == NULL) {
	filename = mallocstrcpy(NULL, "");
	new_file();
	UNSET(VIEW_MODE);

	/* Add this new entry to the open_files structure if we have
        * multibuffer support, or to the main filestruct if we don't. */
	load_file();
    }

#ifdef ENABLE_MULTIBUFFER
    if (!old_multibuffer)
	UNSET(MULTIBUFFER);
#endif

#ifdef DEBUG
    fprintf(stderr, "Main: top and bottom win\n");
#endif

    titlebar(NULL);
    display_main_list();

    if (startline > 0)
	do_gotoline(startline, FALSE);

#ifndef NANO_SMALL
    /* Return here after a SIGWINCH. */
    sigsetjmp(jmpbuf, 1);
#endif

    edit_refresh();

    while (TRUE) {
	reset_cursor();
	if (ISSET(CONSTUPDATE))
	    do_cursorpos(TRUE);

#if !defined(DISABLE_BROWSER) || !defined(DISABLE_HELP) || !defined(DISABLE_MOUSE)
	currshortcut = main_list;
#endif

	kbinput = get_edit_input(&meta_key, &func_key);

	/* Last gasp, stuff that's not in the main lists. */
	if (kbinput != ERR && !is_cntrl_char(kbinput)) {
	    /* Don't stop unhandled printable sequences, so that people
	     * with odd character sets can type. */
	    if (ISSET(VIEW_MODE))
		print_view_warning();
	    else
		do_char(kbinput);
	}
    }
    assert(FALSE);
}
