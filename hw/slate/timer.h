#ifndef TIMER_H
#define TIMER_H

#include "qemu/osdep.h"

#include "hw/loader.h"
#include "qemu/module.h"

#define TYPE_TIMER_DEVICE "timer"
OBJECT_DECLARE_SIMPLE_TYPE(TimerState, TIMER_DEVICE)

#define HOST_TIME_OP_OFFSET 0
#define HOST_TIME_VALUE_OFFSET 1

#define HOST_TIME_OP_VALUE 1
#define HOST_TIME_OP_IRQ_RESET 2

struct TimerState {
  SysBusDevice parent_obj;
  MemoryRegion io_mem;
  qemu_irq read_ready_irq;
  int64_t time_ms;
  int64_t time_us;
};

#endif
