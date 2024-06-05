#include "timer.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"

#define IO_MEM_START_ADDR 22200000

static void on_timer_io_write(void* opaque, hwaddr addr, uint64_t val, unsigned size);
static uint64_t on_timer_io_read(void* opaque, hwaddr addr, unsigned size);

typedef enum { BLEND_FILL, BLEND_BLIT } blend_option_t;

static const MemoryRegionOps timer_ops = {
    .read = on_timer_io_read,
    .write = on_timer_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void on_timer_io_write(void* opaque, hwaddr offset, uint64_t val, unsigned size) {}

static uint64_t on_timer_io_read(void* opaque, hwaddr addr, unsigned size) {
  TimerState* ds = opaque;

  if (addr == HOST_TIME_VALUE_OFFSET + 7) {
    ds->time_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
  }

  if (addr >= HOST_TIME_VALUE_OFFSET && addr < HOST_TIME_VALUE_OFFSET + 8) {
    uint8_t offset = addr - HOST_TIME_VALUE_OFFSET;
    uint8_t val = (ds->time_ms >> offset * 8) & 0xFF;
    return val;
  }

  if (addr == HOST_TIME_VALUE_OFFSET + 15) {
    ds->time_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
  }

  if (addr >= HOST_TIME_VALUE_OFFSET + 8 && addr < HOST_TIME_VALUE_OFFSET + 16) {
    uint8_t offset = addr - HOST_TIME_VALUE_OFFSET - 8;
    uint8_t val = (ds->time_us >> offset * 8) & 0xFF;
    return val;
  }
  return 0;
}

static void realize_fn(Object* dev) {
  SysBusDevice* sbd = SYS_BUS_DEVICE(dev);
  TimerState* ds = TIMER_DEVICE(dev);

  memory_region_init_io(&ds->io_mem, dev, &timer_ops, ds, "timer-io", 0x100);
  sysbus_init_mmio(sbd, &ds->io_mem);
  sysbus_init_irq(sbd, &ds->read_ready_irq);
}

static void class_init_fn(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  dc->desc = "host Timer";
  dc->user_creatable = false;
}

static const TypeInfo ram_display_info = {
    .name = TYPE_TIMER_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TimerState),
    .instance_init = realize_fn,
    .class_init = class_init_fn,
};

static void ram_display_register_types(void) {
  type_register_static(&ram_display_info);
}

type_init(ram_display_register_types)
