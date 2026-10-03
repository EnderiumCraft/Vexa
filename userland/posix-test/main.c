/* posix-test: checks libvexa's POSIX layer, the way a ported program uses it:
 * printf and scanf with floats, libm, strings and numbers, stdio files
 * (seeking, ungetc, getline), directories, stat, time, setjmp, pthreads
 * (mutexes, condition variables, keys, once, per-thread errno), semaphores,
 * mmap and popen. Prints "posix-test: passed" or what failed. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
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
#define ROUNDS 20000

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

int main(void) {
    test_format();
    test_numbers();
    test_math();
    test_strings();
    test_files();
    test_time();
    test_setjmp();
    test_threads();
    test_memory();
    test_processes();
    if (failures) {
        printf("posix-test: %d checks failed\n", failures);
        return 1;
    }
    printf("posix-test: passed\n");
    return 0;
}
