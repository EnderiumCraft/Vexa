/*
 * vsh: the Vexa shell.
 *
 *   command arguments...        run a program (found in $PATH, or by path)
 *   a | b | c                   pipes: each one's output is the next one's input
 *   command < in > out >> log   redirection (2> for errors)
 *   command &                   run in the background
 *   a ; b                       one after the other
 *   'text' "text $HOME" \x      quoting; $NAME expands environment variables,
 *                               $? the last exit code
 *
 * Built in: cd, exit, export, unset, env, help. Ctrl+C stops the running
 * program, not the shell.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

#define MAX_WORDS 128
#define MAX_JOBS 32
#define LINE_MAX 1024

enum token_kind { WORD, PIPE, LESS, GREATER, APPEND, ERROR_GREATER, AMPERSAND, SEMICOLON, END };

struct token {
    enum token_kind kind;
    char *text; /* For words. */
};

static int last_status;
static int background[MAX_JOBS]; /* Process handles of background jobs. */
static long background_ids[MAX_JOBS];

/* ---- Reading and splitting the line ---- */

static char *expand_variable(const char **p) {
    const char *start = *p;
    if (*start == '?') {
        *p = start + 1;
        static char status[16];
        snprintf(status, sizeof(status), "%d", last_status);
        return status;
    }
    size_t n = 0;
    while (isalnum(start[n]) || start[n] == '_') {
        n++;
    }
    *p = start + n;
    if (n == 0) {
        return "$";
    }
    char name[64];
    if (n >= sizeof(name)) {
        n = sizeof(name) - 1;
    }
    memcpy(name, start, n);
    name[n] = '\0';
    char *value = getenv(name);
    return value ? value : "";
}

/* Splits a line into words and operators. Returns the count, or -1 on a
 * quoting error. Words are allocated. */
static int tokenize(const char *line, struct token *tokens, int max) {
    int count = 0;
    const char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (!*p || *p == '#' || count == max - 1) {
            break;
        }
        struct token *t = &tokens[count++];
        t->text = NULL;
        if (*p == '|') { t->kind = PIPE; p++; continue; }
        if (*p == '&') { t->kind = AMPERSAND; p++; continue; }
        if (*p == ';') { t->kind = SEMICOLON; p++; continue; }
        if (*p == '<') { t->kind = LESS; p++; continue; }
        if (*p == '>') {
            t->kind = p[1] == '>' ? APPEND : GREATER;
            p += t->kind == APPEND ? 2 : 1;
            continue;
        }
        if (p[0] == '2' && p[1] == '>') {
            t->kind = ERROR_GREATER;
            p += 2;
            continue;
        }
        /* A word, with quotes and $variables. */
        t->kind = WORD;
        char word[LINE_MAX];
        size_t n = 0;
        while (*p && !strchr(" \t\n\r|&;<>", *p) && n < sizeof(word) - 1) {
            if (*p == '\'') {
                const char *close = strchr(p + 1, '\'');
                if (!close) {
                    return -1;
                }
                for (p++; p < close && n < sizeof(word) - 1; p++) {
                    word[n++] = *p;
                }
                p++;
            } else if (*p == '"') {
                for (p++; *p && *p != '"' && n < sizeof(word) - 1;) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        word[n++] = p[1];
                        p += 2;
                    } else if (*p == '$') {
                        p++;
                        for (const char *v = expand_variable(&p); *v && n < sizeof(word) - 1; v++) {
                            word[n++] = *v;
                        }
                    } else {
                        word[n++] = *p++;
                    }
                }
                if (*p != '"') {
                    return -1;
                }
                p++;
            } else if (*p == '\\' && p[1]) {
                word[n++] = p[1];
                p += 2;
            } else if (*p == '$') {
                p++;
                for (const char *v = expand_variable(&p); *v && n < sizeof(word) - 1; v++) {
                    word[n++] = *v;
                }
            } else {
                word[n++] = *p++;
            }
        }
        word[n] = '\0';
        t->text = strdup(word);
    }
    tokens[count].kind = END;
    return count;
}

/* ---- Running programs ---- */

struct command {
    char *argv[MAX_WORDS];
    int argc;
    char *input, *output, *errors;
    bool append;
};

/* Finds a program: a path as given if it contains '/', else in $PATH. */
static bool find_program(const char *name, char *path, size_t size) {
    struct vx_stat st;
    if (strchr(name, '/')) {
        snprintf(path, size, "%s", name);
        return vx_stat(path, &st) == 0;
    }
    const char *search = getenv("PATH");
    if (!search) {
        search = "/bin";
    }
    while (*search) {
        size_t n = strcspn(search, ":");
        snprintf(path, size, "%.*s/%s", (int)n, search, name);
        if (vx_stat(path, &st) == 0 && st.type == VX_TYPE_FILE) {
            return true;
        }
        search += n;
        if (*search == ':') {
            search++;
        }
    }
    return false;
}

static int open_redirect(const char *path, unsigned flags) {
    int handle = vx_open(path, flags);
    if (handle < 0) {
        fprintf(stderr, "vsh: %s: %s\n", path, vx_strerror(handle));
    }
    return handle;
}

static void close_if(int handle) {
    if (handle > 2) {
        vx_close(handle);
    }
}

/* Runs a pipeline of `count` commands. Returns the last one's exit code. */
static int run_pipeline(struct command *commands, int count, bool in_background) {
    int processes[16];
    int started = 0;
    unsigned group = 0;
    int previous_read = 0; /* Input for the next command: stdin to begin with. */
    int status = 0;

    size_t envc = 0;
    while (environ[envc]) {
        envc++;
    }
    for (int i = 0; i < count && i < 16; i++) {
        struct command *c = &commands[i];
        int in = previous_read, out = 1, err = 2;
        int pipe_handles[2] = {-1, -1};
        if (i + 1 < count) {
            if (vx_pipe(pipe_handles) < 0) {
                fprintf(stderr, "vsh: cannot make a pipe\n");
                break;
            }
            out = pipe_handles[1];
        }
        if (c->input && (in = open_redirect(c->input, VX_OPEN_READ)) < 0) {
            status = 1;
            close_if(pipe_handles[0]);
            close_if(pipe_handles[1]);
            break;
        }
        unsigned create = VX_OPEN_WRITE | VX_OPEN_CREATE | (c->append ? VX_OPEN_APPEND : VX_OPEN_TRUNCATE);
        if (c->output && (out = open_redirect(c->output, create)) < 0) {
            status = 1;
            break;
        }
        if (c->errors && (err = open_redirect(c->errors, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE)) < 0) {
            status = 1;
            break;
        }

        char path[512];
        int process = -1;
        if (!find_program(c->argv[0], path, sizeof(path))) {
            fprintf(stderr, "vsh: %s: command not found\n", c->argv[0]);
            status = 127;
        } else {
            struct vx_spawn spawn = {
                .argv = (const char *const *)c->argv, .argc = (unsigned long)c->argc,
                .envp = (const char *const *)environ, .envc = envc,
                .handles = {in, out, err},
                .flags = group ? VX_SPAWN_JOIN_GROUP : VX_SPAWN_NEW_GROUP, .group = group,
            };
            process = vx_spawn(path, &spawn);
            if (process < 0) {
                fprintf(stderr, "vsh: %s: %s\n", c->argv[0], vx_strerror(process));
                status = 126;
            }
        }
        /* The children have their own references to these now. */
        if (in != previous_read) {
            close_if(in);
        }
        close_if(previous_read);
        if (out != pipe_handles[1]) {
            close_if(out);
        }
        close_if(pipe_handles[1]);
        close_if(err);
        previous_read = pipe_handles[0] >= 0 ? pipe_handles[0] : 0;
        if (process >= 0) {
            if (!group) {
                /* Every program in the pipeline joins the first one's group,
                 * so Ctrl+C reaches all of them. Its id is the group id. */
                group = (unsigned)vx_handle_process_id(process);
            }
            processes[started++] = process;
        }
    }
    close_if(previous_read);

    if (in_background) {
        for (int i = 0; i < started; i++) {
            for (int j = 0; j < MAX_JOBS; j++) {
                if (background[j] == 0) {
                    background[j] = processes[i];
                    background_ids[j] = group;
                    break;
                }
            }
        }
        if (started) {
            printf("[%u] started in the background\n", group);
        }
        return 0;
    }

    if (group) {
        vx_set_foreground(group);
    }
    for (int i = 0; i < started; i++) {
        long code = vx_wait(processes[i], 0);
        while (code == -VX_EINTR) {
            code = vx_wait(processes[i], 0);
        }
        vx_close(processes[i]);
        status = (int)code;
    }
    /* Back to the shell. vsh's group id is its own process id. */
    vx_set_foreground(vx_process_id());
    if (status > 128 && status - 128 == VX_SIGINT) {
        printf("\n");
    }
    return status;
}

static void check_background_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (background[i]) {
            long code = vx_wait(background[i], VX_WAIT_NO_HANG);
            if (code != -VX_EAGAIN) {
                vx_close(background[i]);
                printf("[%ld] finished (exit code %ld)\n", background_ids[i], code);
                background[i] = 0;
                background_ids[i] = 0;
            }
        }
    }
}

/* ---- Built-in commands ---- */

static void builtin_help(void) {
    printf("vsh, the Vexa shell\n"
           "  program args...     run a program from $PATH (%s)\n"
           "  a | b               pipe a's output into b\n"
           "  < in, > out, >> log, 2> errors   redirection\n"
           "  command &           run in the background\n"
           "  a ; b               run one after the other\n"
           "built in: cd [dir], exit [code], export NAME=value, unset NAME, env, help\n"
           "programs: ls /bin\n",
           getenv("PATH") ? getenv("PATH") : "/bin");
}

/* Returns true if the command was built in (and ran it). */
static bool run_builtin(struct command *c) {
    const char *name = c->argv[0];
    if (strcmp(name, "cd") == 0) {
        const char *target = c->argc > 1 ? c->argv[1] : (getenv("HOME") ? getenv("HOME") : "/");
        long error = vx_chdir(target);
        if (error) {
            fprintf(stderr, "cd: %s: %s\n", target, vx_strerror(error));
        }
        last_status = error ? 1 : 0;
        return true;
    }
    if (strcmp(name, "exit") == 0) {
        exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
    }
    if (strcmp(name, "export") == 0) {
        for (int i = 1; i < c->argc; i++) {
            char *equals = strchr(c->argv[i], '=');
            if (equals) {
                *equals = '\0';
                setenv(c->argv[i], equals + 1, 1);
            }
        }
        return true;
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < c->argc; i++) {
            unsetenv(c->argv[i]);
        }
        return true;
    }
    if (strcmp(name, "env") == 0 && c->argc == 1 && !c->output) {
        for (char **e = environ; *e; e++) {
            printf("%s\n", *e);
        }
        return true;
    }
    if (strcmp(name, "help") == 0) {
        builtin_help();
        return true;
    }
    return false;
}

/* Parses and runs one line. */
static void run_line(const char *line) {
    static struct token tokens[MAX_WORDS * 2];
    int count = tokenize(line, tokens, MAX_WORDS * 2);
    if (count < 0) {
        fprintf(stderr, "vsh: unmatched quote\n");
        return;
    }
    static struct command commands[16];
    int position = 0;
    while (position < count) {
        int n = 0;
        memset(commands, 0, sizeof(commands));
        bool in_background = false, error = false;
        /* One pipeline, up to ';', '&' or the end. */
        while (position < count && n < 16) {
            struct command *c = &commands[n];
            while (position < count && tokens[position].kind != PIPE &&
                   tokens[position].kind != SEMICOLON && tokens[position].kind != AMPERSAND) {
                struct token *t = &tokens[position++];
                if (t->kind == WORD) {
                    if (c->argc < MAX_WORDS - 1) {
                        c->argv[c->argc++] = t->text;
                    }
                    continue;
                }
                if (position >= count || tokens[position].kind != WORD) {
                    fprintf(stderr, "vsh: a redirection needs a file name\n");
                    error = true;
                    break;
                }
                char *file = tokens[position++].text;
                if (t->kind == LESS) c->input = file;
                else if (t->kind == ERROR_GREATER) c->errors = file;
                else {
                    c->output = file;
                    c->append = t->kind == APPEND;
                }
            }
            c->argv[c->argc] = NULL;
            if (c->argc == 0 && !error) {
                if (n > 0 || (position < count && tokens[position].kind == PIPE)) {
                    fprintf(stderr, "vsh: empty command in a pipeline\n");
                    error = true;
                }
                break;
            }
            n++;
            if (position < count && tokens[position].kind == PIPE) {
                position++;
                continue;
            }
            break;
        }
        if (position < count && tokens[position].kind == AMPERSAND) {
            in_background = true;
        }
        if (position < count) {
            position++; /* The ';' or '&'. */
        }
        if (error || n == 0) {
            last_status = error ? 2 : last_status;
            if (error) {
                break;
            }
            continue;
        }
        if (n == 1 && !in_background && run_builtin(&commands[0])) {
            continue;
        }
        last_status = run_pipeline(commands, n, in_background);
    }
    for (int i = 0; i < count; i++) {
        free(tokens[i].text);
    }
}

int main(int argc, char **argv) {
    vx_signal(VX_SIGINT, VX_SIGNAL_IGNORE);
    vx_signal(VX_SIGQUIT, VX_SIGNAL_IGNORE);
    vx_set_foreground(vx_process_id());

    /* vsh -c "command": run one line and exit. vsh file: run a script. */
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        run_line(argv[2]);
        return last_status;
    }
    FILE *input = stdin;
    bool interactive = true;
    if (argc > 1) {
        input = fopen(argv[1], "r");
        if (!input) {
            fprintf(stderr, "vsh: cannot open %s\n", argv[1]);
            return 1;
        }
        interactive = false;
    }

    char line[LINE_MAX];
    for (;;) {
        check_background_jobs();
        if (interactive) {
            char cwd[256] = "?";
            vx_getcwd(cwd, sizeof(cwd));
            printf("vexa:%s> ", cwd);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), input)) {
            if (interactive && !feof(input)) {
                continue; /* Interrupted. */
            }
            break; /* Ctrl+D or the end of the script. */
        }
        run_line(line);
    }
    if (interactive) {
        printf("\n");
    }
    return last_status;
}
