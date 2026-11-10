/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2021 Matt Borgerson
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

#ifndef HW_MCPX_APU_H
#define HW_MCPX_APU_H

#include <stdint.h>
/* xbox-core's entry points (apu.c) */
void xb_mcpx_apu_init(uint8_t *ram_ptr);
uint64_t xb_mcpx_apu_read(uint64_t addr, unsigned int size);
void xb_mcpx_apu_write(uint64_t addr, uint64_t val, unsigned int size);

#endif
