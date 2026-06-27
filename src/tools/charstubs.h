/* Stub file for ZIPIT_Z2 IZ2S and iz2jffs */
/* iz2jffs has only ncurses, not ncursesw.  IZ2S has old ncursesw */

#ifndef ZIPIT_CHARSTUBS_H
#define ZIPIT_CHARSTUBS_H

#ifdef USE_WIDE_CHAR
#include <wctype.h>

static char *wchars_to_utf8_str_realloc(char *input, wchar_t *wchars)
{
	int len = wcstombs(NULL, wchars, 0);
	if (len >= 0) {
		input = realloc(input, len + 1);
		if (input) {
			if (wcstombs(input, wchars, len) != len) {
				free(input);
				input = NULL;
			} else {
				input[len] = '\0';
			}
		}
	}
	return input;
}

#else /* USE_WIDE_CHAR */

#define wint_t int
#define wchar_t char
#define wcstombs(d, s, l) strlen(l)
#define wchars_to_utf8_str_realloc(i, wc) wc
#define wget_wch(w, c) ((*(c) = wgetch(w)) ? OK : OK)

#define WACS_VLINE   ACS_VLINE
#define WACS_DIAMOND ACS_DIAMOND
#define WACS_UARROW  ACS_UARROW
#define WACS_DARROW  ACS_DARROW
#define mvwadd_wchnstr(w, y, x, c, n) mvwaddch(w, y, x, c)
#define mvwvline_set(w, y, x, c, n) mvwvline(w, y, x, c, n)

#endif /* USE_WIDE_CHAR */

#endif
