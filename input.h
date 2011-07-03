/* Yash: yet another shell */
/* input.h: functions for input of command line */
/* (C) 2007-2011 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#ifndef YASH_INPUT_H
#define YASH_INPUT_H

#include <stdlib.h>
#include <wchar.h>


struct promptset_T {
    wchar_t *main, *right, *styler;
};

#define PROMPT_RESET L"\\fD"

extern struct promptset_T get_prompt(int type);
static inline void free_prompt(struct promptset_T prompt);
extern void print_prompt(const wchar_t *s)
    __attribute__((nonnull));
extern _Bool unset_nonblocking(int fd);


/* Frees the specified prompt set. */
void free_prompt(struct promptset_T prompt)
{
    free(prompt.main);
    free(prompt.right);
    free(prompt.styler);
}


/* The result type of `inputfunc_T'. */
typedef enum inputresult_T {
    INPUT_OK,           /* A line was read successfully. */
    INPUT_EOF,          /* The end of file was reached. */
    INPUT_INTERRUPTED,  /* SIGINT was received (interactive shell only) */
    INPUT_ERROR,        /* Other error was encountered. */
} inputresult_T;

struct xwcsbuf_T;
struct input_file_info_T;
extern inputresult_T read_input(
	struct xwcsbuf_T *buf, struct input_file_info_T *info, _Bool trap)
    __attribute__((nonnull));

/* The type of input functions.
 * Input is done line-wise: one line is read at a time.
 * The input string is terminated by newline L'\n' except when there is no
 * newline at the end of input.
 * The result is indicated by a value of the `inputresult_T' type. If the return
 * value is other than INPUT_OK, the buffer is unchanged.
 * The input function may be called even after it returned a non-zero. */
typedef inputresult_T inputfunc_T(struct xwcsbuf_T *buf, void *inputinfo);

/* input functions to be used as `inputfunc_T' */
extern inputresult_T input_wcs(struct xwcsbuf_T *buf, void *inputinfo)
    __attribute__((nonnull));
extern inputresult_T input_file(struct xwcsbuf_T *buf, void *inputinfo)
    __attribute__((nonnull));
extern inputresult_T input_interactive(struct xwcsbuf_T *buf, void *inputinfo)
    __attribute__((nonnull));

/* to be used as `inputinfo' for `input_wcs' */
struct input_wcs_info_T {
    const wchar_t *src;  /* the input source code */
};

/* to be used as `inputinfo' for `input_file' */
struct input_file_info_T {
    int fd;
    mbstate_t state;
    size_t bufpos, bufmax, bufsize;
    char buf[];
};
/* `bufsize' is the size of `buf', which must be at least one byte. */

/* to be used as `inputinfo' for `input_interactive' */
struct input_interactive_info_T {
    struct input_file_info_T *fileinfo;
    int prompttype;
#if YASH_ENABLE_LINEEDIT
    wchar_t *linebuffer;
#endif
};


#endif /* YASH_INPUT_H */


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
