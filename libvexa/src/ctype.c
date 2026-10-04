/* The <ctype.h> functions as real functions, for code built against other
 * headers (musl's regex and fnmatch, in libvexa) or that takes their
 * addresses: <ctype.h> itself has them inline. (So it isn't included.) */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int isgraph(int c) { return c > 0x20 && c < 0x7f; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isascii(int c) { return c >= 0 && c < 0x80; }
int toascii(int c) { return c & 0x7f; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
