/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fs_mgr.h>
#include "bootloader.h"
#include "common.h"
#include "mtdutils/mtdutils.h"
#include "roots.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
#include <cutils/properties.h>
#include <fcntl.h>
static void wait_for_device(const char* fn);
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */
static int get_bootloader_message_mtd(struct bootloader_message *out, const Volume* v);
static int set_bootloader_message_mtd(const struct bootloader_message *in, const Volume* v);
static int get_bootloader_message_block(struct bootloader_message *out, const Volume* v);
static int set_bootloader_message_block(const struct bootloader_message *in, const Volume* v);

int get_bootloader_message(struct bootloader_message *out) {
    Volume* v = volume_for_path("/misc");
    if (v == NULL) {
      LOGE("Cannot load volume /misc!\n");
      return -1;
    }
    if (strcmp(v->fs_type, "mtd") == 0) {
        return get_bootloader_message_mtd(out, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        return get_bootloader_message_block(out, v);
    }
    LOGE("unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
}

int set_bootloader_message(const struct bootloader_message *in) {
    Volume* v = volume_for_path("/misc");
    if (v == NULL) {
      LOGE("Cannot load volume /misc!\n");
      return -1;
    }
    if (strcmp(v->fs_type, "mtd") == 0) {
        return set_bootloader_message_mtd(in, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        return set_bootloader_message_block(in, v);
    }
    LOGE("unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
}

// ------------------------------
// for misc partitions on MTD
// ------------------------------

static const int MISC_PAGES = 3;         // number of pages to save
static const int MISC_COMMAND_PAGE = 1;  // bootloader command is this page

static int get_bootloader_message_mtd(struct bootloader_message *out,
                                      const Volume* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition *part = mtd_find_partition_by_name(v->blk_device);
    if (part == NULL || mtd_partition_info(part, NULL, NULL, &write_size)) {
        LOGE("Can't find %s\n", v->blk_device);
        return -1;
    }

    MtdReadContext *read = mtd_read_partition(part);
    if (read == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    const ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("Can't read %s\n(%s)\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(out, &data[write_size * MISC_COMMAND_PAGE], sizeof(*out));
    return 0;
}
static int set_bootloader_message_mtd(const struct bootloader_message *in,
                                      const Volume* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition *part = mtd_find_partition_by_name(v->blk_device);
    if (part == NULL || mtd_partition_info(part, NULL, NULL, &write_size)) {
        LOGE("Can't find %s\n", v->blk_device);
        return -1;
    }

    MtdReadContext *read = mtd_read_partition(part);
    if (read == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("Can't read %s\n(%s)\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(&data[write_size * MISC_COMMAND_PAGE], in, sizeof(*in));

    MtdWriteContext *write = mtd_write_partition(part);
    if (write == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (mtd_write_data(write, data, size) != size) {
        LOGE("Can't write %s\n(%s)\n", v->blk_device, strerror(errno));
        mtd_write_close(write);
        return -1;
    }
    if (mtd_write_close(write)) {
        LOGE("Can't finish %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }

    LOGI("Set boot command \"%s\"\n", in->command[0] != 255 ? in->command : "");
    return 0;
}

/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
//Write FOTA cookie for MMC device
int set_fota_cookie_mmc(void)
{
    int count = 0;
    Volume* v = volume_for_path("/fota");
     if (v == NULL) {
         LOGE("Cannot load volume /fota\n");
         return -1;
    }
    wait_for_device(v->blk_device);

    int fd = open(v->blk_device, O_RDWR|O_SYNC);
    if (fd < 0) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
     }

    char data[512];
    memset(data, 0x0, sizeof(data));
    data[0] = 0x43;
    data[1] = 0x53;
    data[2] = 0x64;
    data[3] = 0x64;

    count = write(fd,(char *)data,512);
    if (count <= 0) {
        LOGE("Failed writing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (close(fd) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    return 0;
}

int reset_fota_cookie_mmc(void)
{
    int count = 0;
    Volume* v = volume_for_path("/fota");
     if (v == NULL) {
         LOGE("Cannot load volume /fota\n");
         return -1;
    }
    wait_for_device(v->blk_device);

    int fd = open(v->blk_device, O_RDWR|O_SYNC);
    if (fd < 0) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
     }

    char data[512];
    memset(data, 0x0, sizeof(data));

    count = write(fd,(char *)data,512);
    if (count <= 0) {
        LOGE("Failed writing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (close(fd) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    return 0;
}

int read_fota_cookie_mmc(struct fota_cookie *out)
{
    Volume* v = volume_for_path("/fota");
    if (v == NULL) {
      LOGE("Cannot load volume /fota!\n");
      return -1;
    }

    wait_for_device(v->blk_device);
    int fd = open(v->blk_device, O_RDWR|O_SYNC);
    if (fd < 0) {
      LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
      return -1;
    }
    char data[512];
    int count = read(fd, &data, 512);
    if (count <= 0) {
      LOGE("Failed reading %s\n(%s)\n", v->blk_device, strerror(errno));
      return -1;
    }
    if (close(fd) != 0) {
      LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
      return -1;
    }
    memcpy(out, &data, sizeof(out));
    return 0;
}
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end  */

// ------------------------------------
// for misc partitions on block devices
// ------------------------------------

static void wait_for_device(const char* fn) {
    int tries = 0;
    int ret;
    struct stat buf;
    do {
        ++tries;
        ret = stat(fn, &buf);
        if (ret) {
            printf("stat %s try %d: %s\n", fn, tries, strerror(errno));
            sleep(1);
        }
    } while (ret && tries < 10);
    if (ret) {
        printf("failed to stat %s\n", fn);
    }
}

static int get_bootloader_message_block(struct bootloader_message *out,
                                        const Volume* v) {
    wait_for_device(v->blk_device);
    FILE* f = fopen(v->blk_device, "rb");
    if (f == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    struct bootloader_message temp;
    int count = fread(&temp, sizeof(temp), 1, f);
    if (count != 1) {
        LOGE("Failed reading %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (fclose(f) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    memcpy(out, &temp, sizeof(temp));
    return 0;
}

static int set_bootloader_message_block(const struct bootloader_message *in,
                                        const Volume* v) {
    wait_for_device(v->blk_device);
    FILE* f = fopen(v->blk_device, "wb");
    if (f == NULL) {
        LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    int count = fwrite(in, sizeof(*in), 1, f);
    if (count != 1) {
        LOGE("Failed writing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (fclose(f) != 0) {
        LOGE("Failed closing %s\n(%s)\n", v->blk_device, strerror(errno));
        return -1;
    }
    return 0;
}
