#ifndef USB_BRIDGE_H
#define USB_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the USB interrupt with each received packet. */
void usb_bridge_receive(const uint8_t* data, uint32_t length);

/* Defined in USB_DEVICE/App/usbd_cdc_if.c. */
uint8_t CDC_Transmit_Idle_FS(void);
uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len);

/* The application, run from main.c. */
void app_setup(void);
void app_loop(void);

#ifdef __cplusplus
}
#endif

#endif
