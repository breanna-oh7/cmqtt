#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>

// Max size of a single packet that can be handed to network_push_packet().
// Must match (or exceed) the packet capture side's own buffer size.
#define NETWORK_PKT_MAX_LEN  1024

// Entry point for core1: connects WiFi, does the TLS/WebSocket/MQTT
// handshake, then loops forever draining queued packets and publishing
// each one as its own MQTT payload. Never returns.
//
// Call via: multicore_launch_core1(network_task);
void network_task(void);

// Called from core0 (safe to call from IRQ context) to hand off one
// completed packet for MQTT publishing. Non-blocking - if the internal
// queue is full (network side behind / not connected yet), the packet is
// silently dropped rather than stalling the caller.
void network_push_packet(const uint8_t *data, uint16_t len);

#endif /* NETWORK_H */