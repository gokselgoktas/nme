#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include <string.h>
#include <ctype.h>

#include <signal.h>

#define NME_VERSION_STRING "0.1"
#define NME_BUILD_FEATURES "unpack:dump"

#define NME_TRUE 1
#define NME_FALSE 0

#define NME_BACKSLASH '\\'
#define NME_FORWARD_SLASH '/'

#define NME_PATH_SEPARATOR NME_FORWARD_SLASH

#define NME_FILE 0
#define NME_DIRECTORY 1
#define NME_END_OF_DIRECTORY -1

#define NME_SILENT -1
#define NME_VERBOSE 0

#define NME_STRINGIFY(MACRO) #MACRO
#define NME_EXPAND_AND_STRINGIFY(MACRO) NME_STRINGIFY(MACRO)

#define NME_ASSERT(CONDITION) \
    do { \
        if ((CONDITION) == NME_FALSE) { \
            die("assertion failed in " __FILE__ " at line " \
                NME_EXPAND_AND_STRINGIFY(__LINE__) " for `" #CONDITION "`"); \
        } \
    } while (NME_FALSE)

typedef struct queue queue_t;
typedef struct entry entry_t;

struct queue {
    size_t head;
    size_t tail;

    size_t size;
    size_t capacity;

    entry_t *data;
};

struct entry {
    char name[32];
    int8_t type;

    uint8_t unused[3];

    uint32_t size;
    uint32_t offset;
};

static size_t const NME_QUEUE_CAPACITY = 4096;

static char const *NME_EXECUTABLE_NAME = NULL;

static char const *NME_INPUT_FILENAME = NULL;
static char const *NME_OUTPUT_PATH = NULL;

static int NME_VERBOSITY = NME_SILENT;

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

static void handle_signal(int signal_identifier)
{
    (void) signal_identifier;

    fprintf(stderr, "%s: aborting\n", NME_EXECUTABLE_NAME);
    exit(EXIT_FAILURE);
}

static void *allocate(size_t size)
{
    void *result = malloc(size);

    if (result == NULL) {
        die("malloc(%lu) failed", size);
    }

    return result;
}

static queue_t *create_queue(size_t capacity)
{
    NME_ASSERT(capacity >= 1);

    queue_t *queue = allocate(sizeof (queue_t));
    memset(queue, 0x00, sizeof (queue_t));

    queue->data = allocate(sizeof (entry_t) * capacity);

    queue->capacity = capacity;
    queue->tail = queue->capacity - 1;

    return queue;
}

static void free_queue(queue_t *queue)
{
    if (queue != NULL) {
        free(queue->data);
    }

    memset(queue, 0x00, sizeof (queue_t));
    free(queue);
}

static void enqueue(queue_t *queue, entry_t const *entry)
{
    NME_ASSERT(queue != NULL || queue->data != NULL);
    NME_ASSERT(queue->size + 1 < queue->capacity);

    queue->tail = (queue->tail + 1) % queue->capacity;
    memmove(queue->data + queue->tail, entry, sizeof (entry_t));

    ++queue->size;
}

static entry_t const *dequeue(queue_t *queue)
{
    NME_ASSERT(queue != NULL || queue->data != NULL);

    entry_t const *entry = queue->data + queue->head;
    queue->head = (queue->head + 1) % queue->capacity;

    --queue->size;

    return entry;
}

static int is_queue_empty(queue_t const *queue)
{
    NME_ASSERT(queue != NULL);
    return (queue->size == 0);
}

static void *read_from_file(FILE *file, void *destination, size_t size)
{
    if (file == NULL || ferror(file) != NME_FALSE) {
        die("invalid or corrupt file");
    } else if (destination == NULL) {
        die("invalid or corrupt destination buffer");
    }

    size_t count = fread(destination, size, 1, file);

    if (count != 1) {
        report("read_from_file(%lu) failed", size);
    }

    return destination;
}

static void write_into_file(FILE *file, void const *source, size_t size)
{
    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (source == NULL) {
        die("invalid or corrupt source buffer");
    }

    size_t count = fwrite(source, size, 1, file);

    if (count != 1) {
        report("write_into_file(0x%08x, %lu) failed", source, size);
    }
}

static void dump_to_file(char const *filename, void const *contents,
    size_t size)
{
    FILE *output = fopen(filename, "wb");
    NME_ASSERT(output != NULL);

    write_into_file(output, contents, size);

    fclose(output);
}

static void extract(char const *filename, FILE *file, size_t size)
{
    NME_ASSERT(filename != NULL || file != NULL);

    uint8_t *buffer = allocate(size);

    read_from_file(file, buffer, size);
    dump_to_file(filename, buffer, size);

    free(buffer);
}

static entry_t *read_entry(FILE *file, entry_t *entry)
{
    if (file == NULL || ferror(file) != NME_FALSE) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    read_from_file(file, entry, sizeof (entry_t));
    entry->name[31] = '\0';

    return entry;
}

static void print_entry(entry_t const *entry)
{
    NME_ASSERT(entry != NULL);

    if (NME_VERBOSITY == NME_SILENT || entry->type == NME_END_OF_DIRECTORY) {
        return;
    }

    printf("[%s %u %u] ", entry->name, entry->offset, entry->size);
}

static void extract_entry(FILE *file, entry_t const *entry)
{
    NME_ASSERT(file != NULL);
    NME_ASSERT(entry != NULL && entry->type == NME_FILE);

    if (NME_OUTPUT_PATH == NULL || entry->size == 0) {
        return;
    }

    char *path = allocate(2048);
    strcpy(path, entry->name);

    size_t length = strlen(NME_OUTPUT_PATH);

    strncpy(path, NME_OUTPUT_PATH, length + 1);

    if (path[length - 1] != NME_PATH_SEPARATOR) {
        char path_separator = NME_PATH_SEPARATOR;
        strncat(path, &path_separator, 1);
    }

    strcat(path, entry->name);
    extract(path, file, entry->size);

    free(path);
}

static void enqueue_entries(FILE *file, queue_t *queue)
{
    NME_ASSERT(file != NULL);
    NME_ASSERT(queue != NULL);

    entry_t entry;
    read_entry(file, &entry);

    while (entry.type != NME_END_OF_DIRECTORY) {
        if (ferror(file) != NME_FALSE) {
            die("invalid or corrupt file");
        } else if (feof(file) == NME_TRUE) {
            die("premature end of file");
        }

        enqueue(queue, &entry);
        read_entry(file, &entry);
    }
}

static int process_archive(void)
{
    FILE *file = fopen(NME_INPUT_FILENAME, "rb");

    if (file == NULL || ferror(file) != NME_FALSE) {
        die("invalid or corrupt file");
    }

    queue_t *queue = create_queue(NME_QUEUE_CAPACITY);
    enqueue_entries(file, queue);

    while (is_queue_empty(queue) == NME_FALSE) {
        entry_t const *entry = dequeue(queue);
        NME_ASSERT(entry != NULL);

        fseek(file, entry->offset, SEEK_SET);

        switch (entry->type) {
        case NME_FILE:
            extract_entry(file, entry);
            break;

        case NME_DIRECTORY:
            enqueue_entries(file, queue);
            break;

        default:
            die("corrupt entry");
            break;
        }

        if (NME_VERBOSITY != NME_SILENT) {
            print_entry(entry);
        }
    }

    free_queue(queue);

    fclose(file);
    return EXIT_SUCCESS;
}

static char const *get_executable_name(char *executable_path)
{
    NME_ASSERT(executable_path != NULL);

    char *executable_name = strrchr(executable_path, '\\');

    if (executable_name == NULL) {
        executable_name = strrchr(executable_path, '/');
    }

    if (executable_name == NULL || *(executable_name + 1) == '\0') {
        return executable_path;
    }

    return ++executable_name;
}

static void display_help_screen(void)
{
    printf(
        "                                             888\n                   "
        "                          888\n                                      "
        "       888\n888  888 88888b.  88888b.   8888b.   .d8888b 888  888  .d"
        "88b.  888d888\n888  888 888 \"88b 888 \"88b     \"88b d88P\"    888 ."
        "88P d8P  Y8b 888P\"\n888  888 888  888 888  888 .d888888 888      888"
        "888K  88888888 888\nY88b 888 888  888 888 d88P 888  888 Y88b.    888 "
        "\"88b Y8b.     888\n \"Y88888 888  888 88888P\"  \"Y888888  \"Y8888P "
        "888  888  \"Y8888  888\n                  888\n                  888"
        "\n                  888\n"
        "\n"
        "Usage:\n"
        "        %s [options] file...\n"
        "\n"
        "Options:\n"
        "        -e [path=`.`] extract files\n"
        "        -h            display this help screen\n"
        "        -v            display version information\n"
        "        -z            print entry metadata \n"
        "\n",
        NME_EXECUTABLE_NAME);
}

static void display_version_information(void)
{
    printf("nme-unpacker (%s) version %s [%s]\nauthored in 2018 $ released int"
        "o the public domain\n", NME_EXECUTABLE_NAME, NME_VERSION_STRING,
        NME_BUILD_FEATURES);
}

static void handle_command_line_option(char option, char const *argument)
{
    switch (option) {
    case 'e':
        NME_OUTPUT_PATH = ".";

        if (argument != NULL) {
            NME_OUTPUT_PATH = argument;
        }
        break;

    case 'h':
        display_help_screen();
        break;

    case 'v':
        display_version_information();
        break;

    case 'z':
        NME_VERBOSITY = NME_VERBOSE;
        break;

    default:
        report("unknown option `%c`", option);
    }
}

static void parse_command_line(int count, char **arguments)
{
    for (int i = 1; i < count; ++i) {
        char const *argument = arguments[i];
        char const *parameters = NULL;

        switch (argument[0]) {
        case '-':
            if (argument[2] != '\0') {
                parameters = argument + 2;
            }

            handle_command_line_option(argument[1], parameters);
            break;

        default:
            if (NME_INPUT_FILENAME != NULL) {
                report("overriding input filename");
            }

            NME_INPUT_FILENAME = arguments[i];
        }
    }
}

int main(int count, char *arguments[])
{
    NME_EXECUTABLE_NAME = get_executable_name(*arguments);

    signal(SIGABRT, handle_signal);
    signal(SIGINT, handle_signal);

    parse_command_line(count, arguments);

    if (count == 1 || NME_INPUT_FILENAME == NULL) {
        fail("no input files");
    }

    return process_archive();
}
