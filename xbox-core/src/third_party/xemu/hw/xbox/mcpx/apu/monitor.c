/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"

/* xbox-core: the monitor's frames go to xbox-core's mixer (mixed into what the AC'97 plays), not to
 * an SDL stream; the mixer's ring of them stands for the stream, so throttle() paces the frames by
 * what is queued there as xemu's does by its stream: frames made while below the high mark, faster
 * below the low. The marks keep 4 to 6 EP frames (21 to 32 ms) ahead of the mixer, which takes 10 ms
 * at a time (xemu's 1 to 3 suit SDL's small pulls; with less here the mixer ran dry between them).
 * As xemu's, the frames (and the global sample count) run ahead once at the start, to fill that. */
void xb_apu_sound(const int16_t *stereo, int frames);

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = (SDL_AudioStream *)d; /* any non-null: xb_apu_queued_bytes answers */
    d->monitor.queued_bytes_low = 4 * sizeof(d->monitor.frame_buf);
    d->monitor.queued_bytes_high = 6 * sizeof(d->monitor.frame_buf);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }
    xb_apu_sound(&d->monitor.frame_buf[0][0], 256);
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
