// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_RP2040ABB_H
#define _BOARDS_RP2040ABB_H

#define RP2040ABB_BOARD

#define PICO_DEFAULT_PIO_USB_DP_PIN 23

#define TESTER_DP_PIN PICO_DEFAULT_PIO_USB_DP_PIN
#define TESTER_BUTTON_PIN 22

#define PICO_DEFAULT_LED_PIN 25

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
