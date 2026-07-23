/**
 * Copyright (c) 2026 Michael Wyrick,  N3UC
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "network.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/structs/powman.h"
#include "pico/multicore.h" 
#include "ao27.pio.h"
#include "pico/cyw43_arch.h"
#include <time.h>

/*
  PIO Resources:
  PIO 0
    sm 0    Rx CLK PLL
    sm 1    NRZI decode (not zero unstuff)
    sm 2    Tx Clock to CPU1 and CPU2
    sm 3
    irq 0   Rx Sample Clock
    irq 1   NRZI clock
    irq 2
    irq 3
    irq 4   NRZI output ready for txclock
    irq 5
    irq 6
    irq 7
    IRQ 0
    IRQ 1
  
  PIO 1
    sm 0    Flag Detector
    sm 1    zero unstuffer, receiveData
    sm 2
    sm 3
    irq 0
    irq 1
    irq 2  flag detector output ready for unstuff
    irq 3  flag detected
    irq 4  
    irq 5  
    irq 6
    irq 7
    IRQ 0  flag detector
    IRQ 1  SM 1 Rx Fifo not Empty

  PIO 2
    sm 0
    sm 1
    sm 2
    sm 3
    irq 0
    irq 1
    irq 2
    irq 3
    irq 4
    irq 5
    irq 6
    irq 7
    IRQ 0
    IRQ 1
*/

#define pinUARTtx     0
#define pinUARTrx     1

#define pinWSLed      2
#define pinSampleCLK  3
#define pinPLLAdd     4
#define pinPLLSub     5

#define pinNRZIdecode 6
#define pinFlag       7     // Last bit received was end of a flag char 
#define pinClk        8     // Decoded data stream clock (falling edge sample)
#define pinData       9     // Decoded data stream (sample on falling clock edge)

#define pinToCPU1     10
#define pinToCPU2     11
#define pin12         12
#define pin13         13

#define pinFromCPU    14
#define pin15         15

#define pin16         16
#define pin17         17

#define pin18         18
#define pin19         19
#define pin20         20
#define pin21         21

#define pinSwitch     22

#define pin26         26
#define pin27         27

#define pin28         28

// --------------------------------------------------------
//    WS LED Code
// --------------------------------------------------------
#define NUMLEDS 3
uint32_t leds[NUMLEDS];
uint32_t ledStatus[NUMLEDS];

/*
  LED API
  31-24  Effect  bits
  23-16  Green
  15-8   Red
  7-0    Blue

    Effect Bits:
    7 6 5 4   3 2 1 0
              x x x x  Time in deci seconds (zero is 16 cycles, 1 is one cycle thru)
          x            Reserved
        x              0 = Always, 1 = one time
      x                0 = solid, 1 = blink every time cycles
    x                  0 = on,  1 = off        
    
  An LED Status array contains the current status of the led
  7 6 5 4  3 2 1 0
           x x x x  Current count down time value
        x           1 = counting down
  x                 0 = on, 1 = off
  
*/

#define LED_NORMAL    0x00000000
#define LED_ONOFF     0x80000000
#define LED_BLINK     0x40000000
#define LED_ONETIME   0x20000000
#define LED_RESERVED  0x10000000
#define LED_COUNTDOWN 0x10000000
#define LED_TIMEMASK  0x0F000000

#define LED_0_1sec  0x01000000
#define LED_0_2sec  0x02000000
#define LED_0_3sec  0x03000000
#define LED_0_4sec  0x04000000
#define LED_0_5sec  0x05000000
#define LED_1_0sec  0x0A000000
#define LED_1_5sec  0x0F000000
#define LED_1_6sec  0x00000000
#define LED_FAST    0x01000000

#define LEDOFF      urgb_u32(LED_NORMAL, 0x00,0x00,0x00)
#define LEDRED      urgb_u32(LED_NORMAL, 0xFF,0x00,0x00)
#define LEDGREEN    urgb_u32(LED_NORMAL, 0x00,0xFF,0x00)
#define LEDBLUE     urgb_u32(LED_NORMAL, 0x00,0x00,0xFF)
#define LEDCYAN     urgb_u32(LED_NORMAL, 0x00,0xFF,0xFF)
#define LEDYELLOW   urgb_u32(LED_NORMAL, 0xFF,0xFF,0x00)
#define LEDMAGENTA  urgb_u32(LED_NORMAL, 0xFF,0x00,0xFF)
#define LEDWHITE    urgb_u32(LED_NORMAL, 0xFF,0xFF,0xFF)

#define Brightness 0.1

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio2, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t effect, uint8_t r, uint8_t g, uint8_t b) {
    return
            ((uint32_t) (effect) << 24) |
            ((uint32_t) (r * Brightness) << 8) |
            ((uint32_t) (g * Brightness) << 16) |
            (uint32_t) (b * Brightness);
}

void Led_Service() {
  while (1) {
    // Update the Status of each led
    for(int i=0; i < NUMLEDS; i++) {
      uint32_t led = leds[i];   
      // Check for off
      if (led & LED_ONOFF) {
        // Is marked as off, so set status to off
        ledStatus[i] = LEDOFF;
      } else if (led & (LED_BLINK | LED_ONETIME)) { // check for blink or one time
        if (!(ledStatus[i] & LED_COUNTDOWN)) {
          ledStatus[i] = led | LED_COUNTDOWN;    // The first time thru so setup and mark
        } else {
          uint8_t cntDown = ((ledStatus[i] & LED_TIMEMASK) >> 24) - 1;
          if (cntDown == 0) {
            if (led & LED_ONETIME) {
              leds[i] &= LED_ONOFF;   // Turn off led as we ended our onetime
              ledStatus[i] = LEDOFF;  // Turn it off 
            }
            if (led & LED_BLINK) {
              if (ledStatus[i] & LED_ONOFF) {  // Led is off so turn it on
                ledStatus[i] = led;
              } else {
                ledStatus[i] = LEDOFF | LED_ONOFF | LED_COUNTDOWN  | (led & LED_TIMEMASK)  ;  // Turn it off and mark it
              }
            }
          } else {
          // Count Down
            uint8_t cntDown = (ledStatus[i] & LED_TIMEMASK) >> 24;
            cntDown--;
            ledStatus[i] &= ~LED_TIMEMASK;  // Mask out the time
            ledStatus[i] |= cntDown << 24;  // Put in the new time
          }       
        }
      } else {
        // Normal LED
        ledStatus[i] = led;
      }

      // Send the status values to the hardware leds
      for(int i=0; i < NUMLEDS; i++) {
        put_pixel(ledStatus[i]);
      }
    }

    sleep_ms(100);
  }
}

// --------------------------------------------------------
// Packet Stuff
// --------------------------------------------------------
uint flagcount = 0;
uint datacount = 0;
uint pio1unknownIRQ0 = 0;
uint pio1unknownIRQ1 = 0;

uint8_t packet[1024];
int packetlen = 0;

// --------------------------------------------------------
//  Flag IRQ Handler
// --------------------------------------------------------
void pio_irq_flag() {
  PIO pio = pio1;
  uint isr = 3;

  if (pio_interrupt_get(pio, isr)) {       

    flagcount++;   
    pio_interrupt_clear(pio, isr);
    if (packetlen > 0) {

      if (packetlen > 2)
        leds[2] = LEDGREEN | LED_ONETIME | LED_FAST;
      else
        leds[2] = LEDRED | LED_ONETIME | LED_0_5sec;

      // for(int i=0; i < packetlen; i++) 
      //   printf("%02X ", packet[i]);
      // printf("%i", packetlen);
      // if (packetlen == 0 ){
      //   printf("EMPTY");
      // }
    
      switch (packet[0]) {
        case 0x27: leds[1] = LEDMAGENTA;
        break;
        case 0x9A: leds[1] = 0x00030000;
        break;
        default: leds[1] = 0x000F0F00 | LED_BLINK | LED_FAST;
        break;
      }
      // printf("\n");
      network_push_packet(packet, (uint16_t)packetlen); //instead of printing to the terminal, pushes the entire finished packet  
    }
    packetlen = 0;
  } else {
    pio1unknownIRQ0++;
  }
}

// --------------------------------------------------------
//  Data IRQ handler
// --------------------------------------------------------
void pio_irq_data() {
  PIO pio = pio1;
  uint sm_data = 1;

  while (!pio_sm_is_rx_fifo_empty(pio, sm_data)) {
    uint32_t data = pio_sm_get(pio, sm_data);
    if (packetlen < 1024) {
      packet[packetlen++] = data >> 24;   // Grab the data byte and save it
    }
    datacount++;
  }
}

// --------------------------------------------------------
//  PIO 0   Setup for Rx Clock and NRZI
// --------------------------------------------------------
void setupPIO0() {
  printf("<setupPIO0> Start\n");
  PIO pio = pio0;

  // Setup Receive Clock PLL SM
  uint sm = 0;
  pio_sm_claim(pio, sm);
  uint offset_pll = pio_add_program(pio, &rxclock_program);
  pio_sm_config sm_pll = rxclock_program_get_default_config(offset_pll);
  // Setup GPIO Pins
  // receive data in
  pio_sm_set_consecutive_pindirs(pio, sm, pinFromCPU, 1, false);
  sm_config_set_in_pins(&sm_pll, pinFromCPU);
  sm_config_set_in_pin_count(&sm_pll, 1);

  // Setup Output Pins
  pio_gpio_init(pio, pinPLLAdd);
  pio_gpio_init(pio, pinPLLSub);
  pio_sm_set_consecutive_pindirs(pio, sm, pinPLLAdd, 2, true);
  sm_config_set_sideset_pins(&sm_pll, pinPLLAdd);
  
  pio_sm_init(pio, sm, offset_pll, &sm_pll);
  
  // Setup NRZI Decoder SM
  sm = 1;
  pio_sm_claim(pio, sm);
  uint offset_nrzi = pio_add_program(pio, &nrzi_program);
  pio_sm_config sm_nrzi = nrzi_program_get_default_config(offset_nrzi);
  // Input Pins
  pio_sm_set_consecutive_pindirs(pio, sm, pinFromCPU, 1, false);
  sm_config_set_in_pins(&sm_nrzi, pinFromCPU);
  sm_config_set_in_pin_count(&sm_nrzi, 1);

  // Output Pins
  pio_gpio_init(pio, pinSampleCLK);
  pio_gpio_init(pio, pinNRZIdecode);
  pio_sm_set_consecutive_pindirs(pio, sm, pinSampleCLK, 1, true);
  pio_sm_set_consecutive_pindirs(pio, sm, pinNRZIdecode, 1, true);
  sm_config_set_sideset_pins(&sm_nrzi, pinSampleCLK);
  sm_config_set_set_pins(&sm_nrzi, pinNRZIdecode, 1);
  pio_sm_init(pio, sm, offset_nrzi, &sm_nrzi);

    // Setup Tx Clock SM
  sm = 2;
  pio_sm_claim(pio, sm);
  uint offset_txclk = pio_add_program(pio, &txclock_program);
  pio_sm_config sm_tx = txclock_program_get_default_config(offset_txclk);
  // Setup GPIO Pins

  // Setup Output Pins
  pio_gpio_init(pio, pinToCPU1);
  pio_gpio_init(pio, pinToCPU2);
  pio_sm_set_consecutive_pindirs(pio, sm, pinToCPU1, 2, true);
  sm_config_set_set_pins(&sm_tx, pinToCPU1, 2);
  
  pio_sm_init(pio, sm, offset_txclk, &sm_tx);

  pio_enable_sm_mask_in_sync(pio, 0x07);
  printf("<setupPIO0> End\n");
}

// --------------------------------------------------------
//  PIO 1
// --------------------------------------------------------

void setupPIO1() {
  printf("<setupPIO1> Start\n");
  PIO pio = pio1;
  
  uint sm_flag_claimed = 2;
  uint sm_unstuff_claimed = 1;

  pio_sm_claim(pio, sm_flag_claimed);
  pio_sm_claim(pio, sm_unstuff_claimed);

  uint offset_flag = pio_add_program(pio, &flag_program);

  pio_sm_config sm_flag = flag_program_get_default_config(offset_flag);
  // Setup GPIO Pins
  // receive data in
  pio_sm_set_consecutive_pindirs(pio, sm_flag_claimed, pinNRZIdecode, 1, false);
  sm_config_set_in_pins(&sm_flag, pinNRZIdecode);
  sm_config_set_in_pin_count(&sm_flag, 1);

  pio_gpio_init(pio, pinFlag);
  pio_gpio_init(pio, pinData);
  pio_sm_set_consecutive_pindirs(pio, sm_flag_claimed, pinFlag, 1, true);
 // pio_sm_set_consecutive_pindirs(pio, sm, pinData, 1, true);
  sm_config_set_sideset_pins(&sm_flag, pinFlag);
//  sm_config_set_out_pins(&sm_flag, pinData, 1);

  // interrupt setup
  irq_set_exclusive_handler(PIO1_IRQ_0, pio_irq_flag);
  irq_set_enabled(PIO1_IRQ_0, true);
  pio_set_irq0_source_enabled(pio, pis_interrupt3, true);

  // Init
  pio_sm_init(pio, sm_flag_claimed, offset_flag, &sm_flag);  // Init SM

    // unstuffer
  // sm = 1;
  

  uint offset_unstuff = pio_add_program(pio, &receiveData_program);
  pio_sm_config sm_unstuff = receiveData_program_get_default_config(offset_unstuff);
  // Setup GPIO Pins
  // receive data in
  pio_sm_set_consecutive_pindirs(pio, sm_unstuff_claimed, pinNRZIdecode, 1, false);
  sm_config_set_in_pins(&sm_unstuff, pinNRZIdecode);
  sm_config_set_in_pin_count(&sm_unstuff, 1);

  // Setup Output Pins
  pio_gpio_init(pio, pinClk);
  pio_gpio_init(pio, pinData);
  pio_sm_set_consecutive_pindirs(pio, sm_unstuff_claimed, pinClk, 1, true);
  pio_sm_set_consecutive_pindirs(pio, sm_unstuff_claimed, pinData, 1, true);
  sm_config_set_sideset_pins(&sm_unstuff, pinClk);
  sm_config_set_jmp_pin(&sm_unstuff, pinFlag);
  
  pio_sm_init(pio, sm_unstuff_claimed, offset_unstuff, &sm_unstuff);

  irq_set_exclusive_handler(PIO1_IRQ_1, pio_irq_data);
  irq_set_enabled(PIO1_IRQ_1, true);
  pio_set_irq1_source_enabled(pio, pis_sm1_rx_fifo_not_empty, true);

  // pio_enable_sm_mask_in_sync(pio, 0x03);        // Enable SM
  pio_enable_sm_mask_in_sync(pio, (1u << sm_flag_claimed) | (1u << sm_unstuff_claimed));
  pio->txf[sm_flag_claimed] = 0x0000007E;                    // Put Flag Char into FIFO

  printf("<setupPIO1> End\n");
}

// --------------------------------------------------------
//  PIO 2
// --------------------------------------------------------
void setupPIO2() {
  PIO pio = pio2;
  uint sm = 1;
  pio_sm_claim(pio, sm);

  pio_gpio_init(pio, pinWSLed);
  pio_sm_set_consecutive_pindirs(pio, sm, pinWSLed, 1, true);

  uint offset = pio_add_program(pio, &wsled_program);
  pio_sm_config c = wsled_program_get_default_config(offset);

  sm_config_set_sideset_pins(&c, pinWSLed);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}

// format the time struct here, or the data struct 
// struct timespec get_time(){
//   struct specs;
//   clock_gettime(CLOCK_REALTIME, &specs);
//   return specs;
// }


static uint32_t core1_stack[8192]; // 8192 uint32_t = 32768 bytes - mbedtls/lwIP/cyw43 need real headroom
// --------------------------------------------------------
//    main
// --------------------------------------------------------


int main() {
  set_sys_clock_khz(156000, true);
  setup_default_uart();
  stdio_init_all();    

  while (!stdio_usb_connected()) {
    sleep_ms(100);
  }
  sleep_ms(500);
  printf("AO-27 Bench CPU Pico\n");

  // What actually caused the *previous* reset - tells us if the earlier
  // crashes were a brownout/power glitch, a watchdog timeout, or a normal
  // power-on/RUN-pin reset.
  uint32_t chip_reset = powman_hw->chip_reset;
  printf("Reset cause: chip_reset=0x%08lx watchdog_caused_reboot=%d\n",
         (unsigned long)chip_reset, (int)watchdog_caused_reboot());
#ifdef POWMAN_CHIP_RESET_HAD_BOR_BITS
  if (chip_reset & POWMAN_CHIP_RESET_HAD_BOR_BITS)
    printf("  -> BROWN-OUT reset (power rail sagged below threshold)\n");
#endif
#ifdef POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS
  if (chip_reset & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)
    printf("  -> POWER GLITCH detected\n");
#endif
#ifdef POWMAN_CHIP_RESET_HAD_POR_BITS
  if (chip_reset & POWMAN_CHIP_RESET_HAD_POR_BITS)
    printf("  -> normal power-on reset\n");
#endif
#ifdef POWMAN_CHIP_RESET_HAD_RUN_BITS
  if (chip_reset & POWMAN_CHIP_RESET_HAD_RUN_BITS)
    printf("  -> RUN pin / reset button\n");
#endif
  if (watchdog_caused_reboot())
    printf("  -> WATCHDOG reset (code hung and the watchdog fired)\n");

  gpio_init(pinFromCPU);
  gpio_set_dir(pinFromCPU, GPIO_IN);
  gpio_pull_up(pinFromCPU); 


  setupPIO0();
  setupPIO1();
  setupPIO2();

  leds[0] = LEDBLUE | LED_BLINK | LED_0_5sec;
  leds[1] = LEDOFF;
  leds[2] = LEDOFF;
  
  sleep_ms(2000);
  // 4. Launch Core 1 using custom stack array in RAM
  multicore_launch_core1_with_stack(network_task, core1_stack, sizeof(core1_stack));
  // multicore_launch_core1(Led_Service);
  // 5. Run LED servicing on Core 0
  Led_Service();
  // return 0;
  while(1) {
    // printf("\nFlag Count: %i,  Data Count: %i, unknown0: %i, unknown1: %i\n", flagcount, datacount, pio1unknownIRQ0, pio1unknownIRQ1);

   sleep_ms(1000);
  }
}