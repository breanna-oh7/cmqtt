#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "network.h"

int main(void) {
    stdio_init_all();

    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(500);
    printf("\n--- USB serial connected, starting network.c test ---\n");

 
    const char *test_payload = "hello";
    network_push_packet((const uint8_t *)test_payload, (uint16_t)strlen(test_payload));
    printf("Queued one test packet (%u bytes)\n", (unsigned)strlen(test_payload));
    
    network_task();
}