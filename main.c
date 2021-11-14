//
// Copyright 2021 informatic
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Usage:
//
//  $CXX main.c -shared -fPIC -o qtextract.so
//
//  # Launch application and extract all embedded/loaded resources
//  LD_PRELOAD=qtextract.so QTEXTRACT_BASE=/tmp ./something
//
//  # Extract a single resource root based on its symbol and exit immediately
//  strings ./something | grep -i qInitResources
//  LD_PRELOAD=qtextract.so QTEXTRACT_BASE=/tmp QTEXTRACT_SINGLE_SYMBOL=_Z28qInitResources_ysm_resourcesv ./something
//

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <iconv.h>
#include <limits.h>
#include <sys/stat.h>
#include <zlib.h>
#include <string.h>
#include <arpa/inet.h>

struct __attribute__((__packed__)) tree_obj {
    uint32_t name_offset;
    uint16_t flags;
    union {
        uint32_t child_count;
        uint32_t locale;
    };
    union {
        uint32_t child_offset;
        uint32_t data_offset;
    };
};

struct __attribute__((__packed__)) name_obj {
    uint16_t length;
    uint32_t hash; // ??
    uint8_t buf[];
};

enum Flags
{
    // must match rcc.h
    Compressed = 0x01,
    Directory = 0x02,
    CompressedZstd = 0x04
};

char* name_convert(uint8_t* names, uint32_t offset) {
    struct name_obj* n = (struct name_obj*) (names + offset);
    char* source = (char*) &n->buf;
    char* target = malloc(htons(n->length)*2+1);
    char* ptarget = target;
    size_t sourcelen = htons(n->length) * 2;
    size_t targetlen = htons(n->length) * 2 + 1;

    iconv_t conv = iconv_open("UTF-8","UTF-16BE");
    iconv(conv, &source, &sourcelen, &ptarget, &targetlen);
    *ptarget = 0;

    iconv_close(conv);

    return target;
}

int started = 0;
void render_tree(uint8_t ver, uint8_t* tree, uint8_t* names, uint8_t* data, uint32_t i, uint32_t depth, char* path) {
    struct tree_obj* node = (struct tree_obj*) (tree + i * (ver == 1 ? 14 : 22));
    for (int indent = 0; indent < depth; indent++ ) printf("  ");

    int flags = htons(node->flags);
    printf("%04d: (%d", i, flags);
    if (flags & Directory) {
        printf(" directory");
    }
    if (flags & Compressed) {
        printf(" compressed");
    }

    char fullpath[PATH_MAX];
    strncpy(fullpath, path, sizeof(fullpath));
    char* name;
    if (i == 0) {
        name = malloc(24);
        snprintf(name, 24, "__root_%08x__", tree);
    } else {
        name = name_convert(names, htonl(node->name_offset));
    }

    strcat(fullpath, "/");
    strcat(fullpath, name);

    printf(") %s [%d] ", fullpath, htonl(node->name_offset));
    if (flags & Directory) {
        printf("-> %d children; offset: %d\n", htonl(node->child_count), htonl(node->child_offset));
        int count = htonl(node->child_count);
        int offset = htonl(node->child_offset);
        mkdir(fullpath, S_IRWXU);
        for (int c = offset; c < offset + count; c++) {
            render_tree(ver, tree, names, data, c, depth + 1, fullpath);
        }
    } else {
        uint32_t data_offset = htonl(node->data_offset);
        uint32_t data_length = htonl(*((uint32_t*) (data + data_offset)));

        data_offset += 4;

        printf("%04x locale; %d offset, %d bytes\n", htonl(node->locale), data_offset, data_length);

        FILE* fd = fopen(fullpath, "wb");
        if (fd == NULL) {
            perror("Unable to dump file");
            exit(-1);
        }

        if (flags & Compressed) {
            uint32_t expected_length = htonl(*((uint32_t*) (data + data_offset)));
            data_offset += 4;
            data_length -= 4;

            uint8_t* out_buffer = malloc(expected_length);
            if (out_buffer == NULL) {
                perror("malloc failed");
                exit(-1);
            }

            z_stream strm;
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            strm.avail_in = data_length;
            strm.next_in = data + data_offset;
            strm.avail_out = expected_length;
            strm.next_out = out_buffer;

            int ret = inflateInit(&strm);
            if (ret != Z_OK) {
                perror("inflateInit() failed");
                exit(-1);
            }

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_STREAM_END) {
                fprintf(stderr, "inflate result: %02x\n", ret);
                perror("inflate() failed");
                exit(-1);
            }
            // assert(ret != Z_STREAM_ERROR);

            fwrite(out_buffer, 1, expected_length, fd);
            free(out_buffer);

            inflateEnd(&strm);
            // printf("%d expected uncompressed length ", expected_length);
        } else {
            if (flags & CompressedZstd) {
                fprintf(stderr, "WARNING: file is compressed with zstd\n");
            }
            fwrite(data + data_offset, 1, data_length, fd);
        }

        fflush(fd);
        fclose(fd);
    }
}

int _Z21qRegisterResourceDataiPKhS0_S0_(int ver, uint8_t* tree, uint8_t* names, uint8_t* data) {
    int (*qRegisterResourceData_orig)(int ver, uint8_t* tree, uint8_t* names, uint8_t* data);
    qRegisterResourceData_orig = dlsym(RTLD_NEXT, "_Z21qRegisterResourceDataiPKhS0_S0_");

    fprintf(stderr, "qRegisterResourceData(%d, %08x, %08x, %08x)\n", ver, tree, names, data);

    if (started) {
        char* extract_base = getenv("QTEXTRACT_BASE");
        if (extract_base == NULL) {
            fprintf(stderr, "Specify extraction base path by passing QTEXTRACT_BASE= environment variable!\n");
            exit(1);
        }
        render_tree(ver, tree, names, data, 0, 0, extract_base);
    }

    return qRegisterResourceData_orig(ver, tree, names, data);
}

void __attribute__((constructor)) startup() {
    printf("Startup!\n");
    int (*resources_init_call)(void);
    void *self = dlopen(NULL, RTLD_LAZY);
    started = 1;

    char* single_symbol = getenv("QTEXTRACT_SINGLE_SYMBOL");
    if (single_symbol != NULL) {
        fprintf(stderr, "Extracting single symbol: %s...\n", single_symbol);
        resources_init_call = (int (*)()) dlsym(self, single_symbol);
        if (resources_init_call == NULL) {
            fprintf(stderr, "Symbol not found!\n");
            exit(1);
        } else {
            fprintf(stderr, "Resource init result: %d\n", resources_init_call());
        }
        exit(0);
    }
}
