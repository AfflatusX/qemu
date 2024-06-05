#ifndef SLATE_RAM_DISP_H
#define SLATE_RAM_DISP_H

#include "qemu/osdep.h"

#include "hw/loader.h"
#include "include/qom/object.h"
#include "qemu/module.h"
#include "ui/console.h"

#define TYPE_RAM_DISPLAY_DEVICE "ram_display"
OBJECT_DECLARE_SIMPLE_TYPE(RamDisplayState, RAM_DISPLAY_DEVICE)

struct RamDisplayState {
  SysBusDevice parent_obj;
  MemoryRegion fb_io;
  MemoryRegion* fb_data;
  DisplaySurface* display_surface;
  QemuConsole* console;
  bool is_updating;
  qemu_irq update_complete_irq;
  uint32_t screen_size;
};

#endif
