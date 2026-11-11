/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
 * Copyright (C) 2014 by Michael Sevakis
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
#include "config.h"
#include "system.h"
#include "sys/types.h"
#include "string-extra.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include "fs_attr.h"
#include "pathfuncs.h"
#include "disk_cache.h"
#include "file_internal.h" /* for struct filestr_cache */
#include "storage.h"
#include "timefuncs.h"
#include "rbunicode.h"
#include "debug.h"
#include "panic.h"
#include "disk.h"
/*#define LOGF_ENABLE*/
#include "logf.h"

#define BYTES2INT32(array, pos) \
    (((uint32_t)array[pos+0] <<  0) | \
     ((uint32_t)array[pos+1] <<  8) | \
     ((uint32_t)array[pos+2] << 16) | \
     ((uint32_t)array[pos+3] << 24))

#define INT322BYTES(array, pos, val) \
    ((array[pos+0] = (uint32_t)(val) >>  0), \
     (array[pos+1] = (uint32_t)(val) >>  8), \
     (array[pos+2] = (uint32_t)(val) >> 16), \
     (array[pos+3] = (uint32_t)(val) >> 24))

#define BYTES2INT16(array, pos) \
    (((uint32_t)array[pos+0] << 0) | \
     ((uint32_t)array[pos+1] << 8))

#define INT162BYTES(array, pos, val) \
    ((array[pos+0] = (uint16_t)(val) >> 0), \
     (array[pos+1] = (uint16_t)(val) >> 8))

#define FATTYPE_FAT12       0
#define FATTYPE_FAT16       1
#define FATTYPE_FAT32       2

/* BPB offsets; generic */
#define BS_JMPBOOT          0
#define BS_OEMNAME          3
#define BPB_BYTSPERSEC      11
#define BPB_SECPERCLUS      13
#define BPB_RSVDSECCNT      14
#define BPB_NUMFATS         16
#define BPB_ROOTENTCNT      17
#define BPB_TOTSEC16        19
#define BPB_MEDIA           21
#define BPB_FATSZ16         22
#define BPB_SECPERTRK       24
#define BPB_NUMHEADS        26
#define BPB_HIDDSEC         28
#define BPB_TOTSEC32        32

/* fat12/16 */
#define BS_DRVNUM           36
#define BS_RESERVED1        37
#define BS_BOOTSIG          38
#define BS_VOLID            39
#define BS_VOLLAB           43
#define BS_FILSYSTYPE       54

/* fat32 */
#define BPB_FATSZ32         36
#define BPB_EXTFLAGS        40
#define BPB_FSVER           42
#define BPB_ROOTCLUS        44
#define BPB_FSINFO          48
#define BPB_BKBOOTSEC       50
#define BS_32_DRVNUM        64
#define BS_32_BOOTSIG       66
#define BS_32_VOLID         67
#define BS_32_VOLLAB        71
#define BS_32_FILSYSTYPE    82

#define BPB_LAST_WORD       510

/* Short and long name directory entry template */
union raw_dirent
{
    struct /* short entry */
    {
        uint8_t  name[8+3];         /*  0 */
        uint8_t  attr;              /* 11 */
        uint8_t  ntres;             /* 12 */
        uint8_t  crttimetenth;      /* 13 */
        uint16_t crttime;           /* 14 */
        uint16_t crtdate;           /* 16 */
        uint16_t lstaccdate;        /* 18 */
        uint16_t fstclushi;         /* 20 */
        uint16_t wrttime;           /* 22 */
        uint16_t wrtdate;           /* 24 */
        uint16_t fstcluslo;         /* 26 */
        uint32_t filesize;          /* 28 */
                                    /* 32 */
    };
    struct /* long entry */
    {
        uint8_t  ldir_ord;          /*  0 */
        uint8_t  ldir_name1[10];    /*  1 */
        uint8_t  ldir_attr;         /* 11 */
        uint8_t  ldir_type;         /* 12 */
        uint8_t  ldir_chksum;       /* 13 */
        uint8_t  ldir_name2[12];    /* 14 */
        uint16_t ldir_fstcluslo;    /* 26 */
        uint8_t  ldir_name3[4];     /* 28 */
                                    /* 32 */
    };
    struct /* raw byte array */
    {
        uint8_t  data[32];          /*  0 */
                                    /* 32 */
    };
};


/* at most 20 LFN entries */
#define FATLONG_MAX_ORDER           20
#define FATLONG_NAME_CHARS          13
#define FATLONG_ORD_F_LAST          0x40

/* attributes */
#define ATTR_LONG_NAME          (ATTR_READ_ONLY | ATTR_HIDDEN | \
                                 ATTR_SYSTEM | ATTR_VOLUME_ID)
#define ATTR_LONG_NAME_MASK     (ATTR_READ_ONLY | ATTR_HIDDEN | \
                                 ATTR_SYSTEM | ATTR_VOLUME_ID | \
                                 ATTR_DIRECTORY | ATTR_ARCHIVE )

#define IS_LDIR_ATTR(attr) \
    (((attr) & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME)

#define IS_VOL_ID_ATTR(attr) \
    (((attr) & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) == ATTR_VOLUME_ID)

/* NTRES flags */
#define FAT_NTRES_LC_NAME           0x08
#define FAT_NTRES_LC_EXT            0x10

#if defined(MAX_VIRT_SECTOR_SIZE) || defined(MAX_VARIABLE_LOG_SECTOR)
#define LOG_SECTOR_SIZE(bpb) fat_bpb->sector_size
#else
#define LOG_SECTOR_SIZE(bpb) SECTOR_SIZE
#endif

#define CLUSTERS_PER_FAT_SECTOR     (LOG_SECTOR_SIZE(fat_bpb) / 4)
#define CLUSTERS_PER_FAT16_SECTOR   (LOG_SECTOR_SIZE(fat_bpb) / 2)
#define DIR_ENTRIES_PER_SECTOR      (LOG_SECTOR_SIZE(fat_bpb) / DIR_ENTRY_SIZE)
#define DIR_ENTRY_SIZE              32
#define FAT_BAD_MARK                0x0ffffff7
#define FAT_EOF_MARK                0x0ffffff8
#define FAT16_BAD_MARK              0xfff7
#define FAT16_EOF_MARK              0xfff8

struct fsinfo
{
    uint32_t freecount; /* last known free cluster count */
    uint32_t nextfree;  /* first cluster to start looking for free
                           clusters, or 0xffffffff for no hint */
};
/* fsinfo offsets */
#define FSINFO_SIGNATURE 0
#define FSINFO_FREECOUNT 488
#define FSINFO_NEXTFREE  492

#define FSINFO_SIGNATURE_VAL 0x41615252

#ifdef HAVE_FAT16SUPPORT
#define BPB_FN_SET16(bpb, fn)      (bpb)->fn##__ = fn##16
#define BPB_FN_SET32(bpb, fn)      (bpb)->fn##__ = fn##32
#define BPB_FN_DECL(fn, args...)   (*fn##__)(struct bpb *bpb , ##args)
#define BPB_CALL(fn, bpb, args...) ((bpb)->fn##__(bpb , ##args))

#define get_next_cluster(bpb, cluster) \
    BPB_CALL(get_next_cluster, (bpb), (cluster))
#define find_free_cluster(bpb, startcluster) \
    BPB_CALL(find_free_cluster, (bpb), (startcluster))
#define update_fat_entry(bpb, entry, value) \
    BPB_CALL(update_fat_entry, (bpb), (entry), (value))
#define fat_recalc_free_internal(bpb) \
    BPB_CALL(fat_recalc_free_internal, (bpb))
#else  /* !HAVE_FAT16SUPPORT */
/* bcm overlay v66: FAT32 or exFAT */
#define get_next_cluster(bpb, c) \
    ((bpb)->is_exfat ? get_next_cluster_ex((bpb), (c)) : get_next_cluster32((bpb), (c)))
#define find_free_cluster(bpb, c) \
    ((bpb)->is_exfat ? find_free_cluster_ex((bpb), (c)) : find_free_cluster32((bpb), (c)))
#define update_fat_entry(bpb, e, v) \
    ((bpb)->is_exfat ? update_fat_entry_ex((bpb), (e), (v)) : update_fat_entry32((bpb), (e), (v)))
#define fat_recalc_free_internal(bpb) \
    ((bpb)->is_exfat ? fat_recalc_free_internal_ex(bpb) : fat_recalc_free_internal32(bpb))
#endif /* HAVE_FAT16SUPPORT */
struct bpb;
static void update_fsinfo32(struct bpb *fat_bpb);
static long get_next_cluster_ex(struct bpb *fat_bpb, long cluster);
static long find_free_cluster_ex(struct bpb *fat_bpb, long startcluster);
static int update_fat_entry_ex(struct bpb *fat_bpb, unsigned long entry, unsigned long val);
static void fat_recalc_free_internal_ex(struct bpb *fat_bpb);

/* Note: This struct doesn't hold the raw values after mounting if
 * bpb_bytspersec isn't the same as the underlying device's logical
   sector size.  Sector counts are then normalized to that sector
   size.
*/
static struct bpb
{
    unsigned long bpb_bytspersec; /* Bytes per sector, typically 512 */
    unsigned long bpb_secperclus; /* Sectors per cluster */
    unsigned long bpb_rsvdseccnt; /* Number of reserved sectors */
    unsigned long bpb_totsec16;   /* Number of sectors on volume (old 16-bit) */
    uint8_t       bpb_numfats;    /* Number of FAT structures, typically 2 */
    uint8_t       bpb_media;      /* Media type (typically 0xf0 or 0xf8) */
    uint16_t      bpb_fatsz16;    /* Number of used sectors per FAT structure */
    unsigned long bpb_totsec32;   /* Number of sectors on the volume
                                     (new 32-bit) */
    uint16_t      last_word;      /* 0xAA55 */
    long          bpb_rootclus;

    /**** FAT32 specific *****/
    unsigned long bpb_fatsz32;
    unsigned long bpb_fsinfo;

    /* variables for internal use */
    unsigned long fatsize;
    sector_t      totalsectors;   /* bcm overlay v72: 64-bit -- exFAT volumes beyond 2 TiB */
    sector_t      rootdirsector;
    unsigned long firstdatasector;
    sector_t startsector;
    unsigned long dataclusters;
    unsigned long fatrgnstart;
    unsigned long fatrgnend;
    struct fsinfo fsinfo;
#ifdef HAVE_FAT16SUPPORT
    unsigned int bpb_rootentcnt;    /* Number of dir entries in the root */
    /* internals for FAT16 support */
    sector_t rootdirsectornum; /* sector offset of root dir relative to start
                                * of first pseudo cluster */
#endif /* HAVE_FAT16SUPPORT */

    /** Additional information kept for each volume **/
#ifdef HAVE_FAT16SUPPORT
    uint8_t is_fat16; /* true if we mounted a FAT16 partition, false if FAT32 */
#endif
#ifdef HAVE_MULTIDRIVE
    uint8_t drive;    /* on which physical device is this located */
#endif
#ifdef HAVE_MULTIVOLUME
    uint8_t volume;   /* on which volume is this located (shortcut) */
#endif
    uint8_t mounted;  /* true if volume is mounted, false otherwise */
#ifdef HAVE_FAT16SUPPORT
    /* some functions are different for different FAT types */
    long BPB_FN_DECL(get_next_cluster, long);
    long BPB_FN_DECL(find_free_cluster, long);
    int  BPB_FN_DECL(update_fat_entry, unsigned long, unsigned long);
    void BPB_FN_DECL(fat_recalc_free_internal);
#endif /* HAVE_FAT16SUPPORT */
#if defined(MAX_VIRT_SECTOR_SIZE) || defined(MAX_VARIABLE_LOG_SECTOR)
    uint16_t sector_size;
#endif
    /* bcm overlay v66: exFAT */
    uint8_t       is_exfat;
    uint16_t      ex_volflags;
    unsigned long ex_bitmap_cluster;
    uint64_t      ex_bitmap_bytes;
    sector_t      ex_bitmap_sector;
    unsigned long ex_upcase_cluster;
    uint64_t      ex_upcase_bytes;
    uint8_t       ex_pct_done;    /* PercentInUse already set to "unknown" */
    uint8_t       ex_dirty_ours;  /* VolumeDirty set by this session */
} fat_bpbs[NUM_VOLUMES]; /* mounted partition info */

#ifdef STORAGE_NEEDS_BOUNCE_BUFFER
#if defined(MAX_VIRT_SECTOR_SIZE)
#define BOUNCE_SECTOR_SIZE MAX_VIRT_SECTOR_SIZE
#elif defined(MAX_VARIABLE_LOG_SECTOR)
#define BOUNCE_SECTOR_SIZE MAX_VARIABLE_LOG_SECTOR
#elif defined(MAX_PHYS_SECTOR_SIZE)
#define BOUNCE_SECTOR_SIZE MAX_PHYS_SECTOR_SIZE
#else
#define BOUNCE_SECTOR_SIZE SECTOR_SIZE
#endif
#ifndef FAT_BOUNCE_SECTORS
#define FAT_BOUNCE_SECTORS 10
#endif
static uint8_t fat_bounce_buffers[NUM_VOLUMES][BOUNCE_SECTOR_SIZE*FAT_BOUNCE_SECTORS] STORAGE_ALIGN_ATTR;
#define FAT_BOUNCE_BUFFER(bpb) \
    (fat_bounce_buffers[IF_MV_VOL((bpb)->volume)])
#endif

#define IS_FAT_SECTOR(bpb, sector) \
    (!((sector) >= (bpb)->fatrgnend || (sector) < (bpb)->fatrgnstart))

/* set error code and jump to routine exit */
#define FAT_ERROR(_rc) \
    ({  __builtin_constant_p(_rc) ?            \
            ({ if (_rc != RC) rc = (_rc); }) : \
            ({ rc = (_rc); });                 \
        goto fat_error; })

#define FAT_BPB(volume) \
    ({  struct bpb * _bpb = &fat_bpbs[IF_MV_VOL(volume)]; \
        if (!_bpb->mounted)                          \
        {                                            \
            DEBUGF("%s() - volume %d not mounted\n", \
                   __func__, IF_MV_VOL(volume));     \
            _bpb = NULL;                             \
        }                                            \
        _bpb; })

enum add_dir_entry_flags
{
    DIRENT_RETURN      = 0x01, /* return the new short entry */
    DIRENT_TEMPL       = 0x0e, /* all TEMPL flags */
    DIRENT_TEMPL_CRT   = 0x02, /* use template crttime */
    DIRENT_TEMPL_WRT   = 0x04, /* use template wrttime */
    DIRENT_TEMPL_ACC   = 0x08, /* use template lstacc time */
    DIRENT_TEMPL_TIMES = 0x0e, /* keep all time fields */
};

struct fatlong_parse_state
{
    int     ord_max;
    int     ord;
    uint8_t chksum;
};

static void cache_commit(struct bpb *fat_bpb)
{
    dc_lock_cache();
#ifdef HAVE_FAT16SUPPORT
    if (!fat_bpb->is_fat16)
#endif
    if (!fat_bpb->is_exfat)
        update_fsinfo32(fat_bpb);
    dc_commit_all(IF_MV(fat_bpb->volume));
    dc_unlock_cache();
}

static void cache_discard(IF_MV_NONVOID(struct bpb *fat_bpb))
{
    dc_lock_cache();
    dc_discard_all(IF_MV(fat_bpb->volume));
    dc_unlock_cache();
}

/* caches a FAT or data area sector */
static void * cache_sector(struct bpb *fat_bpb, sector_t secnum)
{
    unsigned int flags;
    void *buf = dc_cache_probe(IF_MV(fat_bpb->volume,) secnum, &flags);

    if (!flags)
    {
        int rc = storage_read_sectors(IF_MD(fat_bpb->drive,)
                                      secnum + fat_bpb->startsector, 1, buf);
        if (UNLIKELY(rc < 0))
        {
            DEBUGF("%s() - Could not read sector %llu"
                   " (error %d)\n", __func__, (uint64_t)secnum, rc);
            dc_discard_buf(buf);
            return NULL;
        }
    }

    return buf;
}

/* returns a raw buffer for a sector; buffer counts as INUSE but filesystem
 * contents are NOT loaded before returning - use when completely overwriting
 * a sector's contents in order to avoid a fill */
static void * cache_sector_buffer(IF_MV(struct bpb *fat_bpb,)
                                  sector_t secnum)
{
    unsigned int flags;
    return dc_cache_probe(IF_MV(fat_bpb->volume,) secnum, &flags);
}

/* flush a cache buffer to storage */
void dc_writeback_callback(IF_MV(int volume,) sector_t sector, void *buf)
{
    struct bpb * const fat_bpb = &fat_bpbs[IF_MV_VOL(volume)];
    unsigned int copies = !IS_FAT_SECTOR(fat_bpb, sector) ?
                                1 : fat_bpb->bpb_numfats;

    if (fat_bpb->is_exfat && copies && IS_FAT_SECTOR(fat_bpb, sector) &&
        fat_bpb->ex_bitmap_sector)
    {
        /* bcm overlay v67: a FAT sector may link newly allocated clusters;
           their allocation bits go to disk first (a power cut then leaves
           at most clusters that are allocated but unused) */
        dc_commit_range(IF_MV(volume,) fat_bpb->ex_bitmap_sector,
                        (fat_bpb->ex_bitmap_bytes + LOG_SECTOR_SIZE(fat_bpb) - 1) /
                        LOG_SECTOR_SIZE(fat_bpb));
    }

    sector += fat_bpb->startsector;

    while (1)
    {
        int rc = storage_write_sectors(IF_MD(fat_bpb->drive,) sector, 1, buf);
        if (rc < 0)
        {
            panicf("%s() - Could not write sector %llu"
                   " (error %d)\n", __func__, (uint64_t)sector, rc);
        }

        if (--copies == 0)
            break;

        /* Update next FAT */
        sector += fat_bpb->fatsize;
    }
}

static void raw_dirent_set_fstclus(union raw_dirent *ent, long fstclus)
{
    ent->fstclushi = htole16(fstclus >> 16);
    ent->fstcluslo = htole16(fstclus & 0xffff);
}

static int bpb_is_sane(struct bpb *fat_bpb)
{
    if (fat_bpb->bpb_bytspersec % LOG_SECTOR_SIZE(fat_bpb))
    {
        DEBUGF("%s() - Error: sector size is not sane (%lu)\n",
               __func__, fat_bpb->bpb_bytspersec);
        return -1;
    }

    /* The fat_bpb struct does not hold the raw value of bpb_bytspersec, the
     * value is multiplied in cases where bpb_bytspersec != sector_size. We need
     * to undo that multiplication before we do the sanity check. */
    unsigned long secmult = fat_bpb->bpb_bytspersec / LOG_SECTOR_SIZE(fat_bpb);

    if (fat_bpb->bpb_secperclus * fat_bpb->bpb_bytspersec / secmult > 128*1024ul)
    {
        DEBUGF("%s() - Error: cluster size is larger than 128K "
               "(%lu * %lu = %lu)\n", __func__,
               fat_bpb->bpb_bytspersec, fat_bpb->bpb_secperclus,
               fat_bpb->bpb_bytspersec * fat_bpb->bpb_secperclus);
        return -2;
    }

    if (fat_bpb->bpb_numfats != 2)
    {
        DEBUGF("%s() - Warning: NumFATS is not 2 (%u)\n",
               __func__, fat_bpb->bpb_numfats);
    }

    if (fat_bpb->bpb_media != 0xf0 && fat_bpb->bpb_media < 0xf8)
    {
        DEBUGF("%s() - Warning: Non-standard media type "
               "(0x%02x)\n", __func__, fat_bpb->bpb_media);
    }

    if (fat_bpb->last_word != 0xaa55)
    {
        DEBUGF("%s() - Error: Last word is not "
               "0xaa55 (0x%04x)\n", __func__, fat_bpb->last_word);
        return -3;
    }

    if (fat_bpb->fsinfo.freecount >
        (fat_bpb->totalsectors - fat_bpb->firstdatasector) /
        fat_bpb->bpb_secperclus)
    {
        DEBUGF("%s() - Error: FSInfo.Freecount > disk size "
               "(0x%04lx)\n", __func__,
               fat_bpb->fsinfo.freecount);
        return -4;
    }

    return 0;
}

static uint8_t shortname_checksum(const unsigned char *shortname)
{
    /* calculate shortname checksum */
    uint8_t chksum = 0;

    for (unsigned int i = 0; i < 11; i++)
        chksum = (chksum << 7) + (chksum >> 1) + shortname[i];

    return chksum;
}

static void parse_short_direntry(const union raw_dirent *ent,
                                 struct fat_direntry *fatent)
{
    fatent->attr         = ent->attr;
    fatent->crttimetenth = ent->crttimetenth;
    fatent->crttime      = letoh16(ent->crttime);
    fatent->crtdate      = letoh16(ent->crtdate);
    fatent->lstaccdate   = letoh16(ent->lstaccdate);
    fatent->wrttime      = letoh16(ent->wrttime);
    fatent->wrtdate      = letoh16(ent->wrtdate);
    fatent->filesize     = letoh32(ent->filesize);
    fatent->firstcluster = ((uint32_t)letoh16(ent->fstcluslo)      ) |
                           ((uint32_t)letoh16(ent->fstclushi) << 16);

    /* fix the name */
    bool lowercase = ent->ntres & FAT_NTRES_LC_NAME;
    unsigned char c = ent->name[0];

    if (c == 0x05)  /* special kanji char */
        c = 0xe5;

    int j = 0;

    for (int i = 0; c != ' '; c = ent->name[i])
    {
        fatent->shortname[j++] = lowercase ? tolower(c) : c;

        if (++i >= 8)
            break;
    }

    if (ent->name[8] != ' ')
    {
        lowercase = ent->ntres & FAT_NTRES_LC_EXT;
        fatent->shortname[j++] = '.';

        for (int i = 8; i < 11 && (c = ent->name[i]) != ' '; i++)
            fatent->shortname[j++] = lowercase ? tolower(c) : c;
    }

    fatent->shortname[j] = 0;
}

static unsigned char char2dos(unsigned char c, int *np)
{
    /* FIXME: needs conversion to OEM charset FIRST but there is currently
       no unicode function for that! */

    /* smallest tables with one-step lookup that directly map the lists;
       here we're only concerned with what gets through the longname
       filter (check_longname will have been called earlier so common
       illegal chars are neither in these tables nor checked for) */
    static const unsigned char remove_chars_tbl[3] =
        { 0, '.', ' ' };

    static const unsigned char replace_chars_tbl[11] =
        { ',', 0, 0, '[', ';', ']', '=', 0, 0, 0, '+' };

    if (remove_chars_tbl[c % 3] == c)
    {
        /* Illegal char, remove */
        c = 0;
        *np = 0;
    }
    else if (c >= 0x80 || replace_chars_tbl[c % 11] == c)
    {
        /* Illegal char, replace (note: NTFS behavior for extended chars) */
        c = '_';
        *np = 0;
    }
    else
    {
        c = toupper(c);
    }

    return c;
}

/* convert long name into dos name, possibly recommending randomization */
static void create_dos_name(unsigned char *basisname,
                            const unsigned char *name, int *np)
{
    int i;

    /* FIXME: needs conversion to OEM charset FIRST but there is currently
       no unicode function for that! */

    /* as per FAT spec, set "lossy conversion" flag if any destructive
       alterations to the name occur other than case changes */
    *np = -1;

    /* find extension part */
    unsigned char *ext = strrchr(name, '.');
    if (ext && (ext == name || strchr(ext, ' ')))
        ext = NULL; /* handle .dotnames / extensions cannot have spaces */

    /* name part */
    for (i = 0; *name && (!ext || name < ext) && (i < 8); name++)
    {
        unsigned char c = char2dos(*name, np);
        if (c)
            basisname[i++] = c;
    }

    /* pad both name and extension */
    while (i < 11)
        basisname[i++] = ' ';

    if (basisname[0] == 0xe5) /* special kanji character */
        basisname[0] = 0x05;

    /* extension part */
    if (!ext++)
        return; /* no extension */

    for (i = 8; *ext && i < 11; ext++)
    {
        unsigned char c = char2dos(*ext, np);
        if (c)
            basisname[i++] = c;
    }

    if (*ext)
        *np = 0; /* extension too long */
}

static void randomize_dos_name(unsigned char *dosname,
                               const unsigned char *basisname,
                               int *np)
{
    int n = *np;

    memcpy(dosname, basisname, 11);

    if (n < 0)
    {
        /* first one just copies */
        *np = 0;
        return;
    }

    /* the "~n" string can range from "~1" to "~999999"
       of course a directory can have at most 65536 entries which means
       the numbers will never be required to get that big in order to map
       to a unique name */
    if (++n > 999999)
        n = 1;

    unsigned char numtail[8]; /* holds "~n" */
    unsigned int numtaillen = snprintf(numtail, 8, "~%d", n);

    unsigned int basislen = 0;
    while (basislen < 8 && basisname[basislen] != ' ')
        basislen++;

    memcpy(dosname + MIN(8 - numtaillen, basislen), numtail, numtaillen);

    *np = n;
}

/* check long filename for validity */
static int check_longname(const unsigned char *name)
{
    /* smallest table with one-step lookup that directly maps the list */
    static const unsigned char invalid_chars_tbl[19] =
    {
        0, ':', 0, '<', '*', '>', '?', 0, 0,
        '/', '|', 0, 0, 0x7f, 0, '"', '\\', 0, 0
    };

    if (!name)
        return -1;

    unsigned int c = *name;

    do
    {
        if (c < 0x20 || invalid_chars_tbl[c % 19] == c)
            return -2;
    }
    while ((c = *++name));

    /* check trailing space(s) and periods */
    c = *--name;
    if (c == ' ' || c == '.')
        return -3;

    return 0;
}

/* Get first longname entry name offset */
static inline unsigned int longent_char_first(void)
{
    return 1;
}

/* Get the next longname entry offset or 0 if the end is reached */
static inline unsigned int longent_char_next(unsigned int i)
{
    switch (i += 2)
    {
    /* flip endian for elements 14 - 27 */
    case 26: i -= 1; /* return 28 */
    /* Fall-Through */
    case 11: i += 3; /* return 14 */
    }
    /* BYTES2INT16() uses [i + 0] and [i + 1] therefore
     * 30 is the max element available in the raw byte array of size 32 */
    return i < 31 ? i : 0;
}

/* initialize the parse state; call before parsing first long entry */
static void NO_INLINE fatlong_parse_start(struct fatlong_parse_state *lnparse)
{
    /* no inline so gcc can't figure out what isn't initialized here;
       ord_max is king as to the validity of all other fields */
    lnparse->ord_max = -1; /* one resync per parse operation */
}

/* convert the FAT long name entry to a contiguous segment */
static bool fatlong_parse_entry(struct fatlong_parse_state *lnparse,
                                const union raw_dirent *ent,
                                struct fat_direntry *fatent)
{
    int ord = ent->ldir_ord;

    if (ord & FATLONG_ORD_F_LAST)
    {
        /* this entry is the first long entry (first in order but
           containing last part) */
        ord &= ~FATLONG_ORD_F_LAST;

        if (ord == 0 || ord > FATLONG_MAX_ORDER)
        {
            lnparse->ord_max = 0;
            return true;
        }

        lnparse->ord_max = ord;
        lnparse->ord     = ord;
        lnparse->chksum  = ent->ldir_chksum;
    }
    else
    {
        /* valid ordinals yet? */
        if (lnparse->ord_max <= 0)
        {
            if (lnparse->ord_max == 0)
                return true;

            lnparse->ord_max = 0;
            return false; /* try resync */
        }

        /* check ordinal continuity and that the checksum matches the
           one stored in the last entry */
        if (ord == 0 || ord != lnparse->ord - 1 ||
            lnparse->chksum != ent->ldir_chksum)
        {
            lnparse->ord_max = 0;
            return true;
        }
    }

    /* so far so good; save entry information */
    lnparse->ord = ord;

    /* Treat entries as opaque 16-bit values;
       utf8decode happens in fatlong_parse_finish() */
    uint16_t *ucsp = fatent->ucssegs[ord - 1 + 5];
    unsigned int i = longent_char_first();

    while ((*ucsp++ = BYTES2INT16(ent->data, i)))
    {
        if (!(i = longent_char_next(i)))
            return true;
    }

    /* segment may end early only in last entry */
    if (ord == lnparse->ord_max)
    {
        /* the only valid padding, if any, is 0xffff */
        do
        {
            if (!(i = longent_char_next(i)))
                return true;
        }
        while (BYTES2INT16(ent->data, i) == 0xffff);
    }

    /* long filename is corrupt */
    lnparse->ord_max = 0;
    return true;
}

/* finish parsing of the longname entries and do the conversion to
   UTF-8 if we have all the segments */
static bool fatlong_parse_finish(struct fatlong_parse_state *lnparse,
                                 const union raw_dirent *ent,
                                 struct fat_direntry *fatent)
{
    parse_short_direntry(ent, fatent);

    /* ord_max must not have been set to <= 0 because of any earlier problems
       and the ordinal immediately before the shortname entry must be 1 */
    if (lnparse->ord_max <= 0 || lnparse->ord != 1)
        return false;

     /* check the longname checksums against the shortname checksum */
    if (lnparse->chksum != shortname_checksum(ent->name))
        return false;

    /* longname is good so far so convert all the segments to UTF-8 */
    unsigned char * const name = fatent->name;
    unsigned char *p = name;

    /* ensure the last segment is NULL-terminated if it is filled */
    fatent->ucssegs[lnparse->ord_max + 5][0] = 0x0000;

    unsigned long ucc;     /* Decoded codepoint */
    uint16_t *ucsp, ucs;
    for (ucsp = fatent->ucssegs[5], ucs=*ucsp; ucs; ucs = *++ucsp)
    {
        /* end should be hit before ever seeing padding */
        if (ucs == 0xffff)
            return false;

#ifdef UNICODE32
        /* Check for a surrogate UTF16 pair */
        if (ucs >= 0xd800 && ucs < 0xdc00 &&
            *(ucsp+1) >= 0xdc00 && *(ucsp+1) < 0xe000) {
            ucc = 0x10000 + (((ucs & 0x3ff) << 10) | (*(ucsp+1) & 0x3ff));
            ucsp++;
        } else
#endif
            ucc = ucs;

        if ((p = utf8encode(ucc, p)) - name > FAT_DIRENTRY_NAME_MAX)
            return false;
    }

    /* longname ok */
    *p = '\0';
    return true;
}

static sector_t cluster2sec(struct bpb *fat_bpb, long cluster)
{
    long zerocluster = 2;

    if (fat_bpb->is_exfat && cluster > 0)
        cluster &= 0x1fffffff;  /* EXFAT_CLUS_MASK: drop the NoFatChain / >4GiB flags */

    /* negative clusters (FAT16 root dir) don't get the 2 offset */
#ifdef HAVE_FAT16SUPPORT
    if (fat_bpb->is_fat16 && cluster < 0)
    {
        zerocluster = 0;
    }
    else
#endif /* HAVE_FAT16SUPPORT */
    if ((unsigned long)cluster > fat_bpb->dataclusters + 1)
    {
        DEBUGF( "%s() - Bad cluster number (%ld)\n", __func__, cluster);
        return 0;
    }

    return (sector_t)(unsigned long)(cluster - zerocluster)*fat_bpb->bpb_secperclus
            + fat_bpb->firstdatasector;
}

#ifdef HAVE_FAT16SUPPORT
static long get_next_cluster16(struct bpb *fat_bpb, long startcluster)
{
    /* if FAT16 root dir, dont use the FAT */
    if (startcluster < 0)
        return startcluster + 1;

    unsigned long entry = startcluster;
    unsigned long sector = entry / CLUSTERS_PER_FAT16_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT16_SECTOR;

    dc_lock_cache();

    uint16_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (!sec)
    {
        dc_unlock_cache();
        DEBUGF("%s: Could not cache sector %lu\n", __func__, sector);
        return -1;
    }

    long next = letoh16(sec[offset]);

    /* is this last cluster in chain? */
    if (next >= FAT16_EOF_MARK)
        next = 0;

    dc_unlock_cache();
    return next;
}

static long find_free_cluster16(struct bpb *fat_bpb, long startcluster)
{
    unsigned long entry = startcluster;
    unsigned long sector = entry / CLUSTERS_PER_FAT16_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT16_SECTOR;

    for (unsigned long i = 0; i < fat_bpb->fatsize; i++)
    {
        unsigned long nr = (i + sector) % fat_bpb->fatsize;
        uint16_t *sec = cache_sector(fat_bpb, nr + fat_bpb->fatrgnstart);
        if (!sec)
            break;

        for (unsigned long j = 0; j < CLUSTERS_PER_FAT16_SECTOR; j++)
        {
            unsigned long k = (j + offset) % CLUSTERS_PER_FAT16_SECTOR;

            if (letoh16(sec[k]) == 0x0000)
            {
                unsigned long c = nr * CLUSTERS_PER_FAT16_SECTOR + k;
                 /* Ignore the reserved clusters 0 & 1, and also
                    cluster numbers out of bounds */
                if (c < 2 || c > fat_bpb->dataclusters + 1)
                    continue;

                DEBUGF("%s(%lx) == %lx\n", __func__, startcluster, c);

                fat_bpb->fsinfo.nextfree = c;
                return c;
            }
        }

        offset = 0;
    }

    DEBUGF("%s(%lx) == 0\n", __func__, startcluster);
    return 0; /* 0 is an illegal cluster number */
}

static int update_fat_entry16(struct bpb *fat_bpb, unsigned long entry,
                              unsigned long val)
{
    unsigned long sector = entry / CLUSTERS_PER_FAT16_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT16_SECTOR;

    val &= 0xFFFF;

    DEBUGF("%s(entry:%lx,val:%lx)\n", __func__, entry, val);

    if (entry == val)
        panicf("Creating FAT16 loop: %lx,%lx\n", entry, val);

    if (entry < 2)
        panicf("Updating reserved FAT16 entry %lu\n", entry);

    dc_lock_cache();

    int16_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (!sec)
    {
        dc_unlock_cache();
        DEBUGF("Could not cache sector %lu\n", sector);
        return -1;
    }

    uint16_t curval = letoh16(sec[offset]);

    if (val)
    {
        /* being allocated */
        if (curval == 0x0000 && fat_bpb->fsinfo.freecount > 0)
            fat_bpb->fsinfo.freecount--;
    }
    else
    {
        /* being freed */
        if (curval != 0x0000)
            fat_bpb->fsinfo.freecount++;
    }

    DEBUGF("%lu free clusters\n", fat_bpb->fsinfo.freecount);

    sec[offset] = htole16(val);
    dc_dirty_buf(sec);

    dc_unlock_cache();
    return 0;
}

static void fat_recalc_free_internal16(struct bpb *fat_bpb)
{
    unsigned long free = 0;

    for (unsigned long i = 0; i < fat_bpb->fatsize; i++)
    {
        uint16_t *sec = cache_sector(fat_bpb, i + fat_bpb->fatrgnstart);
        if (!sec)
            break;

        for (unsigned long j = 0; j < CLUSTERS_PER_FAT16_SECTOR; j++)
        {
            unsigned long c = i * CLUSTERS_PER_FAT16_SECTOR + j;

            if (c < 2 || c > fat_bpb->dataclusters + 1) /* nr 0 is unused */
                continue;

            if (letoh16(sec[j]) != 0x0000)
                continue;

            free++;
            if (fat_bpb->fsinfo.nextfree == 0xffffffff)
                fat_bpb->fsinfo.nextfree = c;
        }
    }

    fat_bpb->fsinfo.freecount = free;
}
#endif /* HAVE_FAT16SUPPORT */

static void update_fsinfo32(struct bpb *fat_bpb)
{
    uint8_t *fsinfo = cache_sector(fat_bpb, fat_bpb->bpb_fsinfo);
    if (!fsinfo)
    {
        DEBUGF("%s() - Couldn't read FSInfo", __func__);
        return;
    }

    INT322BYTES(fsinfo, FSINFO_FREECOUNT, fat_bpb->fsinfo.freecount);
    INT322BYTES(fsinfo, FSINFO_NEXTFREE, fat_bpb->fsinfo.nextfree);
    dc_dirty_buf(fsinfo);
}

static long get_next_cluster32(struct bpb *fat_bpb, long startcluster)
{
    unsigned long entry = startcluster;
    unsigned long sector = entry / CLUSTERS_PER_FAT_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT_SECTOR;

    dc_lock_cache();

    uint32_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (!sec)
    {
        dc_unlock_cache();
        DEBUGF("%s: Could not cache sector %lu\n", __func__, sector);
        return -1;
    }

    long next = letoh32(sec[offset]) & 0x0fffffff;

    /* is this last cluster in chain? */
    if (next >= FAT_EOF_MARK)
        next = 0;

    dc_unlock_cache();
    return next;
}

static long find_free_cluster32(struct bpb *fat_bpb, long startcluster)
{
    unsigned long entry = startcluster;
    unsigned long sector = entry / CLUSTERS_PER_FAT_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT_SECTOR;

    for (unsigned long i = 0; i < fat_bpb->fatsize; i++)
    {
        unsigned long nr = (i + sector) % fat_bpb->fatsize;
        uint32_t *sec = cache_sector(fat_bpb, nr + fat_bpb->fatrgnstart);
        if (!sec)
            break;

        for (unsigned long j = 0; j < CLUSTERS_PER_FAT_SECTOR; j++)
        {
            unsigned long k = (j + offset) % CLUSTERS_PER_FAT_SECTOR;

            if (!(letoh32(sec[k]) & 0x0fffffff))
            {
                unsigned long c = nr * CLUSTERS_PER_FAT_SECTOR + k;
                 /* Ignore the reserved clusters 0 & 1, and also
                    cluster numbers out of bounds */
                if (c < 2 || c > fat_bpb->dataclusters + 1)
                    continue;

                DEBUGF("%s(%lx) == %lx\n", __func__, startcluster, c);

                fat_bpb->fsinfo.nextfree = c;
                return c;
            }
        }

        offset = 0;
    }

    DEBUGF("%s(%lx) == 0\n", __func__, startcluster);
    return 0; /* 0 is an illegal cluster number */
}

static int update_fat_entry32(struct bpb *fat_bpb, unsigned long entry,
                              unsigned long val)
{
    unsigned long sector = entry / CLUSTERS_PER_FAT_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT_SECTOR;

    DEBUGF("%s(entry:%lx,val:%lx)\n", __func__, entry, val);

    if (entry == val)
        panicf("Creating FAT32 loop: %lx,%lx\n", entry, val);

    if (entry < 2)
        panicf("Updating reserved FAT32 entry %lu\n", entry);

    dc_lock_cache();

    uint32_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (!sec)
    {
        dc_unlock_cache();
        DEBUGF("Could not cache sector %lu\n", sector);
        return -1;
    }

    uint32_t curval = letoh32(sec[offset]);

    if (val)
    {
        /* being allocated */
        if (!(curval & 0x0fffffff) && fat_bpb->fsinfo.freecount > 0)
            fat_bpb->fsinfo.freecount--;
    }
    else
    {
        /* being freed */
        if (curval & 0x0fffffff)
            fat_bpb->fsinfo.freecount++;
    }

    DEBUGF("%lu free clusters\n", fat_bpb->fsinfo.freecount);

    /* don't change top 4 bits */
    sec[offset] = htole32((curval & 0xf0000000) | (val & 0x0fffffff));
    dc_dirty_buf(sec);

    dc_unlock_cache();
    return 0;
}

static void fat_recalc_free_internal32(struct bpb *fat_bpb)
{
    unsigned long free = 0;

    for (unsigned long i = 0; i < fat_bpb->fatsize; i++)
    {
        uint32_t *sec = cache_sector(fat_bpb, i + fat_bpb->fatrgnstart);
        if (!sec)
            break;

        for (unsigned long j = 0; j < CLUSTERS_PER_FAT_SECTOR; j++)
        {
            unsigned long c = i * CLUSTERS_PER_FAT_SECTOR + j;

            if (c < 2 || c > fat_bpb->dataclusters + 1) /* nr 0 is unused */
                continue;

            if (letoh32(sec[j]) & 0x0fffffff)
                continue;

            free++;
            if (fat_bpb->fsinfo.nextfree == 0xffffffff)
                fat_bpb->fsinfo.nextfree = c;
        }
    }

    fat_bpb->fsinfo.freecount = free;
    update_fsinfo32(fat_bpb);
}

static int exfat_mount_internal(struct bpb *fat_bpb, uint8_t *buf);
static int fat_mount_internal(struct bpb *fat_bpb)
{
    int rc;
    /* safe to grab buffer: bpb is irrelevant and no sector will be cached
       for this volume since it isn't mounted */
    uint8_t * const buf = dc_get_buffer();
    if (!buf)
        FAT_ERROR(-1);

    /* Read the sector */
    rc = storage_read_sectors(IF_MD(fat_bpb->drive,) fat_bpb->startsector,
                              1, buf);
    if(rc)
    {
        DEBUGF("%s() - Couldn't read BPB"
               " (err %d)\n", __func__, rc);
        FAT_ERROR(rc * 10 - 2);
    }

    fat_bpb->is_exfat = 0;
    if (!memcmp(buf + BS_OEMNAME, "EXFAT   ", 8))
    {
        /* bcm overlay v66 */
        rc = exfat_mount_internal(fat_bpb, buf);
        if (rc < 0)
        {
            fat_bpb->is_exfat = 0;
            FAT_ERROR(rc * 10 - 1);
        }
#ifdef HAVE_FAT16SUPPORT
        fat_bpb->get_next_cluster__ = get_next_cluster_ex;
        fat_bpb->find_free_cluster__ = find_free_cluster_ex;
        fat_bpb->update_fat_entry__ = update_fat_entry_ex;
        fat_bpb->fat_recalc_free_internal__ = fat_recalc_free_internal_ex;
#endif
        /* the free count comes from the allocation bitmap; reading it is
           deferred until something asks for it (fat_size), so mounting does not
           read ~2 MB (2 TB, 128 KB clusters) at every boot / USB unplug.
           0xfffffffe = "not counted yet" (fat_mount recounts 0xffffffff) */
        fat_bpb->fsinfo.freecount = 0xfffffffe;
#ifdef BOOTLOADER
        fat_bpb->fsinfo.freecount = 0;
#endif
        rc = 0;
        goto fat_error;
    }

    fat_bpb->bpb_bytspersec = BYTES2INT16(buf, BPB_BYTSPERSEC);
    unsigned long secmult = fat_bpb->bpb_bytspersec / LOG_SECTOR_SIZE(fat_bpb);
    /* Sanity check is performed later */

    fat_bpb->bpb_secperclus = secmult * buf[BPB_SECPERCLUS];
    fat_bpb->bpb_rsvdseccnt = secmult * BYTES2INT16(buf, BPB_RSVDSECCNT);
    fat_bpb->bpb_numfats    = buf[BPB_NUMFATS];
    fat_bpb->bpb_media      = buf[BPB_MEDIA];
    fat_bpb->bpb_fatsz16    = secmult * BYTES2INT16(buf, BPB_FATSZ16);
    fat_bpb->bpb_fatsz32    = secmult * BYTES2INT32(buf, BPB_FATSZ32);
    fat_bpb->bpb_totsec16   = secmult * BYTES2INT16(buf, BPB_TOTSEC16);
    fat_bpb->bpb_totsec32   = secmult * BYTES2INT32(buf, BPB_TOTSEC32);
    fat_bpb->last_word      = BYTES2INT16(buf, BPB_LAST_WORD);

    /* calculate a few commonly used values */
    if (fat_bpb->bpb_fatsz16 != 0)
        fat_bpb->fatsize = fat_bpb->bpb_fatsz16;
    else
        fat_bpb->fatsize = fat_bpb->bpb_fatsz32;

    fat_bpb->fatrgnstart = fat_bpb->bpb_rsvdseccnt;
    fat_bpb->fatrgnend   = fat_bpb->bpb_rsvdseccnt + fat_bpb->fatsize;

    if (fat_bpb->bpb_totsec16 != 0)
        fat_bpb->totalsectors = fat_bpb->bpb_totsec16;
    else
        fat_bpb->totalsectors = fat_bpb->bpb_totsec32;

    unsigned int rootdirsectors = 0;
#ifdef HAVE_FAT16SUPPORT
    fat_bpb->bpb_rootentcnt = BYTES2INT16(buf, BPB_ROOTENTCNT);

    if (!fat_bpb->bpb_bytspersec)
        FAT_ERROR(-3);

    rootdirsectors = secmult * ((fat_bpb->bpb_rootentcnt * DIR_ENTRY_SIZE
                     + fat_bpb->bpb_bytspersec - 1) / fat_bpb->bpb_bytspersec);
#endif /* HAVE_FAT16SUPPORT */

    fat_bpb->firstdatasector = fat_bpb->bpb_rsvdseccnt
        + fat_bpb->bpb_numfats * fat_bpb->fatsize
        + rootdirsectors;

    /* Determine FAT type */
    unsigned long datasec = fat_bpb->totalsectors - fat_bpb->firstdatasector;

    if (!fat_bpb->bpb_secperclus)
        FAT_ERROR(-4);

    fat_bpb->dataclusters = datasec / fat_bpb->bpb_secperclus;

#ifdef TEST_FAT
    /*
      we are sometimes testing with "illegally small" fat32 images,
      so we don't use the proper fat32 test case for test code
    */
    if (fat_bpb->bpb_fatsz16)
#else /* !TEST_FAT */
    if (fat_bpb->dataclusters < 65525)
#endif /* TEST_FAT */
    { /* FAT16 */
#ifdef HAVE_FAT16SUPPORT
        fat_bpb->is_fat16 = true;
        if (fat_bpb->dataclusters < 4085)
        { /* FAT12 */
            DEBUGF("This is FAT12. Go away!\n");
            FAT_ERROR(-5);
        }
#else /* !HAVE_FAT16SUPPORT */
        DEBUGF("This is not FAT32. Go away!\n");
        FAT_ERROR(-6);
#endif /* HAVE_FAT16SUPPORT */
    }

#ifdef HAVE_FAT16SUPPORT
    if (fat_bpb->is_fat16)
    {
        /* FAT16 specific part of BPB */
        fat_bpb->rootdirsector = fat_bpb->bpb_rsvdseccnt
            + fat_bpb->bpb_numfats * fat_bpb->bpb_fatsz16;
        long dirclusters = ((rootdirsectors + fat_bpb->bpb_secperclus - 1)
            / fat_bpb->bpb_secperclus); /* rounded up, to full clusters */
        /* I assign negative pseudo cluster numbers for the root directory,
           their range is counted upward until -1. */
        fat_bpb->bpb_rootclus = 0 - dirclusters; /* backwards, before the data */
        fat_bpb->rootdirsectornum = dirclusters * fat_bpb->bpb_secperclus
            - rootdirsectors;
    }
    else
#endif /* HAVE_FAT16SUPPORT */
    {
        /* FAT32 specific part of BPB */
        fat_bpb->bpb_rootclus  = BYTES2INT32(buf, BPB_ROOTCLUS);
        fat_bpb->bpb_fsinfo    = secmult * BYTES2INT16(buf, BPB_FSINFO);
        fat_bpb->rootdirsector = cluster2sec(fat_bpb, fat_bpb->bpb_rootclus);
    }

    rc = bpb_is_sane(fat_bpb);
    if (rc < 0)
    {
        DEBUGF("%s: BPB is insane!\n", __func__);
        FAT_ERROR(rc * 10 - 7);
    }

#ifdef HAVE_FAT16SUPPORT
    if (fat_bpb->is_fat16)
    {
        fat_bpb->fsinfo.freecount = 0xffffffff; /* force recalc later */
        fat_bpb->fsinfo.nextfree = 0xffffffff;
    }
    else
#endif /* HAVE_FAT16SUPPORT */
    {
        /* Read the fsinfo sector */
        rc = storage_read_sectors(IF_MD(fat_bpb->drive,)
                fat_bpb->startsector + fat_bpb->bpb_fsinfo, 1, buf);

        if (rc < 0)
        {
            DEBUGF("%s() - Couldn't read FSInfo"
                   " (error code %d)\n", __func__, rc);
            FAT_ERROR(rc * 10 - 8);
        }

	/* Sanity check FS info */
	long info = BYTES2INT32(buf, FSINFO_SIGNATURE);
	if (info != FSINFO_SIGNATURE_VAL) {
		DEBUGF("%s() FSInfo signature mismatch (%lx)\n",
		       __func__, info);
		FAT_ERROR(-9);
	}

        fat_bpb->fsinfo.freecount = BYTES2INT32(buf, FSINFO_FREECOUNT);
        fat_bpb->fsinfo.nextfree = BYTES2INT32(buf, FSINFO_NEXTFREE);
    }

#ifdef HAVE_FAT16SUPPORT
    /* Fix up calls that change per FAT type */
    if (fat_bpb->is_fat16)
    {
        BPB_FN_SET16(fat_bpb, get_next_cluster);
        BPB_FN_SET16(fat_bpb, find_free_cluster);
        BPB_FN_SET16(fat_bpb, update_fat_entry);
        BPB_FN_SET16(fat_bpb, fat_recalc_free_internal);
    }
    else
    {
        BPB_FN_SET32(fat_bpb, get_next_cluster);
        BPB_FN_SET32(fat_bpb, find_free_cluster);
        BPB_FN_SET32(fat_bpb, update_fat_entry);
        BPB_FN_SET32(fat_bpb, fat_recalc_free_internal);
    }
#endif /* HAVE_FAT16SUPPORT */

    rc = 0;
fat_error:
    dc_release_buffer(buf);
    return rc;
}

static union raw_dirent * cache_direntry(struct bpb *fat_bpb,
                                         struct fat_filestr *filestr,
                                         unsigned int entry)
{
    filestr->eof = false;

#ifdef BOOTLOADER
    if (entry >= MAX_DIRENTRIES)
#else
    if (entry >= (fat_bpb->is_exfat ? EXFAT_MAX_DIRENTRIES : MAX_DIRENTRIES))   /* bcm overlay v89 */
#endif
    {
        DEBUGF("%s() - Dir is too large (entry %u)\n", __func__, entry);
        return NULL;
    }

    unsigned long sector = entry / DIR_ENTRIES_PER_SECTOR;

    if (fat_query_sectornum(filestr) != sector + 1)
    {
        int rc = fat_seek(filestr, sector + 1);
        if (rc < 0)
        {
            if (rc == FAT_SEEK_EOF)
            {
                DEBUGF("%s() - End of dir (entry %u)\n", __func__, entry);
                fat_seek(filestr, sector);
                filestr->eof = true;
            }

            return NULL;
        }
    }

    union raw_dirent *ent = cache_sector(fat_bpb, filestr->lastsector);

    if (ent)
        ent += entry % DIR_ENTRIES_PER_SECTOR;

    return ent;
}

static long next_write_cluster(struct bpb *fat_bpb, long oldcluster)
{
    DEBUGF("%s(old:%lx)\n", __func__, oldcluster);

    long cluster = 0;

    /* cluster already allocated? */
    if (oldcluster)
        cluster = get_next_cluster(fat_bpb, oldcluster);

    if (!cluster)
    {
        /* passed end of existing entries and now need to append */
    #ifdef HAVE_FAT16SUPPORT
        if (UNLIKELY(oldcluster < 0))
            return 0; /* negative, pseudo-cluster of the root dir */
                      /* impossible to append something to the root */
    #endif /* HAVE_FAT16SUPPORT */

        dc_lock_cache();

        long findstart = oldcluster > 0 ?
            oldcluster + 1 : (long)fat_bpb->fsinfo.nextfree;

        cluster = find_free_cluster(fat_bpb, findstart);

        if (cluster)
        {
            /* create the cluster chain */
            if (oldcluster)
                update_fat_entry(fat_bpb, oldcluster, cluster);

            update_fat_entry(fat_bpb, cluster, FAT_EOF_MARK);
        }
        else
        {
        #ifdef TEST_FAT
            if (fat_bpb->fsinfo.freecount > 0)
                panicf("There is free space, but find_free_cluster() "
                       "didn't find it!\n");
        #endif
            DEBUGF("Disk full!\n");
        }

        dc_unlock_cache();
    }

    return cluster;
}

/* extend dir file by one cluster and clear it; file position should be at the
   current last cluster before calling and size of dir checked */
static int fat_extend_dir(struct bpb *fat_bpb, struct fat_filestr *dirstr)
{
    DEBUGF("%s()\n", __func__);

    int rc;

    long cluster    = dirstr->lastcluster;
    long newcluster = next_write_cluster(fat_bpb, cluster);

    if (!newcluster)
    {
        /* no more room or something went wrong */
        DEBUGF("Out of space\n");
        FAT_ERROR(FAT_RC_ENOSPC);
    }

    /* we must clear whole clusters */
    sector_t startsector = cluster2sec(fat_bpb, newcluster);
    sector_t sector      = startsector - 1;

    if (startsector == 0)
        FAT_ERROR(-1);

    for (unsigned int i = 0; i < fat_bpb->bpb_secperclus; i++)
    {
        dc_lock_cache();

        void *sec = cache_sector_buffer(IF_MV(fat_bpb,) ++sector);
        if (!sec)
        {
            dc_unlock_cache();
            DEBUGF("Cannot clear cluster %ld\n", newcluster);
            update_fat_entry(fat_bpb, newcluster, 0);
            FAT_ERROR(-2);
        }

        memset(sec, 0, LOG_SECTOR_SIZE(fat_bpb));
        dc_dirty_buf(sec);
        dc_unlock_cache();
    }

    if (!dirstr->fatfilep->firstcluster)
        dirstr->fatfilep->firstcluster = newcluster;

    dirstr->lastcluster = newcluster;
    dirstr->lastsector  = sector;
    dirstr->clusternum++;
    dirstr->sectornum   = sector - startsector;
    dirstr->eof         = false;

    rc = 0;
fat_error:
    return rc;
}

static void fat_open_internal(IF_MV(int volume,) long startcluster,
                              struct fat_file *file)
{
#ifdef HAVE_MULTIVOLUME
    file->volume       = volume;
#endif
    file->firstcluster = startcluster;
    file->dircluster   = 0;
    file->e.entry      = 0;
    file->e.entries    = 0;
}

#if CONFIG_RTC
static void fat_time(uint16_t *date, uint16_t *time, int16_t *tenth)
{
    struct tm *tm = get_time();

    if (date)
    {
        *date = ((tm->tm_year - 80) << 9) |
                ((tm->tm_mon + 1) << 5) |
                tm->tm_mday;
    }

    if (time)
    {
        *time = (tm->tm_hour << 11) |
                (tm->tm_min << 5) |
                (tm->tm_sec >> 1);
    }

    if (tenth)
        *tenth = (tm->tm_sec & 1) * 100;
}

#else /* !CONFIG_RTC */

static void fat_time(uint16_t *date, uint16_t *time, int16_t *tenth)
{
    /* non-RTC version returns an increment from the supplied time, or a
     * fixed standard time/date if no time given as input */

    /* Macros to convert a 2-digit string to a decimal constant.
       (YEAR), MONTH and DAY are set by the date command, which outputs
       DAY as 00..31 and MONTH as 01..12. The leading zero would lead to
       misinterpretation as an octal constant. */
    #define S100(x) 1 ## x
    #define C2DIG2DEC(x) (S100(x)-100)
    /* The actual build date, as FAT date constant */
    #define BUILD_DATE_FAT (((YEAR - 1980) << 9) \
                            | (C2DIG2DEC(MONTH) << 5) \
                            | C2DIG2DEC(DAY))

    bool date_forced = false;
    bool next_day = false;
    unsigned time2 = 0; /* double time, for CRTTIME with 1s precision */

    if (date && *date < BUILD_DATE_FAT)
    {
        *date = BUILD_DATE_FAT;
        date_forced = true;
    }

    if (time)
    {
        time2 = *time << 1;
        if (time2 == 0 || date_forced)
        {
            time2 = (11 < 6) | 11; /* set to 00:11:11 */
        }
        else
        {
            unsigned mins  = (time2 >> 6) & 0x3f;
            unsigned hours = (time2 >> 12) & 0x1f;

            mins = 11 * ((mins / 11) + 1); /* advance to next multiple of 11 */
            if (mins > 59)
            {
                mins = 11; /* 00 would be a bad marker */
                if (++hours > 23)
                {
                    hours = 0;
                    next_day = true;
                }
            }
            time2 = (hours << 12) | (mins << 6) | mins; /* secs = mins */
        }
        *time = time2 >> 1;
    }

    if (tenth)
        *tenth = (time2 & 1) * 100;

    if (date && next_day)
    {
        static const unsigned char daysinmonth[] =
            { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        unsigned day   = *date & 0x1f;
        unsigned month = (*date >> 5) & 0x0f;
        unsigned year  = (*date >> 9) & 0x7f;

        /* simplification: ignore leap years */
        if (++day > daysinmonth[month-1])
        {
            day = 1;
            if (++month > 12)
            {
                month = 1;
                year++;
            }
        }

        *date = (year << 9) | (month << 5) | day;
    }
}
#endif /* CONFIG_RTC */

static int write_longname(struct bpb *fat_bpb, struct fat_filestr *parentstr,
                          struct fat_file *file, const unsigned char *name,
                          unsigned long ucslen, const unsigned char *shortname,
                          union raw_dirent *srcent, uint8_t attr,
                          unsigned int flags)
{
    DEBUGF("%s(file:0x%lx, first:%d, num:%d, name:%s)\n", __func__,
           file->firstcluster, file->e.entry - file->e.entries + 1,
           file->e.entries, name);

    int rc;
    union raw_dirent *ent;

    uint16_t date = 0, time = 0, tenth = 0;
    fat_time(&date, &time, &tenth);
    time = htole16(time);
    date = htole16(date);

    /* shortname checksum saved in each longname entry */
    uint8_t chksum = shortname_checksum(shortname);

    /* we need to convert the name first since the entries are written in
       reverse order */
    unsigned long ucspadlen = ALIGN_UP(ucslen, FATLONG_NAME_CHARS);
    uint16_t ucsname[ucspadlen];

    for (unsigned long i = 0; i < ucspadlen; i++)
    {
        if (i < ucslen) {
#ifdef UNICODE32
            ucschar_t tmp;
            name = utf8decode(name, &tmp);
            /* For codepoints > U+FFFF we will need to use a UTF16 surrogate
               pair. 'ucslen' already takes this into account! */
            if (tmp < 0x10000) {
                ucsname[i] = tmp;
            } else {
                tmp -= 0x10000;
                ucsname[i++] = 0xd800 | ((tmp >> 10) & 0x3ff); /* High */
                ucsname[i] = 0xdc00 | (tmp & 0x3ff); /* Low */
            }
#else
            name = utf8decode(name, &ucsname[i]);
#endif
        } else if (i == ucslen) {
            ucsname[i] = 0x0000; /* name doesn't fill last block */
        } else /* i > ucslen */ {
            ucsname[i] = 0xffff; /* pad-out to end */
        }
    }

    dc_lock_cache();

    const unsigned int longentries = file->e.entries - 1;
    const unsigned int firstentry  = file->e.entry - longentries;

    /* longame entries */
    for (unsigned int i = 0; i < longentries; i++)
    {
        ent = cache_direntry(fat_bpb, parentstr, firstentry + i);
        if (!ent)
            FAT_ERROR(-2);

        /* verify this entry is free */
        if (ent->name[0] && ent->name[0] != 0xe5)
        {
            panicf("Dir entry %d in sector %x is not free! "
                   "%02x %02x %02x %02x",
                   i + firstentry, (unsigned)parentstr->lastsector,
                   (unsigned)ent->data[0], (unsigned)ent->data[1],
                   (unsigned)ent->data[2], (unsigned)ent->data[3]);
        }

        memset(ent->data, 0, DIR_ENTRY_SIZE);

        unsigned int ord = longentries - i;

        ent->ldir_ord = ord | (i == 0 ? FATLONG_ORD_F_LAST : 0);
        ent->ldir_attr = ATTR_LONG_NAME;
        ent->ldir_chksum = chksum;

        /* set name */
        uint16_t *ucsptr = &ucsname[(ord - 1) * FATLONG_NAME_CHARS];
        for (unsigned j = longent_char_first(); j; j = longent_char_next(j))
        {
            uint16_t ucs = *ucsptr++;
            INT162BYTES(ent->data, j, ucs);
        }

        dc_dirty_buf(ent);
        DEBUGF("Longname entry %d\n", ord);
    }

    /* shortname entry */
    DEBUGF("Shortname '%s'\n", shortname);

    ent = cache_direntry(fat_bpb, parentstr, file->e.entry);
    if (!ent)
        FAT_ERROR(-2);

    if (srcent && (flags & DIRENT_TEMPL))
    {
        /* srcent points to short entry template */
        *ent = *srcent;
    }
    else
    {
        /* make our own short entry */
        memset(ent->data, 0, DIR_ENTRY_SIZE);
        ent->attr = attr;
    }

    /* short name may change even if just moving */
    memcpy(ent->name, shortname, 11);
    raw_dirent_set_fstclus(ent, file->firstcluster);

    if (!(flags & DIRENT_TEMPL_CRT))
    {
        ent->crttimetenth = tenth;
        ent->crttime      = time;
        ent->crtdate      = date;
    }

    if (!(flags & DIRENT_TEMPL_WRT))
    {
        ent->wrttime = time;
        ent->wrtdate = date;
    }

    if (!(flags & DIRENT_TEMPL_ACC))
        ent->lstaccdate = date;

    if (srcent && (flags & DIRENT_RETURN))
        *srcent = *ent; /* caller wants */

    dc_dirty_buf(ent);

    rc = 0;
fat_error:
    dc_unlock_cache();
    return rc;
}

static int add_dir_entry(struct bpb *fat_bpb, struct fat_filestr *parentstr,
                         struct fat_file *file, const char *name,
                         union raw_dirent *srcent, uint8_t attr,
                         unsigned int flags)
{
    DEBUGF("%s(name:\"%s\",first:%lx)\n", __func__, name,
           file->firstcluster);

    int rc;

    unsigned char basisname[11], shortname[11];
    int entries_needed;
    int n = -1;
    unsigned long ucslen = 0;

    if (is_dotdir_name(name) && (attr & ATTR_DIRECTORY))
    {
        /* The "." and ".." directory entries must not be long names */
        int dots = strlcpy(shortname, name, 11);
        memset(&shortname[dots], ' ', 11 - dots);
        entries_needed = 1;
        basisname[0] = '\0';
    }
    else
    {
        rc = check_longname(name);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 1); /* filename is invalid */

        create_dos_name(basisname, name, &n);
        randomize_dos_name(shortname, basisname, &n);

        /* one dir entry needed for every 13 utf16 "code units"
           of filename, plus one entry for the short name.
           Keep in mind that a unicode character can take up to
           two code units!
        */
        ucslen = utf16len_utf8(name);
        if (ucslen > 255)
            FAT_ERROR(-2); /* name is too long */

        entries_needed = (ucslen + FATLONG_NAME_CHARS - 1)
                            / FATLONG_NAME_CHARS + 1;
    }

    int entry = 0, entries_found = 0, firstentry = -1;
    const int entperclus = DIR_ENTRIES_PER_SECTOR*fat_bpb->bpb_secperclus;

    /* step 1: search for a sufficiently-long run of free entries and check
               for duplicate shortname */
    dc_lock_cache();

    for (bool done = false; !done;)
    {
        union raw_dirent *ent = cache_direntry(fat_bpb, parentstr, entry);

        if (!ent)
        {
            if (parentstr->eof)
            {
                DEBUGF("End of dir (entry %d)\n", entry);
                break;
            }

            DEBUGF("Couldn't read dir (entry %d)\n", entry);
            dc_unlock_cache();
            FAT_ERROR(-3);
        }

        switch (ent->name[0])
        {
        case 0:    /* all remaining entries in cluster are free */
            DEBUGF("Found end of dir %d\n", entry);
            int found = entperclus - (entry % entperclus);
            entries_found += found;
            entry += found; /* move entry passed end of cluster */
            done = true;
            break;

        case 0xe5: /* individual free entry */
            entries_found++;
            entry++;
            DEBUGF("Found free entry %d (%d/%d)\n",
                   entry, entries_found, entries_needed);
            break;

        default:   /* occupied */
            entries_found = 0;
            entry++;

            if ((ent->ldir_attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME)
                break; /* ignore long name entry */

            /* check that our intended shortname doesn't already exist */
            if (!strncmp(shortname, ent->name, 11))
            {
                /* shortname exists already, make a new one */
                DEBUGF("Duplicate shortname '%.11s'", shortname);
                randomize_dos_name(shortname, basisname, &n);
                DEBUGF(", changing to '%.11s'\n", shortname);

                /* name has changed, we need to restart search */
                entry = 0;
                firstentry = -1;
            }
            break;
        }

        if (firstentry < 0 && entries_found >= entries_needed)
        {
            /* found adequate space; point to initial free entry */
            firstentry = entry - entries_found;
        }
    }

    dc_unlock_cache();

    /* step 2: extend the dir if necessary */
    if (firstentry < 0)
    {
        DEBUGF("Adding new cluster(s) to dir\n");

        if (entry + entries_needed - entries_found > MAX_DIRENTRIES)
        {
            /* FAT specification allows no more than 65536 entries (2MB)
               per directory */
            DEBUGF("Directory would be too large.\n");
            FAT_ERROR(-4);
        }

        while (entries_found < entries_needed)
        {
            rc = fat_extend_dir(fat_bpb, parentstr);
            if (rc == FAT_RC_ENOSPC)
                FAT_ERROR(RC);
            else if (rc < 0)
                FAT_ERROR(rc * 10 - 5);

            entries_found += entperclus;
            entry += entperclus;
        }

        firstentry = entry - entries_found;
    }

    /* remember the parent directory entry information */
#ifdef HAVE_MULTIVOLUME
    file->volume     = parentstr->fatfilep->volume;
#endif
    file->dircluster = parentstr->fatfilep->firstcluster;
    file->e.entry    = firstentry + entries_needed - 1;
    file->e.entries  = entries_needed;

    /* step 3: add entry */
    DEBUGF("Adding longname to entry %d\n", firstentry);
    rc = write_longname(fat_bpb, parentstr, file, name, ucslen,
                        shortname, srcent, attr, flags);
    if (rc < 0)
        FAT_ERROR(rc * 10 - 6);

    DEBUGF("Added new dir entry %u; using %u entries\n",
           file->e.entry, file->e.entries);

    rc = 0;
fat_error:
    return rc;
}

static int update_short_entry(struct bpb *fat_bpb, struct fat_file *file,
                              uint32_t size, struct fat_direntry *fatent)
{
    DEBUGF("%s(cluster:%lx entry:%d size:%ld)\n",
           __func__, file->firstcluster, file->e.entry, size);

    int rc;

#if CONFIG_RTC
    uint16_t time = 0;
    uint16_t date = 0;
#else
    /* get old time to increment from */
    uint16_t time = letoh16(fatent->wrttime);
    uint16_t date = letoh16(fatent->wrtdate);
#endif
    fat_time(&date, &time, NULL);
    date = htole16(date);
    time = htole16(time);

    /* open the parent directory */
    struct fat_file parent;
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);

    struct fat_filestr parentstr;
    fat_filestr_init(&parentstr, &parent);

    dc_lock_cache();

    union raw_dirent *ent = cache_direntry(fat_bpb, &parentstr, file->e.entry);
    if (!ent)
        FAT_ERROR(-1);

    if (!ent->name[0] || ent->name[0] == 0xe5)
        panicf("Updating size on empty dir entry %d\n", file->e.entry);

    /* basic file data */
    raw_dirent_set_fstclus(ent, file->firstcluster);
    ent->filesize = htole32(size);

    /* time and date info */
    ent->wrttime    = time;
    ent->wrtdate    = date;
    ent->lstaccdate = date;

    if (fatent)
    {
        fatent->name[0] = '\0'; /* not gonna bother here */
        parse_short_direntry(ent, fatent);
    }

    dc_dirty_buf(ent);

    rc = 0;
fat_error:
    dc_unlock_cache();
    return rc;
}

static int free_direntries(struct bpb *fat_bpb, struct fat_file *file)
{
    /* open the parent directory */
    struct fat_file parent;
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);

    struct fat_filestr parentstr;
    fat_filestr_init(&parentstr, &parent);

    for (unsigned int entries = file->e.entries,
                      entry = file->e.entry - entries + 1;
         entries; entries--, entry++)
    {
        DEBUGF("Clearing dir entry %d (%d/%d)\n",
               entry, entries - file->e.entries + 1, file->e.entries);


        dc_lock_cache();

        union raw_dirent *ent = cache_direntry(fat_bpb, &parentstr, entry);
        if (!ent)
        {
            dc_unlock_cache();

            if (entries == file->e.entries)
                return -1; /* nothing at all freed */

            /* longname already destroyed; revert to shortname */
            file->e.entries = 1;
            return 0;
        }

        ent->data[0] = 0xe5;

        dc_dirty_buf(ent);
        dc_unlock_cache();
    }

    /* directory entry info is now gone */
    file->dircluster = 0;
    file->e.entry    = FAT_DIRSCAN_RW_VAL;
    file->e.entries  = 0;

    return 1;
}

static int free_cluster_chain(struct bpb *fat_bpb, long startcluster)
{
    for (long last = startcluster, next; last; last = next)
    {
        next = get_next_cluster(fat_bpb, last);
        int rc = update_fat_entry(fat_bpb, last, 0);
        if (LIKELY(rc >= 0 && !startcluster))
            continue;

        if (rc < 0)
            return startcluster ? -1 : 0;

        startcluster = 0;
    }

    return 1;
}



/****************************************************************************
 ** exFAT  (bcm overlay v66)
 **
 ** exFAT volumes are mounted through the same fat_* interface as FAT16/32, so
 ** everything above this file (file.c, dir.c, dircache, the USB code) is
 ** unchanged.  Written from Microsoft's published exFAT specification.
 **
 ** Differences from FAT that matter here:
 **  - allocation lives in a bitmap (the FAT only links clusters); the FAT
 **    helpers below keep both in step
 **  - a file or directory may be "NoFatChain": one contiguous run whose FAT
 **    entries are meaningless.  Such objects carry EXFAT_CONTIG in
 **    fat_file.firstcluster and their length (from the directory entry) is
 **    kept in a small cache.  They are read in place; before Rockbox changes
 **    their allocation (append past the run, truncate, grow a directory) the
 **    run is written out as an ordinary FAT chain first.  Files Rockbox
 **    creates always use FAT chains.
 **  - a directory entry is a set: File (0x85) + Stream Extension (0xC0) +
 **    File Name (0xC1) x n, protected by a checksum; names carry a hash of the
 **    up-cased name.  There are no "." / ".." entries.
 **  - sizes are 64-bit.  Files of 4 GiB and more are listed (size shown as
 **    4 GiB - 1) and can be deleted / renamed, but not written to.
 ****************************************************************************/

#define EXFAT_CONTIG        0x40000000  /* fat_file.firstcluster: NoFatChain run */
#define EXFAT_BIG           0x20000000  /* (v66-v88: file >= 4 GiB, read-only; v89: no longer set, sizes are 64-bit) */
#define EXFAT_CLUS_MASK     0x1fffffff
#define EXFAT_EOF           0xffffffffu
#define EXFAT_BAD           0xfffffff7u

/* entry types */
#define EXT_EOD             0x00
#define EXT_INUSE           0x80
#define EXT_BITMAP          0x81
#define EXT_UPCASE          0x82
#define EXT_LABEL           0x83
#define EXT_FILE            0x85
#define EXT_GUID            0xa0
#define EXT_STREAM          0xc0
#define EXT_NAME            0xc1

#define EXFAT_MAX_SET       19          /* file + stream + 17 names (255 chars) */
#define EXFAT_NAME_CHARS    15

/* stream extension flags */
#define EXSF_ALLOC_POSSIBLE 0x01
#define EXSF_NOFATCHAIN     0x02
#define EXFAT_VOL_DIRTY    0x0002  /* VolumeFlags: VolumeDirty */

static inline long exfat_clus(long c)
{
    return c > 0 ? (c & EXFAT_CLUS_MASK) : c;
}

static inline bool exfat_vol(const struct bpb *fat_bpb)
{
    return fat_bpb->is_exfat;
}

#ifndef BOOTLOADER
/* up-case table of the (last) mounted exFAT volume, needed for name hashes.
   Stored as 256 pages of 256 characters where only pages that change some
   character take memory (the standard Windows table uses 14), so it costs
   ~16 KB instead of 128 KB.  A table needing more than EXFAT_UPCASE_PAGES
   pages falls back to ASCII-only up-casing (hashes of non-ASCII names may
   then differ from Windows'). */
#ifndef EXFAT_UPCASE_PAGES
#define EXFAT_UPCASE_PAGES 32
#endif
static uint8_t  exfat_upcase_map[256];     /* 0 = identity page, else pool slot + 1 */
static uint16_t exfat_upcase_pool[EXFAT_UPCASE_PAGES][256];
static unsigned int exfat_upcase_used;
static bool exfat_upcase_ok;
static uint8_t exfat_bitmap_buf[16384] STORAGE_ALIGN_ATTR;

static void exfat_upcase_reset(void)
{
    memset(exfat_upcase_map, 0, sizeof exfat_upcase_map);
    exfat_upcase_used = 0;
    exfat_upcase_ok = false;
}

/* returns false when the pool is exhausted */
static bool exfat_upcase_put(unsigned int c, uint16_t u)
{
    if (u == c)
        return true;
    unsigned int page = c >> 8;
    if (!exfat_upcase_map[page])
    {
        if (exfat_upcase_used >= EXFAT_UPCASE_PAGES)
            return false;
        uint16_t *pg = exfat_upcase_pool[exfat_upcase_used];
        for (unsigned int i = 0; i < 256; i++)
            pg[i] = (page << 8) | i;
        exfat_upcase_map[page] = ++exfat_upcase_used;
    }
    exfat_upcase_pool[exfat_upcase_map[page] - 1][c & 0xff] = u;
    return true;
}
#endif

static uint16_t exfat_toupper(uint16_t c)
{
#ifndef BOOTLOADER
    if (exfat_upcase_ok)
    {
        unsigned int slot = exfat_upcase_map[c >> 8];
        return slot ? exfat_upcase_pool[slot - 1][c & 0xff] : c;
    }
#endif
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static uint16_t exfat_name_hash(const uint16_t *name, unsigned int len)
{
    uint16_t hash = 0;
    for (unsigned int i = 0; i < len; i++)
    {
        uint16_t u = exfat_toupper(name[i]);
        hash = ((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u & 0xff);
        hash = ((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u >> 8);
    }
    return hash;
}

static uint16_t exfat_set_checksum(const uint8_t *set, unsigned int count)
{
    uint16_t chk = 0;
    unsigned int bytes = count * DIR_ENTRY_SIZE;
    for (unsigned int i = 0; i < bytes; i++)
    {
        if (i == 2 || i == 3)
            continue;
        chk = ((chk & 1) ? 0x8000 : 0) + (chk >> 1) + set[i];
    }
    return chk;
}

/* --- length of NoFatChain runs (keyed by first cluster) --- */
#ifndef EXFAT_RUNS
#define EXFAT_RUNS 64
#endif
#define EXFAT_RUN_CHAINED 0  /* the run was turned into a FAT chain */
static struct exfat_run
{
    uint32_t clus;      /* first cluster (0 = empty slot) */
    uint32_t count;     /* clusters in the run, or EXFAT_RUN_CHAINED */
#ifdef HAVE_MULTIVOLUME
    uint8_t  volume;
#endif
} exfat_runs[EXFAT_RUNS];
static unsigned int exfat_run_next;

static struct exfat_run *exfat_run_find(struct bpb *fat_bpb, uint32_t clus)
{
    for (unsigned int i = 0; i < EXFAT_RUNS; i++)
    {
        struct exfat_run *r = &exfat_runs[i];
        if (r->clus == clus IF_MV( && r->volume == fat_bpb->volume))
            return r;
    }
    (void)fat_bpb;
    return NULL;
}

static void exfat_run_set(struct bpb *fat_bpb, uint32_t clus, uint32_t count)
{
    struct exfat_run *r = exfat_run_find(fat_bpb, clus);
    if (!r)
    {
        r = &exfat_runs[exfat_run_next];
        exfat_run_next = (exfat_run_next + 1) % EXFAT_RUNS;
    }
    r->clus  = clus;
    r->count = count;
#ifdef HAVE_MULTIVOLUME
    r->volume = fat_bpb->volume;
#endif
}

/* forget every run of a volume (mount / unmount: after a USB session the PC
   may have rewritten anything) */
static void exfat_runs_forget(struct bpb *fat_bpb)
{
    for (unsigned int i = 0; i < EXFAT_RUNS; i++)
        if (1 IF_MV( && exfat_runs[i].volume == fat_bpb->volume))
            exfat_runs[i].clus = 0;
    (void)fat_bpb;
}

static void exfat_run_drop(struct bpb *fat_bpb, uint32_t clus)
{
    struct exfat_run *r = exfat_run_find(fat_bpb, clus);
    if (r)
        r->clus = 0;
}

static uint32_t exfat_bytes2clusters(struct bpb *fat_bpb, uint64_t bytes)
{
    uint64_t cs = (uint64_t)fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb);
    return (uint32_t)((bytes + cs - 1) / cs);
}

/* --- FAT and bitmap --- */

static uint32_t exfat_fat_read(struct bpb *fat_bpb, unsigned long entry, bool *ok)
{
    unsigned long sector = entry / CLUSTERS_PER_FAT_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT_SECTOR;
    uint32_t val = 0;

    dc_lock_cache();
    uint32_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (sec)
        val = letoh32(sec[offset]);
    dc_unlock_cache();
    if (ok)
        *ok = sec != NULL;
    return val;
}

static long get_next_cluster_ex(struct bpb *fat_bpb, long cluster)
{
    bool ok;
    uint32_t next = exfat_fat_read(fat_bpb, exfat_clus(cluster), &ok);
    if (!ok)
        return -1;
    if (next < 2 || next > fat_bpb->dataclusters + 1)
        return 0;   /* EOF, bad or undefined */
    return next;
}

/* bcm overlay v67: before the first change to the volume's metadata in a
   session, the main boot sector gets
     - VolumeDirty (VolumeFlags bit 1) set: until Rockbox marks the volume
       clean again (USB connect, normal shutdown), Windows / macOS / Linux
       treat it as "not cleanly removed" and offer to check it -- after a
       crash that check reclaims any clusters left allocated but unused
     - PercentInUse = 0xff ("unknown"), as the specification asks of an
       implementation that does not keep it current
   Both fields are outside the boot-region checksum; the sector is written at
   once, so it is on disk before anything it covers. */
static void exfat_percent_unknown(struct bpb *fat_bpb)
{
    if (fat_bpb->ex_pct_done)
        return;
    fat_bpb->ex_pct_done = 1;
    dc_lock_cache();
    uint8_t *bs = cache_sector(fat_bpb, 0);
    if (bs && !memcmp(bs + 3, "EXFAT   ", 8))
    {
        uint16_t flags = BYTES2INT16(bs, 106);
        if (!(flags & EXFAT_VOL_DIRTY))
        {
            flags |= EXFAT_VOL_DIRTY;
            bs[106] = flags;
            bs[107] = flags >> 8;
            fat_bpb->ex_dirty_ours = 1;
        }
        bs[112] = 0xff;
        dc_dirty_buf(bs);
        dc_commit_range(IF_MV(fat_bpb->volume,) 0, 1);
    }
    dc_unlock_cache();
}

/* everything on disk; clear VolumeDirty if this session set it */
static void exfat_mark_clean(struct bpb *fat_bpb)
{
    if (!fat_bpb->ex_dirty_ours)
        return;
    dc_lock_cache();
    dc_commit_all(IF_MV(fat_bpb->volume));
    uint8_t *bs = cache_sector(fat_bpb, 0);
    if (bs && !memcmp(bs + 3, "EXFAT   ", 8))
    {
        uint16_t flags = BYTES2INT16(bs, 106) & ~EXFAT_VOL_DIRTY;
        bs[106] = flags;
        bs[107] = flags >> 8;
        dc_dirty_buf(bs);
        dc_commit_range(IF_MV(fat_bpb->volume,) 0, 1);
    }
    dc_unlock_cache();
    fat_bpb->ex_dirty_ours = 0;
    fat_bpb->ex_pct_done = 0;   /* the next change marks it dirty again */
}

/* crash safety: Rockbox's cache flushes dirty sectors in no particular
   order.  Before a directory entry that points at newly allocated clusters is
   written, the allocation (bitmap + FAT) goes to disk first; before clusters
   are freed, the entry that pointed at them goes first.  A power cut can then
   only leave clusters marked used that nothing owns (harmless, chkdsk /
   fsck reclaims them), never a file whose clusters are marked free. */
static void exfat_commit_alloc(struct bpb *fat_bpb)
{
    dc_lock_cache();
    dc_commit_range(IF_MV(fat_bpb->volume,) fat_bpb->fatrgnstart, fat_bpb->fatsize);
    if (fat_bpb->ex_bitmap_sector)
        dc_commit_range(IF_MV(fat_bpb->volume,) fat_bpb->ex_bitmap_sector,
                        (fat_bpb->ex_bitmap_bytes + LOG_SECTOR_SIZE(fat_bpb) - 1) /
                        LOG_SECTOR_SIZE(fat_bpb));
    dc_unlock_cache();
}

/* bitmap: bit (cluster - 2) */
static uint8_t *exfat_bitmap_byte(struct bpb *fat_bpb, unsigned long cluster,
                                  unsigned int *bit)
{
    unsigned long idx = cluster - 2;
    unsigned long byte = idx / 8;
    sector_t sec = fat_bpb->ex_bitmap_sector + byte / LOG_SECTOR_SIZE(fat_bpb);
    uint8_t *buf = cache_sector(fat_bpb, sec);
    *bit = idx % 8;
    return buf ? buf + byte % LOG_SECTOR_SIZE(fat_bpb) : NULL;
}

static int exfat_bitmap_set(struct bpb *fat_bpb, unsigned long cluster, bool used)
{
    unsigned int bit;
    dc_lock_cache();
    uint8_t *p = exfat_bitmap_byte(fat_bpb, cluster, &bit);
    if (!p)
    {
        dc_unlock_cache();
        return -1;
    }
    bool was = (*p >> bit) & 1;
    if (was != used)
    {
        if (used)
        {
            *p |= 1 << bit;
            if (fat_bpb->fsinfo.freecount < 0xfffffffe && fat_bpb->fsinfo.freecount > 0)
                fat_bpb->fsinfo.freecount--;
        }
        else
        {
            *p &= ~(1 << bit);
            if (fat_bpb->fsinfo.freecount < 0xfffffffe)
                fat_bpb->fsinfo.freecount++;
        }
        dc_dirty_buf(p);
        exfat_percent_unknown(fat_bpb);
    }
    dc_unlock_cache();
    return 0;
}

static bool exfat_bitmap_get(struct bpb *fat_bpb, unsigned long cluster)
{
    unsigned int bit;
    dc_lock_cache();
    uint8_t *p = exfat_bitmap_byte(fat_bpb, cluster, &bit);
    bool used = p ? (*p >> bit) & 1 : true;
    dc_unlock_cache();
    return used;
}

/* FAT32-style entry update used by the shared chain code: a nonzero value
   allocates the cluster in the bitmap and links it; zero frees it */
static int update_fat_entry_ex(struct bpb *fat_bpb, unsigned long entry,
                               unsigned long val)
{
    entry = exfat_clus(entry);
    if (entry < 2 || entry > fat_bpb->dataclusters + 1)
        panicf("exFAT: bad cluster %lu\n", entry);
    if (entry == val)
        panicf("exFAT: creating loop %lx\n", entry);

    if (exfat_bitmap_set(fat_bpb, entry, val != 0) < 0)
        return -1;

    uint32_t fatval = val == 0 ? 0 :
                      (val >= FAT_EOF_MARK ? EXFAT_EOF : (uint32_t)val);
    unsigned long sector = entry / CLUSTERS_PER_FAT_SECTOR;
    unsigned long offset = entry % CLUSTERS_PER_FAT_SECTOR;

    dc_lock_cache();
    uint32_t *sec = cache_sector(fat_bpb, sector + fat_bpb->fatrgnstart);
    if (!sec)
    {
        dc_unlock_cache();
        return -1;
    }
    sec[offset] = htole32(fatval);
    dc_dirty_buf(sec);
    dc_unlock_cache();
    return 0;
}

#ifndef BOOTLOADER
/* bcm overlay v86: the first free cluster, from big direct reads of the bitmap (16 KB at a time, 32 bits per test).
   Only a starting hint: the search below checks every bit through the cache (which may hold newer sectors), so a stale
   disk copy cannot make it hand out a used cluster.  Without this, the first file written after mounting a well-filled
   exFAT volume (a setting, a playlist, a log) went through the bitmap one 512-byte cached sector at a time from the
   start -- up to ~4000 single-sector reads on a full 2 TB volume with 128 KB clusters. */
static unsigned long exfat_first_free_hint(struct bpb *fat_bpb)
{
    const unsigned long total = fat_bpb->dataclusters;
    const unsigned long per = sizeof(exfat_bitmap_buf) / LOG_SECTOR_SIZE(fat_bpb);
    const unsigned long nsec = (total + LOG_SECTOR_SIZE(fat_bpb) * 8 - 1) / (LOG_SECTOR_SIZE(fat_bpb) * 8);
    unsigned long bit0 = 0;
    for (unsigned long s = 0; s < nsec; s += per, bit0 += per * LOG_SECTOR_SIZE(fat_bpb) * 8)
    {
        unsigned long n = MIN(per, nsec - s);
        if (storage_read_sectors(IF_MD(fat_bpb->drive,) fat_bpb->startsector + fat_bpb->ex_bitmap_sector + s,
                                 n, exfat_bitmap_buf) < 0)
            return 0;
        const uint32_t *w = (const uint32_t *)exfat_bitmap_buf;
        for (unsigned long i = 0; i < n * LOG_SECTOR_SIZE(fat_bpb) / 4; i++)
            if (w[i] != 0xffffffffu)
            {
                unsigned long b = bit0 + i * 32;
                uint32_t v = w[i];
                while (v & 1) { v >>= 1; b++; }
                return b < total ? b + 2 : 0;
            }
    }
    return 0;
}
#endif

static long find_free_cluster_ex(struct bpb *fat_bpb, long startcluster)
{
    const unsigned long total = fat_bpb->dataclusters;
    const unsigned long bps = LOG_SECTOR_SIZE(fat_bpb) * 8; /* bits per sector */
    unsigned long idx = (startcluster >= 2 && (unsigned long)startcluster <= total + 1) ?
                        (unsigned long)startcluster - 2 : 0;
    unsigned long checked = 0;
#ifndef BOOTLOADER
    if (idx == 0 && fat_bpb->fsinfo.nextfree == 0xffffffff)
    {
        unsigned long h = exfat_first_free_hint(fat_bpb);
        if (h >= 2)
            idx = h - 2;
    }
#endif

    while (checked < total + 8)
    {
        unsigned long secidx = idx / bps;
        unsigned long end = MIN((secidx + 1) * bps, total);

        dc_lock_cache();
        uint8_t *buf = cache_sector(fat_bpb, fat_bpb->ex_bitmap_sector + secidx);
        if (!buf)
        {
            dc_unlock_cache();
            return 0;
        }
        while (idx < end)
        {
            unsigned long b = idx % bps;
#ifndef BOOTLOADER
            if (!(b & 31) && idx + 32 <= end && *(const uint32_t *)(buf + b / 8) == 0xffffffffu)
            {   /* v86: 32 used clusters at a time */
                idx += 32;
                checked += 32;
                continue;
            }
#endif
            if (!(b & 7) && idx + 8 <= end && buf[b / 8] == 0xff)
            {
                idx += 8;
                checked += 8;
                continue;
            }
            if (!(buf[b / 8] & (1u << (b & 7))))
            {
                dc_unlock_cache();
                fat_bpb->fsinfo.nextfree = idx + 2;
                return idx + 2;
            }
            idx++;
            checked++;
        }
        dc_unlock_cache();
        if (idx >= total)
            idx = 0;
    }
    return 0;
}

static void fat_recalc_free_internal_ex(struct bpb *fat_bpb)
{
    unsigned long total = fat_bpb->dataclusters, used = 0, bits = 0;
    sector_t sec = fat_bpb->ex_bitmap_sector;
    unsigned long nsec = (total + LOG_SECTOR_SIZE(fat_bpb) * 8 - 1) / (LOG_SECTOR_SIZE(fat_bpb) * 8);

#ifndef BOOTLOADER
    /* big direct reads; the cache would take one sector at a time */
    unsigned long per = sizeof(exfat_bitmap_buf) / LOG_SECTOR_SIZE(fat_bpb);
    dc_commit_all(IF_MV(fat_bpb->volume));
    for (unsigned long s = 0; s < nsec; s += per)
    {
        unsigned long n = MIN(per, nsec - s);
        if (storage_read_sectors(IF_MD(fat_bpb->drive,) fat_bpb->startsector + sec + s,
                                 n, exfat_bitmap_buf) < 0)
            return;
        for (unsigned long i = 0; i < n * LOG_SECTOR_SIZE(fat_bpb) && bits < total; i++)
        {
            uint8_t v = exfat_bitmap_buf[i];
            unsigned int k = MIN(8ul, total - bits);
            if (k < 8)
                v &= (1u << k) - 1;
            used += __builtin_popcount(v);
            if (fat_bpb->fsinfo.nextfree == 0xffffffff && v != ((1u << k) - 1))
            {
                for (unsigned int b = 0; b < k; b++)
                    if (!(v & (1 << b)))
                    {
                        fat_bpb->fsinfo.nextfree = bits + b + 2;
                        break;
                    }
            }
            bits += k;
        }
    }
#else
    /* the boot loader never needs the free space */
    (void)sec; (void)nsec; (void)bits;
    used = total;
#endif
    fat_bpb->fsinfo.freecount = total - used;
}

/* --- directory entry sets --- */

/* read 'count' raw entries starting at 'entry' of the directory stream */
static int exfat_read_entries(struct bpb *fat_bpb, struct fat_filestr *dirstr,
                              unsigned int entry, unsigned int count, uint8_t *out)
{
    for (unsigned int i = 0; i < count; i++)
    {
        dc_lock_cache();
        union raw_dirent *ent = cache_direntry(fat_bpb, dirstr, entry + i);
        if (!ent)
        {
            dc_unlock_cache();
            return -1;
        }
        memcpy(out + i * DIR_ENTRY_SIZE, ent->data, DIR_ENTRY_SIZE);
        dc_unlock_cache();
    }
    return 0;
}

#ifndef BOOTLOADER
static uint8_t exfat_setio[3 * DC_CACHE_BUFSIZE] CACHEALIGN_ATTR;

/* bcm overlay v91: the entry-set work buffers (608 bytes each, plus 512 for a
   name) used to be locals.  Several of them nest (rename -> add_set ->
   name_taken -> extend_dir -> update_entry), which ran the 1-3 KB thread
   stacks of the playlist, audio and dircache threads past their ends on the
   iPod.  They live here now, one per function, so nesting is fine; the
   recursive mutex keeps two threads (readdir and ftruncate run under the
   shared reader lock) from using the same one.  The bootloader keeps the
   locals, so it builds byte for byte as before. */
static struct mutex exfat_scratch_mtx SHAREDBSS_ATTR;
static uint8_t exfat_rd_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_upd_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_shr_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_nt_other[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_cr_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_rn_old[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_rn_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint8_t exfat_mt_set[EXFAT_MAX_SET * DIR_ENTRY_SIZE];
static uint16_t exfat_bs_ucs[256];
#define EXFAT_SCRATCH(type, name, n, buf) type * const name = (buf)
#define EXFAT_LOCKED(fn) fn##_unlocked
#define EXFAT_LOCKED_SC  static
#else
#define EXFAT_SCRATCH(type, name, n, buf) type name[n]
#define EXFAT_LOCKED(fn) fn
#define EXFAT_LOCKED_SC
#endif

/* bcm overlay v67: put the (dirty, cached) sectors holding an entry set on
   disk now.  A set that straddles a sector boundary is written with ONE
   storage command when its sectors are adjacent, so a crash or power cut
   cannot leave half an updated set (whose checksum would then fail and the
   file would vanish).  Otherwise the sectors go last-first: a new set's
   primary entry only lands after its secondaries. */
static void exfat_flush_sectors(struct bpb *fat_bpb, const sector_t *secs, unsigned int n)
{
    dc_lock_cache();
#ifndef BOOTLOADER
    bool together = n > 1 && n <= 3;
    void *bufs[3];
    for (unsigned int i = 0; together && i < n; i++)
    {
        bufs[i] = dc_find_sector(IF_MV(fat_bpb->volume,) secs[i]);
        if (!bufs[i] || secs[i] != secs[0] + i)
            together = false;
    }
    if (together)
    {
        unsigned int ss = LOG_SECTOR_SIZE(fat_bpb);
        for (unsigned int i = 0; i < n; i++)
            memcpy(exfat_setio + i * ss, bufs[i], ss);
        int rc = storage_write_sectors(IF_MD(fat_bpb->drive,)
                                       secs[0] + fat_bpb->startsector, n, exfat_setio);
        if (rc < 0)
            panicf("%s() - Could not write sector %llu (error %d)\n", __func__,
                   (uint64_t)secs[0], rc);
        for (unsigned int i = 0; i < n; i++)
            dc_clean_buf(bufs[i]);
        dc_unlock_cache();
        return;
    }
#endif
    for (unsigned int i = n; i--; )
        dc_commit_range(IF_MV(fat_bpb->volume,) secs[i], 1);
    dc_unlock_cache();
    (void)fat_bpb;
}

/* write 'count' entries; with 'now' they are on disk when this returns */
static int exfat_write_entries_ex(struct bpb *fat_bpb, struct fat_filestr *dirstr,
                                  unsigned int entry, unsigned int count,
                                  const uint8_t *in, bool now)
{
    sector_t secs[4];
    unsigned int nsec = 0;
    exfat_percent_unknown(fat_bpb);
    for (unsigned int i = 0; i < count; i++)
    {
        dc_lock_cache();
        union raw_dirent *ent = cache_direntry(fat_bpb, dirstr, entry + i);
        if (!ent)
        {
            dc_unlock_cache();
            return -1;
        }
        memcpy(ent->data, in + i * DIR_ENTRY_SIZE, DIR_ENTRY_SIZE);
        dc_dirty_buf(ent);
        sector_t s = dirstr->lastsector;
        if (!nsec || secs[nsec - 1] != s)
        {
            if (nsec < 4)
                secs[nsec] = s;
            nsec++;
        }
        dc_unlock_cache();
    }
    if (now)
    {
        if (nsec > 4)
        {
            dc_lock_cache();
            dc_commit_all(IF_MV(fat_bpb->volume));
            dc_unlock_cache();
        }
        else
            exfat_flush_sectors(fat_bpb, secs, nsec);
    }
    return 0;
}

static int exfat_write_entries(struct bpb *fat_bpb, struct fat_filestr *dirstr,
                               unsigned int entry, unsigned int count, const uint8_t *in)
{
    return exfat_write_entries_ex(fat_bpb, dirstr, entry, count, in, true);
}

/* the set of a file: first entry index and count, from its fat_file */
static inline unsigned int exfat_set_first(const struct fat_file *file)
{
    return file->e.entry - file->e.entries + 1;
}

static void exfat_put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void exfat_put_le32(uint8_t *p, uint32_t v) { exfat_put_le16(p, v); exfat_put_le16(p + 2, v >> 16); }
static void exfat_put_le64(uint8_t *p, uint64_t v) { exfat_put_le32(p, v); exfat_put_le32(p + 4, v >> 32); }
static uint16_t exfat_le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t exfat_le32(const uint8_t *p) { return exfat_le16(p) | (uint32_t)exfat_le16(p + 2) << 16; }
static uint64_t exfat_le64(const uint8_t *p) { return exfat_le32(p) | (uint64_t)exfat_le32(p + 4) << 32; }

/* parse a validated set into a fat_direntry; returns 1.  A name longer than
   Rockbox's 255-byte UTF-8 limit (e.g. 120 CJK characters) is cut at a
   character boundary; it is still returned as 1 -- never 2, which tells the
   upper layer the name is an 8.3 OEM short name and re-decodes it through a
   13-byte buffer (stack overflow) -- and the cut name opens the file, since
   lookups compare against what readdir returns. */
static int exfat_parse_set(struct bpb *fat_bpb, const uint8_t *set, unsigned int count,
                           struct fat_direntry *fatent)
{
    const uint8_t *file = set, *stream = set + DIR_ENTRY_SIZE;
    uint16_t attr = exfat_le16(file + 4);
    uint32_t crt  = exfat_le32(file + 8);
    uint32_t mod  = exfat_le32(file + 12);
    uint32_t acc  = exfat_le32(file + 16);
    uint8_t  flags = stream[1];
    unsigned int namelen = stream[3];
    uint32_t first = exfat_le32(stream + 20);
    uint64_t size  = exfat_le64(stream + 24);

    fatent->attr         = attr & (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM |
                                   ATTR_DIRECTORY | ATTR_ARCHIVE);
    fatent->crttimetenth = file[20];
    fatent->crttime      = crt & 0xffff;
    fatent->crtdate      = crt >> 16;
    fatent->wrttime      = mod & 0xffff;
    fatent->wrtdate      = mod >> 16;
    fatent->lstaccdate   = acc >> 16;
#ifdef BOOTLOADER
    fatent->filesize     = size > 0xffffffffull ? 0xffffffffu : (uint32_t)size;
#else
    fatent->filesize     = size;              /* bcm overlay v89: the real 64-bit size */
#endif
    fatent->shortname[0] = 0;

    long fc = 0;
    if (first >= 2 && first <= fat_bpb->dataclusters + 1)
    {
        fc = first;
        if (flags & EXSF_NOFATCHAIN)
        {
            fc |= EXFAT_CONTIG;
            exfat_run_set(fat_bpb, first, size ? exfat_bytes2clusters(fat_bpb, size) : 1);
        }
#ifdef BOOTLOADER
        if (size > 0xffffffffull && !(attr & ATTR_DIRECTORY))
            fc |= EXFAT_BIG;
#endif
    }
    fatent->firstcluster = fc;

    /* name: UTF-16 -> UTF-8 */
    unsigned char *p = fatent->name, *end = fatent->name + FAT_DIRENTRY_NAME_MAX;
    int rc = 1;
    unsigned int got = 0;
    for (unsigned int e = 2; e < count && got < namelen; e++)
    {
        const uint8_t *ne = set + e * DIR_ENTRY_SIZE;
        for (unsigned int i = 0; i < EXFAT_NAME_CHARS && got < namelen; i++, got++)
        {
            unsigned long ucs = exfat_le16(ne + 2 + 2 * i);
            if (ucs >= 0xd800 && ucs < 0xdc00 && got + 1 < namelen)
            {
                /* surrogate pair, possibly split across two name entries */
                unsigned int j = i + 1;
                const uint8_t *ne2 = ne;
                if (j >= EXFAT_NAME_CHARS) { ne2 += DIR_ENTRY_SIZE; j = 0; }
                unsigned long lo = exfat_le16(ne2 + 2 + 2 * j);
                if (lo >= 0xdc00 && lo < 0xe000)
                {
                    ucs = 0x10000 + (((ucs & 0x3ff) << 10) | (lo & 0x3ff));
                    i++; got++;
                    if (i >= EXFAT_NAME_CHARS) { e++; ne = ne2; i = 0; }
                }
            }
            unsigned char tmp[6];
            unsigned char *q = utf8encode(ucs, tmp);
            if (p + (q - tmp) > end)
            {
                got = namelen;  /* cut here (character boundary) */
                break;
            }
            memcpy(p, tmp, q - tmp);
            p += q - tmp;
        }
    }
    *p = 0;
    return rc;
}

/* scan for the next valid File set at or after scan->entry + 1 */
static int exfat_readdir(struct bpb *fat_bpb, struct fat_filestr *dirstr,
                         struct fat_dirscan_info *scan, struct filestr_cache *cachep,
                         struct fat_direntry *entry)
{
    EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_rd_set);
    unsigned int need = 0, have = 0, setstart = 0;
    int rc = 0;

    scan->entries = 0;

    while (1)
    {
        unsigned int direntry = ++scan->entry;
        if (direntry >= EXFAT_MAX_DIRENTRIES)     /* bcm overlay v89: was 65536 (FAT32's limit) */
            FAT_ERROR(-1);

        unsigned long sector = direntry / DIR_ENTRIES_PER_SECTOR;
        if (cachep->sector != sector)
        {
            if (cachep->sector + 1 != sector)
            {
                int rc2 = fat_seek(dirstr, sector);
                if (rc2 < 0)
                {
                    if (rc2 == FAT_SEEK_EOF)
                        break;
                    FAT_ERROR(rc2 * 10 - 2);
                }
            }

            int rc2 = fat_readwrite(dirstr, 1, cachep->buffer, false);
            if (rc2 <= 0)
            {
                if (rc2 == 0)
                    break; /* eof */
                FAT_ERROR(rc2 * 10 - 3);
            }
            cachep->sector = sector;
        }

        const uint8_t *ent = ((const uint8_t *)cachep->buffer) +
                             (direntry % DIR_ENTRIES_PER_SECTOR) * DIR_ENTRY_SIZE;
        uint8_t type = ent[0];

        if (type == EXT_EOD)
            break;

        if (need)
        {
            /* collecting the secondaries of a set */
            if ((have == 1 && type != EXT_STREAM) ||
                (have >= 2 && type != EXT_NAME && !(type & 0x40)) ||
                !(type & EXT_INUSE))
            {
                /* broken set: drop it and re-examine this entry */
                need = 0;
                scan->entry--;
                continue;
            }
            if (have < EXFAT_MAX_SET)
                memcpy(set + have * DIR_ENTRY_SIZE, ent, DIR_ENTRY_SIZE);
            if (++have < need)
                continue;

            need = 0;
            unsigned int n = MIN(have, (unsigned int)EXFAT_MAX_SET);
            unsigned int namelen = set[DIR_ENTRY_SIZE + 3];
            if (have > EXFAT_MAX_SET || n < 3 ||
                exfat_set_checksum(set, have) != exfat_le16(set + 2) ||
                namelen == 0 || (namelen + EXFAT_NAME_CHARS - 1) / EXFAT_NAME_CHARS > n - 2)
            {
                DEBUGF("%s: bad set at %u\n", __func__, setstart);
                continue;
            }

            rc = exfat_parse_set(fat_bpb, set, n, entry);
            scan->entries = have;
            break;
        }

        if (type == EXT_FILE)
        {
            unsigned int sec = ent[1];
            if (sec < 2 || sec > 255)
                continue;
            need = sec + 1;
            have = 1;
            setstart = direntry;
            memcpy(set, ent, DIR_ENTRY_SIZE);
            continue;
        }
        /* anything else (deleted entries, bitmap, up-case, label, GUID,
           vendor entries...) is skipped */
    }

fat_error:
    if (rc <= 0)
    {
        fat_empty_fat_direntry(entry);
        scan->entry--;
        scan->entries = 0;
    }
    else if (have > EXFAT_MAX_SET)
    {
        /* never happens (checked above) */
    }
    (void)setstart;
    return rc;
}

/* look up a NoFatChain object's run length (clusters); 0xffffffff = unknown */
static uint32_t exfat_run_length(struct bpb *fat_bpb, const struct fat_file *file)
{
    uint32_t clus = exfat_clus(file->firstcluster);
    struct exfat_run *r = exfat_run_find(fat_bpb, clus);
    if (r)
        return r->count;

    if (!file->dircluster || file->e.entries < 3 ||
        file->e.entry == FAT_DIRSCAN_RW_VAL)
        return 0xffffffff;

    /* read its stream extension from the parent directory */
    struct fat_file parent;
    struct fat_filestr parentstr;
    uint8_t stream[DIR_ENTRY_SIZE];
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);
    fat_filestr_init(&parentstr, &parent);
    if (exfat_read_entries(fat_bpb, &parentstr, exfat_set_first(file) + 1, 1, stream) < 0 ||
        stream[0] != EXT_STREAM || exfat_le32(stream + 20) != clus)
        return 0xffffffff;

    uint64_t size = exfat_le64(stream + 24);
    uint32_t count = (stream[1] & EXSF_NOFATCHAIN) ?
                     (size ? exfat_bytes2clusters(fat_bpb, size) : 1) : EXFAT_RUN_CHAINED;
    exfat_run_set(fat_bpb, clus, count);
    return count;
}

/* the cluster after 'cluster' in 'file' (0 = end) */
static long exfat_file_next(struct bpb *fat_bpb, const struct fat_file *file, long cluster)
{
    long fc = file->firstcluster;
    cluster = exfat_clus(cluster);
    if (fc > 0 && (fc & EXFAT_CONTIG))
    {
        uint32_t first = exfat_clus(fc);
        uint32_t n = exfat_run_length(fat_bpb, file);
        if (n == EXFAT_RUN_CHAINED)
            return get_next_cluster_ex(fat_bpb, cluster);
        if (n == 0xffffffff)
        {
            /* unknown length (stale reference): a FAT link if there is one,
               otherwise the next cluster of the run */
            long next = get_next_cluster_ex(fat_bpb, cluster);
            if (next > 0 && exfat_bitmap_get(fat_bpb, next))
                return next;
            return (unsigned long)cluster + 1 <= fat_bpb->dataclusters + 1 ? cluster + 1 : 0;
        }
        return (uint32_t)cluster + 1 < first + n ? cluster + 1 : 0;
    }
    return get_next_cluster_ex(fat_bpb, cluster);
}

/* turn a NoFatChain run into an ordinary FAT chain (before changing its
   allocation); the directory entry is updated by the caller */
static int exfat_make_chained(struct bpb *fat_bpb, struct fat_file *file)
{
    long fc = file->firstcluster;
    if (fc <= 0 || !(fc & EXFAT_CONTIG))
        return 0;

    uint32_t first = exfat_clus(fc);
    uint32_t n = exfat_run_length(fat_bpb, file);
    if (n == EXFAT_RUN_CHAINED)
    {
        file->firstcluster &= ~EXFAT_CONTIG;
        return 0;
    }
    if (n == 0xffffffff || n == 0)
    {
        DEBUGF("%s: run length unknown (%lx)\n", __func__, (unsigned long)first);
        return -1;
    }

    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t c = first + i;
        if (update_fat_entry_ex(fat_bpb, c, i + 1 < n ? c + 1 : FAT_EOF_MARK) < 0)
            return -2;
    }

    exfat_run_set(fat_bpb, first, EXFAT_RUN_CHAINED);
    file->firstcluster &= ~EXFAT_CONTIG;
    return 1;
}

static long next_write_cluster(struct bpb *fat_bpb, long oldcluster);

/* the next cluster when writing: inside a NoFatChain run it is simply the
   next one; past its end the run becomes a FAT chain and grows like one */
static long exfat_write_next(struct bpb *fat_bpb, struct fat_file *file, long cluster)
{
    long fc = file->firstcluster;
    cluster = exfat_clus(cluster);
    if (fc > 0 && (fc & EXFAT_CONTIG))
    {
        uint32_t n = exfat_run_length(fat_bpb, file);
        if (n != 0xffffffff && n != EXFAT_RUN_CHAINED &&
            (uint32_t)cluster + 1 < exfat_clus(fc) + n)
            return cluster + 1;
        if (exfat_make_chained(fat_bpb, file) < 0)
            return 0;
    }
    return next_write_cluster(fat_bpb, cluster);
}

static void exfat_now(uint32_t *stamp, uint8_t *ms10)
{
    uint16_t date = 0, time = 0;
    int16_t tenth = 0;
    fat_time(&date, &time, &tenth);
    *stamp = (uint32_t)date << 16 | time;
    if (ms10)
        *ms10 = tenth;
}

/* rewrite the stream extension of 'file' (first cluster, flags, size) and
   optionally its modification time; recomputes the set checksum */
static int exfat_update_entry(struct bpb *fat_bpb, struct fat_file *file,
                              bool setsize, uint64_t size, bool touch,
                              struct fat_direntry *fatent)
{
    if (!file->dircluster)
        return 0;   /* root, or removed */

    unsigned int count = file->e.entries;
    if (count < 3 || count > EXFAT_MAX_SET)
        return -1;

    EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_upd_set);
    struct fat_file parent;
    struct fat_filestr parentstr;
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);
    fat_filestr_init(&parentstr, &parent);

    exfat_commit_alloc(fat_bpb);

    unsigned int first = exfat_set_first(file);
    if (exfat_read_entries(fat_bpb, &parentstr, first, count, set) < 0)
        return -2;
    if (set[0] != EXT_FILE || set[DIR_ENTRY_SIZE] != EXT_STREAM)
    {
        panicf("exFAT: updating a non-file entry %u\n", first);
        return -3;
    }

    uint8_t *stream = set + DIR_ENTRY_SIZE;
    long fc = file->firstcluster;
    uint32_t clus = exfat_clus(fc);
    uint8_t flags = EXSF_ALLOC_POSSIBLE;
    if (fc > 0 && (fc & EXFAT_CONTIG))
        flags |= EXSF_NOFATCHAIN;
    stream[1] = (stream[1] & ~(EXSF_ALLOC_POSSIBLE | EXSF_NOFATCHAIN)) | flags;
    exfat_put_le32(stream + 20, clus);
    if (setsize)
    {
        exfat_put_le64(stream + 8, size);   /* valid data length */
        exfat_put_le64(stream + 24, size);  /* data length */
    }
    if (touch)
    {
        uint32_t now;
        uint8_t ms10;
        exfat_now(&now, &ms10);
        exfat_put_le32(set + 12, now);      /* modified */
        exfat_put_le32(set + 16, now);      /* accessed */
        set[21] = ms10;
        set[23] = 0;                        /* modified UTC offset: unknown */
        set[24] = 0;
        exfat_put_le16(set + 4, exfat_le16(set + 4) |
                       ((exfat_le16(set + 4) & ATTR_DIRECTORY) ? 0 : ATTR_ARCHIVE));
    }
    exfat_put_le16(set + 2, exfat_set_checksum(set, count));

    int rc = exfat_write_entries(fat_bpb, &parentstr, first, count, set);
    if (rc >= 0 && fatent)
    {
        exfat_parse_set(fat_bpb, set, count, fatent);
    }
    return rc;
}

/* bcm overlay v67: lower the lengths in 'file's entry on disk to at most
   'maxsize' (written at once); nothing else changes */
static int exfat_shrink_entry(struct bpb *fat_bpb, struct fat_file *file, uint64_t maxsize)
{
    unsigned int count = file->e.entries;
    if (count < 3 || count > EXFAT_MAX_SET)
        return -1;

    EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_shr_set);
    struct fat_file parent;
    struct fat_filestr parentstr;
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);
    fat_filestr_init(&parentstr, &parent);

    unsigned int first = exfat_set_first(file);
    if (exfat_read_entries(fat_bpb, &parentstr, first, count, set) < 0 ||
        set[0] != EXT_FILE || set[DIR_ENTRY_SIZE] != EXT_STREAM)
        return -2;

    uint8_t *stream = set + DIR_ENTRY_SIZE;
    if (exfat_le64(stream + 24) <= maxsize)
        return 0;
    exfat_put_le64(stream + 24, maxsize);
    if (exfat_le64(stream + 8) > maxsize)
        exfat_put_le64(stream + 8, maxsize);
    if (!maxsize)
    {
        /* empty: no first cluster (the one kept in memory is re-entered at
           close; a crash before that only leaks it) */
        exfat_put_le32(stream + 20, 0);
        stream[1] &= ~EXSF_NOFATCHAIN;
    }
    exfat_put_le16(set + 2, exfat_set_checksum(set, count));
    return exfat_write_entries(fat_bpb, &parentstr, first, count, set) < 0 ? -3 : 1;
}

/* build a new entry set for 'name'; returns the number of entries or < 0 */
static int exfat_build_set(uint8_t *set, const char *name, uint8_t attr)
{
    EXFAT_SCRATCH(uint16_t, ucs, 256, exfat_bs_ucs);
    unsigned int len = 0;
    const unsigned char *p = (const unsigned char *)name;

    while (*p)
    {
        ucschar_t c;
        p = utf8decode(p, &c);
#ifdef UNICODE32
        if (c >= 0x10000)
        {
            if (len + 2 > 255)
                return -2;
            c -= 0x10000;
            ucs[len++] = 0xd800 | ((c >> 10) & 0x3ff);
            ucs[len++] = 0xdc00 | (c & 0x3ff);
        }
        else
#endif
        {
            if (len + 1 > 255)
                return -2;
            ucs[len++] = c;
        }
    }
    if (!len)
        return -1;

    unsigned int names = (len + EXFAT_NAME_CHARS - 1) / EXFAT_NAME_CHARS;
    unsigned int count = 2 + names;
    memset(set, 0, count * DIR_ENTRY_SIZE);

    uint32_t now;
    uint8_t ms10;
    exfat_now(&now, &ms10);

    uint8_t *file = set;
    file[0] = EXT_FILE;
    file[1] = count - 1;
    exfat_put_le16(file + 4, (attr & ATTR_DIRECTORY) ? ATTR_DIRECTORY :
                             ((attr & (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM)) | ATTR_ARCHIVE));
    exfat_put_le32(file + 8, now);
    exfat_put_le32(file + 12, now);
    exfat_put_le32(file + 16, now);
    file[20] = ms10;
    file[21] = ms10;

    uint8_t *stream = set + DIR_ENTRY_SIZE;
    stream[0] = EXT_STREAM;
    stream[1] = EXSF_ALLOC_POSSIBLE;
    stream[3] = len;
    exfat_put_le16(stream + 4, exfat_name_hash(ucs, len));

    for (unsigned int i = 0; i < names; i++)
    {
        uint8_t *ne = set + (2 + i) * DIR_ENTRY_SIZE;
        ne[0] = EXT_NAME;
        for (unsigned int j = 0; j < EXFAT_NAME_CHARS; j++)
        {
            unsigned int k = i * EXFAT_NAME_CHARS + j;
            exfat_put_le16(ne + 2 + 2 * j, k < len ? ucs[k] : 0);
        }
    }
    return count;
}

/* fat_extend_dir() for exFAT: the same, but the new cluster is zeroed with a
   few large direct writes instead of sector by sector through the 32-sector
   disk cache (a 128 KB cluster would be 256 single-sector writes and would
   flush everything else out of the cache); cached copies of its sectors are
   dropped first so nothing stale can be written over the zeros later */
static int exfat_new_dir_cluster(struct bpb *fat_bpb, struct fat_filestr *dirstr)
{
#ifdef BOOTLOADER
    return fat_extend_dir(fat_bpb, dirstr);
#else
    long cluster = dirstr->lastcluster;
    long newcluster = next_write_cluster(fat_bpb, cluster);
    if (!newcluster)
        return FAT_RC_ENOSPC;

    sector_t start = cluster2sec(fat_bpb, newcluster);
    unsigned long count = fat_bpb->bpb_secperclus;
    if (!start)
        return -1;

    dc_lock_cache();
    dc_discard_range(IF_MV(fat_bpb->volume,) start, count);
    memset(exfat_bitmap_buf, 0, sizeof exfat_bitmap_buf);
    unsigned long per = sizeof exfat_bitmap_buf / LOG_SECTOR_SIZE(fat_bpb);
    for (unsigned long done = 0; done < count; )
    {
        unsigned long n = MIN(per, count - done);
        if (storage_write_sectors(IF_MD(fat_bpb->drive,) fat_bpb->startsector + start + done,
                                  n, exfat_bitmap_buf) < 0)
        {
            dc_unlock_cache();
            update_fat_entry(fat_bpb, newcluster, 0);
            return -2;
        }
        done += n;
    }
    dc_unlock_cache();

    if (!dirstr->fatfilep->firstcluster)
        dirstr->fatfilep->firstcluster = newcluster;

    dirstr->lastcluster = newcluster;
    dirstr->lastsector  = start + count - 1;
    dirstr->clusternum++;
    dirstr->sectornum   = count - 1;
    dirstr->eof         = false;
    return 0;
#endif
}

/* grow a directory by one zeroed cluster and record its new size in its
   parent; dirstr must be positioned at its last cluster */
static int exfat_extend_dir(struct bpb *fat_bpb, struct fat_filestr *dirstr)
{
    struct fat_file *dir = dirstr->fatfilep;
    int rc;

    if (dir->firstcluster > 0 && (dir->firstcluster & EXFAT_CONTIG))
    {
        rc = exfat_make_chained(fat_bpb, dir);
        if (rc < 0)
            return rc * 10 - 1;
        dirstr->lastcluster = exfat_clus(dirstr->lastcluster);
    }

    rc = exfat_new_dir_cluster(fat_bpb, dirstr);
    if (rc < 0)
        return rc;
    exfat_commit_alloc(fat_bpb);

    if (dir->dircluster && dir->firstcluster != fat_bpb->bpb_rootclus)
    {
        /* new size = clusters in the chain */
        uint64_t bytes = (uint64_t)(dirstr->clusternum + 1) *
                         fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb);
        rc = exfat_update_entry(fat_bpb, dir, true, bytes, false, NULL);
        if (rc < 0)
            return rc * 10 - 2;
    }
    return 0;
}

/* Windows compares exFAT names case-insensitively with the volume's up-case
   table.  Rockbox's own lookup only folds ASCII case, so "Ü.mp3" could be
   created next to "ü.mp3" -- two names Windows treats as the same file.  For
   names with non-ASCII characters scan the directory for such a twin
   (same name hash, then compare up-cased); 'self' (rename) is skipped. */
static bool exfat_name_taken(struct bpb *fat_bpb, struct fat_filestr *dirstr,
                             const uint8_t *set, const struct fat_file *self)
{
    unsigned int len = set[DIR_ENTRY_SIZE + 3];
    uint16_t hash = exfat_le16(set + DIR_ENTRY_SIZE + 4);
    bool ascii = true;
    for (unsigned int i = 0; i < len && ascii; i++)
        if (exfat_le16(set + (2 + i / EXFAT_NAME_CHARS) * DIR_ENTRY_SIZE + 2 + 2 * (i % EXFAT_NAME_CHARS)) >= 0x80)
            ascii = false;
    if (ascii)
        return false;   /* the upper layer already refused ASCII-case twins */

    EXFAT_SCRATCH(uint8_t, other, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_nt_other);
    for (unsigned int entry = 0; entry < EXFAT_MAX_DIRENTRIES; entry++)
    {
        dc_lock_cache();
        union raw_dirent *ent = cache_direntry(fat_bpb, dirstr, entry);
        if (!ent)
        {
            dc_unlock_cache();
            break;
        }
        uint8_t type = ent->data[0], sec = ent->data[1];
        dc_unlock_cache();
        if (type == EXT_EOD)
            break;
        if (type != EXT_FILE || sec < 2 || sec + 1 > EXFAT_MAX_SET)
            continue;
        unsigned int n = sec + 1;
        if (self && self->dircluster == dirstr->fatfilep->firstcluster &&
            exfat_set_first(self) == entry)
        {
            entry += n - 1;
            continue;
        }
        if (exfat_read_entries(fat_bpb, dirstr, entry, n, other) < 0)
            break;
        if (other[DIR_ENTRY_SIZE] == EXT_STREAM && other[DIR_ENTRY_SIZE + 3] == len &&
            exfat_le16(other + DIR_ENTRY_SIZE + 4) == hash)
        {
            /* equal for Windows (full up-casing) but not for Rockbox (which
               folds ASCII only)?  Names Rockbox also sees as equal are its own
               business: an overwrite-rename creates the new entry before it
               removes the victim. */
            bool same = true, rbsame = true;
            for (unsigned int i = 0; i < len && same; i++)
            {
                unsigned int o = (2 + i / EXFAT_NAME_CHARS) * DIR_ENTRY_SIZE + 2 + 2 * (i % EXFAT_NAME_CHARS);
                if (o + 2 > n * DIR_ENTRY_SIZE)
                {
                    same = false;
                    break;
                }
                uint16_t a = exfat_le16(other + o), b = exfat_le16(set + o);
                if (exfat_toupper(a) != exfat_toupper(b))
                    same = false;
                if (a != b && (a >= 0x80 || b >= 0x80 ||
                               (a | 0x20) != (b | 0x20) || (a | 0x20) < 'a' || (a | 0x20) > 'z'))
                    rbsame = false;
            }
            if (same && !rbsame)
                return true;
        }
        entry += n - 1;
    }
    return false;
}

/* find room for and write a new entry set in parentstr's directory */
static int exfat_add_set(struct bpb *fat_bpb, struct fat_filestr *parentstr,
                         struct fat_file *file, uint8_t *set, unsigned int count,
                         bool now)
{
    int rc;
    unsigned int entry = 0, found = 0, firstentry = 0;
    bool have = false;
    const unsigned int entperclus = DIR_ENTRIES_PER_SECTOR * fat_bpb->bpb_secperclus;

    dc_lock_cache();
    while (1)
    {
        union raw_dirent *ent = cache_direntry(fat_bpb, parentstr, entry);
        if (!ent)
        {
            if (parentstr->eof)
                break;
            dc_unlock_cache();
            FAT_ERROR(-1);
        }

        uint8_t type = ent->data[0];
        if (type == EXT_EOD)
        {
            /* the rest of the directory is free */
            unsigned int rest = entperclus - (entry % entperclus);
            if (!found)
                firstentry = entry;
            found += rest;
            entry += rest;
            /* keep going: following clusters (if any) are free too */
            if (found >= count)
            {
                have = true;
                break;
            }
            continue;
        }
        if (!(type & EXT_INUSE))
        {
            if (!found)
                firstentry = entry;
            if (++found >= count)
            {
                have = true;
                break;
            }
        }
        else
            found = 0;
        entry++;
    }
    dc_unlock_cache();

    if (!have)
    {
        if (!found)
            firstentry = entry;
        if (firstentry + count > EXFAT_MAX_DIRENTRIES)
            FAT_ERROR(-2);
        while (found < count)
        {
            rc = exfat_extend_dir(fat_bpb, parentstr);
            if (rc == FAT_RC_ENOSPC)
                FAT_ERROR(RC);
            else if (rc < 0)
                FAT_ERROR(rc * 10 - 3);
            found += entperclus;
        }
    }

#ifdef HAVE_MULTIVOLUME
    file->volume     = parentstr->fatfilep->volume;
#endif
    file->dircluster = parentstr->fatfilep->firstcluster;
    file->e.entry    = firstentry + count - 1;
    file->e.entries  = count;

    exfat_put_le16(set + 2, exfat_set_checksum(set, count));
    exfat_commit_alloc(fat_bpb);
    rc = exfat_write_entries_ex(fat_bpb, parentstr, firstentry, count, set, now);
    if (rc < 0)
        FAT_ERROR(rc * 10 - 4);

    rc = 0;
fat_error:
    return rc;
}

/* free the clusters of 'file' (chain or contiguous run) */
static int exfat_free_data(struct bpb *fat_bpb, struct fat_file *file)
{
    long fc = file->firstcluster;
    if (fc <= 0)
        return 1;
    if (fc & EXFAT_CONTIG)
    {
        uint32_t first = exfat_clus(fc);
        uint32_t n = exfat_run_length(fat_bpb, file);
        if (n == EXFAT_RUN_CHAINED)
            return free_cluster_chain(fat_bpb, first);
        if (n == 0xffffffff)
            return -1;
        for (uint32_t i = 0; i < n; i++)
            if (exfat_bitmap_set(fat_bpb, first + i, false) < 0)
                return -2;
        exfat_run_drop(fat_bpb, first);
        return 1;
    }
    exfat_run_drop(fat_bpb, exfat_clus(fc));
    return free_cluster_chain(fat_bpb, exfat_clus(fc));
}

static int exfat_free_set(struct bpb *fat_bpb, struct fat_file *file)
{
    struct fat_file parent;
    struct fat_filestr parentstr;
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &parent);
    fat_filestr_init(&parentstr, &parent);

    unsigned int first = exfat_set_first(file);
    exfat_percent_unknown(fat_bpb);
    for (unsigned int i = 0; i < file->e.entries; i++)
    {
        dc_lock_cache();
        union raw_dirent *ent = cache_direntry(fat_bpb, &parentstr, first + i);
        if (!ent)
        {
            dc_unlock_cache();
            return i ? 0 : -1;
        }
        ent->data[0] &= ~EXT_INUSE;
        dc_dirty_buf(ent);
        dc_unlock_cache();
    }

    file->dircluster = 0;
    file->e.entry    = FAT_DIRSCAN_RW_VAL;
    file->e.entries  = 0;
    return 1;
}

/* mount: boot sector already in buf */
static int exfat_mount_internal(struct bpb *fat_bpb, uint8_t *buf)
{
    int rc;
    unsigned int bps_shift = buf[108], spc_shift = buf[109];

    if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift)
        FAT_ERROR(-1);
    if (BYTES2INT16(buf, BPB_LAST_WORD) != 0xaa55)
        FAT_ERROR(-2);

    fat_bpb->bpb_bytspersec = 1ul << bps_shift;
    if (fat_bpb->bpb_bytspersec % LOG_SECTOR_SIZE(fat_bpb))
        FAT_ERROR(-3);
    unsigned long secmult = fat_bpb->bpb_bytspersec / LOG_SECTOR_SIZE(fat_bpb);

    uint32_t fatoff   = BYTES2INT32(buf, 80);
    uint32_t fatlen   = BYTES2INT32(buf, 84);
    uint32_t heapoff  = BYTES2INT32(buf, 88);
    uint32_t clusters = BYTES2INT32(buf, 92);
    uint32_t root     = BYTES2INT32(buf, 96);
    uint64_t vollen   = (uint64_t)BYTES2INT32(buf, 72) | (uint64_t)BYTES2INT32(buf, 76) << 32;
    uint16_t volflags = BYTES2INT16(buf, 106);
    unsigned int nfats = buf[110];

    if (nfats != 1)
    {
        DEBUGF("%s: %u FATs (TexFAT) not supported\n", __func__, nfats);
        FAT_ERROR(-4);
    }
    if (clusters + 2 >= EXFAT_CLUS_MASK || root < 2 || root > clusters + 1)
        FAT_ERROR(-5);

    fat_bpb->is_exfat        = 1;
    fat_bpb->bpb_secperclus  = secmult << spc_shift;
    fat_bpb->bpb_numfats     = 1;
    fat_bpb->bpb_media       = 0xf8;
    fat_bpb->bpb_rsvdseccnt  = secmult * fatoff;
    fat_bpb->fatsize         = secmult * fatlen;
    fat_bpb->fatrgnstart     = secmult * fatoff;
    fat_bpb->fatrgnend       = fat_bpb->fatrgnstart + fat_bpb->fatsize;
    fat_bpb->firstdatasector = secmult * heapoff;
    fat_bpb->dataclusters    = clusters;
    fat_bpb->totalsectors    = secmult * vollen;
    fat_bpb->bpb_rootclus    = root;
    fat_bpb->rootdirsector   = cluster2sec(fat_bpb, root);
    fat_bpb->last_word       = 0xaa55;
    fat_bpb->fsinfo.freecount = 0xffffffff;
    fat_bpb->fsinfo.nextfree  = 0xffffffff;
    fat_bpb->ex_bitmap_sector = 0;
    fat_bpb->ex_volflags      = volflags;
    fat_bpb->ex_pct_done      = 0;
    fat_bpb->ex_dirty_ours    = 0;
    exfat_runs_forget(fat_bpb);

    if ((uint64_t)fat_bpb->firstdatasector + (uint64_t)clusters * fat_bpb->bpb_secperclus >
        (uint64_t)secmult * vollen)
        FAT_ERROR(-6);

    /* the root directory: allocation bitmap and up-case table entries
       (mounted is still false: use cache_sector directly) */
    long clus = root;
    unsigned int found = 0;
    for (unsigned int guard = 0; clus && guard < 4096 && found != 3; guard++)
    {
        sector_t sec = cluster2sec(fat_bpb, clus);
        for (unsigned int s = 0; s < fat_bpb->bpb_secperclus && found != 3; s++)
        {
            dc_lock_cache();
            uint8_t *d = cache_sector(fat_bpb, sec + s);
            if (!d)
            {
                dc_unlock_cache();
                FAT_ERROR(-7);
            }
            for (unsigned int i = 0; i < LOG_SECTOR_SIZE(fat_bpb); i += DIR_ENTRY_SIZE)
            {
                const uint8_t *e = d + i;
                if (e[0] == EXT_EOD)
                {
                    found |= 0x100;
                    break;
                }
                if (e[0] == EXT_BITMAP && !(e[1] & 1)) /* first bitmap */
                {
                    fat_bpb->ex_bitmap_cluster = exfat_le32(e + 20);
                    fat_bpb->ex_bitmap_bytes   = exfat_le64(e + 24);
                    found |= 1;
                }
                else if (e[0] == EXT_UPCASE)
                {
                    fat_bpb->ex_upcase_cluster = exfat_le32(e + 20);
                    fat_bpb->ex_upcase_bytes   = exfat_le64(e + 24);
                    found |= 2;
                }
            }
            dc_unlock_cache();
            if (found & 0x100)
                break;
        }
        if (found & 0x100)
            break;
        clus = get_next_cluster_ex(fat_bpb, clus);
    }

    if (!(found & 1) || fat_bpb->ex_bitmap_cluster < 2 ||
        fat_bpb->ex_bitmap_bytes * 8 < clusters)
        FAT_ERROR(-8);

    /* the bitmap must be one contiguous run (it always is in practice) */
    {
        uint32_t need = exfat_bytes2clusters(fat_bpb, fat_bpb->ex_bitmap_bytes);
        long c = fat_bpb->ex_bitmap_cluster;
        for (uint32_t i = 1; i < need; i++)
        {
            long next = get_next_cluster_ex(fat_bpb, c);
            if (next != c + 1)
            {
                /* maybe NoFatChain-style (FAT not maintained): accept if the
                   FAT entry is simply unset */
                if (next != 0)
                    FAT_ERROR(-9);
            }
            c++;
        }
        fat_bpb->ex_bitmap_sector = cluster2sec(fat_bpb, fat_bpb->ex_bitmap_cluster);
    }

#ifndef BOOTLOADER
    /* up-case table (compressed form: 0xffff, n = n identity mappings) */
    exfat_upcase_reset();
    if ((found & 2) && fat_bpb->ex_upcase_cluster >= 2 &&
        fat_bpb->ex_upcase_bytes >= 2 && fat_bpb->ex_upcase_bytes <= 2 * 65536 + 4096)
    {
        bool fits = true;
        long c = fat_bpb->ex_upcase_cluster;
        uint64_t left = fat_bpb->ex_upcase_bytes;
        unsigned int idx = 0;
        bool skip = false;
        while (c && left && idx < 65536)
        {
            sector_t sec = cluster2sec(fat_bpb, c);
            for (unsigned int s = 0; s < fat_bpb->bpb_secperclus && left && idx < 65536; s++)
            {
                dc_lock_cache();
                uint8_t *d = cache_sector(fat_bpb, sec + s);
                if (!d)
                {
                    dc_unlock_cache();
                    FAT_ERROR(-10);
                }
                for (unsigned int i = 0; i + 1 < LOG_SECTOR_SIZE(fat_bpb) && left >= 2 && idx < 65536;
                     i += 2, left -= 2)
                {
                    uint16_t v = exfat_le16(d + i);
                    if (skip)
                    {
                        idx += v;   /* identity mappings stay */
                        skip = false;
                    }
                    else if (v == 0xffff)
                        skip = true;
                    else
                    {
                        if (!exfat_upcase_put(idx, v))
                            fits = false;
                        idx++;
                    }
                }
                dc_unlock_cache();
            }
            long next = get_next_cluster_ex(fat_bpb, c);
            c = next > 0 ? next : (left ? c + 1 : 0);
        }
        exfat_upcase_ok = fits;
        if (!fits)
            exfat_upcase_reset();
    }
#endif

    rc = 0;
fat_error:
    return rc;
}


/** File entity functions **/

#if defined(MAX_VARIABLE_LOG_SECTOR)
int fat_file_sector_size(IF_MV_NONVOID(const struct fat_file *file))
{
    const struct bpb *fat_bpb = FAT_BPB(file->volume);

    return LOG_SECTOR_SIZE(fat_bpb);
}
#endif

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_create_file)(struct fat_file *parent, const char *name,
                    uint8_t attr, struct fat_file *file,
                    struct fat_direntry *fatent)
{
    DEBUGF("%s(\"%s\",%lx,%lx)\n", __func__, name, (long)file, (long)parent);
    struct bpb * const fat_bpb = FAT_BPB(parent->volume);
    if (!fat_bpb)
        return -1;

    int rc;

    fat_open_internal(IF_MV(parent->volume,) 0, file);

    struct fat_filestr parentstr;
    fat_filestr_init(&parentstr, parent);

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66 */
        EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_cr_set);
        rc = check_longname(name);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 1);

        int count = exfat_build_set(set, name, attr);
        if (count < 0)
            FAT_ERROR(count * 10 - 2);
        if (exfat_name_taken(fat_bpb, &parentstr, set, NULL))
            FAT_ERROR(-6);  /* a Windows-equal name (non-ASCII case) exists */

        if (attr & ATTR_DIRECTORY)
        {
            /* a directory starts with one zeroed cluster; no dot entries */
            struct fat_filestr dirstr;
            fat_filestr_init(&dirstr, file);
            rc = exfat_new_dir_cluster(fat_bpb, &dirstr);
            if (rc == FAT_RC_ENOSPC)
                FAT_ERROR(RC);
            else if (rc < 0)
                FAT_ERROR(rc * 10 - 3);

            uint8_t *stream = set + DIR_ENTRY_SIZE;
            uint64_t bytes = (uint64_t)fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb);
            exfat_put_le32(stream + 20, file->firstcluster);
            exfat_put_le64(stream + 8, bytes);
            exfat_put_le64(stream + 24, bytes);
        }

        rc = exfat_add_set(fat_bpb, &parentstr, file, set, count, false);
        if (rc == FAT_RC_ENOSPC)
            FAT_ERROR(RC);
        else if (rc < 0)
            FAT_ERROR(rc * 10 - 4);

        if (fatent)
            exfat_parse_set(fat_bpb, set, count, fatent);

        rc = 0;
        goto fat_error;
    }

    const bool isdir = attr & ATTR_DIRECTORY;
    unsigned int addflags = fatent ? DIRENT_RETURN : 0;
    union raw_dirent *newentp = (isdir || fatent) ?
                                alloca(sizeof (union raw_dirent)) : NULL;

    if (isdir)
    {
        struct fat_filestr dirstr;
        fat_filestr_init(&dirstr, file);

        /* create the first cluster */
        rc = fat_extend_dir(fat_bpb, &dirstr);
        if (rc == FAT_RC_ENOSPC)
            FAT_ERROR(RC);
        else if (rc < 0)
            FAT_ERROR(rc * 10 - 2);

        struct fat_file dummyfile;

        /* add the "." entry */
        fat_open_internal(IF_MV(0,) file->firstcluster, &dummyfile);

        /* this returns the short entry template for the remaining entries */
        rc = add_dir_entry(fat_bpb, &dirstr, &dummyfile, ".", newentp,
                           attr, DIRENT_RETURN);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 3);

        /* and the ".." entry */
        /* the root cluster is cluster 0 in the ".." entry */
        fat_open_internal(IF_MV(0,)
            parent->firstcluster == fat_bpb->bpb_rootclus ?
                0 : parent->firstcluster, &dummyfile);

        rc = add_dir_entry(fat_bpb, &dirstr, &dummyfile, "..", newentp,
                           attr, DIRENT_TEMPL_TIMES);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 4);

        addflags |= DIRENT_TEMPL_TIMES;
    }

    /* lastly, add the entry in the parent directory */
    rc = add_dir_entry(fat_bpb, &parentstr, file, name, newentp,
                       attr, addflags);
    if (rc == FAT_RC_ENOSPC)
        FAT_ERROR(RC);
    else if (rc < 0)
        FAT_ERROR(rc * 10 - 5);

    if (fatent)
    {
        strcpy(fatent->name, name);
        parse_short_direntry(newentp, fatent);
    }

    rc = 0;
fat_error:
    if (rc < 0)
        free_cluster_chain(fat_bpb, file->firstcluster);

    cache_commit(fat_bpb);
    return rc;
}

bool fat_dir_is_parent(const struct fat_file *dir, const struct fat_file *file)
{
    /* if the directory file's first cluster is the same as the file's
       directory cluster and they're on the same volume, 'dir' is its parent
       directory; the file must also have a dircluster (ie. not removed) */
    long dircluster = file->dircluster;
    return dircluster && dircluster == dir->firstcluster
            IF_MV( && dir->volume == file->volume );
}

bool fat_file_is_same(const struct fat_file *file1,
                      const struct fat_file *file2)
{
    /* first, triviality */
    if (file1 == file2)
        return true;

    /* if the directory info matches and the volumes are the same, file1 and
       file2 refer to the same file/directory */
    return file1->dircluster == file2->dircluster
           && file1->e.entry == file2->e.entry
           IF_MV( && file1->volume == file2->volume );
}

int fat_open(const struct fat_file *parent, long startcluster,
             struct fat_file *file)
{
    if (!parent)
        return -2; /* this does _not_ open any root */

    struct bpb * const fat_bpb = FAT_BPB(parent->volume);
    if (!fat_bpb)
        return -1;

    /* inherit basic parent information; dirscan info is expected to have been
       initialized beforehand (usually via scanning for the entry ;) */
#ifdef HAVE_MULTIVOLUME
    file->volume       = parent->volume;
#endif
    file->firstcluster = startcluster;
    file->dircluster   = parent->firstcluster;

    return 0;
}

int fat_open_rootdir(IF_MV(int volume,) struct fat_file *dir)
{
    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (!fat_bpb)
        return -1;

    fat_open_internal(IF_MV(volume,) fat_bpb->bpb_rootclus, dir);
    return 0;
}

int fat_remove(struct fat_file *file, enum fat_remove_op what)
{
    struct bpb * const fat_bpb = FAT_BPB(file->volume);
    if (!fat_bpb)
        return -1;

    int rc;

    if (file->firstcluster == fat_bpb->bpb_rootclus)
    {
        DEBUGF("Trying to remove root of volume %d\n",
               IF_MV_VOL(file->volume));
        FAT_ERROR(-2);
    }

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66 */
        if ((what & FAT_RM_DIRENTRIES) && !(what & FAT_RM_DATA) &&
            file->firstcluster > 0 && (file->firstcluster & EXFAT_CONTIG))
        {
            /* the data outlives the entry (file still open): keep its length
               by turning the run into a FAT chain while we still can */
            if (exfat_make_chained(fat_bpb, file) < 0)
                FAT_ERROR(-6);
        }
        if (file->dircluster && (what & FAT_RM_DIRENTRIES))
        {
            if (file->firstcluster > 0 && (file->firstcluster & EXFAT_CONTIG))
                exfat_run_length(fat_bpb, file);    /* remember it before the entry goes */
            rc = exfat_free_set(fat_bpb, file);
            if (rc <= 0)
                FAT_ERROR(rc * 10 - 3);
            if (what & FAT_RM_DATA)
            {
                dc_lock_cache();
                dc_commit_all(IF_MV(fat_bpb->volume));
                dc_unlock_cache();
            }
        }
        if (file->firstcluster && (what & FAT_RM_DATA))
        {
            rc = exfat_free_data(fat_bpb, file);
            if (rc < 0)
                FAT_ERROR(rc * 10 - 4);
            file->firstcluster = 0;
        }
        rc = 0;
        goto fat_error;
    }

    if (file->dircluster && (what & FAT_RM_DIRENTRIES))
    {
        /* free everything in the parent directory */
        DEBUGF("Removing dir entries: %lX\n", file->dircluster);
        rc = free_direntries(fat_bpb, file);
        if (rc <= 0)
            FAT_ERROR(rc * 10 - 3);
    }

    if (file->firstcluster && (what & FAT_RM_DATA))
    {
        /* mark all clusters in the chain as free */
        DEBUGF("Removing cluster chain: %lX\n", file->firstcluster);
        rc = free_cluster_chain(fat_bpb, file->firstcluster);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 4);

        /* at least the first cluster was freed */
        file->firstcluster = 0;

        if (rc == 0)
            FAT_ERROR(-5);
    }

    rc = 0;
fat_error:
    cache_commit(fat_bpb);
    return rc;
}

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_rename)(struct fat_file *parent, struct fat_file *file,
               const unsigned char *newname)
{
    struct bpb * const fat_bpb = FAT_BPB(parent->volume);
    if (!fat_bpb)
        return -1;

    int rc;
    /* save old file; don't change it unless everything succeeds */
    struct fat_file newfile = *file;

#ifdef HAVE_MULTIVOLUME
    /* rename only works on the same volume */
    if (file->volume != parent->volume)
    {
        DEBUGF("No rename across volumes!\n");
        FAT_ERROR(-2);
    }
#endif

    /* root directories can't be renamed */
    if (file->firstcluster == fat_bpb->bpb_rootclus)
    {
        DEBUGF("Trying to rename root of volume %d\n",
               IF_MV_VOL(file->volume));
        FAT_ERROR(-3);
    }

    if (!file->dircluster)
    {
        /* file was removed but is still open */
        DEBUGF("File has no dir cluster!\n");
        FAT_ERROR(-4);
    }

    struct fat_file dir;
    struct fat_filestr dirstr;

    /* open old parent */
    fat_open_internal(IF_MV(file->volume,) file->dircluster, &dir);
    fat_filestr_init(&dirstr, &dir);

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66: new set with the new name and the old file/stream
           data; then the old set goes.  No ".." to fix up on exFAT. */
        EXFAT_SCRATCH(uint8_t, old, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_rn_old);
        EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_rn_set);
        unsigned int oldcount = file->e.entries;
        if (oldcount < 3 || oldcount > EXFAT_MAX_SET ||
            exfat_read_entries(fat_bpb, &dirstr, exfat_set_first(file), oldcount, old) < 0 ||
            old[0] != EXT_FILE || old[DIR_ENTRY_SIZE] != EXT_STREAM)
            FAT_ERROR(-10);

        rc = check_longname(newname);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 11);
        int count = exfat_build_set(set, (const char *)newname, 0);
        if (count < 0)
            FAT_ERROR(count * 10 - 12);

        /* keep attributes and times (file entry bytes 4..25) */
        memcpy(set + 4, old + 4, 22);
        /* keep flags, lengths and first cluster; new name length and hash */
        uint8_t *st = set + DIR_ENTRY_SIZE, *ost = old + DIR_ENTRY_SIZE;
        st[1] = ost[1];
        memcpy(st + 8, ost + 8, 24);

        fat_filestr_init(&dirstr, parent);
        if (exfat_name_taken(fat_bpb, &dirstr, set, file))
            FAT_ERROR(-15);

        if ((unsigned int)count == oldcount &&
            parent->firstcluster == file->dircluster)
        {
            /* bcm overlay v67: same directory, same set size: rewrite the
               set where it is, in one write -- a crash leaves the old name
               or the new one, never neither and never both */
            exfat_put_le16(set + 2, exfat_set_checksum(set, count));
            fat_open_internal(IF_MV(file->volume,) file->dircluster, &dir);
            fat_filestr_init(&dirstr, &dir);
            rc = exfat_write_entries(fat_bpb, &dirstr, exfat_set_first(file), count, set);
            if (rc < 0)
                FAT_ERROR(rc * 10 - 16);
            rc = 0;
            goto fat_error;
        }

        /* the new set is on disk before the old one is removed: a crash in
           between leaves both names, not none */
        rc = exfat_add_set(fat_bpb, &dirstr, &newfile, set, count, true);
        if (rc == FAT_RC_ENOSPC)
            FAT_ERROR(RC);
        else if (rc < 0)
            FAT_ERROR(rc * 10 - 13);

        rc = exfat_free_set(fat_bpb, file);
        if (rc <= 0)
        {
            exfat_free_set(fat_bpb, &newfile);
            FAT_ERROR(rc * 10 - 14);
        }

        *file = newfile;
        rc = 0;
        goto fat_error;
    }

    /* fetch a copy of the existing short entry */
    dc_lock_cache();

    union raw_dirent *ent = cache_direntry(fat_bpb, &dirstr, file->e.entry);
    if (!ent)
    {
        dc_unlock_cache();
        FAT_ERROR(-5);
    }

    union raw_dirent rawent = *ent;

    dc_unlock_cache();

    /* create new name in new parent directory */
    fat_filestr_init(&dirstr, parent);
    rc = add_dir_entry(fat_bpb, &dirstr, &newfile, newname, &rawent,
                       0, DIRENT_TEMPL_CRT | DIRENT_TEMPL_WRT);
    if (rc == FAT_RC_ENOSPC)
        FAT_ERROR(RC);
    else if (rc < 0)
        FAT_ERROR(rc * 10 - 6);

    /* if renaming a directory and it was a move, update the '..' entry to
       keep it pointing to its parent directory */
    if ((rawent.attr & ATTR_DIRECTORY) && newfile.dircluster != file->dircluster)
    {
        /* open the dir that was renamed */
        fat_open_internal(IF_MV(newfile.volume,) newfile.firstcluster, &dir);
        fat_filestr_init(&dirstr, &dir);

        /* obtain dot-dot directory entry */
        dc_lock_cache();
        ent = cache_direntry(fat_bpb, &dirstr, 1);
        if (!ent)
        {
            dc_unlock_cache();
            FAT_ERROR(-7);
        }

        if (strncmp("..         ", ent->name, 11))
        {
            /* .. entry must be second entry according to FAT spec (p.29) */
            DEBUGF("Second dir entry is not double-dot!\n");
            dc_unlock_cache();
            FAT_ERROR(-8);
        }

        /* parent cluster is 0 if parent dir is the root - FAT spec (p.29) */
        long parentcluster = 0;
        if (parent->firstcluster != fat_bpb->bpb_rootclus)
            parentcluster = parent->firstcluster;

        raw_dirent_set_fstclus(ent, parentcluster);

        dc_dirty_buf(ent);
        dc_unlock_cache();
    }

    /* remove old name */
    rc = free_direntries(fat_bpb, file);
    if (rc <= 0)
        FAT_ERROR(rc * 10 - 9);

    /* finally, update old file with new directory entry info */
    *file = newfile;

    rc = 0;
fat_error:
    if (rc < 0 && !fat_file_is_same(&newfile, file))
        free_direntries(fat_bpb, &newfile);

    cache_commit(fat_bpb);
    return rc;
}

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_modtime)(struct fat_file *parent, struct fat_file *file,
                time_t modtime)
{
    struct bpb * const fat_bpb = FAT_BPB(parent->volume);

    if (!fat_bpb)
        return -1;

    int rc;

    struct fat_filestr parentstr;
    fat_filestr_init(&parentstr, parent);

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66 */
        EXFAT_SCRATCH(uint8_t, set, EXFAT_MAX_SET * DIR_ENTRY_SIZE, exfat_mt_set);
        unsigned int count = file->e.entries;
        unsigned int first = exfat_set_first(file);
        uint16_t date, time;
        if (count < 3 || count > EXFAT_MAX_SET ||
            exfat_read_entries(fat_bpb, &parentstr, first, count, set) < 0 ||
            set[0] != EXT_FILE)
        {
            rc = -2;
        }
        else
        {
            dostime_localtime(modtime, &date, &time);
            exfat_put_le32(set + 12, (uint32_t)date << 16 | time);
            exfat_put_le32(set + 16, (uint32_t)date << 16 | time);
            set[21] = 0;
            set[23] = 0;
            exfat_put_le16(set + 2, exfat_set_checksum(set, count));
            rc = exfat_write_entries(fat_bpb, &parentstr, first, count, set) < 0 ? -3 : 0;
        }
        cache_commit(fat_bpb);
        return rc;
    }

    dc_lock_cache();

    union raw_dirent *ent = cache_direntry(fat_bpb, &parentstr, file->e.entry);
    if (!ent)
        FAT_ERROR(-2);

    uint16_t date;
    uint16_t time;
    dostime_localtime(modtime, &date, &time);

    ent->wrttime    = htole16(time);
    ent->wrtdate    = htole16(date);
    ent->lstaccdate = htole16(date);

    dc_dirty_buf(ent);

    rc = 0;
fat_error:
    dc_unlock_cache();
    cache_commit(fat_bpb);
    return rc;
}

/** File stream functions **/

#ifndef BOOTLOADER
uint64_t fat_file_max_size(const struct fat_file *file)
{
    const struct bpb *fat_bpb = FAT_BPB(file->volume);
    if (fat_bpb && fat_bpb->is_exfat)
        return (uint64_t)0xffffffffu * LOG_SECTOR_SIZE(fat_bpb);   /* bcm overlay v89 */
    return 0xffffffffu;
}
#endif

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_closewrite)(struct fat_filestr *filestr, fat_size_t size,
                   struct fat_direntry *fatentp)
{
    DEBUGF("%s(size=%ld)\n", __func__, size);
    struct fat_file * const file = filestr->fatfilep;
    struct bpb * const fat_bpb = FAT_BPB(file->volume);
    if (!fat_bpb)
        return -1;

    int rc;

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66 */
#ifdef BOOTLOADER
        if (file->firstcluster > 0 && (file->firstcluster & EXFAT_BIG))
            FAT_ERROR(-5);  /* >= 4 GiB: never written */
#endif
        struct fat_file old = *file;
        bool freeold = false;
        if (!size && file->firstcluster)
        {
            /* emptied: the entry says so on disk before the clusters go */
            if (file->firstcluster > 0 && (file->firstcluster & EXFAT_CONTIG))
                exfat_run_length(fat_bpb, &old);
            file->firstcluster = 0;
            fat_rewind(filestr);
            freeold = true;
        }
        if (file->dircluster)
        {
            rc = exfat_update_entry(fat_bpb, file, true, size, true, fatentp);
            if (rc < 0)
                FAT_ERROR(rc * 10 - 7);
        }
        else if (fatentp)
            fat_empty_fat_direntry(fatentp);
        if (freeold)
        {
            rc = exfat_free_data(fat_bpb, &old);
            if (rc < 0)
                FAT_ERROR(rc * 10 - 6);
        }
        rc = 0;
        goto fat_error;
    }

    if (!size && file->firstcluster)
    {
        /* empty file */
        rc = update_fat_entry(fat_bpb, file->firstcluster, 0);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 2);

        file->firstcluster = 0;
        fat_rewind(filestr);
    }

    if (file->dircluster)
    {
        rc = update_short_entry(fat_bpb, file, size, fatentp);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 3);
    }
    else if (fatentp)
    {
        fat_empty_fat_direntry(fatentp);
    }

#ifdef TEST_FAT
    if (file->firstcluster)
    {
        unsigned long count = 0;
        for (long next = file->firstcluster; next;
             next = get_next_cluster(fat_bpb, next))
        {
            DEBUGF("cluster %ld: %lx\n", count, next);
            count++;
        }

        uint32_t len = count * fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb);
        DEBUGF("File is %lu clusters (chainlen=%lu, size=%lu)\n",
               count, len, size );

        if (len > size + fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb))
            panicf("Cluster chain is too long\n");

        if (len < size)
            panicf("Cluster chain is too short\n");
    }
#endif /* TEST_FAT */

    rc = 0;
fat_error:
    cache_commit(fat_bpb);
    return rc;
}

void fat_filestr_init(struct fat_filestr *fatstr, struct fat_file *file)
{
    fatstr->fatfilep = file;
    fat_rewind(fatstr);
}

sector_t fat_query_sectornum(const struct fat_filestr *filestr)
{
    /* return next sector number to be transferred */
    struct bpb * const fat_bpb = FAT_BPB(filestr->fatfilep->volume);
    if (!fat_bpb)
        return INVALID_SECNUM;

    return fat_bpb->bpb_secperclus*filestr->clusternum + filestr->sectornum + 1;
}

/* helper for fat_readwrite */
static long transfer(struct bpb *fat_bpb, sector_t start, long count,
                     char *buf, bool write)
{
    long rc = 0;

    DEBUGF("%s(s=%llx, c=%lx, wr=%u)\n", __func__,
           (uint64_t)(start + fat_bpb->startsector), count, write ? 1 : 0);

    if (write)
    {
        sector_t firstallowed;
#ifdef HAVE_FAT16SUPPORT
        if (fat_bpb->is_fat16)
            firstallowed = fat_bpb->rootdirsector;
        else
#endif /* HAVE_FAT16SUPPORT */
            firstallowed = fat_bpb->firstdatasector;

        if (start < firstallowed)
            panicf("Write %llu before data\n", (uint64_t)(firstallowed - start));

        if (start + count > fat_bpb->totalsectors)
        {
            panicf("Write %llu after data\n",
                   (uint64_t)(start + count - fat_bpb->totalsectors));
        }
    }

#ifdef STORAGE_NEEDS_BOUNCE_BUFFER
    if(UNLIKELY(STORAGE_OVERLAP((uintptr_t)buf))) {
        void* xfer_buf = FAT_BOUNCE_BUFFER(fat_bpb);
        while(count > 0) {
            int xfer_count = MIN(count*LOG_SECTOR_SIZE(fat_bpb), (FAT_BOUNCE_SECTORS * BOUNCE_SECTOR_SIZE)) / LOG_SECTOR_SIZE(fat_bpb);

            if(write) {
                memcpy(xfer_buf, buf, xfer_count * LOG_SECTOR_SIZE(fat_bpb));
                rc = storage_write_sectors(IF_MD(fat_bpb->drive,)
                                           start + fat_bpb->startsector, xfer_count, xfer_buf);
            } else {
                rc = storage_read_sectors(IF_MD(fat_bpb->drive,)
                                          start + fat_bpb->startsector, xfer_count, xfer_buf);
                memcpy(buf, xfer_buf, xfer_count * LOG_SECTOR_SIZE(fat_bpb));
            }

            if(rc < 0)
                break;

            buf += xfer_count * LOG_SECTOR_SIZE(fat_bpb);
            start += xfer_count;
            count -= xfer_count;
        }
    } else {
#endif
        if(write) {
            rc = storage_write_sectors(IF_MD(fat_bpb->drive,)
                                       start + fat_bpb->startsector, count, buf);
        } else {
            rc = storage_read_sectors(IF_MD(fat_bpb->drive,)
                                      start + fat_bpb->startsector, count, buf);
        }
#ifdef STORAGE_NEEDS_BOUNCE_BUFFER
    }
#endif

    if (rc < 0)
    {
        DEBUGF("Couldn't %s sector %llx (err %ld)\n",
               write ? "write":"read", (uint64_t)start, rc);
        return rc;
    }

    return 0;
}

long fat_readwrite(struct fat_filestr *filestr, unsigned long sectorcount,
                   void *buf, bool write)
{
    struct fat_file * const file = filestr->fatfilep;
    struct bpb * const fat_bpb = FAT_BPB(file->volume);
    if (!fat_bpb)
        return -1;

    bool eof = filestr->eof;

    if ((eof && !write) || !sectorcount)
        return 0;

    long rc;

    long          cluster    = filestr->lastcluster;
    sector_t sector     = filestr->lastsector;
    long          clusternum = filestr->clusternum;
    unsigned long sectornum  = filestr->sectornum;

    DEBUGF("%s(file:0x%lx,count:0x%lx,buf:%lx,%s)\n", __func__,
           file->firstcluster, sectorcount, (long)buf,
           write ? "write":"read");
    DEBUGF("%s: sec:%llx numsec:%ld eof:%d\n", __func__,
           (uint64_t)sector, (long)sectornum, eof ? 1 : 0);

    eof = false;

#ifdef BOOTLOADER
    if (write && fat_bpb->is_exfat && file->firstcluster > 0 &&
        (file->firstcluster & EXFAT_BIG))
        return -1;  /* bcm overlay v66: files >= 4 GiB are read-only here */
#endif

    if (!sector)
    {
        /* look up first sector of file */
        long newcluster = exfat_clus(file->firstcluster);

        if (write && !newcluster)
        {
            /* file is empty; try to allocate its first cluster */
            newcluster = next_write_cluster(fat_bpb, 0);
            file->firstcluster = newcluster;
        }

        if (newcluster)
        {
            cluster = newcluster;
            sector = cluster2sec(fat_bpb, cluster) - 1;

        #ifdef HAVE_FAT16SUPPORT
            if (fat_bpb->is_fat16 && file->firstcluster < 0)
            {
                sector += fat_bpb->rootdirsectornum;
                sectornum = fat_bpb->rootdirsectornum;
            }
        #endif /* HAVE_FAT16SUPPORT */
        }
    }

    if (!sector)
    {
        sectorcount = 0;
        eof = true;
    }

    unsigned long transferred = 0;
    unsigned long count = 0;
    sector_t last = sector;

    while (transferred + count < sectorcount)
    {
        if (++sectornum >= fat_bpb->bpb_secperclus)
        {
            /* out of sectors in this cluster; get the next cluster */
            long newcluster;
            if (fat_bpb->is_exfat)
                newcluster = write ? exfat_write_next(fat_bpb, file, cluster) :
                                     exfat_file_next(fat_bpb, file, cluster);
            else
                newcluster = write ? next_write_cluster(fat_bpb, cluster) :
                                     get_next_cluster(fat_bpb, cluster);
            if (newcluster)
            {
                cluster = newcluster;
                sector = cluster2sec(fat_bpb, cluster) - 1;
                clusternum++;
                sectornum = 0;

                /* jumped clusters right at start? */
                if (!count)
                    last = sector;
            }
            else
            {
                sectornum--; /* remain in previous position */
                eof = true;
                break;
            }
        }

        /* find sequential sectors and transfer them all at once */
        if (sector != last || count >= FAT_MAX_TRANSFER_SIZE)
        {
            /* not sequential/over limit */
            rc = transfer(fat_bpb, last - count + 1, count, buf, write);
            if (rc < 0)
                FAT_ERROR(rc * 10 - 2);

            transferred += count;
            buf += count * LOG_SECTOR_SIZE(fat_bpb);
            count = 0;
        }

        count++;
        last = ++sector;
    }

    if (count)
    {
        /* transfer any remainder */
        rc = transfer(fat_bpb, last - count + 1, count, buf, write);
        if (rc < 0)
            FAT_ERROR(rc * 10 - 3);

        transferred += count;
    }

    rc = (eof && write) ? FAT_RC_ENOSPC : (long)transferred;
fat_error:
    filestr->lastcluster = cluster;
    filestr->lastsector  = sector;
    filestr->clusternum  = clusternum;
    filestr->sectornum   = sectornum;
    filestr->eof         = eof;

    if (rc >= 0)
        DEBUGF("Sectors transferred: %ld\n", rc);

    return rc;
}

void fat_rewind(struct fat_filestr *filestr)
{
    /* rewind the file position */
    filestr->lastcluster  = exfat_clus(filestr->fatfilep->firstcluster);
    filestr->lastsector   = 0;
    filestr->clusternum   = 0;
    filestr->sectornum    = FAT_FILE_RW_VAL;
    filestr->eof          = false;
}

void fat_seek_to_stream(struct fat_filestr *filestr,
                        const struct fat_filestr *filestr_seek_to)
{
    filestr->lastcluster = filestr_seek_to->lastcluster;
    filestr->lastsector  = filestr_seek_to->lastsector;
    filestr->clusternum  = filestr_seek_to->clusternum;
    filestr->sectornum   = filestr_seek_to->sectornum;
    filestr->eof         = filestr_seek_to->eof;
}

int fat_seek(struct fat_filestr *filestr, unsigned long seeksector)
{
    const struct fat_file * const file = filestr->fatfilep;
    struct bpb * const fat_bpb = FAT_BPB(file->volume);
    if (!fat_bpb)
        return -1;

    int rc;
    long          cluster    = exfat_clus(file->firstcluster);
    sector_t      sector     = 0;
    long          clusternum = 0;
    unsigned long sectornum  = FAT_FILE_RW_VAL;

#ifdef HAVE_FAT16SUPPORT
    if (fat_bpb->is_fat16 && cluster < 0) /* FAT16 root dir */
        seeksector += fat_bpb->rootdirsectornum;
#endif /* HAVE_FAT16SUPPORT */

    filestr->eof = false;
    if (seeksector)
    {
        if (cluster == 0)
        {
            DEBUGF("Seeking beyond the end of empty file! "
                   "(sector %lu, cluster %ld)\n", seeksector,
                   seeksector / fat_bpb->bpb_secperclus);
            FAT_ERROR(FAT_SEEK_EOF);
        }

        /* we need to find the sector BEFORE the requested, since
           the file struct stores the last accessed sector */
        seeksector--;
        clusternum = seeksector / fat_bpb->bpb_secperclus;
        sectornum = seeksector % fat_bpb->bpb_secperclus;

        long numclusters = clusternum;

        if (filestr->clusternum && clusternum >= filestr->clusternum)
        {
            /* seek forward from current position */
            cluster = filestr->lastcluster;
            numclusters -= filestr->clusternum;
        }

        if (fat_bpb->is_exfat && file->firstcluster > 0 &&
            (file->firstcluster & EXFAT_CONTIG))
        {
            /* bcm overlay v66: a NoFatChain run of known length: direct */
            uint32_t n = exfat_run_length(fat_bpb, file);
            if (n != 0xffffffff && n != EXFAT_RUN_CHAINED)
            {
                if ((unsigned long)clusternum >= n)
                    FAT_ERROR(FAT_SEEK_EOF);
                cluster = exfat_clus(file->firstcluster) + clusternum;
                numclusters = 0;
            }
        }

        for (long i = 0; i < numclusters; i++)
        {
            cluster = fat_bpb->is_exfat ?
                      exfat_file_next(fat_bpb, file, cluster) :
                      get_next_cluster(fat_bpb, cluster);

            if (!cluster)
            {
                DEBUGF("Seeking beyond the end of the file! "
                       "(sector %lu, cluster %ld)\n", seeksector, i);
                FAT_ERROR(FAT_SEEK_EOF);
            }
        }

        sector = cluster2sec(fat_bpb, cluster) + sectornum;
    }

    DEBUGF("%s(%lx, %lx) == %lx, %lx, %lx\n", __func__,
           file->firstcluster, seeksector, cluster, sector, sectornum);

    filestr->lastcluster = cluster;
    filestr->lastsector  = sector;
    filestr->clusternum  = clusternum;
    filestr->sectornum   = sectornum;

    rc = 0;
fat_error:
    return rc;
}

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_truncate)(const struct fat_filestr *filestr)
{
    DEBUGF("%s(): %lX\n", __func__, filestr->lastcluster);

    struct bpb * const fat_bpb = FAT_BPB(filestr->fatfilep->volume);
    if (!fat_bpb)
        return -1;

    int rc = 1;

    if (fat_bpb->is_exfat)
    {
        /* bcm overlay v66 */
        struct fat_file *file = filestr->fatfilep;
#ifdef BOOTLOADER
        if (file->firstcluster > 0 && (file->firstcluster & EXFAT_BIG))
            return -1;
#endif
        if (file->firstcluster > 0 && (file->firstcluster & EXFAT_CONTIG) &&
            exfat_make_chained(fat_bpb, file) < 0)
            return -1;

        /* bcm overlay v67: clusters are about to be cut off the chain; the
           length on disk must not reach into them any more (a crash would
           leave the file pointing at free clusters).  The exact length is
           written at close; this one is the truncation point rounded up to a
           sector. */
        long lastc = filestr->lastcluster;
        if (lastc > 0 && file->dircluster && get_next_cluster(fat_bpb, lastc) > 0)
        {
            /* position 0 (O_TRUNC): the file is empty on disk from now on */
            uint64_t keep = filestr->sectornum == FAT_FILE_RW_VAL ? 0 :
                            ((uint64_t)filestr->clusternum * fat_bpb->bpb_secperclus +
                             filestr->sectornum + 1) * LOG_SECTOR_SIZE(fat_bpb);
            if (exfat_shrink_entry(fat_bpb, file, keep) < 0)
                return -1;
        }
    }

    long last = filestr->lastcluster;
    long next = 0;

    /* truncate trailing clusters after the current position */
    if (last)
    {
        next = get_next_cluster(fat_bpb, last);
        int rc2 = update_fat_entry(fat_bpb, last, FAT_EOF_MARK);
        if (rc2 < 0)
            FAT_ERROR(rc2 * 10 - 2);

    }

    int rc2 = free_cluster_chain(fat_bpb, next);
    if (rc2 <= 0)
    {
        DEBUGF("Failed freeing cluster chain\n");
        rc = 0; /* still partial success */
    }

fat_error:
    return rc;
}


/** Directory stream functions **/

EXFAT_LOCKED_SC int EXFAT_LOCKED(fat_readdir)(struct fat_filestr *dirstr, struct fat_dirscan_info *scan,
                struct filestr_cache *cachep, struct fat_direntry *entry)
{
    int rc = 0;

    {
        /* bcm overlay v66 */
        struct bpb *xbpb = FAT_BPB(dirstr->fatfilep->volume);
        if (xbpb && xbpb->is_exfat)
            return exfat_readdir(xbpb, dirstr, scan, cachep, entry);
    }

    /* long file names are stored in special entries; each entry holds up to
       13 UTF-16 characters' thus, UTF-8 converted names can be max 255 chars
       (1020 bytes) long, not including the trailing '\0'. */
    struct fatlong_parse_state lnparse;
    fatlong_parse_start(&lnparse);

    scan->entries = 0;

#if defined(MAX_VIRT_SECTOR_SIZE) || defined(MAX_VARIABLE_LOG_SECTOR)
    struct bpb *fat_bpb = FAT_BPB(dirstr->fatfilep->volume);
#endif

    while (1)
    {
        unsigned int direntry = ++scan->entry;
        if (direntry >= MAX_DIRENTRIES)
        {
            DEBUGF("%s() - Dir is too large (entry %u)\n", __func__,
                   direntry);
            FAT_ERROR(-1);
        }

        unsigned long sector = direntry / DIR_ENTRIES_PER_SECTOR;
        if (cachep->sector != sector)
        {
            if (cachep->sector + 1 != sector)
            {
                /* Nothing cached or sector isn't contiguous */
                int rc2 = fat_seek(dirstr, sector);
                if (rc2 < 0)
                    FAT_ERROR(rc2 * 10 - 2);
            }

            int rc2 = fat_readwrite(dirstr, 1, cachep->buffer, false);
            if (rc2 <= 0)
            {
                if (rc2 == 0)
                    break; /* eof */

                DEBUGF("%s() - Couldn't read dir (err %d)\n", __func__, rc);
                FAT_ERROR(rc2 * 10 - 3);
            }

            cachep->sector = sector;
        }

        unsigned int index = direntry % DIR_ENTRIES_PER_SECTOR;
        union raw_dirent *ent = &((union raw_dirent *)cachep->buffer)[index];

        if (ent->name[0] == 0)
            break;    /* last entry */

        if (ent->name[0] == 0xe5)
        {
            scan->entries = 0;
            continue; /* free entry */
        }

        ++scan->entries;

        if (IS_LDIR_ATTR(ent->ldir_attr))
        {
            /* LFN entry */
            if (UNLIKELY(!fatlong_parse_entry(&lnparse, ent, entry)))
            {
                /* resync so we don't return just the short name if what we
                   landed in the middle of is valid (directory changes
                   between calls likely created the situation; ignoring this
                   case can be harmful elsewhere and is destructive to the
                   entry series itself) */
                struct bpb *fat_bpb = FAT_BPB(dirstr->fatfilep->volume);
                if (!fat_bpb)
                    FAT_ERROR(-4);

                dc_lock_cache();

                while (--scan->entry != FAT_DIRSCAN_RW_VAL) /* at beginning? */
                {
                    ent = cache_direntry(fat_bpb, dirstr, scan->entry);

                    /* name[0] == 0 shouldn't happen here but... */
                    if (!ent || ent->name[0] == 0 || ent->name[0] == 0xe5 ||
                        !IS_LDIR_ATTR(ent->ldir_attr))
                        break;
                }

                dc_unlock_cache();

                /* retry it once from the new position */
                scan->entries = 0;
                continue;
            }
        }
        else if (!IS_VOL_ID_ATTR(ent->attr)) /* ignore volume id entry */
        {
            rc = 1;

            if (!fatlong_parse_finish(&lnparse, ent, entry))
            {
                /* the long entries failed to pass all checks or there is
                   just a short entry. */
                DEBUGF("SN-DOS:'%s'", entry->shortname);
                strcpy(entry->name, entry->shortname);
                scan->entries = 1;
                rc = 2; /* name is OEM */
            }

            DEBUGF("LN:\"%s\"", entry->name);
            break;
        }
    } /* end while */

fat_error:
    if (rc <= 0)
    {
        /* error or eod; stay on last good position */
        fat_empty_fat_direntry(entry);
        scan->entry--;
        scan->entries = 0;
    }

    return rc;
}

void fat_rewinddir(struct fat_dirscan_info *scan)
{
    /* rewind the directory scan counter to the beginning */
    scan->entry   = FAT_DIRSCAN_RW_VAL;
    scan->entries = 0;
}

/** Mounting and unmounting functions **/

bool fat_ismounted(IF_MV_NONVOID(int volume))
{
    return !!FAT_BPB(volume);
}

int fat_mount(IF_MV(int volume,) IF_MD(int drive,) sector_t startsector)
{
    int rc;

    struct bpb * const fat_bpb = &fat_bpbs[IF_MV_VOL(volume)];
    if (fat_bpb->mounted)
        FAT_ERROR(-1); /* double mount */

    /* fill-in basic info first */
    fat_bpb->startsector = startsector;
#ifdef HAVE_MULTIVOLUME
    fat_bpb->volume      = volume;
#endif
#ifdef HAVE_MULTIDRIVE
    fat_bpb->drive       = drive;
#endif
#if defined(MAX_VIRT_SECTOR_SIZE) || defined(MAX_VARIABLE_LOG_SECTOR)
    fat_bpb->sector_size = disk_get_log_sector_size(IF_MD(drive));
#endif

    rc = fat_mount_internal(fat_bpb);
    if (rc < 0)
        FAT_ERROR(rc * 10 - 2);

    /* it worked */
    fat_bpb->mounted = true;

    /* calculate freecount if unset */
    if (fat_bpb->fsinfo.freecount == 0xffffffff)
        fat_recalc_free(IF_MV(fat_bpb->volume));

    DEBUGF("Freecount: %ld\n", fat_bpb->fsinfo.freecount);
    DEBUGF("Nextfree: 0x%lx\n", fat_bpb->fsinfo.nextfree);
    DEBUGF("Cluster count: 0x%lx\n", fat_bpb->dataclusters);
    DEBUGF("Sectors per cluster: %lu\n", fat_bpb->bpb_secperclus);
    DEBUGF("FAT sectors: 0x%lx\n", fat_bpb->fatsize);

    rc = 0;
fat_error:
    return rc;
}

int fat_unmount(IF_MV_NONVOID(int volume))
{
    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (!fat_bpb)
        return -1; /* not mounted */

    if (fat_bpb->is_exfat)
        exfat_mark_clean(fat_bpb);

    /* free the entries for this volume */
    cache_discard(IF_MV(fat_bpb));
    if (fat_bpb->is_exfat)
        exfat_runs_forget(fat_bpb);
    fat_bpb->mounted = false;

    return 0;
}


/* bcm overlay v67: called on a normal shutdown (with the file system
   writer lock held): exFAT volumes changed in this session are marked clean */
void fat_mark_clean(void)
{
    for (unsigned int i = 0; i < NUM_VOLUMES; i++)
    {
        struct bpb *fat_bpb = &fat_bpbs[i];
        if (fat_bpb->mounted && fat_bpb->is_exfat)
            exfat_mark_clean(fat_bpb);
    }
}

/** Debug screen stuff **/

#if defined(MAX_VIRT_SECTOR_SIZE) || defined(MAX_VARIABLE_LOG_SECTOR)
/* This isn't necessarily the same as storage's logical sector size;
   we can have situations where the filesystem (and partition table)
   uses a larger "virtual sector" than the underlying storage device */
int fat_get_bytes_per_sector(IF_MV_NONVOID(int volume))
{
    int bytes = 0;

    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (fat_bpb)
        bytes = fat_bpb->bpb_bytspersec;

    return bytes;
}
#endif /* MAX_VIRT_SECTOR_SIZE || MAX_VARIAGLE_LOG_SECTOR */

unsigned int fat_get_cluster_size(IF_MV_NONVOID(int volume))
{
    unsigned int size = 0;

    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (fat_bpb)
        size = fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb);

    return size;
}

void fat_recalc_free(IF_MV_NONVOID(int volume))
{
    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (!fat_bpb)
        return;

    dc_lock_cache();
    fat_recalc_free_internal(fat_bpb);
    dc_unlock_cache();
}

bool fat_size(IF_MV(int volume,) sector_t *size, sector_t *free)
{
    struct bpb * const fat_bpb = FAT_BPB(volume);
    if (!fat_bpb)
        return false;

    unsigned long factor = fat_bpb->bpb_secperclus * LOG_SECTOR_SIZE(fat_bpb) / 1024;

    if (fat_bpb->is_exfat && fat_bpb->fsinfo.freecount >= 0xfffffffe)
    {
        /* bcm overlay v67: counted on first use (see exFAT mount) */
        dc_lock_cache();
        fat_recalc_free_internal(fat_bpb);
        dc_unlock_cache();
    }

    /* bcm overlay v72: 64-bit products (KiB) -- volumes beyond 4 TiB */
    if (size) *size = (sector_t)fat_bpb->dataclusters * factor;
    if (free) *free = (sector_t)fat_bpb->fsinfo.freecount * factor;

    return true;
}


/** Misc. **/

void fat_empty_fat_direntry(struct fat_direntry *entry)
{
    entry->name[0]      = 0;
    entry->shortname[0] = 0;
    entry->attr         = 0;
    entry->crttimetenth = 0;
    entry->crttime      = 0;
    entry->crtdate      = 0;
    entry->lstaccdate   = 0;
    entry->wrttime      = 0;
    entry->wrtdate      = 0;
    entry->filesize     = 0;
    entry->firstcluster = 0;
}

#ifndef BOOTLOADER
/* bcm overlay v91: the entry points that reach the exFAT scratch buffers */
static inline void exfat_scratch_lock(void)   { mutex_lock(&exfat_scratch_mtx); }
static inline void exfat_scratch_unlock(void) { mutex_unlock(&exfat_scratch_mtx); }

int fat_create_file(struct fat_file *parent, const char *name,
                    uint8_t attr, struct fat_file *file,
                    struct fat_direntry *fatent)
{
    exfat_scratch_lock();
    int rc = fat_create_file_unlocked(parent, name, attr, file, fatent);
    exfat_scratch_unlock();
    return rc;
}

int fat_rename(struct fat_file *parent, struct fat_file *file,
               const unsigned char *newname)
{
    exfat_scratch_lock();
    int rc = fat_rename_unlocked(parent, file, newname);
    exfat_scratch_unlock();
    return rc;
}

int fat_modtime(struct fat_file *parent, struct fat_file *file,
                time_t modtime)
{
    exfat_scratch_lock();
    int rc = fat_modtime_unlocked(parent, file, modtime);
    exfat_scratch_unlock();
    return rc;
}

int fat_closewrite(struct fat_filestr *filestr, fat_size_t size,
                   struct fat_direntry *fatentp)
{
    exfat_scratch_lock();
    int rc = fat_closewrite_unlocked(filestr, size, fatentp);
    exfat_scratch_unlock();
    return rc;
}

int fat_truncate(const struct fat_filestr *filestr)
{
    exfat_scratch_lock();
    int rc = fat_truncate_unlocked(filestr);
    exfat_scratch_unlock();
    return rc;
}

int fat_readdir(struct fat_filestr *dirstr, struct fat_dirscan_info *scan,
                struct filestr_cache *cachep, struct fat_direntry *entry)
{
    exfat_scratch_lock();
    int rc = fat_readdir_unlocked(dirstr, scan, cachep, entry);
    exfat_scratch_unlock();
    return rc;
}
#endif /* !BOOTLOADER */

void fat_init(void)
{
#ifndef BOOTLOADER
    mutex_init(&exfat_scratch_mtx);
#endif
    dc_lock_cache();

    /* mark the possible volumes as not mounted */
    for (unsigned int i = 0; i < NUM_VOLUMES; i++)
    {
        dc_discard_all(IF_MV(i));
        fat_bpbs[i].mounted = false;
    }

    dc_unlock_cache();
}
