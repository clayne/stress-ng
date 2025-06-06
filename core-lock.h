/*
 * Copyright (C) 2024-2025 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#ifndef CORE_LOCK_H
#define CORE_LOCK_H

extern int stress_lock_mem_map(void);
extern void stress_lock_mem_unmap(void);

extern void *stress_lock_create(const char *name);
extern int stress_lock_destroy(void *lock_handle);
extern int stress_lock_acquire(void *lock_handle);
extern int stress_lock_acquire_relax(void *lock_handle);
extern int stress_lock_release(void *lock_handle);

#endif
