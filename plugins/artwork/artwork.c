/*
    Album Art plugin for DeaDBeeF
    Copyright (C) 2009-2017 Alexey Yakovenko <waker@users.sourceforge.net>
    Copyright (C) 2009-2011 Viktor Semykin <thesame.ml@gmail.com>
    Copyright (C) 2014-2016 Ian Nartowicz <deadbeef@nartowicz.co.uk>

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.

    3. This notice may not be removed or altered from any source distribution.
*/

#ifdef HAVE_CONFIG_H
    #include "../../config.h"
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fnmatch.h>
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if HAVE_SYS_CDEFS_H
#include <sys/cdefs.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include "../../deadbeef.h"
#include "artwork_flac.h"
#include "albumartorg.h"
#include "artwork.h"
#include "artwork_internal.h"
#include "cache.h"
#include "coverinfo.h"
#include "lastfm.h"
#include "musicbrainz.h"
#include "mp4tagutil.h"
#include "../../strdupa.h"
#include "wos.h"

#ifndef DISPATCH_QUEUE_CONCURRENT
#define DISPATCH_QUEUE_CONCURRENT NULL
#endif

#define ENABLE_MUSICBRAINZ 0
#define ENABLE_ALBUMART_ORG 0

#define trace(...) { deadbeef->log_detailed (&plugin.plugin.plugin, 0, __VA_ARGS__); }

DB_functions_t *deadbeef;
ddb_artwork_plugin_t plugin;

#define MAX_LISTENERS 100
static ddb_artwork_listener_t listeners[MAX_LISTENERS];
static void *listeners_userdata[MAX_LISTENERS];

dispatch_queue_t sync_queue; // used in artwork_internal, therefore not static
static dispatch_queue_t process_queue;
static dispatch_queue_t fetch_queue;
static dispatch_semaphore_t fetch_semaphore;

static int64_t last_job_idx;
static int64_t cancellation_idx;

#define MAX_COVERS_IN_CACHE 20
static ddb_cover_info_t *cover_cache[MAX_COVERS_IN_CACHE];

#ifdef ANDROID
#define DEFAULT_DISABLE_CACHE 1
#define DEFAULT_SAVE_TO_MUSIC_FOLDERS 1
static int artwork_disable_cache = DEFAULT_DISABLE_CACHE;
static int artwork_save_to_music_folders = DEFAULT_SAVE_TO_MUSIC_FOLDERS;
#else
#define DEFAULT_DISABLE_CACHE 0
#define DEFAULT_SAVE_TO_MUSIC_FOLDERS 0
int artwork_disable_cache = DEFAULT_DISABLE_CACHE;
int artwork_save_to_music_folders = DEFAULT_SAVE_TO_MUSIC_FOLDERS;
#endif

static int artwork_enable_embedded;
static int artwork_enable_local;
#ifdef USE_VFS_CURL
    static int artwork_enable_lfm;
#if ENABLE_MUSICBRAINZ
    static int artwork_enable_mb;
#endif
#if ENABLE_ALBUMART_ORG
    static int artwork_enable_aao;
#endif
    static int artwork_enable_wos;
#endif
static int missing_artwork;
static char *nocover_path;

static time_t cache_reset_time;
static time_t default_reset_time;

#define DEFAULT_FILEMASK "front.png;front.jpg;front.jpeg;folder.png;folder.jpg;folder.jpeg;cover.png;cover.jpg;cover.jpeg;f.png;f.jpg;f.jpeg;*front*.png;*front*.jpg;*front*.jpeg;*cover*.png;*cover*.jpg;*cover.jpeg;*folder*.png;*folder*.jpg;*folder*.jpeg;*.png;*.jpg;*.jpeg"
#define DEFAULT_FOLDERS "art;scans;covers;artwork;artworks"

static char *artwork_filemask;
static char *artwork_folders;

static char *album_tf;
static char *artist_tf;
static char *title_tf;
static char *query_compare_tf;

// esc_char is needed to prevent using file path separators,
// e.g. to avoid writing arbitrary files using "../../../filename"
static char
esc_char (char c) {
#ifndef WIN32
    if (c == '/') {
        return '\\';
    }
#else
    if (c == '\\') {
        return '_';
    }
#endif
    return c;
}

static int
make_cache_dir_path (const char *artist, char *outpath, int outsize) {
    char esc_artist[NAME_MAX+1];
    if (artist) {
        size_t i = 0;
        while (artist[i] && i < NAME_MAX) {
            esc_artist[i] = esc_char (artist[i]);
            i++;
        }
        esc_artist[i] = '\0';
    }
    else {
        strcpy (esc_artist, "Unknown artist");
    }

    if (make_cache_root_path (outpath, outsize) < 0) {
        return -1;
    }

    const size_t size_left = outsize - strlen (outpath);
    int path_length;
    path_length = snprintf (outpath+strlen (outpath), size_left, "covers2/%s/", esc_artist);
    if (path_length >= size_left) {
        trace ("Cache path truncated at %d bytes\n", (int)size_left);
        return -1;
    }

    return 0;
}

static int
make_cache_path (const char *filepath, const char *album, const char *artist, char *outpath, int outsize) {
    outpath[0] = '\0';

    if (!album || !*album) {
        if (filepath) {
            album = filepath;
        }
        else if (artist && *artist) {
            album = artist;
        }
        else {
            trace ("not possible to get any unique album name\n");
            return -1;
        }
    }
    if (!artist || !*artist) {
        artist = "Unknown artist";
    }

    if (make_cache_dir_path (artist, outpath, outsize-NAME_MAX)) {
        return -1;
    }

    int name_size = outsize - (int)strlen (outpath);
    int max_album_chars = min (NAME_MAX, name_size) - (int)sizeof ("1.jpg.part");
    if (max_album_chars <= 0) {
        trace ("Path buffer not long enough for %s and filename\n", outpath);
        return -1;
    }

    char esc_album[max_album_chars+1];
    const char *palbum = strlen (album) > max_album_chars ? album+strlen (album)-max_album_chars : album;
    size_t i = 0;
    do {
        esc_album[i] = esc_char (palbum[i]);
    } while (palbum[i++]);

    sprintf (outpath+strlen (outpath), "%s%s", esc_album, ".jpg");
    return 0;
}

static int
strings_equal (const char *s1, const char *s2)
{
    return (s1 == s2) || (s1 && s2 && !strcasecmp (s1, s2));
}

static char *filter_custom_mask = NULL;

static int
filter_custom (const struct dirent *f)
{
// FNM_CASEFOLD is not defined on solaris. On other platforms it is.
// It should be safe to define it as FNM_INGORECASE if it isn't defined.
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD FNM_IGNORECASE
#endif
    return !fnmatch (filter_custom_mask, f->d_name, FNM_CASEFOLD);
}

static int
vfs_scan_results (struct dirent *entry, const char *container_uri, ddb_cover_info_t *cover)
{
    /* VFS container, double check the match in case scandir didn't implement filtering */
    if (filter_custom (entry)) {
        trace ("found cover %s in %s\n", entry->d_name, container_uri);
        size_t len = strlen (container_uri) + strlen(entry->d_name) + 2;
        cover->image_filename = malloc (len);
        snprintf (cover->image_filename, len, "%s:%s", container_uri, entry->d_name);
        return 0;
    }

    return -1;
}

static int
dir_scan_results (struct dirent **files, int files_count, const char *container, ddb_cover_info_t *cover)
{
    /* Local file in a directory */
    for (size_t i = 0; i < files_count; i++) {
        trace ("found cover %s in local folder\n", files[0]->d_name);
        size_t len = strlen (container) + strlen(files[i]->d_name) + 2;
        cover->image_filename = malloc (len);
        snprintf (cover->image_filename, len, "%s/%s", container, files[i]->d_name);
        struct stat stat_struct;
        if (!stat (cover->image_filename, &stat_struct) && S_ISREG (stat_struct.st_mode) && stat_struct.st_size > 0) {
            return 0;
        }
        else {
            free (cover->image_filename);
            cover->image_filename = NULL;
        }
    }

    return -1;
}

static int
scan_local_path (char *mask, const char *local_path, const char *uri, DB_vfs_t *vfsplug, ddb_cover_info_t *cover)
{
    filter_custom_mask = mask;
    struct dirent **files = NULL;
    int (* custom_scandir)(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
    custom_scandir = vfsplug ? vfsplug->scandir : scandir;
    int files_count = custom_scandir (local_path, &files, filter_custom, NULL);
    if (files_count > 0) {
        int err = -1;
        if (uri) {
            err = vfs_scan_results (files[0], uri, cover);
        }
        else {
            err = dir_scan_results (files, files_count, local_path, cover);
        }

        for (size_t i = 0; i < files_count; i++) {
            free (files[i]);
        }
        free (files);

        return err;
    }

    free (files);
    return -1;
}

static const char *filter_strcasecmp_name = NULL;

static int
filter_strcasecmp (const struct dirent *f)
{
    return !strcasecmp (filter_strcasecmp_name, f->d_name);
}

// FIXME: this returns only one path that matches subfolder. Usually that's enough, but can be improved.
static char *
get_case_insensitive_path (const char *local_path, const char *subfolder, DB_vfs_t *vfsplug) {
    filter_strcasecmp_name = subfolder;
    struct dirent **files = NULL;
    int (* custom_scandir)(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
    custom_scandir = vfsplug ? vfsplug->scandir : scandir;
    int files_count = custom_scandir (local_path, &files, filter_strcasecmp, NULL);
    if (files_count > 0) {
        size_t l = strlen (local_path) + strlen (files[0]->d_name) + 2;
        char *ret = malloc (l);
        snprintf (ret, l, "%s/%s", local_path, files[0]->d_name);

        for (size_t i = 0; i < files_count; i++) {
            free (files[i]);
        }
        free (files);

        return ret;
    }
    free (files);
    return NULL;
}

static int
local_image_file (const char *local_path, const char *uri, DB_vfs_t *vfsplug, ddb_cover_info_t *cover)
{
    if (!artwork_filemask) {
        return -1;
    }

    char *p;

    char *filemask = strdup (artwork_filemask);
    strcpy (filemask, artwork_filemask);
    const char *filemask_end = filemask + strlen (filemask);
    while ((p = strrchr (filemask, ';'))) {
        *p = '\0';
    }

    char *folders = strdup (artwork_folders);
    strcpy (folders, artwork_folders);
    const char *folders_end = folders + strlen (folders);
    while ((p = strrchr (folders, ';'))) {
        *p = '\0';
    }

    int root = 1;
    char *folder = folders;
    while (folder < folders_end) {
        char *path;
        if (root) {
            root = 0;
            path = strdup (local_path);
        }
        else {
            path = get_case_insensitive_path (local_path, folder, vfsplug);
            folder += strlen (folder)+1;
        }
        trace ("scanning %s for artwork\n", path);
        for (char *mask = filemask; mask < filemask_end; mask += strlen (mask)+1) {
            if (mask[0] && path && !scan_local_path (mask, path, uri, vfsplug, cover)) {
                free (filemask);
                free (folders);
                free (path);
                return 0;
            }
        }
        free (path);
    }

    trace ("No cover art files in local folder\n");
    free (filemask);
    free (folders);
    return -1;
}

static const uint8_t *
id3v2_skip_str (int enc, const uint8_t *ptr, const uint8_t *end) {
    if (enc == 0 || enc == 3) {
        while (ptr < end && *ptr) {
            ptr++;
        }
        ptr++;
        return ptr < end ? ptr : NULL;
    }
    else {
        while (ptr < end-1 && (ptr[0] || ptr[1])) {
            ptr += 2;
        }
        ptr += 2;
        return ptr < end ? ptr : NULL;
    }
}

static const uint8_t *
id3v2_artwork (const DB_id3v2_frame_t *f, int minor_version, int type)
{
    if ((minor_version > 2 && strcmp (f->id, "APIC")) || (minor_version == 2 && strcmp (f->id, "PIC"))) {
        return NULL;
    }

    if (f->size < 20) {
        trace ("artwork: id3v2 APIC frame is too small\n");
        return NULL;
    }

    const uint8_t *data = f->data;

    if (minor_version == 4 && (f->flags[1] & 1)) {
        data += 4;
    }
    const uint8_t *end = f->data + f->size;
    int enc = *data;
    data++;

    if (minor_version > 2) {
        // mime type is ascii always, the enc above is for the picture type
        const uint8_t *mime_end = id3v2_skip_str (0, data, end);
        if (!mime_end) {
            trace ("artwork: corrupted id3v2 APIC frame\n");
            return NULL;
        }
        if (type == -1 || *mime_end == type) {
            trace ("artwork: picture type=%d\n", *mime_end);
            return NULL;
        }
        trace ("artwork: mime-type=%s, picture type: %d\n", data, *mime_end);
        data = mime_end;
    }
    else {
        data += 3; // image format
    }
    data++; // picture type
    data = id3v2_skip_str (enc, data, end); // description
    if (!data) {
        trace ("artwork: corrupted id3v2 APIC frame\n");
        return NULL;
    }

    return data;
}

static const uint8_t *
apev2_artwork (const DB_apev2_frame_t *f)
{
    if (strcasecmp (f->key, "cover art (front)")) {
        return NULL;
    }

    const uint8_t *data = f->data;
    const uint8_t *end = f->data + f->size;
    while (data < end && *data)
        data++;
    if (data == end) {
        trace ("artwork: apev2 cover art frame has no name\n");
        return NULL;
    }

    size_t sz = end - ++data;
    if (sz < 20) {
        trace ("artwork: apev2 cover art frame is too small\n");
        return NULL;
    }

//    uint8_t *ext = strrchr (f->data, '.');
//    if (!ext || !*++ext) {
//        trace ("artwork: apev2 cover art name has no extension\n");
//        return NULL;
//    }

//    if (strcasecmp (ext, "jpeg") &&
//        strcasecmp (ext, "jpg") &&
#ifdef USE_IMLIB2
//        strcasecmp (ext, "gif") &&
//        strcasecmp (ext, "tif") &&
//        strcasecmp (ext, "tiff") &&
#endif
//        strcasecmp (ext, "png")) {
//        trace ("artwork: unsupported file type: %s\n", ext);
//        return NULL;
//    }

    return data;
}



static int
id3_extract_art (const char *outname, ddb_cover_info_t *cover) {
    int err = -1;

    DB_id3v2_tag_t id3v2_tag;
    memset (&id3v2_tag, 0, sizeof (id3v2_tag));
    DB_FILE *id3v2_fp = deadbeef->fopen (cover->filepath);
    if (id3v2_fp && !deadbeef->junk_id3v2_read_full (NULL, &id3v2_tag, id3v2_fp)) {
        int minor_version = id3v2_tag.version[0];
        DB_id3v2_frame_t *fprev = NULL;
        for (DB_id3v2_frame_t *f = id3v2_tag.frames; f; f = f->next) {
            const uint8_t *image_data = id3v2_artwork (f, minor_version, 3);
            if (!image_data) {
                // try any cover if front is not available
                image_data = id3v2_artwork (f, minor_version, 0);
            }
            if (image_data) {
                const size_t sz = f->size - (image_data - f->data);
                if (sz <= 0) {
                    continue;
                }
                if (!artwork_disable_cache) {
                    trace ("will write id3v2 APIC (%d bytes) into %s\n", (int)sz, outname);
                    struct stat stat_struct;
                    if (!stat (outname, &stat_struct) && S_ISREG (stat_struct.st_mode) && stat_struct.st_size > 0) {
                        cover->image_filename = strdup (outname);
                        err = 0;
                        break;
                    }
                    else if (!write_file (outname, (const char *)image_data, sz)) {
                        cover->image_filename = strdup(outname);
                        err = 0;
                        break;
                    }
                }
                else {
                    // steal the frame memory from DB_id3v2_tag_t
                    if (fprev) {
                        fprev->next = f->next;
                    }
                    else {
                        id3v2_tag.frames = f->next;
                    }

                    cover->blob = (char *)f;
                    cover->blob_size = f->size;
                    cover->blob_image_offset = (uint64_t)((char *)image_data - (char *)cover->blob);
                    cover->blob_image_size = sz;
                    err = 0;
                    break;
                }
            }
            fprev = f;
        }
    }
    deadbeef->junk_id3v2_free (&id3v2_tag);
    if (id3v2_fp) {
        deadbeef->fclose (id3v2_fp);
    }
    return err;
}

static int
apev2_extract_art (const char *outname, ddb_cover_info_t *cover) {
    int err = -1;
    DB_apev2_tag_t apev2_tag;
    memset (&apev2_tag, 0, sizeof (apev2_tag));
    DB_FILE *apev2_fp = deadbeef->fopen (cover->filepath);
    if (apev2_fp && !deadbeef->junk_apev2_read_full (NULL, &apev2_tag, apev2_fp)) {
        DB_apev2_frame_t *fprev = NULL;
        for (DB_apev2_frame_t *f = apev2_tag.frames; f; f = f->next) {
            const uint8_t *image_data = apev2_artwork (f);
            if (image_data) {
                const size_t sz = f->size - (image_data - f->data);
                if (sz <= 0) {
                    continue;
                }
                trace ("will write apev2 cover art (%d bytes) into %s\n", sz, outname);
                if (!artwork_disable_cache) {
                    struct stat stat_struct;
                    if (!stat (outname, &stat_struct) && S_ISREG (stat_struct.st_mode) && stat_struct.st_size > 0) {
                        cover->image_filename = strdup (outname);
                        err = 0;
                        break;
                    }
                    else if (!write_file (outname, (const char *)image_data, sz)) {
                        cover->image_filename = strdup (outname);
                        err = 0;
                        break;
                    }
                }
                else {
                    // steal the frame memory from DB_id3v2_tag_t
                    if (fprev) {
                        fprev->next = f->next;
                    }
                    else {
                        apev2_tag.frames = f->next;
                    }

                    cover->blob = (char *)f;
                    cover->blob_size = f->size;
                    cover->blob_image_offset = (uint64_t)((char *)image_data - (char *)cover->blob);
                    cover->blob_image_size = sz;
                    err = 0;
                    break;
                }
            }
            fprev = f;
        }

    }
    deadbeef->junk_apev2_free (&apev2_tag);
    if (apev2_fp) {
        deadbeef->fclose (apev2_fp);
    }
    return err;
}

static int
mp4_extract_art (const char *outname, ddb_cover_info_t *cover) {
    int ret = -1;
    mp4p_atom_t *mp4file = NULL;
    DB_FILE* fp = NULL;
    uint8_t* image_blob = NULL;

    if (!strcasestr (cover->filepath, ".mp4") && !strcasestr (cover->filepath, ".m4a") && !strcasestr (cover->filepath, ".m4b")) {
        return -1;
    }

    fp = deadbeef->fopen (cover->filepath);
    if (!fp) {
        goto error;
    }

    mp4p_file_callbacks_t callbacks = {0};
    callbacks.ptrhandle = fp;
    mp4_init_ddb_file_callbacks (&callbacks);
    mp4file = mp4p_open(&callbacks);
    if (!mp4file) {
        goto error;
    }

    mp4p_atom_t *covr = mp4_get_cover_atom(mp4file);
    if (!covr) {
        goto error;
    }

    mp4p_ilst_meta_t *data = covr->data;

    uint32_t sz = data->data_size;
    image_blob = malloc (sz);
    if (data->blob) {
        image_blob = memcpy (image_blob, data->blob, sz);
    }
    else if (data->values) {
        uint16_t *v = data->values;
        for (size_t i = 0; i < sz/2; i++, v++) {
            image_blob[i*2+0] = (*v) >> 8;
            image_blob[i*2+1] = (*v) & 0xff;
        }
    }
    else {
        goto error;
    }

    trace ("will write mp4 cover art (%u bytes) into %s\n", sz, outname);
    if (!artwork_disable_cache) {
        struct stat stat_struct;
        if (!stat (outname, &stat_struct) && S_ISREG (stat_struct.st_mode) && stat_struct.st_size > 0) {
            ret = 0;
            cover->image_filename = strdup (outname);
        }
        else if (!write_file (outname, (char *)image_blob, sz)) {
            ret = 0;
            cover->image_filename = strdup (outname);
        }
        else {
            trace ("Failed to write mp4 cover to file %s\n");
            goto error;
        }
        free (image_blob);
        image_blob = NULL;
    }
    else {
        cover->blob = (char *)image_blob;
        image_blob = NULL;
        cover->blob_size = data->data_size;
        cover->blob_image_size = data->data_size;
        ret = 0;
    }

error:
    if (image_blob) {
        free(image_blob);
        image_blob = NULL;
    }
    if (mp4file) {
        mp4p_atom_free_list (mp4file);
        mp4file = NULL;
    }
    if (fp) {
        deadbeef->fclose (fp);
        fp = NULL;
    }
    return ret;
}

static int
web_lookups (const char *cache_path, ddb_cover_info_t *cover) {
    if (!cache_path) {
        return 0;
    }
#if USE_VFS_CURL
    if (artwork_enable_lfm) {
        if (!fetch_from_lastfm (cover->artist, cover->album, cache_path)) {
            cover->image_filename = strdup (cache_path);
            return 1;
        }
        if (errno == ECONNABORTED) {
            return 0;
        }
    }
#if ENABLE_MUSICBRAINZ
    // Albumart.org and Musicbrainz have either changed their APIs, or are broken in general -- therefore disabled.
    if (artwork_enable_mb) {
        if (!fetch_from_musicbrainz (cover->artist, cover->album, cache_path)) {
            cover->image_filename = strdup (cache_path);
            return 1;
        }
        if (errno == ECONNABORTED) {
            return 0;
        }
    }
#endif

#if ENABLE_ALBUMART_ORG
    if (artwork_enable_aao) {
        if (!fetch_from_albumart_org (cover->artist, cover->album, cache_path)) {
            cover->image_filename = strdup (cache_path);
            return 1;
        }
        if (errno == ECONNABORTED) {
            return 0;
        }
    }
#endif
#endif

    return -1;
}

static char *
vfs_path (const char *fname)
{
    if (fname[0] == '/' || strstr (fname, "file://") == fname) {
        return NULL;
    }

    char *p = strstr (fname, "://");
    if (p) {
        p += 3;
        char *q = strrchr (p, ':');
        if (q) {
            *q = '\0';
        }
    }
    return p;
}

static DB_vfs_t *
scandir_plug (const char *vfs_fname)
{
    DB_vfs_t **vfsplugs = deadbeef->plug_get_vfs_list ();
    for (size_t i = 0; vfsplugs[i]; i++) {
        if (vfsplugs[i]->is_container && vfsplugs[i]->is_container (vfs_fname) && vfsplugs[i]->scandir) {
            return vfsplugs[i];
        }
    }

    return NULL;
}

// might be used from process_query, when cache impl is finalized
#if 0
static int
path_more_recent (const char *fname, const time_t placeholder_mtime)
{
    struct stat stat_buf;
    return !stat (fname, &stat_buf) && stat_buf.st_mtime > placeholder_mtime;
}

// returns 1 if enough time passed since the last attemp to find the requested cover
static int
recheck_missing_artwork (const char *input_fname, const time_t placeholder_mtime)
{
    int res = 0;
    char *fname = strdup (input_fname);
    /* Check if local files could have new associated artwork */
    if (deadbeef->is_local_file (fname)) {
        char *vfs_fname = vfs_path (fname);
        const char *real_fname = vfs_fname ? vfs_fname : fname;

        /* Recheck artwork if file (track or VFS container) was modified since the last check */
        if (path_more_recent (real_fname, placeholder_mtime)) {
            return 1;
        }

        /* Recheck local artwork if the directory contents have changed */
        char *dname = strdup (dirname (fname));
        res = artwork_enable_local && path_more_recent (dname, placeholder_mtime);
        free (dname);
    }

    free (fname);
    return res;
}
#endif

// Behavior:
// Local cover: path is returned
// Found in cache: path is returned
// Embedded cover: !cache_disabled ? save_to_cash&return_path : return blob
// Web cover: save_to_local ? save_to_local&return_path : ( !cache_disabled ? save_to_cache&return_path : NOP )
static void
process_query (ddb_cover_info_t *cover) {
    char cache_path_buf[PATH_MAX];
    char *cache_path = NULL;

    if (!artwork_disable_cache) {
        cache_path = cache_path_buf;
        make_cache_path (cover->filepath, cover->album, cover->artist, cache_path, sizeof (cache_path_buf));
    }

    int islocal = deadbeef->is_local_file (cover->filepath);

    if (artwork_enable_local && islocal) {
        char *fname_copy = strdup (cover->filepath);
        if (fname_copy) {
            char *vfs_fname = vfs_path (fname_copy);
            if (vfs_fname) {
                /* Search inside scannable VFS containers */
                DB_vfs_t *plugin = scandir_plug (vfs_fname);
                if (plugin && !local_image_file (vfs_fname, fname_copy, plugin, cover)) {
                    free (fname_copy);
                    cover->cover_found = 1;
                    return;
                }
            }

            /* Search in file directory */
            if (!local_image_file (dirname (vfs_fname ? vfs_fname : fname_copy), NULL, NULL, cover)) {
                free (fname_copy);
                cover->cover_found = 1;
                return;
            }

            free (fname_copy);
        }
    }

    if (artwork_enable_embedded && islocal) {
#ifdef USE_METAFLAC
        // try to load embedded from flac metadata
        trace ("trying to load artwork from Flac tag for %s\n", cover->filepath);
        if (!flac_extract_art (cache_path, cover)) {
            cover->cover_found = 1;
            return;
        }
#endif

        // try to load embedded from id3v2
        trace ("trying to load artwork from id3v2 tag for %s\n", cover->filepath);
        if (!id3_extract_art (cache_path, cover)) {
            cover->cover_found = 1;
            return;
        }

        // try to load embedded from apev2
        trace ("trying to load artwork from apev2 tag for %s\n", cover->filepath);
        if (!apev2_extract_art (cache_path, cover)) {
            cover->cover_found = 1;
            return;
        }

        // try to load embedded from mp4
        trace ("trying to load artwork from mp4 tag for %s\n", cover->filepath);
        if (!mp4_extract_art (cache_path, cover)) {
            cover->cover_found = 1;
            return;
        }
    }

    // Don't allow downloading from the web without disk cache.
    // Even if saving to music folders is enabled -- we don't want to flood.
    // Mainly because we don't know if saving to music folder would succeed,
    // and whether using local covers is turned on.
    if (!cache_path) {
        cover->cover_found = 0;
        return;
    }

#ifdef USE_VFS_CURL
#if 0
#warning FIXME not needed during development; also this assumes that disk cache is used for everything
    /* Flood control, don't retry missing artwork for an hour unless something changes */
    struct stat placeholder_stat;
    if (!stat (cache_path, &placeholder_stat) && placeholder_stat.st_mtime + 60*60 > time (NULL)) {
        int recheck = recheck_missing_artwork (filepath, placeholder_stat.st_mtime);
        if (!recheck) {
            return 0;
        }
    }
#endif

    /* Web lookups */
    if (artwork_enable_wos && strlen (cover->filepath) > 3 && !strcasecmp (cover->filepath+strlen (cover->filepath)-3, ".ay")) {
        if (!fetch_from_wos (cover->title, cache_path)) {
            cover->image_filename = strdup(cache_path);
            cover->cover_found = 1;
            return;
        }
        if (errno == ECONNABORTED) {
            cover->cover_found = 0;
            return;
        }
    }
    else { // don't attempt to load ay covers from regular music services
        int res = web_lookups (cache_path, cover);
        if (res < 0) {
            /* Try stripping parenthesised text off the end of the album name */
            char *p = strpbrk (cover->album, "([");
            if (p) {
                *p = '\0';
                res = web_lookups (cache_path, cover);
                *p = '(';
            }
        }
        if (res >= 0) {
            cover->cover_found = res;

            if (res && artwork_save_to_music_folders && cover->image_filename) {
                // save to the music folder (only if not present)
                char *slash = strrchr (cover->filepath, '/');

                if (!slash) {
                    return;
                }
                size_t len = slash - cover->filepath + 1;

                char covername[] = "cover.jpg";  // FIXME: configurable name
                char coverpath[len + sizeof (covername)];
                memcpy (coverpath, cover->filepath, len);
                memcpy (coverpath + len, covername, sizeof (covername));

                struct stat stat_struct;
                if (stat (coverpath, &stat_struct)) {
                    copy_file(cover->image_filename, coverpath);
                }
            }

            return;
        }
    }
#endif

    /* Touch placeholder */
    write_file (cache_path, NULL, 0);

    cover->cover_found = 0;
}

static void
queue_clear (void) {
    dispatch_sync(sync_queue, ^{
        artwork_abort_all_http_requests();
        cancellation_idx = last_job_idx++;
    });
}

static ddb_cover_info_t *
sync_cover_info_alloc (void) {
    __block ddb_cover_info_t *cover = NULL;
    dispatch_sync(sync_queue, ^{
        cover = cover_info_alloc();
    });
    return cover;
}

static void
sync_cover_info_ref (ddb_cover_info_t *cover) {
    dispatch_sync(sync_queue, ^{
        cover_info_ref(cover);
    });
}

static void
sync_cover_info_release (ddb_cover_info_t *cover) {
    dispatch_sync(sync_queue, ^{
        cover_info_release(cover);
    });
}

static void
cover_update_cache (ddb_cover_info_t *cover) {
    // any empty items?
    time_t minTimestamp = INT64_MAX;
    int minTimestampIdx = -1;
    int emptyIdx = -1;
    for (int i = 0; i < MAX_COVERS_IN_CACHE; i++) {
        if (!cover_cache[i]) {
            emptyIdx = i;
            break;
        }

        if (cover_cache[i]->timestamp < minTimestamp || emptyIdx == -1) {
            minTimestamp = cover_cache[i]->timestamp;
            minTimestampIdx = i;
        }
    }

    if (emptyIdx < 0) {
        cover_info_release(cover_cache[minTimestampIdx]);
        cover_cache[minTimestampIdx] = NULL;
        emptyIdx = minTimestampIdx;
    }
    cover_cache[emptyIdx] = cover;
    cover->timestamp = time(NULL);
    cover_info_ref (cover);
}

static void
cover_cache_free (void) {
    for (int i = 0; i < MAX_COVERS_IN_CACHE; i++) {
        if (!cover_cache[i]) {
            continue;
        }
        cover_info_release(cover_cache[i]);
        cover_cache[i] = NULL;
    }
}

static ddb_cover_info_t *
cover_cache_find (ddb_cover_info_t * cover) {
    for (int i = 0; i < MAX_COVERS_IN_CACHE; i++) {
        if (!cover_cache[i]) {
            continue;
        }

        ddb_cover_info_t *cached_cover = cover_cache[i];

        if (!strcmp (cover->filepath, cached_cover->filepath)) {
            return cached_cover;
        }
    }

    return NULL;
}

static void
execute_callback (ddb_cover_callback_t callback, ddb_cover_info_t *cover, ddb_cover_query_t *query) {
    int cover_found = cover->cover_found;

    // Make all the callbacks (and free the chain), with data if a file was written
    if (cover_found) {
        trace ("artwork fetcher: cover art file found: %s\n", cover->image_filename);
        sync_cover_info_ref(cover);
        callback (0, query, cover);
    }
    else {
        trace ("artwork fetcher: no cover art found\n");
        callback (-1, query, NULL);
    }
}

// To handle concurrency correctly, we need to be able to squash the queries
#define MAX_SQUASHED_QUERIES 50
typedef struct artwork_query_s {
    ddb_cover_query_t *queries[MAX_SQUASHED_QUERIES];
    ddb_cover_callback_t callbacks[MAX_SQUASHED_QUERIES];
    int query_count;
    struct artwork_query_s *next;
} artwork_query_t;

static artwork_query_t *query_head;
static artwork_query_t *query_tail;


static int
queries_squashable (ddb_cover_query_t *query1, ddb_cover_query_t *query2) {
    if (query1->type != query2->type
        || query1->flags != query2->flags) {
        return 0;
    }

    if (query1->track == query2->track) {
        return 1;
    }

    const char *uri1 = deadbeef->pl_find_meta (query1->track, ":URI");
    const char *uri2 = deadbeef->pl_find_meta (query2->track, ":URI");

    if (uri1 == uri2) {
        return 1;
    }

    // if all metadata is defined -- compare tracknr, title, album, artist
    ddb_tf_context_t ctx = {0};
    ctx._size = sizeof (ddb_tf_context_t);

    char query1title[1000];
    char query2title[1000];
    ctx.it = query1->track;
    deadbeef->tf_eval (&ctx, query_compare_tf, query1title, sizeof (query1title));
    ctx.it = query2->track;
    deadbeef->tf_eval (&ctx, query_compare_tf, query2title, sizeof (query2title));

    if (query1title[0] && query2title[0] && !strcmp (query1title, query2title)) {
        return 1;
    }

    return 0;
}

static int
squash_query(ddb_cover_callback_t callback, ddb_cover_query_t *query) {
    __block int squashed = 0;
    dispatch_sync(sync_queue, ^{
        artwork_query_t *q = query_head;
        for (; q; q = q->next) {
            if (queries_squashable (query, q->queries[0]) && q->query_count < MAX_SQUASHED_QUERIES-1) {
                squashed = 1;
                break;
            }
        }
 
        if (!q) {
            // create new
            q = calloc(1, sizeof (artwork_query_t));
            if (query_tail) {
                query_tail->next = q;
                query_tail = q;
            }
            else {
                query_head = query_tail = q;
            }
            squashed = 0;
        }
        q->queries[q->query_count] = query;
        q->callbacks[q->query_count] = callback;
        q->query_count += 1;
    });

    return squashed;
}

static void callback_and_free_squashed (ddb_cover_info_t *cover, ddb_cover_query_t *query) {
    __block artwork_query_t *squashed_queries = NULL;
    dispatch_sync (sync_queue, ^{
        cover_update_cache (cover);
        // find & remove from the queries list
        artwork_query_t *q = query_head;
        artwork_query_t *prev = NULL;
        for (; q; q = q->next) {
            if (q->queries[0] == query) {
                break;
            }
            prev = q;
        }

        if (q) {
            if (prev) {
                prev->next = q->next;
            }
            else {
                query_head = q->next;
            }
            if (q == query_tail) {
                query_tail = prev;
            }
            squashed_queries = q;
        }
    });

    // send the result
    if (squashed_queries != NULL) {
        for (int i = 0; i < squashed_queries->query_count; i++) {
            execute_callback (squashed_queries->callbacks[i], cover, squashed_queries->queries[i]);
        }
        free (squashed_queries);
    }
    sync_cover_info_release (cover);
}

static void
cover_get (ddb_cover_query_t *query, ddb_cover_callback_t callback) {
    __block int cancel_query = 0;
    __block int64_t job_idx = 0;
    dispatch_sync(sync_queue, ^{
        job_idx = last_job_idx++;
    });

    if (cancel_query) {
        callback(-1, query, NULL);
        return;
    }

    dispatch_async(process_queue, ^{
        if (!query->track) {
            callback (-1, query, NULL);
            return;
        }

        /* Process this query, hopefully writing a file into cache */
        ddb_cover_info_t *cover = sync_cover_info_alloc();

        if (!album_tf) {
            album_tf = deadbeef->tf_compile ("%album%");
        }
        if (!artist_tf) {
            artist_tf = deadbeef->tf_compile ("%artist%");
        }
        if (!title_tf) {
            title_tf = deadbeef->tf_compile ("%title%");
        }
        if (!query_compare_tf) {
            query_compare_tf = deadbeef->tf_compile ("$if($and(%title%,%artist%,%album%),%track number% - %title% - %artist% - %album%)");
        }

        deadbeef->pl_lock ();
        strncat (cover->filepath, deadbeef->pl_find_meta (query->track, ":URI"), sizeof(cover->filepath) - strlen(cover->filepath) - 1);
        deadbeef->pl_unlock ();

        ddb_tf_context_t ctx = {0};
        ctx._size = sizeof (ddb_tf_context_t);
        ctx.it = query->track;
        deadbeef->tf_eval (&ctx, album_tf, cover->album, sizeof (cover->album));
        deadbeef->tf_eval (&ctx, artist_tf, cover->artist, sizeof (cover->artist));
        deadbeef->tf_eval (&ctx, title_tf, cover->title, sizeof (cover->title));


        if (sync_queue == NULL) {
            callback (-1, query, NULL);
            return;
        }

        __block int cancel_job = 0;
        dispatch_sync(sync_queue, ^{
            if (job_idx < cancellation_idx) {
                cancel_job = 1;
            }
        });

        if (cancel_job) {
            callback (-1, query, NULL);
            return;
        }

        // check the cache
        ddb_cover_info_t *cached_cover = cover_cache_find (cover);
        if (cached_cover) {
            cached_cover->timestamp = time(NULL);
            cover = cached_cover;
            execute_callback (callback, cover, query);
        }
        else {
            // Check if another query for the same thing is already present in the queue, and squash.
            if (squash_query(callback, query)) {
                return;
            }

            // fetch on concurrent fetch queue
            dispatch_semaphore_wait(fetch_semaphore, DISPATCH_TIME_FOREVER);
            __block int cancel_job = 0;
            dispatch_sync(sync_queue, ^{
                if (job_idx < cancellation_idx) {
                    cancel_job = 1;
                }
            });

            if (cancel_job) {
                callback (-1, query, NULL);
                dispatch_semaphore_signal (fetch_semaphore);
                return;
            }

            dispatch_async (fetch_queue, ^{
                process_query (cover);

                // update queue, and notity the caller
                callback_and_free_squashed (cover, query);
                dispatch_semaphore_signal (fetch_semaphore);
            });
        }
    });
}

static void
artwork_reset (void) {
    trace ("artwork: reset queue\n");
    queue_clear ();
}

static void
artwork_add_listener (ddb_artwork_listener_t listener, void *user_data) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i] == NULL) {
            listeners[i] = listener;
            listeners_userdata[i] = user_data;
            break;
        }
    }
}

void
artwork_remove_listener (ddb_artwork_listener_t listener, void *user_data) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i] == listener) {
            listeners[i] = NULL;
            listeners_userdata[i] = NULL;
            break;
        }
    }
}

static void
get_fetcher_preferences (void) {
    artwork_disable_cache = deadbeef->conf_get_int ("artwork.disable_cache", DEFAULT_DISABLE_CACHE);
    artwork_save_to_music_folders = deadbeef->conf_get_int ("artwork.save_to_music_folders", DEFAULT_SAVE_TO_MUSIC_FOLDERS);

    artwork_enable_embedded = deadbeef->conf_get_int ("artwork.enable_embedded", 1);
    artwork_enable_local = deadbeef->conf_get_int ("artwork.enable_localfolder", 1);
    deadbeef->conf_lock ();
    const char *new_artwork_filemask = deadbeef->conf_get_str_fast ("artwork.filemask", NULL);
    if (!new_artwork_filemask || !new_artwork_filemask[0]) {
        new_artwork_filemask = DEFAULT_FILEMASK;
    }
    if (!strings_equal (artwork_filemask, new_artwork_filemask)) {
        char *old_artwork_filemask = artwork_filemask;
        artwork_filemask = strdup (new_artwork_filemask);
        if (old_artwork_filemask) {
            free (old_artwork_filemask);
        }
    }

    const char *new_artwork_folders = deadbeef->conf_get_str_fast ("artwork.folders", NULL);
    if (!new_artwork_folders || !new_artwork_folders[0]) {
        new_artwork_folders = DEFAULT_FOLDERS;
    }
    if (!strings_equal (artwork_folders, new_artwork_folders)) {
        char *old_artwork_folders = artwork_folders;
        artwork_folders = strdup (new_artwork_folders);
        if (old_artwork_folders) {
            free (old_artwork_folders);
        }
    }
    deadbeef->conf_unlock ();
#ifdef USE_VFS_CURL
    artwork_enable_lfm = deadbeef->conf_get_int ("artwork.enable_lastfm", 0);
#if ENABLE_MUSICBRAINZ
    artwork_enable_mb = deadbeef->conf_get_int ("artwork.enable_musicbrainz", 0);
#endif
#if ENABLE_ALBUMART_ORG
    artwork_enable_aao = deadbeef->conf_get_int ("artwork.enable_albumartorg", 0);
#endif
    artwork_enable_wos = deadbeef->conf_get_int ("artwork.enable_wos", 0);
#endif
    missing_artwork = deadbeef->conf_get_int ("artwork.missing_artwork", 1);
    if (missing_artwork == 2) {
        deadbeef->conf_lock ();
        const char *new_nocover_path = deadbeef->conf_get_str_fast ("artwork.nocover_path", NULL);
        if (!strings_equal (new_nocover_path, nocover_path)) {
            char *old_nocover_path = nocover_path;
            nocover_path = new_nocover_path ? strdup (new_nocover_path) : NULL;
            if (old_nocover_path) {
                free (old_nocover_path);
            }
        }
        deadbeef->conf_unlock ();
    }
}

static void
artwork_configchanged (void) {
    cache_configchanged ();

    int old_artwork_disable_cache = artwork_disable_cache;

    int old_artwork_enable_embedded = artwork_enable_embedded;
    int old_artwork_enable_local = artwork_enable_local;
    char *old_artwork_filemask = strdup(artwork_filemask ? artwork_filemask : "");
    char *old_artwork_folders = strdup(artwork_folders ? artwork_folders : "");
#ifdef USE_VFS_CURL
    int old_artwork_enable_lfm = artwork_enable_lfm;
#if ENABLE_MUSICBRAINZ
    int old_artwork_enable_mb = artwork_enable_mb;
#endif
#if ENABLE_ALBUMART_ORG
    int old_artwork_enable_aao = artwork_enable_aao;
#endif
    int old_artwork_enable_wos = artwork_enable_wos;
#endif
    int old_missing_artwork = missing_artwork;
    const char *old_nocover_path = nocover_path;
//    int old_scale_towards_longer = scale_towards_longer;

    get_fetcher_preferences ();

    int cache_did_reset = 0;
    if (old_artwork_disable_cache != artwork_disable_cache || old_missing_artwork != missing_artwork || old_nocover_path != nocover_path) {
        trace ("artwork config changed, invalidating default artwork...\n");
        default_reset_time = time (NULL);
        cache_did_reset = 1;
    }

    if (old_artwork_enable_embedded != artwork_enable_embedded
        || old_artwork_enable_local != artwork_enable_local
#ifdef USE_VFS_CURL
        || old_artwork_enable_lfm != artwork_enable_lfm
#if ENABLE_MUSICBRAINZ
        || old_artwork_enable_mb != artwork_enable_mb
#endif
#if ENABLE_ALBUMART_ORG
        || old_artwork_enable_aao != artwork_enable_aao
#endif
        || old_artwork_enable_wos != artwork_enable_wos
#endif
        || strcmp(old_artwork_filemask, artwork_filemask)
        || strcmp(old_artwork_folders, artwork_folders)
        || cache_did_reset
        ) {

        dispatch_sync (sync_queue, ^{
            /* All artwork is now (including this second) obsolete */
            deadbeef->conf_set_int64 ("artwork.cache_reset_time", cache_reset_time);

            /* Wait for a new second to start before proceeding */
            while (time (NULL) == cache_reset_time) {
                usleep (100000);
            }

            cover_cache_free ();
            for (int i = 0; i < MAX_LISTENERS; i++) {
                if (listeners[i] != NULL) {
                    listeners[i](DDB_ARTWORK_SETTINGS_DID_CHANGE, listeners_userdata[i], 0, 0);
                }
            }
        });
        queue_clear();
    }
    free (old_artwork_filemask);
    free (old_artwork_folders);
}

static int
artwork_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_CONFIGCHANGED:
        artwork_configchanged ();
        break;
    }
    return 0;
}

static int
invalidate_playitem_cache (DB_plugin_action_t *action, ddb_action_context_t ctx)
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    if (!plt)
        return -1;

    DB_playItem_t *it = deadbeef->plt_get_first (plt, PL_MAIN);
    while (it) {
        if (deadbeef->pl_is_selected (it)) {
            deadbeef->pl_lock ();
            const char *url = strdupa (deadbeef->pl_find_meta (it, ":URI"));
            deadbeef->pl_unlock ();

            char album[1000];
            char artist[1000];
            ddb_tf_context_t ctx = {0};
            ctx._size = sizeof (ddb_tf_context_t);
            ctx.it = it;
            deadbeef->tf_eval (&ctx, album_tf, album, sizeof (album));
            deadbeef->tf_eval (&ctx, artist_tf, artist, sizeof (artist));
            char cache_path[PATH_MAX];
            if (!make_cache_path (url, album, artist, cache_path, PATH_MAX)) {
                char subdir_path[PATH_MAX];
                make_cache_dir_path (artist, subdir_path, PATH_MAX);
                const char *subdir_name = basename (subdir_path);
                const char *entry_name = basename (cache_path);
                trace ("Expire %s from cache\n", cache_path);
                remove_cache_item (cache_path, subdir_path, subdir_name, entry_name);
            }
            deadbeef->pl_unlock ();

            for (int i = 0; i < MAX_LISTENERS; i++) {
                if (listeners[i] != NULL) {
                    listeners[i](DDB_ARTWORK_SETTINGS_DID_CHANGE, listeners_userdata[i], (intptr_t)it, 0);
                }
            }
        }
        deadbeef->pl_item_unref (it);
        it = deadbeef->pl_get_next (it, PL_MAIN);
    }
    deadbeef->plt_unref (plt);

    return 0;
}

static DB_plugin_action_t *
artwork_get_actions (DB_playItem_t *it)
{
    if (!it) // Only currently show for the playitem context menu
        return NULL;

    static DB_plugin_action_t context_action = {
        .title = "Refresh Cover Art",
        .name = "invalidate_playitem_cache",
        .callback2 = invalidate_playitem_cache,
        .flags = DB_ACTION_ADD_MENU | DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS,
        .next = NULL
    };

    return &context_action;
}

static int
artwork_plugin_stop (void) {
    queue_clear ();

    // lock semaphore
    for (int i = 0; i < FETCH_CONCURRENT_LIMIT; i++) {
        dispatch_semaphore_wait(fetch_semaphore, DISPATCH_TIME_FOREVER);
    }
    printf ("release fetch\n");
    dispatch_release(fetch_queue);
    fetch_queue = NULL;
    printf ("release process\n");
    dispatch_release(process_queue);
    process_queue = NULL;
    printf ("release sync\n");
    dispatch_release(sync_queue);
    sync_queue = NULL;

    // unlock semaphore
    for (int i = 0; i < FETCH_CONCURRENT_LIMIT; i++) {
        dispatch_semaphore_signal (fetch_semaphore);
    }
    printf ("release fetch_semaphore\n");
    dispatch_release(fetch_semaphore);
    fetch_semaphore = NULL;
    printf ("released all\n");

    cover_cache_free ();

    cover_info_cleanup();

    if (artwork_filemask) {
        free (artwork_filemask);
    }
    if (artwork_folders) {
        free (artwork_folders);
    }

    if (album_tf) {
        deadbeef->tf_free (album_tf);
        album_tf = NULL;
    }

    if (artist_tf) {
        deadbeef->tf_free (artist_tf);
        artist_tf = NULL;
    }

    if (title_tf) {
        deadbeef->tf_free (title_tf);
        title_tf = NULL;
    }

    if (query_compare_tf) {
        deadbeef->tf_free (query_compare_tf);
        query_compare_tf = NULL;
    }

    stop_cache_cleaner ();

    return 0;
}

static int
artwork_plugin_start (void)
{
    get_fetcher_preferences ();
    cache_reset_time = deadbeef->conf_get_int64 ("artwork.cache_reset_time", 0);

#ifdef USE_IMLIB2
    imlib_set_cache_size (0);
#endif

    sync_queue = dispatch_queue_create("ArtworkSyncQueue", NULL);
    process_queue = dispatch_queue_create("ArtworkProcessQueue", NULL);
    fetch_queue = dispatch_queue_create("ArtworkFetchQueue", DISPATCH_QUEUE_CONCURRENT);
    fetch_semaphore = dispatch_semaphore_create(FETCH_CONCURRENT_LIMIT);

    start_cache_cleaner ();

    return 0;
}

static const char settings_dlg[] =
// on android, cache is always off and music is saved to music folders by default
#ifndef ANDROID
    "property \"Disable disk cache\" checkbox artwork.disable_cache 0;\n"
    "property \"Save downloaded covers to music folders\" checkbox artwork.save_to_music_folders 0;\n"
#endif
    "property \"Fetch from embedded tags\" checkbox artwork.enable_embedded 1;\n"
    "property \"Fetch from local folder\" checkbox artwork.enable_localfolder 1;\n"
    "property \"Local file mask\" entry artwork.filemask \"" DEFAULT_FILEMASK "\";\n"
    "property \"Artwork folders\" entry artwork.folders \"" DEFAULT_FOLDERS "\";\n"
#ifdef USE_VFS_CURL
    "property \"Fetch from Last.fm\" checkbox artwork.enable_lastfm 0;\n"
#if ENABLE_MUSICBRAINZ
    "property \"Fetch from MusicBrainz\" checkbox artwork.enable_musicbrainz 0;\n"
#endif
#if ENABLE_ALBUMART_ORG
    "property \"Fetch from Albumart.org\" checkbox artwork.enable_albumartorg 0;\n"
#endif
    "property \"Fetch from World of Spectrum (AY files only)\" checkbox artwork.enable_wos 0;\n"
#endif
// android doesn't display any image when cover is not found, and positioning algorithm is really different,
// therefore the below options are useless
#ifndef ANDROID
    "property box vbox[2] spacing=4 border=8;\n"
    "property box hbox[1] height=-1;"
    "property \"When no artwork is found\" select[3] artwork.missing_artwork 1 \"leave blank\" \"use DeaDBeeF default cover\" \"display custom image\";"
    "property \"Custom image path\" file artwork.nocover_path \"\";\n"
#endif
;

// define plugin interface
ddb_artwork_plugin_t plugin = {
    .plugin.plugin.api_vmajor = DB_API_VERSION_MAJOR,
    .plugin.plugin.api_vminor = DB_API_VERSION_MINOR,
    .plugin.plugin.version_major = DDB_ARTWORK_MAJOR_VERSION,
    .plugin.plugin.version_minor = DDB_ARTWORK_MINOR_VERSION,
    .plugin.plugin.type = DB_PLUGIN_MISC,
    .plugin.plugin.id = "artwork2",
    .plugin.plugin.name = "Album Artwork",
    .plugin.plugin.descr = "Loads album artwork from embedded tags, local directories, or internet services",
    .plugin.plugin.copyright =
        "Album Art plugin for DeaDBeeF\n"
        "Copyright (C) 2009-2011 Viktor Semykin <thesame.ml@gmail.com>\n"
        "Copyright (C) 2009-2016 Alexey Yakovenko <waker@users.sourceforge.net>\n"
        "Copyright (C) 2014-2016 Ian Nartowicz <deadbeef@nartowicz.co.uk>\n"
        "\n"
        "This software is provided 'as-is', without any express or implied\n"
        "warranty.  In no event will the authors be held liable for any damages\n"
        "arising from the use of this software.\n"
        "\n"
        "Permission is granted to anyone to use this software for any purpose,\n"
        "including commercial applications, and to alter it and redistribute it\n"
        "freely, subject to the following restrictions:\n"
        "\n"
        "1. The origin of this software must not be misrepresented; you must not\n"
        " claim that you wrote the original software. If you use this software\n"
        " in a product, an acknowledgment in the product documentation would be\n"
        " appreciated but is not required.\n"
        "\n"
        "2. Altered source versions must be plainly marked as such, and must not be\n"
        " misrepresented as being the original software.\n"
        "\n"
        "3. This notice may not be removed or altered from any source distribution.\n"
    ,
    .plugin.plugin.website = "http://deadbeef.sf.net",
    .plugin.plugin.start = artwork_plugin_start,
    .plugin.plugin.stop = artwork_plugin_stop,
    .plugin.plugin.configdialog = settings_dlg,
    .plugin.plugin.message = artwork_message,
    .plugin.plugin.get_actions = artwork_get_actions,
    .cover_get = cover_get,
    .reset = artwork_reset,
    .cover_info_release = sync_cover_info_release,
    .add_listener = artwork_add_listener,
    .remove_listener = artwork_remove_listener,
};

DB_plugin_t *
artwork_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
