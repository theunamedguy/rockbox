/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2014 by Amaury Pouly
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#define _XOPEN_SOURCE 500
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "rbscsi.h"

static void misc_std_printf(void *user, const char *fmt, ...)
{
    (void) user;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

#if defined(_WIN32) || defined(__WIN32__)
#define RB_SCSI_WINDOWS
#include <windows.h>
#include <ntddscsi.h>
#include <stdint.h>
typedef HANDLE rb_scsi_handle_t;
#elif defined(__linux) || defined(__linux__) || defined(linux)
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <dirent.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#define RB_SCSI_LINUX
typedef int rb_scsi_handle_t;
#else
typedef void *rb_scsi_handle_t;
#endif

struct rb_scsi_device_t
{
    rb_scsi_handle_t handle;
    void *user;
    rb_scsi_printf_t printf;
    unsigned flags;
};

/* Linux */
#ifdef RB_SCSI_LINUX
/* the values for hdr.driver_status are not defined in public headers */
#define DRIVER_SENSE        0x08

rb_scsi_device_t rb_scsi_open(const char *path, unsigned flags, void *user,
    rb_scsi_printf_t printf)
{
    if(printf == NULL)
        printf = misc_std_printf;
    int fd = open(path, (flags & RB_SCSI_READ_ONLY) ? O_RDONLY : O_RDWR);
    if(fd < 0)
    {
        if(flags & RB_SCSI_DEBUG)
            printf(user, "rb_scsi: open failed: %s\n", strerror(errno));
        return NULL;
    }
    rb_scsi_device_t dev = malloc(sizeof(struct rb_scsi_device_t));
    dev->flags = flags;
    dev->handle = fd;
    dev->user = user;
    dev->printf = printf ? printf : misc_std_printf;
    return dev;
}

int rb_scsi_raw_xfer(rb_scsi_device_t dev, struct rb_scsi_raw_cmd_t *raw)
{
#define rb_printf(...) \
    do{ if(dev->flags & RB_SCSI_DEBUG) dev->printf(dev->user, __VA_ARGS__); }while(0)
    sg_io_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    /* prepare transfer descriptor */
    hdr.interface_id = 'S';
    switch(raw->dir)
    {
        case RB_SCSI_NONE: hdr.dxfer_direction = SG_DXFER_NONE; break;
        case RB_SCSI_READ: hdr.dxfer_direction = SG_DXFER_FROM_DEV; break;
        case RB_SCSI_WRITE: hdr.dxfer_direction = SG_DXFER_TO_DEV; break;
        default: rb_printf("rb_scsi: invalid direction"); return -1;
    }
    hdr.cmd_len = raw->cdb_len;
    hdr.mx_sb_len = raw->sense_len;
    hdr.dxfer_len = raw->buf_len;
    hdr.dxferp = raw->buf;
    hdr.cmdp = raw->cdb;
    hdr.sbp = raw->sense;
    hdr.timeout = raw->tmo * 1000;
    hdr.flags = SG_FLAG_LUN_INHIBIT;
    /* do raw command */
    if(ioctl(dev->handle, SG_IO, &hdr) < 0)
    {
        raw->status = errno;
        rb_printf("rb_scsi: ioctl failed: %s\n", strerror(raw->status));
        return RB_SCSI_OS_ERROR;
    }
    /* error handling is weird: host status has the priority */
    if(hdr.host_status)
    {
        rb_printf("rb_scsi: host error: %d\n", hdr.host_status);
        raw->status = hdr.host_status;
        return RB_SCSI_ERROR;
    }
    raw->status = hdr.status & 0x7e;
    raw->buf_len -= hdr.resid;
    raw->sense_len = hdr.sb_len_wr;
    /* driver error can report sense */
    if((hdr.driver_status & 0xf) == DRIVER_SENSE)
    {
        rb_printf("rb_scsi: driver status reported sense\n");
        return RB_SCSI_SENSE;
    }
    /* otherwise errors */
    if(hdr.driver_status)
    {
        rb_printf("rb_scsi: driver error: %d\n", hdr.driver_status);
        return RB_SCSI_ERROR;
    }
    /* scsi status can imply sense */
    if(raw->status == RB_SCSI_CHECK_CONDITION || raw->status == RB_SCSI_COMMAND_TERMINATED)
        return RB_SCSI_SENSE;
    return raw->status ? RB_SCSI_STATUS : RB_SCSI_OK;
#undef rb_printf
}

void rb_scsi_close(rb_scsi_device_t dev)
{
    close(dev->handle);
    free(dev);
}

static int is_hctl(const char *name)
{
    char *end;
    strtoul(name, &end, 0); /* h */
    if(*end != ':')
        return 0;
    strtoul(end + 1, &end, 0); /* c */
    if(*end != ':')
        return 0;
    strtoul(end + 1, &end, 0); /* t */
    if(*end != ':')
        return 0;
    strtoul(end + 1, &end, 0); /* l */
    return *end == 0;
}

static int _resolve_link_dev_path(char *path, size_t pathsz)
{
    /* make sure it is a directory */
    struct stat st;
    if(stat(path, &st) < 0)
        return 0;
    if(!S_ISDIR(st.st_mode))
        return 0;
    if(chdir(path) < 0)
        return 0;
    if(getcwd(path, pathsz) == NULL)
        return 0;
    return 1;
}

static int resolve_link_dev_path(char *path, size_t pathsz)
{
    /* change directory, ask the current path and resolve current directory */
    char curdir[PATH_MAX];
    if(getcwd(curdir, sizeof(curdir)) == NULL)
        return 0;
    int ret = _resolve_link_dev_path(path, pathsz);
    chdir(curdir);
    return ret;
}

static int scan_resolve_first_dev_path(char *path, size_t pathsz)
{
    size_t pathlen = strlen(path);
    char *pathend = path + pathlen;
    DIR *dir = opendir(path);
    if(dir == NULL)
        return 0;
    struct dirent *d;
    int ret = 0;
    while((d = readdir(dir)))
    {
        if(!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        /* we found the entry, if it is a symlink, resolve it, otherwise it must be a directory */
        if(d->d_type == DT_LNK)
        {
            snprintf(pathend, pathsz - pathlen, "/%s", d->d_name);
            ret = resolve_link_dev_path(path, pathsz);
        }
        else if(d->d_type == DT_DIR)
        {
            snprintf(path, pathsz, "/dev/%s", d->d_name);
            ret = 1;
        }
        break;
    }
    closedir(dir);
    return ret;
}

static int scan_resolve_dev_path(char *path, size_t pathsz, const char *match)
{
    size_t pathlen = strlen(path);
    char *pathend = path + pathlen;
    DIR *dir = opendir(path);
    if(dir == NULL)
        return 0;
    struct dirent *d;
    int ret = 0;
    while((d = readdir(dir)))
    {
        if(strcmp(d->d_name, match))
            continue;
        /* we found the entry, there are two case:
         * - directory: we need to scan it and find the first entry
         * - symlink: we need to see where it goes and extract the basename */
        snprintf(pathend, pathsz - pathlen, "/%s", d->d_name);
        if(d->d_type == DT_DIR)
            ret = scan_resolve_first_dev_path(path, pathsz);
        else if(d->d_type == DT_LNK)
            ret = resolve_link_dev_path(path, pathsz);
        break;
    }
    closedir(dir);
    return ret;
}

static char *read_file_or_null(const char *path)
{
    FILE *f = fopen(path, "r");
    if(f == NULL)
        return NULL;
    char buffer[1024];
    if(fgets(buffer, sizeof(buffer), f) == NULL)
    {
        fclose(f);
        return NULL;
    }
    fclose(f);
    /* the kernel appends a '\n' at the end, remove it */
    size_t len = strlen(buffer);
    if(len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = 0;
    return strdup(buffer);
}

struct rb_scsi_devent_t *rb_scsi_list(void)
{
    /* list devices in /sys/bus/scsi/devices
     * we only keep entries of the form h:c:t:l */
#define SYS_SCSI_DEV_PATH   "/sys/bus/scsi/devices"
    DIR *dir = opendir(SYS_SCSI_DEV_PATH);
    if(dir == NULL)
        return NULL;
    struct dirent *d;
    struct rb_scsi_devent_t *dev = malloc(sizeof(struct rb_scsi_devent_t));
    dev[0].scsi_path = NULL;
    dev[0].block_path = NULL;
    int nr_dev = 0;
    while((d = readdir(dir)))
    {
        if(!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        /* make sure the name is of the form h:c:t:l, we do not want targets or hosts */
        if(!is_hctl(d->d_name))
            continue;
        /* we now need to scan the directory to find the block and scsi generic device path:
         * block: there should be a 'block' entry
         * scsi: there should be a 'scsi_generic' entry
         * Both entries can either be a symlink to the devide, or a directory with a single entry */
        char scsi_path[PATH_MAX];
        snprintf(scsi_path, sizeof(scsi_path), SYS_SCSI_DEV_PATH "/%s", d->d_name);
        if(!scan_resolve_dev_path(scsi_path, sizeof(scsi_path), "scsi_generic"))
            continue;
        char block_path[PATH_MAX];
        snprintf(block_path, sizeof(block_path), SYS_SCSI_DEV_PATH "/%s", d->d_name);
        if(!scan_resolve_dev_path(block_path, sizeof(block_path), "block"))
            block_path[0] = 0;
        dev = realloc(dev, (2 + nr_dev) * sizeof(struct rb_scsi_devent_t));
        dev[nr_dev].scsi_path = strdup(scsi_path);
        dev[nr_dev].block_path = block_path[0] == 0 ? NULL : strdup(block_path);
        /* fill vendor/model/rev */
        snprintf(scsi_path, sizeof(scsi_path), SYS_SCSI_DEV_PATH "/%s/vendor", d->d_name);
        dev[nr_dev].vendor = read_file_or_null(scsi_path);
        snprintf(scsi_path, sizeof(scsi_path), SYS_SCSI_DEV_PATH "/%s/model", d->d_name);
        dev[nr_dev].model = read_file_or_null(scsi_path);
        snprintf(scsi_path, sizeof(scsi_path), SYS_SCSI_DEV_PATH "/%s/rev", d->d_name);
        dev[nr_dev].rev = read_file_or_null(scsi_path);

        /* sentinel */
        dev[++nr_dev].scsi_path = NULL;
        dev[nr_dev].block_path = NULL;
    }
    closedir(dir);
    return dev;
}
/* Windows */
#elif defined(RB_SCSI_WINDOWS)
/* return either path or something allocated with malloc() */
static const char *map_to_physical_drive(const char *path, unsigned flags, void *user,
    rb_scsi_printf_t printf)
{
    /* don't do anything if path starts with '\' */
    if(path[0] == '\\')
        return path;
    /* Convert to UNC path (C: -> \\.\C:) otherwise it won't work) */
    char *unc_path = malloc(strlen(path) + 5);
    sprintf(unc_path, "\\\\.\\%s", path);
    if(flags & RB_SCSI_DEBUG)
        printf(user, "rb_scsi: map to UNC path: %s\n", unc_path);
    return unc_path;
}

rb_scsi_device_t rb_scsi_open(const char *path, unsigned flags, void *user,
    rb_scsi_printf_t printf)
{
    if(printf == NULL)
        printf = misc_std_printf;
    /* magic to auto-detect physical drive */
    const char *open_path = map_to_physical_drive(path, flags, user, printf);
    HANDLE h = CreateFileA(open_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING, NULL);
    /* free path if it was allocated */
    if(open_path != path)
        free((char *)open_path);
    if(h == INVALID_HANDLE_VALUE)
    {
        if(flags & RB_SCSI_DEBUG)
        {
            LPSTR msg = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, NULL);
            printf(user, "rb_scsi: open failed: %s\n", msg);
            LocalFree(msg);
        }
        return NULL;
    }
    rb_scsi_device_t dev = malloc(sizeof(struct rb_scsi_device_t));
    dev->flags = flags;
    dev->handle = h;
    dev->user = user;
    dev->printf = printf ? printf : misc_std_printf;
    return dev;
}

int rb_scsi_raw_xfer(rb_scsi_device_t dev, struct rb_scsi_raw_cmd_t *raw)
{
#define rb_printf(...) \
    do{ if(dev->flags & RB_SCSI_DEBUG) dev->printf(dev->user, __VA_ARGS__); }while(0)
    /* we need to allocate memory for SCSI_PASS_THROUGH_WITH_BUFFERS and the
     * data and sense */
    DWORD length = sizeof(SCSI_PASS_THROUGH) + raw->buf_len + raw->sense_len;
    DWORD returned = 0;
    SCSI_PASS_THROUGH *spt = malloc(length);
    memset(spt, 0, length);
    spt->Length = sizeof(SCSI_PASS_THROUGH);
    spt->PathId = 0;
    spt->TargetId = 0;
    spt->Lun = 0;
    spt->CdbLength = raw->cdb_len;
    memcpy(spt->Cdb, raw->cdb, raw->cdb_len);
    spt->SenseInfoLength = raw->sense_len;
    spt->SenseInfoOffset = spt->Length; /* Sense after header */
    switch(raw->dir)
    {
        case RB_SCSI_NONE: spt->DataIn = SCSI_IOCTL_DATA_UNSPECIFIED; break;
        case RB_SCSI_READ: spt->DataIn = SCSI_IOCTL_DATA_IN; break;
        case RB_SCSI_WRITE: spt->DataIn = SCSI_IOCTL_DATA_OUT; break;
        default:
            free(spt);
            rb_printf("rb_scsi: invalid direction"); return -1;
    }
    spt->DataTransferLength = raw->buf_len;
    spt->DataBufferOffset = spt->SenseInfoOffset + spt->SenseInfoLength; /* Data after Sense */
    spt->TimeOutValue = raw->tmo;
    /* on write, copy data to buffer */
    if(raw->dir == RB_SCSI_WRITE)
        memcpy((BYTE *)spt + spt->DataBufferOffset, raw->buf, raw->buf_len);
    BOOL status = DeviceIoControl(dev->handle, IOCTL_SCSI_PASS_THROUGH,
        spt, length, spt, length, &returned, FALSE);
    if(!status)
    {
        if(dev->flags & RB_SCSI_DEBUG)
        {
            LPSTR msg = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, NULL);
            printf(dev->user, "rb_scsi: ioctl failed: %s\n", msg);
            LocalFree(msg);
        }
        free(spt);
        return RB_SCSI_OS_ERROR;
    }
    /* on read, copy data from buffer */
    if(raw->dir == RB_SCSI_READ)
    {
        raw->buf_len = spt->DataTransferLength;
        memcpy(raw->buf, (BYTE *)spt + spt->DataBufferOffset, raw->buf_len);
    }
    /* check status */
    raw->status = spt->ScsiStatus;
    if(raw->status == RB_SCSI_CHECK_CONDITION || raw->status == RB_SCSI_COMMAND_TERMINATED)
    {
        memcpy(raw->sense, (BYTE *)spt + spt->SenseInfoOffset, raw->sense_len);
        free(spt);
        return RB_SCSI_SENSE;
    }
    else
        raw->sense_len = 0;
    free(spt);
    return raw->status ? RB_SCSI_STATUS : RB_SCSI_OK;
#undef rb_printf
}

void rb_scsi_close(rb_scsi_device_t dev)
{
    CloseHandle(dev->handle);
    free(dev);
}

struct rb_scsi_devent_t *rb_scsi_list(void)
{
    /* unimplemented */
    struct rb_scsi_devent_t *dev = malloc(sizeof(struct rb_scsi_devent_t));
    dev[0].scsi_path = NULL;
    dev[0].block_path = NULL;
    return dev;
}
/* other targets */
#else
rb_scsi_device_t rb_scsi_open(const char *path, unsigned flags, void *user,
    rb_scsi_printf_t printf)
{
    if(printf == NULL)
        printf = misc_std_printf;
    (void) path;
    if(flags & RB_SCSI_DEBUG)
        printf(user, "rb_scsi: unimplemented on this platform\n");
    return NULL;
}

int rb_scsi_raw_xfer(rb_scsi_device_t dev, struct rb_scsi_raw_cmd_t *raw)
{
    return RB_SCSI_ERROR;
}

void rb_scsi_close(rb_scsi_device_t dev)
{
    free(dev);
}

struct rb_scsi_devent_t *rb_scsi_list(void)
{
    /* unimplemented */
    struct rb_scsi_devent_t *dev = malloc(sizeof(struct rb_scsi_devent_t));
    dev[0].scsi_path = NULL;
    dev[0].block_path = NULL;
    return dev;
}
#endif

void rb_scsi_decode_sense(rb_scsi_device_t dev, void *_sense, int sense_len)
{
#define rb_printf(...) \
    do{ if(dev->flags & RB_SCSI_DEBUG) dev->printf(dev->user, __VA_ARGS__); }while(0)

    uint8_t *sense = _sense;
    uint8_t type = sense[0] & 0x7f;
    switch(type)
    {
        case 0x70: case 0x73: rb_printf("Current sense: "); break;
        case 0x71: case 0x72: rb_printf("Previous sense: "); break;
        default: rb_printf("Unknown sense\n"); return;
    }
    unsigned key = sense[2] & 0xf;
    switch(key)
    {
        case 0: rb_printf("no sense"); break;
        case 1: rb_printf("recovered error"); break;
        case 2: rb_printf("not ready"); break;
        case 3: rb_printf("medium error"); break;
        case 4: rb_printf("hardware error"); break;
        case 5: rb_printf("illegal request"); break;
        case 6: rb_printf("unit attention"); break;
        case 7: rb_printf("data protect"); break;
        case 8: rb_printf("blank check"); break;
        case 9: rb_printf("vendor specific"); break;
        case 10: rb_printf("copy aborted"); break;
        case 11: rb_printf("aborted command"); break;
        default: rb_printf("unknown key"); break;
    }
    rb_printf("\n");

#undef rb_printf
}

void rb_scsi_free_list(struct rb_scsi_devent_t *list)
{
    if(list == NULL)
        return;
    for(struct rb_scsi_devent_t *p = list; p->scsi_path; p++)
    {
        free(p->scsi_path);
        if(p->block_path)
            free(p->block_path);
        if(p->vendor)
            free(p->vendor);
        if(p->model)
            free(p->model);
        if(p->rev)
            free(p->rev);
    }
    free(list);
}
