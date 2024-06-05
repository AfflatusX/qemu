/*
 * This represents a framebuffer based display, it should
 * be backed by a host memory device.
 */

#include "ram_disp.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/osdep.h"
#include "qom/object.h"
#include "slate_utils.h"
#include "sysemu/hostmem.h"
#include "ui/console.h"

#define RAM_DISPLAY_READY_SIGNAL 0x0
#define RAM_DISPLAY_WRITE_SIGNAL 0x1
#define RAM_DISPLAY_WRITE_OFFSET 0x1
#define RAM_DISPLAY_DATA_ADDRESS 0x21800000
#define RAM_DISPLAY_IO_ADDRESS 0x21900000
#define COLOR_FORMAT PIXMAN_b8g8r8a8

static void ram_fb_write(void* opaque, hwaddr addr, uint64_t val, unsigned size);
static uint64_t ram_fb_read(void* opaque, hwaddr addr, unsigned size);
static void update_display_surface(RamDisplayState* ds);
static void signal_display_update_complete(RamDisplayState* ds);

static void ram_fb_write(void* opaque, hwaddr addr, uint64_t val, unsigned size) {
  RamDisplayState* ds = opaque;

  if (addr == RAM_DISPLAY_WRITE_OFFSET && val == RAM_DISPLAY_WRITE_SIGNAL) {
    if (ds->is_updating) {
      return;
    }
    ds->is_updating = true;
    update_display_surface(ds);
  } else if (addr == RAM_DISPLAY_WRITE_OFFSET && val == RAM_DISPLAY_READY_SIGNAL) {
    qemu_irq_lower(ds->update_complete_irq);
  }
}

static uint64_t ram_fb_read(void* opaque, hwaddr addr, unsigned size) {
  return 0;
}

static void update_qemu_console(QemuConsole* con, RamDisplayState* ds) {
  if (ds->display_surface && ds->display_surface != qemu_console_surface(con)) {
    dpy_gfx_replace_surface(con, ds->display_surface);
  }
  /* simple full screen update */
  dpy_gfx_update_full(con);
}

static void display_update_callback(void* dev) {
  RamDisplayState* ds = RAM_DISPLAY_DEVICE(dev);
  update_qemu_console(ds->console, ds);
  signal_display_update_complete(ds);
}

static void signal_display_update_complete(RamDisplayState* ds) {
  if (ds->is_updating) {
    ds->is_updating = false;
    qemu_irq_raise(ds->update_complete_irq);
  }
}

static const MemoryRegionOps ram_fb_ops = {
    .read = ram_fb_read,
    .write = ram_fb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const GraphicHwOps console_ops = {
    .gfx_update = display_update_callback,
};

static void ramfb_unmap_display_surface(pixman_image_t* image, void* unused) {
  void* data = pixman_image_get_data(image);
  uint32_t size = pixman_image_get_stride(image) * pixman_image_get_height(image);
  cpu_physical_memory_unmap(data, size, 0, 0);
}

static void update_display_surface(RamDisplayState* ds) {
  if (ds->screen_size == 0) {
    error_report("screen_size not set");
    exit(EXIT_FAILURE);
  }
  void* data;
  hwaddr size, mapsize, linesize, stride;
  linesize = ds->screen_size * PIXMAN_FORMAT_BPP(COLOR_FORMAT) / 8;
  stride = linesize;
  mapsize = size = stride * (ds->screen_size - 1) + linesize;
  data = cpu_physical_memory_map(RAM_DISPLAY_DATA_ADDRESS, &mapsize, false);
  if (size != mapsize) {
    error_report("unable to memory map ram fb");
    exit(EXIT_FAILURE);
  }
  if (ds->display_surface == NULL) {
    ds->display_surface = qemu_create_displaysurface_from(
        ds->screen_size, ds->screen_size, COLOR_FORMAT, stride, data);
  } else {
    ds->display_surface->image = pixman_image_create_bits(
        COLOR_FORMAT, ds->screen_size, ds->screen_size, (void*)data, stride);
  }

  pixman_image_set_destroy_function(ds->display_surface->image, ramfb_unmap_display_surface, NULL);
}

static void create_virtual_framebuffer_device(RamDisplayState* ds) {
  const char* ram_device_name = "fb_ram";
  const char* ram_dev_id = g_strdup(ram_device_name);
  Object* o = object_resolve_path_type(ram_dev_id, TYPE_MEMORY_BACKEND, NULL);
  if (o == NULL) {
    error_report("memory backend object %s does not exist..", ram_device_name);
    exit(EXIT_FAILURE);
  }
  HostMemoryBackend* backend = MEMORY_BACKEND(o);
  MemoryRegion* region = host_memory_backend_get_memory(backend);

  host_memory_backend_set_mapped(backend, true);
  memory_region_add_subregion(get_system_memory(), RAM_DISPLAY_DATA_ADDRESS, region);
  ds->fb_data = region;
}

static void ram_display_realize_fn(Object* dev) {
  SysBusDevice* sbd = SYS_BUS_DEVICE(dev);
  RamDisplayState* ds = RAM_DISPLAY_DEVICE(dev);

  memory_region_init_io(&ds->fb_io, NULL, &ram_fb_ops, ds, "ram_fb_io", 0x40);
  memory_region_add_subregion(get_system_memory(), RAM_DISPLAY_IO_ADDRESS, &ds->fb_io);
  // create display device
  ds->console = graphic_console_init(NULL, 0, &console_ops, ds);
  ds->display_surface = NULL;

  create_virtual_framebuffer_device(ds);
  sysbus_init_irq(sbd, &ds->update_complete_irq);
}

static void ram_display_class_init_fn(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  dc->desc = "ram framebuffer display";
  dc->user_creatable = false;
  object_class_property_add(
      klass,
      "screensize",
      "uint32",
      get_uint32,
      set_uint32,
      NULL,
      (void*)OFFSET_OF(RamDisplayState, screen_size));
  object_class_property_set_description(
      klass, "screensize", "the display screen size of the machine.");
}

static const TypeInfo ram_display_info = {
    .name = TYPE_RAM_DISPLAY_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RamDisplayState),
    .instance_init = ram_display_realize_fn,
    .class_init = ram_display_class_init_fn,
};

static void ram_display_register_types(void) {
  type_register_static(&ram_display_info);
}

type_init(ram_display_register_types)
