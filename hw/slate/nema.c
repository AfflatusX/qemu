#include <cairo.h>

#include "nema.h"

#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"

#include "math.h"

static void on_nema_io_write(void* opaque, hwaddr addr, uint64_t val, unsigned size);
static uint64_t on_nema_io_read(void* opaque, hwaddr addr, unsigned size);
static void cairo_apply_clip(cairo_t* cr, nema_region_t* region);

typedef uint8_t lv_opa_t;
typedef int16_t lv_coord_t;

#define COLOR_FACTOR (1.0 / 255.0)

typedef enum { BLEND_FILL, BLEND_BLIT } blend_option_t;

static const MemoryRegionOps nema_ops = {
    .read = on_nema_io_read,
    .write = on_nema_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

typedef struct {
  pixman_image_t* dest_tex;
  pixman_image_t* src_tex;
  pixman_image_t* mask_tex;
  cairo_surface_t* dest_surface;
  cairo_surface_t* src_surface;
  uint32_t const_color;
  bool const_color_set;
  nema_blend_mode_t blending_mode;
  blend_option_t blending_option;
  pixman_rectangle16_t rects[1];
} pipeline_ctx_t;

typedef struct {
  double r;
  double g;
  double b;
  double a;
} cairo_color_t;

#if NEMA_DEBUG_MASK

static void debug_mask(uint8_t* mask, uint16_t height, uint16_t width, uint16_t stride);

#endif

static cairo_color_t argb32_to_cairo_color(uint32_t color) {
  cairo_color_t c = {0};

  c.a = ((color >> 24) & 0xFF) / 255.0;
  c.r = ((color >> 16) & 0xFF) / 255.0;
  c.g = ((color >> 8) & 0xFF) / 255.0;
  c.b = ((color)&0xFF) / 255.0;

  return c;
}

static lv_opa_t* round_mask_if_needed(lv_opa_t* mask, lv_coord_t stride, lv_coord_t height) {
  if (stride % 4 == 0) {
    return NULL;
  }
  uint8_t padding = (4 - stride % 4) % 4;
  lv_opa_t* buf = g_malloc((stride + padding) * height);
  lv_opa_t* src_ptr = mask;
  lv_opa_t* dest_ptr = buf;
  for (uint16_t i = 0; i < height; i++) {
    memcpy(dest_ptr, src_ptr, stride);
    memset(dest_ptr + stride, 0, padding);
    src_ptr += stride;
    dest_ptr += (stride + padding);
  }
  return buf;
}

static void assert_not_null(void* ptr, const char* desc) {
  if (ptr == NULL) {
    error_report("ASSERT_NOT_NULL: %s failed.", desc);
    exit(EXIT_FAILURE);
  }
}

static void unmap_pixman_img(pixman_image_t* image, bool is_write) {
  void* data = pixman_image_get_data(image);
  uint32_t size = pixman_image_get_stride(image) * pixman_image_get_height(image);
  if (is_write) {
    cpu_physical_memory_unmap(data, 0, is_write, is_write ? size : 0);
  } else {
    g_free(data);
  }
}

static void unmap_pixman_imgs(pipeline_ctx_t* ctx) {
  if (ctx->dest_tex != NULL) {
    unmap_pixman_img(ctx->dest_tex, true);
    ctx->dest_tex = NULL;
  }

  if (ctx->mask_tex != NULL) {
    unmap_pixman_img(ctx->mask_tex, false);
    ctx->mask_tex = NULL;
  }

  if (ctx->src_tex != NULL) {
    unmap_pixman_img(ctx->src_tex, false);
    ctx->src_tex = NULL;
  }
}

static void clean_up_ctx(pipeline_ctx_t* ctx) {
  unmap_pixman_imgs(ctx);
  if (ctx->dest_surface != NULL) {
    cairo_surface_destroy(ctx->dest_surface);
  }
  if (ctx->src_surface != NULL) {
    cairo_surface_destroy(ctx->src_surface);
  }
  memset(ctx, 0, sizeof(pipeline_ctx_t));
}

static void validate_ctx(NEMAState* ds, pipeline_ctx_t* ctx, const char* desc) {
  nema_blend_mode_t blend_mode = ctx->blending_mode;

  if (!ds->has_clip_set) {
    error_report("%s: clip area not set.", desc);
    goto fail;
  }

  if (ctx->dest_tex == NULL) {
    error_report("dest_tex not set.");
    goto fail;
  }

  if (ctx->dest_surface == NULL) {
    error_report("dest_surface not set.");
    goto fail;
  }

  if (blend_mode & NEMA_BL_MASK) {
    if (ctx->mask_tex == NULL) {
      error_report("NEMA_BL_MASK is on, mask_tex not set.");
      goto fail;
    }
  } else {
    if (ctx->mask_tex != NULL) {
      error_report("NEMA_BL_MASK is off, mask_tex is set.");
      goto fail;
    }
  }

  if (blend_mode & NEMA_BL_OPA) {
    if (!ctx->const_color_set) {
      error_report("NEMA_BL_OPA is on, const color not set.");
      goto fail;
    }
  } else {
    if (ctx->const_color_set) {
      error_report("NEMA_BL_OPA is off, const color is set.");
      goto fail;
    }
  }
  return;
fail:
  exit(EXIT_FAILURE);
}

static void cairo_prepare_source(cairo_t* cr, pipeline_ctx_t* ctx, cairo_color_t color) {
  if (ctx->mask_tex != NULL) {
    error_report("cairo + mask is not implemented.");
    exit(EXIT_FAILURE);
  }
  // for color only ones simply use set_source_rgba and multiply the opacity
  if (!(ctx->blending_mode & NEMA_BL_OPA)) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    return;
  }
  double opacity = ((ctx->const_color >> 24) & 0xFF) / 255.0;
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a * opacity);
}

static void blend_opacity_if_needed(NEMAState* ds, pipeline_ctx_t* ctx) {
  if (!(ctx->blending_mode & NEMA_BL_OPA)) {
    return;
  }
  uint8_t opacity = (ctx->const_color >> 24) & 0xFF;
  if (ctx->mask_tex == NULL) {
    pixman_image_t* dest_tex = ctx->dest_tex;
    if (dest_tex == NULL) {
      error_report("dest_tex is NULL");
      exit(EXIT_FAILURE);
    }
    int dest_width = pixman_image_get_width(dest_tex);
    int dest_height = pixman_image_get_height(dest_tex);

    // create a mask_tex using the alpha channel of const color
    int16_t stride = dest_width % 4 == 0 ? dest_width : ((dest_width >> 2) + 1) << 2;
    uint32_t size = dest_height * stride;
    uint8_t* mask_data = g_malloc(size);
    memset(mask_data, opacity, size);
    ctx->mask_tex =
        pixman_image_create_bits(PIXMAN_a8, dest_width, dest_height, (void*)mask_data, stride);
  } else {
    // multiply existing mask with the alpha channel
    uint32_t size = pixman_image_get_stride(ctx->mask_tex) * pixman_image_get_height(ctx->mask_tex);
    uint8_t* data = (uint8_t*)pixman_image_get_data(ctx->mask_tex);
    for (uint32_t i = 0; i < size; i++) {
      data[i] = (uint32_t)((uint32_t)data[i] * opacity) >> 8;
    }
  }
}

static void execute_fill_rect(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  validate_ctx(ds, ctx, "execute_fill_rect");

  uint32_t color = cmd.u_int_c;
  PixelFormat pf = qemu_pixelformat_from_pixman(PIXMAN_a8r8g8b8);
  pixman_color_t fill_color = qemu_pixman_color(&pf, color);

  pixman_image_t* solid_fill = pixman_image_create_solid_fill(&fill_color);

  blend_opacity_if_needed(ds, ctx);
  pixman_image_set_clip_region(ctx->dest_tex, &ds->pixman_clip_region);
  pixman_image_composite(
      PIXMAN_OP_OVER,
      solid_fill,
      ctx->mask_tex,
      ctx->dest_tex,
      0,
      0,
      0,
      0,
      cmd.int_a,
      cmd.int_b,
      cmd.u_int_a,
      cmd.u_int_b);
}

static void execute_fill_rect_rounded(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  cairo_color_t color = argb32_to_cairo_color(cmd.u_int_c);
  cairo_t* cr = cairo_create(ctx->dest_surface);

  double x = (double)cmd.int_a, y = (double)cmd.int_b, width = (double)cmd.u_int_a,
         height = (double)cmd.u_int_b, aspect = width / height, corner_radius = (double)cmd.int_c;

  double radius = corner_radius / aspect;
  double degrees = M_PI / 180.0;

  cairo_apply_clip(cr, &ds->nema_clip_region);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path(cr);

  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

  cairo_fill(cr);
  cairo_destroy(cr);
}

static void execute_draw_rect(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  cairo_color_t color = argb32_to_cairo_color(cmd.u_int_c);
  cairo_t* cr = cairo_create(ctx->dest_surface);

  double x = (double)cmd.int_a, y = (double)cmd.int_b, width = (double)cmd.u_int_a,
         height = (double)cmd.u_int_b, aspect = width / height;

  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

  cairo_stroke(cr);
  cairo_destroy(cr);
}

static void cairo_apply_clip(cairo_t* cr, nema_region_t* region) {
  cairo_rectangle(cr, region->x, region->y, region->w, region->h);
  cairo_clip(cr);
}

static void execute_draw_rect_rounded(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  cairo_color_t color = argb32_to_cairo_color(cmd.u_int_c);
  cairo_t* cr = cairo_create(ctx->dest_surface);

  double x = (double)cmd.int_a, y = (double)cmd.int_b, width = (double)cmd.u_int_a,
         height = (double)cmd.u_int_b, aspect = width / height, corner_radius = (double)cmd.int_c;

  double radius = corner_radius / aspect;
  double degrees = M_PI / 180.0;

  cairo_apply_clip(cr, &ds->nema_clip_region);

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path(cr);

  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

  cairo_stroke(cr);
  cairo_destroy(cr);
}

static void execute_draw_line(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  // use cairo to draw a line onto the framebuffer
  cairo_color_t color = argb32_to_cairo_color(cmd.u_int_b);
  cairo_t* cr = cairo_create(ctx->dest_surface);

  cairo_apply_clip(cr, &ds->nema_clip_region);
  cairo_prepare_source(cr, ctx, color);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, cmd.u_int_a);
  cairo_move_to(cr, cmd.int_a, cmd.int_b);
  cairo_line_to(cr, cmd.int_c, cmd.int_d);
  cairo_stroke(cr);

  cairo_destroy(cr);
}

static void execute_blit(NEMAState* ds, pipeline_ctx_t* ctx) {
  validate_ctx(ds, ctx, "execute_blit");

  assert_not_null(ctx->dest_tex, "dest_tex");
  assert_not_null(ctx->src_tex, "src_tex");

  blend_opacity_if_needed(ds, ctx);
  pixman_image_set_clip_region(ctx->dest_tex, &ds->pixman_clip_region);
  pixman_image_composite(
      PIXMAN_OP_OVER,
      ctx->src_tex,
      ctx->mask_tex,
      ctx->dest_tex,
      0,
      0,
      0,
      0,
      0,
      0,
      pixman_image_get_width(ctx->dest_tex),
      pixman_image_get_height(ctx->dest_tex));
}

static void* read_tex(
    NEMAState* ds,
    pipeline_ctx_t* ctx,
    nema_cmd_t cmd,
    pixman_image_t** image,
    pixman_format_code_t format,
    bool is_write) {
  hwaddr expected_len, read_len;
  bool allocated_mem = false;
  void* data;

  expected_len = cmd.int_a * cmd.u_int_b;
  read_len = expected_len;
  if (is_write) {
    data = cpu_physical_memory_map(cmd.addr_a, &read_len, is_write);
    if (expected_len != read_len || data == NULL) {
      error_report(
          "width, %d, height: %d, stride %d, addr: 0x%x\n",
          cmd.u_int_a,
          cmd.u_int_b,
          cmd.int_a,
          cmd.addr_a);
      error_report(
          "TEX mem map failed - expected: %llu, actual: %llu, address: 0x%x\n",
          expected_len,
          read_len,
          cmd.addr_a);
      exit(EXIT_FAILURE);
    }
  } else {
    data = g_malloc(expected_len);
    allocated_mem = true;
    if (cmd.addr_a > 0x10000000 && cmd.addr_a < 0x20000000) {
      // use itcm address space since this is in the flash region
      MemTxResult res = address_space_rw(
          &ds->system_itcm_as,
          cmd.addr_a - 0x10000000,
          MEMTXATTRS_UNSPECIFIED,
          data,
          expected_len,
          false);
      if (res != 0) {
        error_report("read from itcm addr: 0x%x, res: %d\n", cmd.addr_a, res);
        exit(EXIT_FAILURE);
      }
    } else {
      cpu_physical_memory_read(cmd.addr_a, data, expected_len);
    }
  }

  int32_t stride = cmd.int_a;

  if (format == PIXMAN_a8) {
    // this is mask, see if we need to round the mask first
    lv_opa_t* rounded_mask = round_mask_if_needed(data, stride, cmd.u_int_b);
    if (rounded_mask != NULL) {
      if (allocated_mem) {
        g_free(data);
      }
      data = rounded_mask;
      uint8_t padding = 4 - stride % 4;
      stride += padding;
    }
  }

  *image = pixman_image_create_bits(format, cmd.u_int_a, cmd.u_int_b, data, stride);
  return data;
}

static void process_cmd(NEMAState* ds, nema_cmd_t cmd, pipeline_ctx_t* ctx) {
  pixman_image_t** image;
  void* data;
  switch (cmd.op) {
    case NEMA_OP_BIND_TEX:
      switch (cmd.u_int_8_a) {
        case NEMA_TEX0:
          image = &ctx->dest_tex;
          data = read_tex(ds, ctx, cmd, image, PIXMAN_a8r8g8b8, true);
          ctx->dest_surface = cairo_image_surface_create_for_data(
              data, CAIRO_FORMAT_ARGB32, cmd.u_int_a, cmd.u_int_b, cmd.int_a);
          break;
        case NEMA_TEX1:
          image = &ctx->src_tex;
          read_tex(ds, ctx, cmd, image, PIXMAN_a8r8g8b8, false);
          break;
        case NEMA_TEX3:
          image = &ctx->mask_tex;
          read_tex(ds, ctx, cmd, image, PIXMAN_a8, false);
#if NEMA_DEBUG_MASK
          debug_mask(, cmd.u_int_a, cmd.u_int_b, cmd.int_a);
#endif
          break;
        default:
          error_report("invalid tex id");
          exit(EXIT_FAILURE);
      }
      break;

    case NEMA_OP_SET_CLIP:
      pixman_region_init_rect(
          &ds->pixman_clip_region, cmd.int_a, cmd.int_b, cmd.u_int_a, cmd.u_int_b);
      ds->nema_clip_region.x = cmd.int_a;
      ds->nema_clip_region.y = cmd.int_b;
      ds->nema_clip_region.w = cmd.u_int_a;
      ds->nema_clip_region.h = cmd.u_int_b;
      ds->has_clip_set = true;
      break;
    case NEMA_OP_SET_BLEND_FILL:
      ctx->blending_option = BLEND_FILL;
      ctx->blending_mode = cmd.u_int_a;
      break;
    case NEMA_OP_SET_BLEND_BLIT:
      ctx->blending_option = BLEND_BLIT;
      ctx->blending_mode = cmd.u_int_a;
      break;
    case NEMA_OP_BLIT:
      execute_blit(ds, ctx);
      clean_up_ctx(ctx);
      break;
    case NEMA_OP_FILL_RECT:
      execute_fill_rect(ds, cmd, ctx);
      clean_up_ctx(ctx);
      break;
    case NEMA_OP_SET_CONST_COLOR:
      ctx->const_color = cmd.u_int_a;
      ctx->const_color_set = true;
      break;
    case NEMA_OP_DRAW_LINE:
      execute_draw_line(ds, cmd, ctx);
      clean_up_ctx(ctx);
      break;
    case NEMA_OP_FILL_RECT_ROUNDED:
      execute_fill_rect_rounded(ds, cmd, ctx);
      clean_up_ctx(ctx);
      break;
    case NEMA_OP_DRAW_RECT_ROUNDED:
      execute_draw_rect_rounded(ds, cmd, ctx);
      clean_up_ctx(ctx);
      break;
    case NEMA_OP_DRAW_RECT:
      execute_draw_rect(ds, cmd, ctx);
      clean_up_ctx(ctx);
      break;
    default:
      error_report("unsupported command: %d\n", cmd.op);
      exit(EXIT_FAILURE);
      break;
  }
}

static void process_cl(NEMAState* ds, nema_cmdlist_t* cl) {
  // int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
  pipeline_ctx_t ctx = {0};
  for (uint8_t i = 0; i < cl->next_cmd_slot; i++) {
    process_cmd(ds, cl->list[i], &ctx);
  }
  // int64_t end_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
  // printf("HOST CL: %d -> %lld ms\n", cl->next_cmd_slot, end_time - start_time);
  qemu_irq_raise(ds->inst_processed);
}

#if NEMA_DEBUG_MASK

static void debug_mask(uint8_t* mask, uint16_t width, uint16_t height, uint16_t stride) {
  uint8_t* ptr = mask;
  printf("============ start debug ===============\n");
  printf("w: %d, h: %d, stride: %d\n", width, height, stride);

  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      uint8_t color = *(ptr + j);
      printf("%02x ", color);
    }
    printf("\n");
    ptr += stride;
  }
  printf("============ ended debug ===============\n");
}

#endif

#if NEMA_DEBUG_CL

static void trace_cmd(nema_cmd_t cmd, uint8_t index) {
  printf("=============== CMD %d ================\n", index);
  printf("op: %d\n", cmd.op);
  printf("addr_a: 0x%x\n", cmd.addr_a);
  printf("addr_b: 0x%x\n", cmd.addr_b);
  printf("u_int_8_a: %d\n", cmd.u_int_8_a);
  printf("u_int_a: %u\n", cmd.u_int_a);
  printf("u_int_b: %u\n", cmd.u_int_b);
  printf("u_int_c: %u\n", cmd.u_int_c);
  printf("int_a: %d\n", cmd.int_a);
  printf("int_b: %d\n", cmd.int_b);
  fflush(stdout);
}

static void trace_cl(nema_cmdlist_t* cl) {
  printf("=============== V CL %d ================\n", cl->next_cmd_slot);
  for (uint8_t i = 0; i < cl->next_cmd_slot; i++) {
    trace_cmd(cl->list[i], i);
  }
  printf("=============== END CL ================\n");
}

#endif

static void on_nema_io_write(void* opaque, hwaddr offset, uint64_t val, unsigned size) {
  NEMAState* ds = opaque;

  if (offset == NEMA_REG_OP && val == NEMA_REG_OP_SUBMIT_COMMAND_LIST) {
    hwaddr len = sizeof(nema_cmdlist_t);
    nema_cmdlist_t* cl = cpu_physical_memory_map(NEMA_CL_MEM_START, &len, false);
    if (len != sizeof(nema_cmdlist_t)) {
      error_report("failed to load cl data from phy mem.\n");
      exit(EXIT_FAILURE);
    }
#if NEMA_DEBUG_CL
    trace_cl(cl);
#endif
    process_cl(ds, cl);
    // uint32_t ts_end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    // printf("nema tick: %d ms\n", ts_end - ts_start);
  } else if (offset == NEMA_REG_SIG && val == NEMA_REG_OP_SIGNAL_RECEIVED) {
    qemu_irq_lower(ds->inst_processed);
  }
}

static uint64_t on_nema_io_read(void* opaque, hwaddr addr, unsigned size) {
  return 0;
}
static void realize_fn(Object* dev) {
  SysBusDevice* sbd = SYS_BUS_DEVICE(dev);
  NEMAState* ds = NEMA_DEVICE(dev);

  memory_region_init_io(&ds->io_mem, dev, &nema_ops, ds, "nema-io", NEMA_IO_MEM_SIZE);
  sysbus_init_mmio(sbd, &ds->io_mem);
  sysbus_init_irq(sbd, &ds->inst_processed);
  memory_region_init_ram(&ds->fb_mem, dev, "nema-fb", NEMA_FB_MEM_SIZE, &error_fatal);
  address_space_init(&ds->fb_mem_as, &ds->fb_mem, "nema-fb-as");

  if (sizeof(nema_cmdlist_t) > NEMA_CL_MEM_SIZE) {
    error_report("sizeof(nema_cmdlist_t) > NEMA_CL_MEM_SIZE\n");
    exit(EXIT_FAILURE);
  }

  printf("nema_cmdlist_t size: %lu bytes\n", sizeof(nema_cmdlist_t));

  memory_region_init_ram(&ds->cl_mem, dev, "nema-cl", NEMA_CL_MEM_SIZE, &error_fatal);
  address_space_init(&ds->cl_mem_as, &ds->cl_mem, "nema-cl-as");
}

static void class_init_fn(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  dc->desc = "NEMA GPU";
  dc->user_creatable = false;
}

static const TypeInfo nema_info = {
    .name = TYPE_NEMA_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NEMAState),
    .instance_init = realize_fn,
    .class_init = class_init_fn,
};

static void ram_display_register_types(void) {
  type_register_static(&nema_info);
}

type_init(ram_display_register_types)
