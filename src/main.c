#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include <string.h>

#include <signal.h>

#define NME_VERSION_STRING "0.1"
#define NME_BUILD_FEATURES "unpack:dump"

#define NME_TRUE 1
#define NME_FALSE 0

#define NME_BACKSLASH '\\'
#define NME_FORWARD_SLASH '/'

#define NME_PATH_SEPARATOR NME_FORWARD_SLASH

#define NME_STRINGIFY(MACRO) #MACRO
#define NME_EXPAND_AND_STRINGIFY(MACRO) NME_STRINGIFY(MACRO)

#define NME_ASSERT(CONDITION) \
    do { \
        if ((CONDITION) == NME_FALSE) { \
            die("assertion failed in " __FILE__ " at line " \
                NME_EXPAND_AND_STRINGIFY(__LINE__) " for `" #CONDITION "`"); \
        } \
    } while (NME_FALSE)

typedef struct entry entry_t;

struct entry {
    char name[32];
    int8_t type;

    uint8_t unused[3];

    uint32_t size;
    uint32_t offset;
};

static char const *NME_EXECUTABLE_NAME = NULL;

static void report(char const *message, ...)
{
    if (message == NULL) {
        fprintf(stderr, "%s: unknown error\n", NME_EXECUTABLE_NAME);
        return;
    }

    va_list arguments;
    va_start(arguments, message);

    char *buffer = malloc(1024);

    vsnprintf(buffer, 1024, message, arguments);
    fprintf(stderr, "%s: %s\n", NME_EXECUTABLE_NAME, buffer);

    free(buffer);
    va_end(arguments);
}

static void fail(char const *message, ...)
{
    va_list arguments;
    va_start(arguments, message);

    report(message, arguments);

    va_end(arguments);
    exit(EXIT_FAILURE);
}

static void die(char const *message, ...)
{
    va_list arguments;
    va_start(arguments, message);

    report(message, arguments);

    va_end(arguments);
    abort();
}

void handle_signal(int signal_identifier)
{
    (void) signal_identifier;

    fprintf(stderr, "%s: aborting\n", NME_EXECUTABLE_NAME);
    exit(EXIT_FAILURE);
}

static void fix_path_separators(char *input)
{
    for (; input != NULL; input = strchr(input, NME_BACKSLASH)) {
        *input = NME_PATH_SEPARATOR;
    }
}

static char const *extract_executable_name(char *filename)
{
    char *executable_name = NULL;

    fix_path_separators(filename);
    executable_name = strrchr(filename, NME_PATH_SEPARATOR);

    if (executable_name == NULL || *(executable_name + 1) == '\0') {
        return filename;
    }

    return ++executable_name;
}

int main(int count, char *arguments[])
{
    NME_EXECUTABLE_NAME = extract_executable_name(*arguments);

    signal(SIGABRT, handle_signal);
    signal(SIGINT, handle_signal);

    return EXIT_SUCCESS;
}
