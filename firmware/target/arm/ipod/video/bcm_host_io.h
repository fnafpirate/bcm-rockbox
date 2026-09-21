/*
 * bcm_host_io.h - BCM2722 (VideoCore) memory access primitives used by bcm_host.c
 *
 * On target these are implemented in lcd-video.c (see 0001-ipodvideo-bcm-host-io.patch)
 * on top of the existing bcm_write_addr()/bcm_read32() code. The host-side unit test
 * (test/bcm_host_test.c) supplies a mock VideoCore RAM instead.
 *
 * Rules learned from the stock firmware and from lcd-video.c:
 *   - every transfer is 32-bit aligned, length a multiple of 4 ("the BCM ignores small
 *     unaligned writes" - lcd-video.c);
 *   - writes latch the destination once and auto-increment (this is how the stock code
 *     and Rockbox upload vmcs.bin);
 *   - reads here set the source address for every word (slow but known-good in
 *     lcd-video.c's bcm_read32()).
 */
#ifndef BCM_HOST_IO_H
#define BCM_HOST_IO_H

#include <stdint.h>
#include <stddef.h>

void bcm_io_lock(void);     /* exclude the LCD tick and lcd_update_rect() from the BCM ports */
void bcm_io_unlock(void);
void bcm_io_read(uint32_t vc_addr, void *dst, size_t len);
void bcm_io_write(uint32_t vc_addr, const void *src, size_t len);
void bcm_io_doorbell(void); /* BCM_CONTROL = 0x31 */
void bcm_io_idle(void);     /* called while waiting; sleep(1) on target, so poll counts are ~10 ms units */

/* Target only (lcd-video.c): replace the VideoCore OS image and power-cycle the BCM so that it
 * boots from `img` (16-bit aligned, must stay valid). Returns false if the NOR vmcs section was
 * not found. */
#include <stdbool.h>
bool bcm_use_vmcs_image(const void *img, unsigned len);

#endif
