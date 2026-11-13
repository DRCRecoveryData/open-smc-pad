#include "usb_host.h"

#include <USBHost.h>

#include "link.h"

static bool s_started = false;

bool usbHostBegin() {
  if (s_started) return true;
  s_started = USBHost.begin();
  linkLog(s_started ? "USB host started" : "USB host start failed");
  return s_started;
}

bool usbHostStarted() { return s_started; }
