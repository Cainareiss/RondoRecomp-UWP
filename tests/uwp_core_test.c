/* Exercises the real core through the UWP bridge without a commercial disc.
 * This generated PC Engine IPL only reads the pad and changes a blue backdrop. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int make_directory(const char *path)
{
    wchar_t wide[1536];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1536) ? _wmkdir(wide) : -1;
}
static int remove_directory(const char *path)
{
    wchar_t wide[1536];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1536) ? _wrmdir(wide) : -1;
}
static int remove_file(const char *path)
{
    wchar_t wide[1536];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1536) ? _wremove(wide) : -1;
}
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0700)
#define remove_directory(path) rmdir(path)
#define remove_file(path) remove(path)
#endif

#include "CoreSession.h"
#include "pce/pce.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #condition, error); \
    failed = 1; goto cleanup; \
} } while (0)

static bool write_file(const char *path, const void *bytes, size_t length)
{
    FILE *file = disc_fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = disc_fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long count = ftell(file);
    if (count < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    uint8_t *bytes = malloc(count ? (size_t)count : 1);
    if (!bytes || fread(bytes, 1, (size_t)count, file) != (size_t)count) { free(bytes); fclose(file); return NULL; }
    fclose(file);
    *length = (size_t)count;
    return bytes;
}

static uint64_t fingerprint(const uint8_t *bytes, size_t count)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    while (count--) { hash ^= *bytes++; hash *= UINT64_C(0x100000001b3); }
    return hash;
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : "uwp-core-fixture";
    char cue[1536], image[1536], state[1536], bram[1536], other_state[1536], temporary[1600];
    char error[256] = "";
    int failed = 0;
    bool directory_created = false, temporary_directory_created = false;
    RondoSession *session = NULL;
    uint8_t *saved = NULL, *roundtrip = NULL;
    uint8_t sectors[16 * 2048] = {0};
    int16_t pcm[RONDO_SESSION_MAX_AUDIO_FRAMES * 2];
    snprintf(cue, sizeof cue, "%s/pad-\xC3\xA1.cue", directory);
    snprintf(image, sizeof image, "%s/pad.bin", directory);
    state[0] = bram[0] = other_state[0] = 0;
    /* Refuse to reuse an existing directory: cleanup owns only this fixture. */
    CHECK(make_directory(directory) == 0);
    directory_created = true;

    uint8_t *header = sectors + 2048;
    header[2] = 2;               /* IPL is one sector, record 2 */
    header[3] = 1;
    header[5] = header[7] = 0x40; /* load/jump $4000, mapped to CD RAM */
    memcpy(header + 0x20, "PC Engine CD-ROM SYSTEM", 23);
    static const uint8_t program[] = {
        0x78,                   /* SEI */
        0xA9, 0x00,             /* LDA #0 */
        0x8D, 0x00, 0x10,       /* STA $1000: select buttons, clear off */
        /* loop at $4006 */
        0xA9, 0x00,
        0x8D, 0x02, 0x04,       /* VCE palette address = 0 */
        0x8D, 0x03, 0x04,
        0xAD, 0x00, 0x10,       /* read active-low pad */
        0x29, 0x07,             /* blue component 0..7 */
        0x8D, 0x04, 0x04,
        0xA9, 0x00,
        0x8D, 0x05, 0x04,
        0x4C, 0x06, 0x40,
    };
    memcpy(sectors + 2 * 2048, program, sizeof program);
    uint64_t id = fingerprint(sectors + 2 * 2048, 2048);
    snprintf(state, sizeof state, "%s/rondo-%016llx-en.state", directory, (unsigned long long)id);
    snprintf(bram, sizeof bram, "%s/rondo-%016llx-en.bram", directory, (unsigned long long)id);
    CHECK(write_file(image, sectors, sizeof sectors));
    static const char sheet[] = "FILE \"pad.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n";
    CHECK(write_file(cue, sheet, sizeof sheet - 1));

    session = rondo_session_create();
    CHECK(session != NULL);
    CHECK(!rondo_session_active(session));
    CHECK(!rondo_session_step(session, 0));
    CHECK(!rondo_session_load(session, error, sizeof error));
    CHECK(!rondo_session_open(session, "missing.cue", directory, error, sizeof error));
    CHECK(!rondo_session_active(session));
    CHECK(rondo_session_open(session, cue, directory, error, sizeof error));
    CHECK(rondo_session_active(session));
    CHECK(rondo_session_build_id()[0] != 0);
    CHECK(!rondo_session_load(session, error, sizeof error)); /* no state yet */

    uint64_t samples = 0;
    const int frames = 120;
    for (int frame = 0; frame < frames; frame++) {
        CHECK(rondo_session_step(session, frame == 0 ? PAD_I : 0));
        int width, height, stride;
        const uint32_t *pixels = rondo_session_framebuffer(session, &width, &height, &stride);
        CHECK(pixels && width == 256 && height == 240 && stride == PCE_FB_MAX_W);
        /* Real CPU execution, joypad I/O and palette rendering must agree. */
        CHECK((pixels[0] & 0xFF) == (uint32_t)(frame == 0 ? 6 * 255 / 7 : 255));
        int first = rondo_session_audio(session, pcm, 13);
        int rest = rondo_session_audio(session, pcm, RONDO_SESSION_MAX_AUDIO_FRAMES);
        CHECK(first == 13 && rest > 700 && first + rest <= 738);
        samples += (uint64_t)(first + rest);
        CHECK(rondo_session_audio(session, pcm, RONDO_SESSION_MAX_AUDIO_FRAMES) == 0);
    }
    CHECK(samples == (uint64_t)frames * RONDO_SESSION_AUDIO_RATE * RONDO_SESSION_FPS_DEN / RONDO_SESSION_FPS_NUM);
    CHECK(rondo_session_step(session, PAD_II));
    rondo_session_discard_audio(session);
    CHECK(rondo_session_audio(session, pcm, RONDO_SESSION_MAX_AUDIO_FRAMES) == 0);
    CHECK(rondo_session_step(session, PAD_I)); /* deliberately leave this output pending */
    CHECK(rondo_session_step(session, 0));
    CHECK(rondo_session_audio(session, pcm, RONDO_SESSION_MAX_AUDIO_FRAMES) <= 738);

    CHECK(rondo_session_save(session, error, sizeof error));
    size_t saved_length = 0, roundtrip_length = 0;
    saved = read_file(state, &saved_length);
    CHECK(saved && saved_length > 32);
    /* Refuse a new state write at its temporary path. The committed slot must
     * remain byte-for-byte unchanged when the attempted save fails. */
    snprintf(temporary, sizeof temporary, "%s.tmp", state);
    CHECK(make_directory(temporary) == 0);
    temporary_directory_created = true;
    CHECK(rondo_session_step(session, PAD_I));
    CHECK(!rondo_session_save(session, error, sizeof error));
    roundtrip = read_file(state, &roundtrip_length);
    CHECK(roundtrip && roundtrip_length == saved_length && !memcmp(saved, roundtrip, saved_length));
    free(roundtrip); roundtrip = NULL;
    CHECK(remove_directory(temporary) == 0);
    temporary_directory_created = false;
    roundtrip = read_file(bram, &roundtrip_length);
    CHECK(roundtrip && roundtrip_length == 0x800 && !memcmp(roundtrip, "HUBM", 4));
    free(roundtrip); roundtrip = NULL;
    CHECK(rondo_session_step(session, PAD_I));
    CHECK(rondo_session_load(session, error, sizeof error));
    CHECK(rondo_session_audio(session, pcm, RONDO_SESSION_MAX_AUDIO_FRAMES) == 0);
    CHECK(rondo_session_save(session, error, sizeof error));
    roundtrip = read_file(state, &roundtrip_length);
    CHECK(roundtrip && roundtrip_length == saved_length && !memcmp(saved, roundtrip, saved_length));
    free(roundtrip); roundtrip = NULL;
    /* Keep a valid v1 header but claim a 4 GiB payload (code_len is the native
     * uint64_t at byte 24). Reject it before allocation and keep the live machine
     * unchanged: saving again must reproduce the original committed state. */
    uint8_t hostile_header[32];
    memcpy(hostile_header, saved, sizeof hostile_header);
    uint64_t hostile_length = UINT32_MAX;
    memcpy(hostile_header + 24, &hostile_length, sizeof hostile_length);
    CHECK(write_file(state, hostile_header, sizeof hostile_header));
    CHECK(!rondo_session_load(session, error, sizeof error));
    CHECK(strstr(error, "payload size is out of range") != NULL);
    CHECK(rondo_session_active(session));
    CHECK(rondo_session_save(session, error, sizeof error));
    roundtrip = read_file(state, &roundtrip_length);
    CHECK(roundtrip && roundtrip_length == saved_length && !memcmp(saved, roundtrip, saved_length));
    free(roundtrip); roundtrip = NULL;
    rondo_session_destroy(session); session = NULL;

    /* A fresh heap instance must load the same state and keep user BRAM. */
    session = rondo_session_create();
    CHECK(session && rondo_session_open(session, cue, directory, error, sizeof error));
    CHECK(rondo_session_load(session, error, sizeof error));
    CHECK(rondo_session_step(session, PAD_I));
    int width, height, stride;
    const uint32_t *pixels = rondo_session_framebuffer(session, &width, &height, &stride);
    CHECK(pixels && (pixels[0] & 0xFF) == 6 * 255 / 7);

    /* Change an unused byte in the IPL: a distinct fingerprint gets another slot,
     * and an accidentally copied state from the previous disc is refused. */
    sectors[2 * 2048 + 128] = 1;
    uint64_t other_id = fingerprint(sectors + 2 * 2048, 2048);
    snprintf(other_state, sizeof other_state, "%s/rondo-%016llx-en.state", directory, (unsigned long long)other_id);
    CHECK(write_file(image, sectors, sizeof sectors));
    CHECK(rondo_session_open(session, cue, directory, error, sizeof error));
    CHECK(!rondo_session_load(session, error, sizeof error));
    CHECK(write_file(other_state, saved, saved_length));
    CHECK(!rondo_session_load(session, error, sizeof error));
    CHECK(rondo_session_active(session));
    CHECK(rondo_session_step(session, 0));

    printf("UWP core: synthetic IPL, pad/video, %llu exact audio frames, state roundtrip, fresh session and disc isolation passed.\n",
           (unsigned long long)samples);
cleanup:
    rondo_session_destroy(session);
    free(saved);
    free(roundtrip);
    if (temporary_directory_created) remove_directory(temporary);
    if (directory_created) {
        remove_file(cue);
        remove_file(image);
        if (state[0]) remove_file(state);
        if (bram[0]) remove_file(bram);
        if (other_state[0]) remove_file(other_state);
        /* Windows replacement keeps a previous version as a recovery file. */
        const char *committed[] = {state, bram, other_state};
        for (size_t i = 0; i < sizeof committed / sizeof committed[0]; i++) {
            if (!committed[i][0]) continue;
            snprintf(temporary, sizeof temporary, "%s.bak", committed[i]);
            remove_file(temporary);
            snprintf(temporary, sizeof temporary, "%s.tmp", committed[i]);
            remove_file(temporary);
        }
        remove_directory(directory);
    }
    return failed;
}
