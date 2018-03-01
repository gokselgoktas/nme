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

#define NME_PATH_SEPARATOR '/'

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

struct entry {
    char name[32];
    int8_t type;

    uint8_t unused[3];

    uint32_t size;
    uint32_t offset;
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

    return memset(result, 0x00, size);
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

static int has_extension(char const *filename, char const *extension)
{
    NME_ASSERT(filename != NULL);
    NME_ASSERT(extension != NULL);

    char *suffix = strrchr(filename, '.');

    if (suffix != NULL) {
        ++suffix;

        return strnicmp(suffix, extension, 3) == 0;
    }

    return NME_FALSE;
}

static void *read_from_file(FILE *file, void *destination, size_t size)
{
    if (file == NULL || ferror(file) != 0) {
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

static uint8_t *read_file_contents(char const *filename, size_t *size)
{
    NME_ASSERT(filename != NULL);
    NME_ASSERT(size != NULL);

    FILE *file = fopen(filename, "rb");

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    }

    fseek(file, 0, SEEK_END);
    *size = (size_t) ftell(file);

    void *contents = allocate(*size);
    read_from_file(file, contents, *size);

    fclose(file);

    return contents;
}

static void dump_to_file(char const *filename, void const *contents,
    size_t size)
{
    FILE *output = fopen(filename, "wb");
    NME_ASSERT(output != NULL);

    write_into_file(output, contents, size);

    fclose(output);
}

static void extract_file_subsection(char const *filename, FILE *file,
    size_t size)
{
    NME_ASSERT(filename != NULL || file != NULL);

    uint8_t *buffer = allocate(size);

    read_from_file(file, buffer, size);
    dump_to_file(filename, buffer, size);

    free(buffer);
}

static void free_image(image_t *image)
{
    free(image->line_offsets.values);
    free(image->pixel_data);

    free(image);
}

static image_t *read_image_information(FILE *file, image_t *image)
{
    NME_ASSERT(image != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    size_t const non_header_data_size = sizeof (uint8_t *) +
        sizeof (uint32_t) + sizeof (line_offsets_t) + sizeof (wad_t const *);

    read_from_file(file, image, sizeof (image_t) - non_header_data_size);
    image->name[31] = '\0';

    fseek(file, 6, SEEK_CUR);

    return image;
}

static image_t *read_image_pixel_data(FILE *file, image_t *image)
{
    NME_ASSERT(image != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    image->pixel_data = allocate(image->pixel_data_size);
    read_from_file(file, image->pixel_data, image->pixel_data_size);

    return image;
}

static image_t *read_image_line_offsets(FILE *file, image_t *image)
{
    NME_ASSERT(image != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    size_t const non_header_data_size = sizeof (uint32_t *);

    read_from_file(file, &image->line_offsets, sizeof (line_offsets_t) -
        non_header_data_size);

    if (image->height == 0) {
        return image;
    }

    image->line_offsets.values = allocate(sizeof (uint32_t) * image->height);

    read_from_file(file, image->line_offsets.values, sizeof (uint32_t) *
        image->height);

    return image;
}

static void extract_bmp_image(char const *path, image_t const *image)
{
    NME_ASSERT(path != NULL);
    NME_ASSERT(image != NULL && image->parent != NULL);

    /* TODO */
}

static void extract_rle_image(char const *path, image_t const *image)
{
    NME_ASSERT(path != NULL);
    NME_ASSERT(image != NULL);

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

static void process_wad_archive(char const *path, FILE *file, wad_t *wad)
{
    NME_ASSERT(wad != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    fseek(file, 400, SEEK_CUR);

    read_from_file(file, &wad->number_of_palettes, sizeof (uint32_t));

    if (wad->number_of_palettes == 0) {
        free(wad->palettes);
        return;
    }

    wad->palettes = allocate(wad->number_of_palettes * sizeof (palette_t));
    read_from_file(file, wad->palettes,
        wad->number_of_palettes * sizeof (palette_t));

    read_from_file(file, &wad->number_of_images, sizeof (uint32_t));

    if (wad->number_of_images == 0) {
        free(wad->palettes);
        return;
    }

    for (uint32_t i = 0; i < wad->number_of_images; ++i) {
        image_t *image = allocate(sizeof (image_t));

        image->parent = wad;

        read_image_information(file, image);
        read_image_pixel_data(file, image);

        int is_rle = has_extension(image->name, "rle");

        if (is_rle == NME_TRUE) {
            read_image_line_offsets(file, image);
        }

        read_from_file(file, &image->palette_id, sizeof (uint32_t));

        if (NME_VERBOSITY != NME_SILENT) {
            print_image_information(image);
        }

        if (is_rle == NME_TRUE) {
            extract_rle_image(path, image);
        } else {
            extract_bmp_image(path, image);
        }

        free_image(image);
    }

    free(wad->palettes);
}

static entry_t *read_entry_information(FILE *file, entry_t *entry)
{
    NME_ASSERT(entry != NULL);

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    } else if (feof(file) == NME_TRUE) {
        die("premature end of file");
    }

    read_from_file(file, entry, sizeof (entry_t));
    entry->name[31] = '\0';

    return entry;
}

static void extract_entry_contents(FILE *file, entry_t const *entry)
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

    if (has_extension(entry->name, "wad") == NME_TRUE) {
        wad_t *wad = allocate(sizeof (wad_t));
        wad->entry = entry;

        process_wad_archive(path, file, wad);

        free(wad);
    } else {
        extract_file_subsection(path, file, entry->size);
    }

    free(path);
}

static void print_entry_information(entry_t const *entry)
{
    NME_ASSERT(entry != NULL);

    if (NME_VERBOSITY == NME_SILENT || entry->type == NME_END_OF_DIRECTORY) {
        return;
    }

    printf("[%s %u %u] ", entry->name, entry->offset, entry->size);
}

static void enqueue_entry_hierarchy(FILE *file, queue_t *queue)
{
    NME_ASSERT(file != NULL);
    NME_ASSERT(queue != NULL);

    entry_t entry;
    read_entry_information(file, &entry);

    while (entry.type != NME_END_OF_DIRECTORY) {
        if (ferror(file) != 0) {
            die("invalid or corrupt file");
        } else if (feof(file) == NME_TRUE) {
            die("premature end of file");
        }

        enqueue(queue, &entry);
        read_entry_information(file, &entry);
    }
}

static int process_dir_archive(void)
{
    FILE *file = fopen(NME_INPUT_FILENAME, "rb");

    if (file == NULL || ferror(file) != 0) {
        die("invalid or corrupt file");
    }

    queue_t *queue = create_queue(NME_QUEUE_CAPACITY);
    enqueue_entry_hierarchy(file, queue);

    while (is_queue_empty(queue) == NME_FALSE) {
        entry_t const *entry = dequeue(queue);
        NME_ASSERT(entry != NULL);

        fseek(file, entry->offset, SEEK_SET);

        switch (entry->type) {
        case NME_FILE:
            extract_entry_contents(file, entry);
            break;

        case NME_DIRECTORY:
            enqueue_entry_hierarchy(file, queue);
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
