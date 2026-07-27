# Cores
This program runs two programs: ao27.c on core0, and network.c on core1. 

ao27.c contains the low-level PIO Assembly that handles:
- recieve/transmit clock
- NRZI decoding
- flag detecting
- zero unstuffing
- wsled api. 

Once the packet, which is identified by being in between two flags, 0x7E (01111110), it's pushed to a queue to be published by MQTT in network.c

network.c uses mbedtls, lwipopts, and cyw43 to handle the connection to wifi (in this case an iPhone hotspot), connecting to the MQTT Explorer Broker, the WebSocket Handshake, base64 encoding the data, and publishing the packet to MQTT. 

Note: 
- When connecting to wifi, hotspot usually works best. 
  - Make sure to turn on "Maximize Compatibility" (allows 5GHz signal from hotspot to be read by the pico, which oeprates at a lower frequency)
# Core1 / network.c
## Configurations in cetwork.C 
Ctrl + F and paste the following to find and set configurations 

- DEFINE WIFI : Setup the WiFi ID and Password. Note that when using a Hotspot, to change the password so it doesn't have apostrophe's 
  - Ex: instead of JohnDoe'sPhone, just do JohnDoePhone
- DEFINE BROKER : Setup the MQTT Explorer Broker parameters, such host, port, ws path, etc. 

## network.c

# Core0 / ao27.c
## PIO Resources:
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
      sm 0    cyw43_arch wifi chip takes up SM 0 on default
      sm 1    zero unstuffer, receiveData
      sm 2    Flag Detector
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
      sm 1  wsled
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

## WSLED API
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

