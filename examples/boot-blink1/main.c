// Blinky example with Cypress bootloader support
// Build: make -C boot-blink1
// Program to EEPROM: fx2tool -d 04b4:8614 -B program -f boot-blink1/boot-blink1.ihex

#include <fx2lib.h>
#include <fx2usb.h>
#include <fx2delay.h>
#include <fx2eeprom.h>
#include <fx2regs.h>
#include <fx2ints.h>

// USB descriptors with bootloader support
usb_desc_device_c usb_device = {
  .bLength              = sizeof(struct usb_desc_device),
  .bDescriptorType      = USB_DESC_DEVICE,
  .bcdUSB               = 0x0200,
  .bDeviceClass         = USB_DEV_CLASS_VENDOR,
  .bDeviceSubClass      = USB_DEV_SUBCLASS_VENDOR,
  .bDeviceProtocol      = USB_DEV_PROTOCOL_VENDOR,
  .bMaxPacketSize0      = 64,
  .idVendor             = 0x04b4,
  .idProduct            = 0x8614,  // Changed from 0x8613 to avoid CYUSB3 driver conflict
  .bcdDevice            = 0x0001,
  .iManufacturer        = 1,
  .iProduct             = 2,
  .iSerialNumber        = 0,
  .bNumConfigurations   = 1,
};

usb_desc_interface_c usb_interface = {
  .bLength              = sizeof(struct usb_desc_interface),
  .bDescriptorType      = USB_DESC_INTERFACE,
  .bInterfaceNumber     = 0,
  .bAlternateSetting    = 0,
  .bNumEndpoints        = 0,
  .bInterfaceClass      = USB_IFACE_CLASS_VENDOR,
  .bInterfaceSubClass   = USB_IFACE_SUBCLASS_VENDOR,
  .bInterfaceProtocol   = USB_IFACE_PROTOCOL_VENDOR,
  .iInterface           = 0,
};

usb_configuration_c usb_config = {
  {
    .bLength              = sizeof(struct usb_desc_configuration),
    .bDescriptorType      = USB_DESC_CONFIGURATION,
    .bNumInterfaces       = 1,
    .bConfigurationValue  = 1,
    .iConfiguration       = 0,
    .bmAttributes         = USB_ATTR_RESERVED_1,
    .bMaxPower            = 50,
  },
  {
    { .interface = &usb_interface },
    { 0 }
  }
};

usb_configuration_set_c usb_configs[] = {
  &usb_config,
};

usb_ascii_string_c usb_strings[] = {
  [0] = "whitequark@whitequark.org",
  [1] = "FX2 series Cypress-class bootloader with one blinking LED",
};

usb_descriptor_set_c usb_descriptor_set = {
  .device           = &usb_device,
  .config_count     = ARRAYSIZE(usb_configs),
  .configs          = usb_configs,
  .string_count     = ARRAYSIZE(usb_strings),
  .strings          = usb_strings,
};

// Cypress bootloader vendor requests
enum {
  USB_REQ_CYPRESS_EEPROM_SB  = 0xA2,
  USB_REQ_CYPRESS_EXT_RAM    = 0xA3,
  USB_REQ_CYPRESS_RENUMERATE = 0xA8,
  USB_REQ_CYPRESS_EEPROM_DB  = 0xA9,
  USB_REQ_LIBFX2_PAGE_SIZE   = 0xB0,
};

volatile bool pending_setup;
uint8_t page_size = 0;

void handle_usb_setup(__xdata struct usb_req_setup *req) {
  req;
  if(pending_setup) {
    STALL_EP0();
  } else {
    pending_setup = true;
  }
}

void handle_pending_usb_setup(void) {
  __xdata struct usb_req_setup *req = (__xdata struct usb_req_setup *)SETUPDAT;

  // Handle re-enumerate request
  if(req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_OUT) &&
     req->bRequest == USB_REQ_CYPRESS_RENUMERATE) {
    pending_setup = false;
    USBCS |= _DISCON;
    delay_ms(10);
    USBCS &= ~_DISCON;
    return;
  }

  // Handle page size setting
  if(req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_OUT) &&
     req->bRequest == USB_REQ_LIBFX2_PAGE_SIZE) {
    page_size = req->wValue;
    pending_setup = false;
    ACK_EP0();
    return;
  }

  // Handle EEPROM read/write
  if((req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_IN) ||
      req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_OUT)) &&
     (req->bRequest == USB_REQ_CYPRESS_EEPROM_SB ||
      req->bRequest == USB_REQ_CYPRESS_EEPROM_DB)) {
    bool     arg_read  = (req->bmRequestType & USB_DIR_IN);
    bool     arg_dbyte = (req->bRequest == USB_REQ_CYPRESS_EEPROM_DB);
    uint8_t  arg_chip  = arg_dbyte ? 0x51 : 0x50;
    uint16_t arg_addr  = req->wValue;
    uint16_t arg_len   = req->wLength;
    pending_setup = false;

    while(arg_len > 0) {
      uint8_t len = arg_len < 64 ? arg_len : 64;

      if(arg_read) {
        while(EP0CS & _BUSY);
        if(!eeprom_read(arg_chip, arg_addr, EP0BUF, len, arg_dbyte)) {
          STALL_EP0();
          break;
        }
        SETUP_EP0_IN_BUF(len);
      } else {
        SETUP_EP0_OUT_BUF();
        while(EP0CS & _BUSY);
        if(!eeprom_write(arg_chip, arg_addr, EP0BUF, len, arg_dbyte, page_size,
                         /*timeout=*/166)) {
          STALL_EP0();
          break;
        }
        ACK_EP0();
      }

      arg_len  -= len;
      arg_addr += len;
    }
    return;
  }

  // Handle external RAM access
  if((req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_IN) ||
      req->bmRequestType == (USB_RECIP_DEVICE|USB_TYPE_VENDOR|USB_DIR_OUT)) &&
     req->bRequest == USB_REQ_CYPRESS_EXT_RAM) {
    bool     arg_read = (req->bmRequestType & USB_DIR_IN);
    uint16_t arg_addr = req->wValue;
    uint16_t arg_len  = req->wLength;
    pending_setup = false;

    while(arg_len > 0) {
      uint8_t len = arg_len < 64 ? arg_len : 64;

      if(arg_read) {
        while(EP0CS & _BUSY);
        xmemcpy(EP0BUF, (__xdata void *)arg_addr, len);
        SETUP_EP0_IN_BUF(len);
      } else {
        SETUP_EP0_OUT_BUF();
        while(EP0CS & _BUSY);
        xmemcpy((__xdata void *)arg_addr, EP0BUF, arg_len);
        ACK_EP0();
      }

      arg_len  -= len;
      arg_addr += len;
    }
    return;
  }

  pending_setup = false;
  STALL_EP0();
}

// Blinky timer interrupt
void isr_TF0(void) __interrupt(_INT_TF0) {
  static int i;
  if(i++ % 64 == 0)
    PA0 = !PA0;
}

int main(void) {
  // Initialize CPU
  CPUCS = _CLKOE|_CLKSPD1;

  // Configure blinky pins
  PA0 = 1;      // set PA0 to high
  OEA = 0b1;    // set PA0 as output

  // Configure TIMER0 for blinking
  TCON = _M0_0; // use 16-bit counter mode
  ET0 = 1;      // generate an interrupt
  TR0 = 1;      // run

  // Initialize USB with bootloader support
  usb_init(/*disconnect=*/false);

  // Enable interrupts
  EA = 1;

  // Main loop handles USB bootloader requests and blinks LED
  while(1) {
    if(pending_setup)
      handle_pending_usb_setup();
  }
}
