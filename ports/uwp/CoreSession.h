/* Small, SDL-free host interface for the Xbox UWP frontend.
 * One active session per process: the upstream dispatch/video code has globals.
 * Call every function on the emulation/UI thread, never from an audio callback. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RONDO_SESSION_FPS_NUM 7159090u
#define RONDO_SESSION_FPS_DEN (455u * 263u)
#define RONDO_SESSION_AUDIO_RATE 44100u
#define RONDO_SESSION_MAX_AUDIO_FRAMES 2048

typedef struct RondoSession RondoSession;

RondoSession *rondo_session_create(void);
/* The host must flush BRAM explicitly before destroy so errors can be shown. */
void rondo_session_destroy(RondoSession *session);
/* save_dir must already exist (ApplicationData.LocalFolder on UWP). Paths are
 * UTF-8. A failed start closes the attempted machine and leaves it inactive. */
bool rondo_session_open(RondoSession *session, const char *disc, const char *save_dir,
                        char *error, int error_length);
bool rondo_session_active(const RondoSession *session);
/* pad uses PAD_* from pce/pce.h. A step mixes one frame's audio into a bounded
 * buffer. Any audio not consumed from the preceding step is discarded. */
bool rondo_session_step(RondoSession *session, uint8_t pad);
/* Pixels are 0xAARRGGBB; stride is measured in uint32_t pixels, not bytes.
 * The returned storage belongs to the session and remains valid until destroy. */
const uint32_t *rondo_session_framebuffer(const RondoSession *session,
                                         int *width, int *height, int *stride);
/* Copy up to capacity_frames stereo S16 frames, consuming the copied samples.
 * The integer sample clock in step avoids drift against the PCE's 59.826 Hz. */
int rondo_session_audio(RondoSession *session, int16_t *output, int capacity_frames);
void rondo_session_discard_audio(RondoSession *session);
bool rondo_session_flush_bram(RondoSession *session, char *error, int error_length);
bool rondo_session_save(RondoSession *session, char *error, int error_length);
bool rondo_session_load(RondoSession *session, char *error, int error_length);
const char *rondo_session_build_id(void);

#ifdef __cplusplus
}
#endif
