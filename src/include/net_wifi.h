#ifndef NET_WIFI_H_
#define NET_WIFI_H_

// ─── WiFi initialisation module ───────────────────────────────────────────────
//
// Handles CYW43/WiFi bring-up independently of the GEMDRIVE protocol layer.
// WiFi runs entirely in background IRQ (pico_cyw43_arch_lwip_threadsafe_background),
// so it does not interfere with the timing-critical DMA-based Atari cartridge bus.
//
// Typical call sequence:
//   main():          net_wifi_start()   — starts async connect, returns immediately
//   gemdrvemul loop: net_wifi_poll()    — logs status changes, safe to call every iteration

// Start the async WiFi connection. Returns immediately; does not block.
// Called once before the GEMDRIVE loop. If connection fails later, GEMDRIVE
// continues unaffected with the memory filesystem backend.
void net_wifi_start(void);

// Check WiFi status and log any state changes.
// Rate-limited internally (checks at most once per second).
// Safe to call on every iteration of the GEMDRIVE command loop.
void net_wifi_poll(void);

// Log the most recent WiFi status line unconditionally.
// Safe to call at any time; replays the last known state.
// Useful for the GEMDRIVE startup banner (USB may not have been ready earlier).
void net_wifi_log_status(void);

#endif // NET_WIFI_H_
