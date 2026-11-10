/* xbox-core: what xemu's APU uses of SDL (its sound goes to xbox-core's mixer, not an SDL stream) */
#ifndef XB_XEMU_SDL_H
#define XB_XEMU_SDL_H
#include <unistd.h>
typedef struct SDL_AudioStream SDL_AudioStream;
/* xbox-core: the APU's "stream" is xbox-core's mixer's ring of its sound: what is queued there paces
 * the frame thread as xemu's SDL stream does (throttle()) */
int xb_apu_queued_bytes(void);
static inline int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { return s ? xb_apu_queued_bytes() : -1; }
static inline int SDL_GetNumLogicalCPUCores(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
