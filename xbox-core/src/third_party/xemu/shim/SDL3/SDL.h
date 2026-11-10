/* xbox-core: what xemu's APU uses of SDL (its sound goes to xbox-core's mixer, not an SDL stream) */
#ifndef XB_XEMU_SDL_H
#define XB_XEMU_SDL_H
#include <unistd.h>
typedef struct SDL_AudioStream SDL_AudioStream;
static inline int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { (void)s; return -1; }
static inline int SDL_GetNumLogicalCPUCores(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
