#ifndef PICO_CYW43_ARCH_H
#define PICO_CYW43_ARCH_H
static inline void cyw43_arch_lwip_begin(void) {}
static inline void cyw43_arch_lwip_end(void) {}
// Not static inline: the host test driver provides a real implementation
// that injects scripted MOUNT responses on each poll -- see
// test_probe_multislot_mount.c.
int cyw43_arch_poll(void);
#endif
