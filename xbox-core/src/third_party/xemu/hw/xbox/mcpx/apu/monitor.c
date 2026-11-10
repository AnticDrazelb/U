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
 * an SDL stream; pacing is by the clock (throttle() with no stream). */
void xb_apu_sound(const int16_t *stereo, int frames);

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = sizeof(d->monitor.frame_buf);
    d->monitor.queued_bytes_high = 3 * sizeof(d->monitor.frame_buf);
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
