#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include <string.h>
#include <ctype.h>

#include <signal.h>

#define NME_VERSION_STRING "0.2"
#define NME_BUILD_FEATURES "unpack:dump"

#define NME_TRUE 1
#define NME_FALSE 0

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

#define NME_PRAGMA(PRAGMA) _Pragma(NME_EXPAND_AND_STRINGIFY(PRAGMA))

#define NME_PACK(ALIGNMENT) NME_PRAGMA(pack(ALIGNMENT))
#define NME_DEFAULT_ALIGNMENT

typedef struct queue queue_t;
typedef struct entry entry_t;

typedef struct wad wad_t;
typedef struct palette palette_t;
typedef struct line_offsets line_offsets_t;
typedef struct image image_t;

struct queue {
    size_t head;
    size_t tail;

    size_t size;
    size_t capacity;

    entry_t *data;
};

NME_PACK(1)
struct entry {
    char name[32];
    int8_t type;

    uint8_t unused[3];

    uint32_t size;
    uint32_t offset;

    entry_t const *parent;
};

struct wad {
    uint32_t number_of_palettes;
    palette_t *palettes;

    uint32_t number_of_images;
    image_t *images;

    entry_t const *entry;
};

NME_PACK(1)
struct palette {
    uint16_t colors[256];
    char comment[13];
};

struct line_offsets {
    uint32_t data_block_size;

    uint8_t name[4];

    uint32_t width;
    uint32_t height;

    uint32_t *values;
};

NME_PACK(1)
struct image {
    char name[32];

    uint64_t pixel_data_size;
    uint32_t unused[2];

    uint32_t height;
    uint32_t width;

    uint16_t color_depth;

    uint8_t *pixel_data;
    line_offsets_t line_offsets;

    uint32_t palette_id;

    wad_t const *parent;
};

static size_t const NME_QUEUE_CAPACITY = 4096;

static char const *NME_EXECUTABLE_NAME = NULL;

static FILE *NME_INPUT_FILE = NULL;
static char const *NME_INPUT_FILENAME = NULL;

static FILE *NME_OUTPUT_FILE = NULL;
static char const *NME_OUTPUT_PATH = NULL;

static char const NME_PATH_SEPARATOR = '/';

static int NME_VERBOSITY = NME_SILENT;

static size_t NME_MAXIMUM_HEAP_USAGE = 0;
static size_t NME_CURRENT_HEAP_USAGE = 0;

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
    size_t *memory = malloc(size + sizeof (size_t));

    if (memory == NULL) {
        die("malloc(%lu) failed", size);
    }

    NME_MAXIMUM_HEAP_USAGE += size;
    NME_CURRENT_HEAP_USAGE += size;

    *(memory++) = size;

    return memset(memory, 0x00, size);
}

static void release(void *memory)
{
    size_t *size = memory;

    if (size == NULL) {
        return;
    }

    NME_CURRENT_HEAP_USAGE -= *(--size);
    free(size);
}

static char *prepend(char *string, char const *prefix, size_t length)
{
    NME_ASSERT(string != NULL && prefix != NULL);

    memmove(string + length, string, strlen(string) + 1);
    return strncpy(string, prefix, length);
}

static uint8_t get_red(uint16_t color)
{
    float red = (float) ((color >> 11) & 0x1F);
    return (uint8_t) (8.225806f * red);
}

static uint8_t get_green(uint16_t color)
{
    float green = (float) ((color >> 5) & 0x3F);
    return (uint8_t) (4.047619f * green);
}

static uint8_t get_blue(uint16_t color)
{
    float blue = (float) (color & 0x1F);
    return (uint8_t) (8.225806f * blue);
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
        release(queue->data);
    }

    memset(queue, 0x00, sizeof (queue_t));
    release(queue);
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

static int has_extension(char const *filename, char const *extension)
{
    NME_ASSERT(filename != NULL);
    NME_ASSERT(extension != NULL);

    char *suffix = strrchr(filename, '.');

    if (suffix != NULL) {
        return strnicmp(++suffix, extension, strlen(extension)) == 0;
    }

    return NME_FALSE;
}

static char *get_path_for_entry(entry_t const *entry)
{
    char *path = allocate(4096);

    if (NME_OUTPUT_PATH == NULL || entry == NULL) {
        return path;
    }

    strcpy(path, entry->name);
    entry = entry->parent;

    for (; entry != NULL; entry = entry->parent) {
        prepend(path, entry->name, strlen(entry->name));
        prepend(path, &NME_PATH_SEPARATOR, 1);
    }

    return prepend(path, NME_OUTPUT_PATH, strlen(NME_OUTPUT_PATH));
}

static char *get_path_for_wad(wad_t const *wad)
{
    NME_ASSERT(wad != NULL);
    return get_path_for_entry(wad->entry);
}

static char *get_path_for_image(image_t const *image)
{
    NME_ASSERT(image != NULL);

    char *path = get_path_for_wad(image->parent);
    strncat(path, &NME_PATH_SEPARATOR, 1);

    return strcat(path, image->name);
}

static uint64_t hash(char const *string)
{
    NME_ASSERT(string != NULL);

    uint64_t result = 5381;

    for (; *string != '\0'; ++string) {
        result = (result << 5) + result + *string;
    }

    return result;
}

static void check_file_health(FILE *file)
{
    NME_ASSERT(file != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }
}

static void *read_from_file(void *destination, size_t size)
{
    if (destination == NULL) {
        die("invalid or corrupt destination buffer");
    }

    check_file_health(NME_INPUT_FILE);
    size_t count = fread(destination, size, 1, NME_INPUT_FILE);

    if (count != 1) {
        report("read_from_file(%lu) failed", size);
    }

    return destination;
}

static void write_into_file(void const *source, size_t size)
{
    if (source == NULL) {
        die("invalid or corrupt source buffer");
    }

    check_file_health(NME_OUTPUT_FILE);
    size_t count = fwrite(source, size, 1, NME_OUTPUT_FILE);

    if (count != 1) {
        report("write_into_file(0x%08x, %lu) failed", source, size);
    }
}

static uint8_t *read_file_contents(char const *filename, size_t *size)
{
    NME_ASSERT(filename != NULL);
    NME_ASSERT(size != NULL);

    FILE *file = fopen(filename, "rb");
    check_file_health(file);

    fseek(file, 0, SEEK_END);
    *size = (size_t) ftell(file);

    void *contents = allocate(*size);
    read_from_file(contents, *size);

    fclose(file);

    return contents;
}

static void dump_to_file(char const *filename, void const *contents,
    size_t size)
{
    NME_ASSERT(filename != NULL);

    NME_OUTPUT_FILE = fopen(filename, "wb");

    check_file_health(NME_OUTPUT_FILE);
    write_into_file(contents, size);

    fclose(NME_OUTPUT_FILE);
}

static void extract_file_subsection(char const *filename, size_t size)
{
    uint8_t *buffer = allocate(size);

    read_from_file(buffer, size);
    dump_to_file(filename, buffer, size);

    release(buffer);
}

static void free_image(image_t *image)
{
    release(image->line_offsets.values);
    release(image->pixel_data);

    release(image);
}

static image_t *read_image_information(image_t *image)
{
    NME_ASSERT(image != NULL);

    size_t const non_header_data_size = sizeof (uint8_t *) +
        sizeof (uint32_t) + sizeof (line_offsets_t) + sizeof (wad_t const *);

    read_from_file(image, sizeof (image_t) - non_header_data_size);
    image->name[31] = '\0';

    fseek(NME_INPUT_FILE, 6, SEEK_CUR);

    return image;
}

static image_t *read_image_pixel_data(image_t *image)
{
    NME_ASSERT(image != NULL);

    image->pixel_data = allocate(image->pixel_data_size);
    read_from_file(image->pixel_data, image->pixel_data_size);

    return image;
}

static image_t *read_image_line_offsets(image_t *image)
{
    NME_ASSERT(image != NULL);

    size_t const non_header_data_size = sizeof (uint32_t *);

    read_from_file(&image->line_offsets, sizeof (line_offsets_t) -
        non_header_data_size);

    if (image->height == 0) {
        return image;
    }

    image->line_offsets.values = allocate(sizeof (uint32_t) * image->height);

    read_from_file(image->line_offsets.values, sizeof (uint32_t) *
        image->height);

    return image;
}

static void extract_bmp_image(image_t const *image)
{
    NME_ASSERT(image != NULL && image->parent != NULL);

    /* TODO */
}

static void extract_rle_image(image_t const *image)
{
    NME_ASSERT(image != NULL && image->parent != NULL);

    /* TODO */
}

static void print_image_information(image_t const *image)
{
    NME_ASSERT(image != NULL);

    if (NME_VERBOSITY == NME_SILENT) {
        return;
    }

    printf("{$ %s # %llu w %u h %u @ %u ~ %u} ", image->name,
        image->pixel_data_size, image->width, image->height,
        image->color_depth, image->palette_id);
}

static void process_wad_archive(wad_t *wad)
{
    NME_ASSERT(wad != NULL);

    check_file_health(NME_INPUT_FILE);
    fseek(NME_INPUT_FILE, 400, SEEK_CUR);

    read_from_file(&wad->number_of_palettes, sizeof (uint32_t));

    if (wad->number_of_palettes == 0) {
        release(wad->palettes);
        return;
    }

    wad->palettes = allocate(wad->number_of_palettes * sizeof (palette_t));
    read_from_file(wad->palettes,
        wad->number_of_palettes * sizeof (palette_t));

    read_from_file(&wad->number_of_images, sizeof (uint32_t));

    if (wad->number_of_images == 0) {
        release(wad->palettes);
        return;
    }

    for (uint32_t i = 0; i < wad->number_of_images; ++i) {
        image_t *image = allocate(sizeof (image_t));

        image->parent = wad;

        read_image_information(image);
        read_image_pixel_data(image);

        int is_rle = has_extension(image->name, "rle");

        if (is_rle == NME_TRUE) {
            read_image_line_offsets(image);
        }

        read_from_file(&image->palette_id, sizeof (uint32_t));

        if (NME_VERBOSITY != NME_SILENT) {
            print_image_information(image);
        }

        if (is_rle == NME_TRUE) {
            extract_rle_image(image);
        } else {
            extract_bmp_image(image);
        }

        free_image(image);
    }

    release(wad->palettes);
}

static entry_t *read_entry_information(entry_t *entry)
{
    NME_ASSERT(entry != NULL);

    size_t const non_header_data_size = sizeof (entry_t const *);

    check_file_health(NME_INPUT_FILE);
    read_from_file(entry, sizeof (entry_t) - non_header_data_size);

    entry->name[31] = '\0';
    return entry;
}

static void extract_entry_contents(entry_t const *entry)
{
    NME_ASSERT(entry != NULL && entry->type == NME_FILE);

    if (NME_OUTPUT_PATH == NULL || entry->size == 0) {
        return;
    }

    if (has_extension(entry->name, "wad") == NME_TRUE) {
        wad_t *wad = allocate(sizeof (wad_t));
        wad->entry = entry;

        process_wad_archive(wad);
        release(wad);
    } else {
        char *path = get_path_for_entry(entry);
        uint64_t salt = hash(path);

        sprintf(path, "%s%c%llu-%s", NME_OUTPUT_PATH, NME_PATH_SEPARATOR, salt,
            entry->name);

        extract_file_subsection(path, entry->size);
        release(path);
    }
}

static void print_entry_information(entry_t const *entry)
{
    NME_ASSERT(entry != NULL);

    if (NME_VERBOSITY == NME_SILENT || entry->type == NME_END_OF_DIRECTORY) {
        return;
    }

    printf("[%s %u %u] ", entry->name, entry->offset, entry->size);
}

static void enqueue_entry_hierarchy(queue_t *queue, entry_t const *parent)
{
    NME_ASSERT(queue != NULL);

    entry_t *entry = allocate(sizeof (entry_t));
    entry->parent = parent;

    read_entry_information(entry);

    while (entry->type != NME_END_OF_DIRECTORY) {
        enqueue(queue, entry);
        read_entry_information(entry);
    }

    release(entry);
}

static int process_dir_archive(void)
{
    NME_INPUT_FILE = fopen(NME_INPUT_FILENAME, "rb");
    check_file_health(NME_INPUT_FILE);

    queue_t *queue = create_queue(NME_QUEUE_CAPACITY);
    enqueue_entry_hierarchy(queue, NULL);

    while (is_queue_empty(queue) == NME_FALSE) {
        entry_t const *entry = dequeue(queue);
        NME_ASSERT(entry != NULL);

        fseek(NME_INPUT_FILE, entry->offset, SEEK_SET);

        switch (entry->type) {
        case NME_FILE:
            extract_entry_contents(entry);
            break;

        case NME_DIRECTORY:
            enqueue_entry_hierarchy(queue, entry);
            break;

        default:
            die("corrupt entry");
            break;
        }

        if (NME_VERBOSITY != NME_SILENT) {
            print_entry_information(entry);
        }
    }

    free_queue(queue);

    fclose(NME_INPUT_FILE);

    if (NME_VERBOSITY != NME_SILENT) {
        report("used %u bytes of heap memory", NME_MAXIMUM_HEAP_USAGE);

        if (NME_CURRENT_HEAP_USAGE != 0) {
            die("leaked %u bytes of heap memory", NME_CURRENT_HEAP_USAGE);
        }
    }

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
        "        -z            print entry information\n"
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

    return process_dir_archive();
}
