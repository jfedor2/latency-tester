#ifndef _CALLBACKS_H_
#define _CALLBACKS_H_

void report_received_callback(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);
void descriptor_received_callback(uint16_t vendor_id, uint16_t product_id, const uint8_t* report_descriptor, int len, uint16_t interface);
void umount_callback(uint8_t dev_addr, uint8_t instance);

#endif
