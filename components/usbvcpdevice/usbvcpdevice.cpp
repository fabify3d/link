#include <stdio.h>
#include "usbvcpdevice.h"

// Static member initialization
bool USBVCPDevice::usb_host_installed_ = false;
bool USBVCPDevice::cdc_acm_installed_ = false;