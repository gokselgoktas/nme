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

static char const *NME_EXECUTABLE_NAME = NULL;

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
    return EXIT_SUCCESS;
}
