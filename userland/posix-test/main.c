/* posix-test: checks libvexa's POSIX layer, the way a ported program uses it:
 * printf and scanf with floats, libm, strings and numbers, stdio files
 * (seeking, ungetc, getline), directories, stat, time, setjmp, pthreads
 * (mutexes, condition variables, keys, once, per-thread errno), semaphores,
 * mmap, popen, dup and dup2, posix_spawn with waitpid, dlopen, and BSD
 * sockets (UDP and TCP over the loopback interface, select, getaddrinfo). Prints
 * "posix-test: passed" or what failed. */
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <iconv.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <regex.h>
#include <semaphore.h>
#include <setjmp.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <wctype.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            printf("posix-test: FAILED line %d: %s\n", __LINE__, #condition);              \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

static void check_text(int line, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("posix-test: FAILED line %d: got \"%s\", want \"%s\"\n", line, got, want);
        failures++;
    }
}
#define CHECK_TEXT(got, want) check_text(__LINE__, got, want)

static bool near(double a, double b) {
    return fabs(a - b) <= 1e-12 * (fabs(b) > 1 ? fabs(b) : 1);
}

static void test_format(void) {
    char text[128];
    snprintf(text, sizeof text, "%.3f|%e|%g|%g|%g", 3.14159265, 12345.678, 0.0001, 1e20, 100.0);
    CHECK_TEXT(text, "3.142|1.234568e+04|0.0001|1e+20|100");
    snprintf(text, sizeof text, "%8.2f|%-8.1f|%+.0f|%a", -2.5, 2.25, 2.5, 1.0);
    CHECK_TEXT(text, "   -2.50|2.2     |+2|0x1p+0");
    snprintf(text, sizeof text, "%.17g|%Lf|%f|%F", 0.1, 1.5L, INFINITY, -NAN);
    CHECK_TEXT(text, "0.10000000000000001|1.500000|inf|-NAN");
    snprintf(text, sizeof text, "%lld|%llu|%#x|%05d|%n", -1234567890123LL, 18446744073709551615ULL,
             255, 42, &(int){0});
    CHECK_TEXT(text, "-1234567890123|18446744073709551615|0xff|00042|");
    char *joined = NULL;
    CHECK(asprintf(&joined, "%s-%d", "vexa", 26) == 7);
    CHECK_TEXT(joined, "vexa-26");
    free(joined);
}

static void test_numbers(void) {
    CHECK(strtod("0.1", NULL) == 0.1);
    CHECK(strtod("2.2250738585072014e-308", NULL) == 2.2250738585072014e-308);
    CHECK(strtod("123456789012345678", NULL) == 123456789012345678.0);
    CHECK(strtod("0x1.8p3", NULL) == 12.0);
    CHECK(isinf(strtod("inf", NULL)) && isnan(strtod("nan", NULL)));
    char *end;
    CHECK(strtoll("-0x7fffffffffffffff", &end, 0) == -0x7fffffffffffffffLL && *end == '\0');
    CHECK(strtoul("777", NULL, 8) == 511);
    /* Every double printed with %.17g reads back the same. */
    double values[] = {1.0 / 3, 6.02214076e23, 5e-324, 1.7976931348623157e308, 0.3};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        char text[64];
        snprintf(text, sizeof text, "%.17g", values[i]);
        CHECK(strtod(text, NULL) == values[i]);
    }

    int a = 0;
    unsigned b = 0;
    double d = 0;
    float f = 0;
    char word[16], rest[16];
    int n = sscanf("  42 0x1F 2.5e3 -0.75 hello world", "%d %x %lf %f %15s %15[a-z]", &a, &b, &d,
                   &f, word, rest);
    CHECK(n == 6 && a == 42 && b == 31 && d == 2500.0 && f == -0.75f);
    CHECK_TEXT(word, "hello");
    CHECK_TEXT(rest, "world");
    CHECK(sscanf("12:34", "%d:%d", &a, &n) == 2 && a == 12 && n == 34);
    CHECK(sscanf("", "%d", &a) == EOF);
    CHECK(sscanf("abc", "%d", &a) == 0);
}

static void test_math(void) {
    CHECK(near(sin(M_PI / 6), 0.5));
    CHECK(near(cos(M_PI), -1.0));
    CHECK(near(atan2(1, 1), M_PI / 4));
    CHECK(sqrt(2.0) == 1.4142135623730951);
    CHECK(near(pow(2, 0.5), M_SQRT2));
    CHECK(near(exp(1), M_E));
    CHECK(near(log(M_E * M_E), 2));
    CHECK(near(log10(1000), 3));
    CHECK(floor(-2.5) == -3 && ceil(-2.5) == -2 && round(2.5) == 3 && trunc(-2.7) == -2);
    CHECK(fmod(10, 3) == 1);
    CHECK(near(hypot(3, 4), 5));
    CHECK(fabsf(sinf(1.0f) - 0.84147098f) < 1e-6f);
    CHECK(lround(-3.5) == -4);
}

static void test_strings(void) {
    char text[] = "a,b,,c";
    char *save, *token = strtok_r(text, ",", &save);
    CHECK(token && strcmp(token, "a") == 0);
    token = strtok_r(NULL, ",", &save);
    CHECK(token && strcmp(token, "b") == 0);
    token = strtok_r(NULL, ",", &save);
    CHECK(token && strcmp(token, "c") == 0);
    CHECK(strcasecmp("Vexa", "vEXA") == 0);
    CHECK(strncasecmp("abcX", "ABCy", 3) == 0);
    CHECK(strstr("hello world", "o w") != NULL);
    char copy[8];
    CHECK(strlcpy(copy, "too long text", sizeof copy) == 13 && strcmp(copy, "too lon") == 0);
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);
}

static void test_files(void) {
    FILE *file = fopen("/tmp/posix-test.txt", "w+");
    CHECK(file != NULL);
    if (!file) {
        return;
    }
    fprintf(file, "first line\nsecond %d %.2f\nthird\n", 7, 1.25);
    CHECK(ftell(file) == 31);
    rewind(file);
    char *line = NULL;
    size_t size = 0;
    CHECK(getline(&line, &size, file) == 11);
    CHECK_TEXT(line, "first line\n");
    char word[16];
    int number = 0;
    float value = 0;
    CHECK(fscanf(file, "%15s %d %f", word, &number, &value) == 3);
    CHECK(strcmp(word, "second") == 0 && number == 7 && value == 1.25f);
    CHECK(fgetc(file) == '\n');
    int c = fgetc(file);
    CHECK(c == 't' && ungetc(c, file) == 't' && fgetc(file) == 't');
    CHECK(fseek(file, -3, SEEK_END) == 0 && fgetc(file) == 'r');
    CHECK(fseek(file, 6, SEEK_SET) == 0);
    fputs("LINE", file); /* Writing after reading. */
    rewind(file);
    CHECK(getline(&line, &size, file) == 11);
    CHECK_TEXT(line, "first LINE\n");
    free(line);
    CHECK(fclose(file) == 0);

    struct stat info;
    CHECK(stat("/tmp/posix-test.txt", &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 31);
    CHECK(stat("/tmp", &info) == 0 && S_ISDIR(info.st_mode));
    CHECK(stat("/tmp/not-there", &info) == -1 && errno == ENOENT);

    int fd = open("/tmp/posix-test.txt", O_RDONLY);
    char bytes[6] = {0};
    CHECK(fd >= 0 && pread(fd, bytes, 5, 6) == 5 && strcmp(bytes, "LINE\n") == 0);
    CHECK(open("/tmp/posix-test.txt", O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
    close(fd);

    CHECK(mkdir("/tmp/posix-dir", 0755) == 0);
    CHECK(close(open("/tmp/posix-dir/one", O_CREAT | O_WRONLY, 0644)) == 0);
    CHECK(close(open("/tmp/posix-dir/two", O_CREAT | O_WRONLY, 0644)) == 0);
    DIR *dir = opendir("/tmp/posix-dir");
    int found = 0;
    for (struct dirent *entry; dir && (entry = readdir(dir));) {
        if (strcmp(entry->d_name, "one") == 0 || strcmp(entry->d_name, "two") == 0) {
            found++;
        }
    }
    CHECK(dir && found == 2);
    if (dir) {
        closedir(dir);
    }
    CHECK(unlink("/tmp/posix-dir/one") == 0 && unlink("/tmp/posix-dir/two") == 0);
    CHECK(rmdir("/tmp/posix-dir") == 0 && access("/tmp/posix-dir", F_OK) == -1);
    unlink("/tmp/posix-test.txt");

    char cwd[PATH_MAX];
    CHECK(chdir("/etc") == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, "/etc") == 0);
    chdir("/");

    char path[] = "/tmp/posix-XXXXXX";
    fd = mkstemp(path);
    CHECK(fd >= 0 && strcmp(path, "/tmp/posix-XXXXXX") != 0);
    close(fd);
    unlink(path);
}

static void test_time(void) {
    struct timespec now, later;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    struct timespec pause = {0, 20 * 1000 * 1000};
    nanosleep(&pause, NULL);
    clock_gettime(CLOCK_MONOTONIC, &later);
    double elapsed = (later.tv_sec - now.tv_sec) + (later.tv_nsec - now.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.015 && elapsed < 2);
    CHECK(time(NULL) > 1700000000);

    time_t moment = 1700000000; /* 2023-11-14 22:13:20 UTC */
    struct tm parts;
    gmtime_r(&moment, &parts);
    CHECK(parts.tm_year == 123 && parts.tm_mon == 10 && parts.tm_mday == 14 && parts.tm_wday == 2);
    char text[64];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %a %b %j", &parts);
    CHECK_TEXT(text, "2023-11-14 22:13:20 Tue Nov 318");
    CHECK(timegm(&parts) == moment);
    struct tm local;
    localtime_r(&moment, &local);
    CHECK(mktime(&local) == moment);
}

static jmp_buf jump;
static void jump_back(int value) {
    longjmp(jump, value);
}

static void test_setjmp(void) {
    volatile int passes = 0;
    int value = setjmp(jump);
    passes++;
    if (value == 0) {
        jump_back(5);
    }
    CHECK(value == 5 && passes == 2);
}

#define THREADS 4
#define ROUNDS 5000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static long counter, ready;
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_calls;
static sem_t semaphore;
static int destructor_calls;

static void count_once(void) {
    once_calls++;
}

static void destructor(void *value) {
    (void)value;
    __atomic_add_fetch(&destructor_calls, 1, __ATOMIC_RELAXED);
}

static void *work(void *arg) {
    long number = (long)arg;
    pthread_once(&once, count_once);
    pthread_setspecific(key, (void *)(number + 100));
    errno = (int)number; /* Each thread has its own. */
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    ready++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    sem_post(&semaphore);
    bool own = pthread_getspecific(key) == (void *)(number + 100) && errno == (int)number;
    return (void *)(long)own;
}

static void test_threads(void) {
    CHECK(pthread_key_create(&key, destructor) == 0);
    CHECK(sem_init(&semaphore, 0, 0) == 0);
    pthread_t threads[THREADS];
    for (long i = 0; i < THREADS; i++) {
        CHECK(pthread_create(&threads[i], NULL, work, (void *)i) == 0);
    }
    pthread_mutex_lock(&lock);
    while (ready < THREADS) {
        pthread_cond_wait(&changed, &lock);
    }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < THREADS; i++) {
        sem_wait(&semaphore);
    }
    for (int i = 0; i < THREADS; i++) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && result == (void *)1L);
    }
    CHECK(counter == THREADS * ROUNDS);
    CHECK(once_calls == 1);
    CHECK(destructor_calls == THREADS);
    CHECK(pthread_getspecific(key) == NULL);
    CHECK(sem_trywait(&semaphore) == -1 && errno == EAGAIN);

    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t recursive;
    pthread_mutex_init(&recursive, &attributes);
    CHECK(pthread_mutex_lock(&recursive) == 0 && pthread_mutex_lock(&recursive) == 0);
    CHECK(pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0);
    CHECK(pthread_mutex_trylock(&lock) == 0 && pthread_mutex_trylock(&lock) == EBUSY);
    pthread_mutex_unlock(&lock);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 50 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    CHECK(pthread_cond_timedwait(&changed, &lock, &deadline) == ETIMEDOUT);
    pthread_mutex_unlock(&lock);
}

static void test_memory(void) {
    char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    if (page != MAP_FAILED) {
        page[0] = 1;
        page[8191] = 2;
        CHECK(page[4096] == 0 && munmap(page, 8192) == 0);
    }
    void *aligned = NULL;
    CHECK(posix_memalign(&aligned, 256, 1000) == 0 && ((unsigned long)aligned & 255) == 0);
    aligned = realloc(aligned, 5000);
    CHECK(aligned != NULL);
    free(aligned);
    CHECK(sysconf(_SC_NPROCESSORS_ONLN) >= 1);
}

static void test_processes(void) {
    FILE *pipe = popen("echo from-popen", "r");
    char line[64] = "";
    CHECK(pipe && fgets(line, sizeof line, pipe));
    CHECK(strncmp(line, "from-popen", 10) == 0);
    if (pipe) {
        CHECK(pclose(pipe) == 0);
    }
    CHECK(system("echo system > /dev/null") == 0);
}

static void read_file(const char *path, char *out, size_t size) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out, size - 1);
        out[n > 0 ? n : 0] = '\0';
        close(fd);
    }
}

static void test_spawn(void) {
    char text[64];
    /* dup: two handles, one file position. */
    int fd = open("/tmp/posix-dup.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int copy = dup(fd);
    CHECK(fd >= 0 && copy >= 0 && copy != fd);
    CHECK(write(fd, "ab", 2) == 2);
    close(fd);
    CHECK(write(copy, "cd", 2) == 2); /* (Still open, after the first.) */
    close(copy);
    read_file("/tmp/posix-dup.txt", text, sizeof text);
    CHECK_TEXT(text, "abcd");
    /* dup2: standard output into a file, and back. */
    int saved = dup(1);
    fd = open("/tmp/posix-dup2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(dup2(fd, 1) == 1);
    CHECK(write(1, "redirected", 10) == 10);
    CHECK(dup2(saved, 1) == 1);
    close(fd);
    close(saved);
    read_file("/tmp/posix-dup2.txt", text, sizeof text);
    CHECK_TEXT(text, "redirected");
    /* posix_spawnp, its output in a file (a file action), and its exit code. */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/tmp/posix-spawn.txt",
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *hello[] = {"hello-world", NULL};
    pid_t pid = 0;
    int status = -1;
    CHECK(posix_spawnp(&pid, "hello-world", &actions, NULL, hello, NULL) == 0);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    posix_spawn_file_actions_destroy(&actions);
    read_file("/tmp/posix-spawn.txt", text, sizeof text);
    CHECK(strncmp(text, "Hello, world!", 13) == 0);
    char *shell[] = {"vsh", "-c", "exit 3", NULL};
    CHECK(posix_spawnp(&pid, "vsh", NULL, NULL, shell, NULL) == 0);
    CHECK(waitpid(-1, &status, 0) == pid && WEXITSTATUS(status) == 3);
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    char *missing[] = {"no-such-program", NULL};
    CHECK(posix_spawnp(&pid, "no-such-program", NULL, NULL, missing, NULL) == ENOENT);
}

static void test_dlopen(void) {
    void *library = dlopen("libvexa-test.so", RTLD_NOW);
    CHECK(library != NULL);
    if (!library) {
        printf("posix-test: dlopen: %s\n", dlerror());
        return;
    }
    int (*add)(int, int) = (int (*)(int, int))dlsym(library, "vexa_test_add");
    int (*length)(const char *) = (int (*)(const char *))dlsym(library, "vexa_test_length");
    int *loaded = dlsym(library, "vexa_test_loaded");
    CHECK(add && add(40, 2) == 42);
    CHECK(length && length("seven!!") == 7); /* (It calls libvexa's strlen.) */
    CHECK(loaded && *loaded == 42);          /* (Its initializer ran.) */
    CHECK(dlopen("libvexa-test.so", RTLD_NOW) == library);
    CHECK(dlsym(library, "no_such_symbol") == NULL && dlerror() != NULL);
    CHECK(dlsym(RTLD_DEFAULT, "printf") == (void *)printf);
    CHECK(dlopen("libno-such-library.so", RTLD_NOW) == NULL && dlerror() != NULL);
    CHECK(dlclose(library) == 0);
}

static void test_sockets(void) {
    /* UDP to itself, with select and the sender's address. */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in here = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(here);
    CHECK(u >= 0 && bind(u, (struct sockaddr *)&here, sizeof(here)) == 0);
    CHECK(getsockname(u, (struct sockaddr *)&here, &length) == 0 && ntohs(here.sin_port) != 0);
    CHECK(sendto(u, "ping", 4, 0, (struct sockaddr *)&here, sizeof(here)) == 4);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(u, &readable);
    struct timeval wait = {2, 0};
    CHECK(select(u + 1, &readable, NULL, NULL, &wait) == 1 && FD_ISSET(u, &readable));
    char text[16] = "";
    struct sockaddr_in from;
    socklen_t from_length = sizeof(from);
    CHECK(recvfrom(u, text, sizeof text, 0, (struct sockaddr *)&from, &from_length) == 4);
    CHECK(memcmp(text, "ping", 4) == 0 && from.sin_port == here.sin_port);
    /* Non-blocking: nothing there, so EAGAIN at once. */
    CHECK(fcntl(u, F_SETFL, O_NONBLOCK) == 0 && (fcntl(u, F_GETFL) & O_NONBLOCK));
    CHECK(recv(u, text, sizeof text, 0) == -1 && errno == EAGAIN);
    close(u);
    /* TCP: listen, connect, accept, both ways. */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    length = sizeof(server);
    CHECK(bind(listener, (struct sockaddr *)&server, sizeof(server)) == 0);
    CHECK(listen(listener, 4) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&server, &length) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (struct sockaddr *)&server, sizeof(server)) == 0);
    int peer = accept(listener, NULL, NULL);
    CHECK(peer >= 0);
    CHECK(send(client, "hello", 5, 0) == 5 && recv(peer, text, sizeof text, 0) == 5);
    CHECK(memcmp(text, "hello", 5) == 0);
    CHECK(write(peer, "back", 4) == 4 && read(client, text, sizeof text) == 4);
    CHECK(memcmp(text, "back", 4) == 0);
    close(peer);
    close(client);
    close(listener);
    /* Names. */
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *found = NULL;
    CHECK(getaddrinfo("localhost", "80", &hints, &found) == 0 && found);
    if (found) {
        struct sockaddr_in *a = (struct sockaddr_in *)found->ai_addr;
        CHECK(a->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(a->sin_port) == 80);
        freeaddrinfo(found);
    }
    CHECK(inet_addr("10.0.2.15") == htonl(0x0a00020f));
    char address[INET_ADDRSTRLEN];
    struct in_addr a = {htonl(0xc0a80001)};
    CHECK_TEXT(inet_ntop(AF_INET, &a, address, sizeof address), "192.168.0.1");
}

/* Thread-local storage: the program's own (initialized and zeroed), and a
 * dlopened library's (through __tls_get_addr); threads meet at a barrier. */
static __thread int tls_value = 5;
static __thread char tls_zeroed[64];
static pthread_barrier_t barrier;
static int serial_count;

static void *tls_work(void *arg) {
    int *(*library_tls)(void) = (int *(*)(void))arg;
    bool ok = tls_value == 5 && tls_zeroed[63] == 0;
    tls_value = 6;
    tls_zeroed[63] = 1;
    int *mine = library_tls ? library_tls() : NULL;
    ok = ok && (!mine || *mine == 7);
    if (mine) {
        *mine = 8;
    }
    if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
        __atomic_add_fetch(&serial_count, 1, __ATOMIC_RELAXED);
    }
    ok = ok && tls_value == 6 && (!mine || (*mine == 8 && library_tls() == mine));
    return (void *)(long)ok;
}

static void test_tls(void) {
    void *library = dlopen("libvexa-test.so", RTLD_NOW);
    int *(*library_tls)(void) = library ? (int *(*)(void))dlsym(library, "vexa_test_tls") : NULL;
    CHECK(library_tls != NULL);
    CHECK(tls_value == 5 && tls_zeroed[0] == 0);
    tls_value = 1;
    int *main_copy = library_tls ? library_tls() : NULL;
    CHECK(!main_copy || *main_copy == 7);
    if (main_copy) {
        *main_copy = 1;
    }
    CHECK(pthread_barrier_init(&barrier, NULL, 4) == 0);
    pthread_t threads[3];
    for (int i = 0; i < 3; i++) {
        CHECK(pthread_create(&threads[i], NULL, tls_work, (void *)library_tls) == 0);
    }
    if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
        __atomic_add_fetch(&serial_count, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < 3; i++) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && result == (void *)1L);
    }
    CHECK(serial_count == 1);
    CHECK(tls_value == 1 && (!main_copy || (*main_copy == 1 && library_tls() == main_copy)));
    pthread_barrier_destroy(&barrier);

    /* F_DUPFD: the lowest free handle from 10 up, past any in use there
     * (a socket's too). */
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int dup_low = fcntl(1, F_DUPFD, socket_fd);
    CHECK(socket_fd >= 0 && dup_low > socket_fd);
    int dup_high = fcntl(1, F_DUPFD_CLOEXEC, 10);
    CHECK(dup_high >= 10 && write(dup_high, "", 0) == 0);
    close(dup_low);
    close(dup_high);
    close(socket_fd);

    /* Files that are buffers. */
    char *text = NULL;
    size_t size = 0;
    FILE *memory = open_memstream(&text, &size);
    CHECK(memory != NULL);
    if (memory) {
        fprintf(memory, "%d %s", 42, "memstream");
        fflush(memory);
        CHECK(size == 12 && strcmp(text, "42 memstream") == 0);
        fputs(", more", memory);
        fclose(memory);
        CHECK(size == 18 && strcmp(text, "42 memstream, more") == 0);
        free(text);
    }
    char fixed[16] = "abc def";
    FILE *in = fmemopen(fixed, strlen(fixed), "r");
    char word[8] = "";
    CHECK(in && fscanf(in, "%7s", word) == 1 && strcmp(word, "abc") == 0);
    CHECK(in && fgetc(in) == ' ' && fgetc(in) == 'd');
    if (in) {
        fclose(in);
    }
}

/* Text: character sets, regular expressions, patterns, wide characters,
 * long options, times read back; the system's name; directories. */
static void test_text(void) {
    iconv_t cd = iconv_open("UTF-8", "ISO-8859-1");
    char latin1[] = "caf\xe9", utf8[16] = "", *in = latin1, *out = utf8;
    size_t in_left = 4, out_left = sizeof(utf8) - 1;
    CHECK(cd != (iconv_t)-1 && iconv(cd, &in, &in_left, &out, &out_left) == 0);
    CHECK(strcmp(utf8, "caf\xc3\xa9") == 0);
    if (cd != (iconv_t)-1) {
        iconv_close(cd);
    }

    regex_t re;
    regmatch_t m[2];
    CHECK(regcomp(&re, "([0-9]+)px", REG_EXTENDED) == 0);
    CHECK(regexec(&re, "width: 640px;", 2, m, 0) == 0 && m[1].rm_so == 7 && m[1].rm_eo == 10);
    CHECK(regexec(&re, "width: auto", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);
    CHECK(fnmatch("*.vxapp", "Files.vxapp", 0) == 0 && fnmatch("*.png", "a/b.png", FNM_PATHNAME));
    CHECK(iswalpha(0xe9) && towupper(0xe9) == 0xc9 && !iswdigit('x'));

    char *args[] = {"prog", "--width=640", "--verbose", "-h", "480", "--name", "x", NULL};
    static int verbose;
    static const struct option longs[] = {
        {"width", required_argument, NULL, 'w'}, {"verbose", no_argument, &verbose, 1},
        {"name", required_argument, NULL, 'n'}, {NULL, 0, NULL, 0},
    };
    int width = 0, height = 0, c;
    const char *name = NULL;
    optind = 1;
    while ((c = getopt_long(7, args, "h:", longs, NULL)) != -1) {
        if (c == 'w') {
            width = atoi(optarg);
        } else if (c == 'h') {
            height = atoi(optarg);
        } else if (c == 'n') {
            name = optarg;
        }
    }
    CHECK(width == 640 && height == 480 && verbose == 1 && name && strcmp(name, "x") == 0);
    optind = 1;

    struct tm tm = {0};
    CHECK(strptime("2026-10-04 12:34:56", "%Y-%m-%d %H:%M:%S", &tm) != NULL);
    CHECK(tm.tm_year == 126 && tm.tm_mon == 9 && tm.tm_mday == 4 && tm.tm_min == 34);

    struct utsname u;
    CHECK(uname(&u) == 0 && strcmp(u.sysname, "Vexa") == 0 && strcmp(u.machine, "x86_64") == 0);

    struct dirent **list;
    int n = scandir("/etc", &list, NULL, alphasort);
    bool motd = false, sorted = true;
    for (int i = 0; i < n; i++) {
        motd |= strcmp(list[i]->d_name, "motd") == 0;
        sorted &= i == 0 || strcmp(list[i - 1]->d_name, list[i]->d_name) <= 0;
        free(list[i]);
    }
    CHECK(n > 0 && motd && sorted);
    if (n >= 0) {
        free(list);
    }
    DIR *etc = opendir("/etc");
    struct stat st;
    CHECK(etc && fstatat(dirfd(etc), "motd", &st, 0) == 0 && S_ISREG(st.st_mode));
    if (etc) {
        closedir(etc);
    }
}

int main(int argc, char **argv) {
    /* -v: says which part it's on (to find one that hangs). */
    bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    static const struct {
        const char *name;
        void (*run)(void);
    } parts[] = {
        {"format", test_format},   {"numbers", test_numbers}, {"math", test_math},
        {"strings", test_strings}, {"files", test_files},     {"time", test_time},
        {"setjmp", test_setjmp},   {"threads", test_threads}, {"memory", test_memory},
        {"processes", test_processes}, {"spawn", test_spawn},
        {"dlopen", test_dlopen},   {"tls", test_tls},         {"sockets", test_sockets},
        {"text", test_text},
    };
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        if (verbose) {
            printf("posix-test: %s\n", parts[i].name);
        }
        parts[i].run();
    }
    if (failures) {
        printf("posix-test: %d checks failed\n", failures);
        return 1;
    }
    printf("posix-test: passed\n");
    return 0;
}
