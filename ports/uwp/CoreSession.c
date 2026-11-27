#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "CoreSession.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fileapifromapp.h>
#include <io.h>
#ifdef _MSC_VER
#pragma comment(lib, "OneCore.lib")
#endif
#else
#include <unistd.h>
#endif

#include "pce/pce.h"
#include "pce/discid.h"
#include "pce/state.h"
#include "recomp/recomp.h"

struct RondoSession {
    Pce machine;                 /* heap allocation: Pce exceeds typical stack budgets */
    bool running;
    bool bram_missing;
    uint64_t disc_id;
    uint64_t sample_remainder;
    int audio_count, audio_offset;
    int16_t samples[RONDO_SESSION_MAX_AUDIO_FRAMES * 2];
    char bram_path[1536];
    char state_path[1536];
};

static void message(char *error, int length, const char *text)
{
    if (error && length > 0) snprintf(error, (size_t)length, "%s", text);
}

static bool fail(char *error, int length, const char *text)
{
    message(error, length, text);
    return false;
}

static bool paths(RondoSession *s, const char *directory, const char *language)
{
    size_t n = strlen(directory);
    const char *separator = n && (directory[n - 1] == '/' || directory[n - 1] == '\\') ? "" : "/";
    int b = snprintf(s->bram_path, sizeof s->bram_path, "%s%srondo-%016llx-%s.bram",
                     directory, separator, (unsigned long long)s->disc_id, language);
    int t = snprintf(s->state_path, sizeof s->state_path, "%s%srondo-%016llx-%s.state",
                     directory, separator, (unsigned long long)s->disc_id, language);
    return b >= 0 && (size_t)b < sizeof s->bram_path && t >= 0 && (size_t)t < sizeof s->state_path;
}

/* Publish only a completely written sibling file. On Windows the UWP-aware
 * replacement API also keeps the previous file in .bak, including its documented
 * replacement-failure cases. No delete-then-rename gap is introduced. */
static bool publish_file(const char *temporary, const char *target)
{
    FILE *file = disc_fopen(temporary, "r+b");
    if (!file) return false;
#ifdef _WIN32
    bool committed = _commit(_fileno(file)) == 0;
#else
    bool committed = fsync(fileno(file)) == 0;
#endif
    committed = fclose(file) == 0 && committed;
    if (!committed) return false;
#ifdef _WIN32
    wchar_t wide_temp[1600], wide_target[1600], wide_backup[1600];
    char backup[1600];
    snprintf(backup, sizeof backup, "%s.bak", target);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, temporary, -1, wide_temp, 1600) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, wide_target, 1600) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, backup, -1, wide_backup, 1600)) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExFromAppW(wide_target, GetFileExInfoStandard, &attributes)) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        return MoveFileFromAppW(wide_temp, wide_target) != 0;
    }
    if (!DeleteFileFromAppW(wide_backup) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    return ReplaceFileFromAppW(wide_target, wide_temp, wide_backup,
                               REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL) != 0;
#else
    return rename(temporary, target) == 0;
#endif
}

RondoSession *rondo_session_create(void)
{
    return calloc(1, sizeof(RondoSession));
}

void rondo_session_destroy(RondoSession *s)
{
    if (!s) return;
    if (s->running) pce_close(&s->machine);
    free(s);
}

bool rondo_session_active(const RondoSession *s) { return s && s->running; }

bool rondo_session_open(RondoSession *s, const char *disc, const char *save_dir,
                        char *error, int error_length)
{
    char detail[256];
    message(error, error_length, "");
    if (!s || !disc || !disc[0] || !save_dir || !save_dir[0])
        return fail(error, error_length, "A disc image and an existing save directory are required.");
    if (!disc_is_image(disc) || strlen(disc) >= sizeof s->machine.disc.tracks[0].path)
        return fail(error, error_length, "Use a .cue or .chd image with a shorter UTF-8 path.");
    if (s->running && !rondo_session_flush_bram(s, error, error_length)) return false;
    if (s->running) pce_close(&s->machine);
    s->running = false;
    s->audio_count = s->audio_offset = 0;
    s->sample_remainder = 0;
    if (discid_identify(disc, &s->disc_id) == DISC_INVALID)
        return fail(error, error_length, "The image is not a readable PC Engine CD.");
    DiscLanguage language = discid_language(disc);
    if (!paths(s, save_dir, language == DISC_LANG_JA ? "ja" : language == DISC_LANG_EN ? "en" : "unknown"))
        return fail(error, error_length, "The save directory path is too long.");
    if (!pce_init(&s->machine, disc, detail, sizeof detail))
        return fail(error, error_length, detail);
    if (!pce_boot(&s->machine, detail, sizeof detail)) {
        pce_close(&s->machine);
        return fail(error, error_length, detail);
    }

    errno = 0;
    FILE *file = disc_fopen(s->bram_path, "rb");
    s->bram_missing = file == NULL && errno == ENOENT;
    if (file) {
        uint8_t backup[0x800];
        size_t count = fread(backup, 1, sizeof backup, file);
        int extra = fgetc(file);
        bool ok = count == sizeof backup && extra == EOF && !ferror(file) && !memcmp(backup, "HUBM", 4);
        fclose(file);
        if (!ok) {
            pce_close(&s->machine);
            return fail(error, error_length, "The backup RAM file is damaged; it was preserved.");
        }
        memcpy(s->machine.bram, backup, sizeof backup);
    } else if (!s->bram_missing) {
        pce_close(&s->machine);
        return fail(error, error_length, "Cannot read backup RAM from the save directory.");
    }
    s->machine.bram_dirty = false;
    s->running = true;
    return true;
}

bool rondo_session_step(RondoSession *s, uint8_t pad)
{
    if (!rondo_session_active(s)) return false;
    s->machine.pad = pad;
    pce_run_frame(&s->machine);
    s->sample_remainder += (uint64_t)RONDO_SESSION_AUDIO_RATE * RONDO_SESSION_FPS_DEN;
    int frames = (int)(s->sample_remainder / RONDO_SESSION_FPS_NUM);
    s->sample_remainder %= RONDO_SESSION_FPS_NUM;
    pce_audio_render(&s->machine, s->samples, frames);
    s->audio_offset = 0;
    s->audio_count = frames;
    return true;
}

const uint32_t *rondo_session_framebuffer(const RondoSession *s, int *width, int *height, int *stride)
{
    bool running = rondo_session_active(s);
    if (width) *width = running ? s->machine.vdc.fb_w : 0;
    if (height) *height = running ? s->machine.vdc.fb_h : 0;
    if (stride) *stride = running ? PCE_FB_MAX_W : 0;
    return running ? s->machine.vdc.fb : NULL;
}

int rondo_session_audio(RondoSession *s, int16_t *output, int capacity_frames)
{
    if (!rondo_session_active(s) || !output || capacity_frames <= 0) return 0;
    int count = s->audio_count < capacity_frames ? s->audio_count : capacity_frames;
    memcpy(output, s->samples + s->audio_offset * 2, (size_t)count * 2 * sizeof *output);
    s->audio_offset += count;
    s->audio_count -= count;
    return count;
}

void rondo_session_discard_audio(RondoSession *s)
{
    if (s) s->audio_count = s->audio_offset = 0;
}

bool rondo_session_flush_bram(RondoSession *s, char *error, int error_length)
{
    message(error, error_length, "");
    if (!rondo_session_active(s)) return fail(error, error_length, "No game is running.");
    if (!s->machine.bram_dirty && !s->bram_missing) return true;
    char temporary[1600];
    snprintf(temporary, sizeof temporary, "%s.tmp", s->bram_path);
    FILE *file = disc_fopen(temporary, "wb");
    if (!file) return fail(error, error_length, "Cannot write backup RAM to the save directory.");
    bool ok = fwrite(s->machine.bram, 1, 0x800, file) == 0x800;
    ok = fclose(file) == 0 && ok;
    if (ok) ok = publish_file(temporary, s->bram_path);
    if (!ok) return fail(error, error_length, "Writing backup RAM failed; retry before closing the game.");
    s->machine.bram_dirty = s->bram_missing = false;
    return true;
}

bool rondo_session_save(RondoSession *s, char *error, int error_length)
{
    char detail[256];
    if (!rondo_session_flush_bram(s, error, error_length)) return false;
    char temporary[1600];
    snprintf(temporary, sizeof temporary, "%s.tmp", s->state_path);
    bool ok = pce_state_save(&s->machine, temporary, s->disc_id, detail, sizeof detail);
    if (ok && !publish_file(temporary, s->state_path)) {
        snprintf(detail, sizeof detail, "Cannot replace the state file; the previous save and temporary file were preserved.");
        ok = false;
    }
    message(error, error_length, ok ? "" : detail);
    return ok;
}

bool rondo_session_load(RondoSession *s, char *error, int error_length)
{
    char detail[256];
    message(error, error_length, "");
    if (!rondo_session_active(s)) return fail(error, error_length, "No game is running.");
    if (!pce_state_load(&s->machine, s->state_path, s->disc_id, detail, sizeof detail))
        return fail(error, error_length, detail);
    /* State restore intentionally keeps the old host ring. None of that output
     * belongs to the restored timeline; never replay it on the Xbox audio voice. */
    s->machine.snd.head = s->machine.snd.count = 0;
    s->machine.pad = 0;
    s->sample_remainder = 0;
    rondo_session_discard_audio(s);
    return true;
}

const char *rondo_session_build_id(void) { return recomp_build_id; }
