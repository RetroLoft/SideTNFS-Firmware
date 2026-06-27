#include "include/romemul.h"
#include "include/gemdrvemul.h"
#include "include/net_wifi.h"
#include "hardware/clocks.h"

int main(void)
{
    // Voltage must be raised before the clock, not after.
    vreg_set_voltage(RP2040_VOLTAGE);
    set_sys_clock_khz(RP2040_CLOCK_FREQ_KHZ, true);

#ifdef SIDETNFS_DEBUG
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 1);
    // Give USB host up to 3 s to enumerate; proceed regardless.
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    LOG("SIDETNFS booting...\n");
#endif

    // ROM emulator must be live before anything else so the Atari never
    // sees an empty cartridge slot, regardless of WiFi outcome.
    COPY_FIRMWARE_TO_RAM((uint16_t *)gemdrvemulROM, gemdrvemulROM_length);
    init_protocol_parser();
    init_romemul(NULL, gemdrvemul_dma_irq_handler_lookup_callback, false);

    LOG("ROM emulator running. Starting WiFi...\n");

    // WiFi is best-effort: C: keeps working even if CYW43 init fails.
    if (cyw43_arch_init()) {
        LOG("cyw43_arch_init failed — continuing without WiFi\n");
    } else {
        net_wifi_start();
    }

    // Enter the GEMDRIVE command dispatch loop — never returns.
    init_gemdrvemul();

    return 0;
}
