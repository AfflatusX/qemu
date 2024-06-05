#ifndef NEMA_H
#define NEMA_H

#include "qemu/osdep.h"

#include "hw/loader.h"
#include "qemu/module.h"
#include "ui/qemu-pixman.h"

// DEBUG OPTIONS

#define NEMA_DEBUG_CL 0
#define NEMA_DEBUG_MASK 0

// END DEBUG OPTIONS

#define NEMA_MAX_CMD_COUNT 250
#define NEMA_MEM_START 0x22000000
#define NEMA_IO_MEM_SIZE 0x1000
#define NEMA_FB_MEM_SIZE 2 * MiB
#define NEMA_CL_MEM_SIZE 0x4000

#define NEMA_CL_MEM_START NEMA_MEM_START + NEMA_FB_MEM_SIZE + NEMA_IO_MEM_SIZE

typedef struct {
  uint8_t op;
  uint32_t addr_a;
  uint32_t addr_b;

  uint8_t u_int_8_a;

  uint32_t u_int_a;
  uint32_t u_int_b;
  uint32_t u_int_c;

  int32_t int_a;
  int32_t int_b;
  int32_t int_c;
  int32_t int_d;
} nema_cmd_t;

typedef struct {
  nema_cmd_t list[NEMA_MAX_CMD_COUNT];
  uint8_t next_cmd_slot;
} nema_cmdlist_t;

#define TYPE_NEMA_DEVICE "nema"
OBJECT_DECLARE_SIMPLE_TYPE(NEMAState, NEMA_DEVICE)

#define NEMA_REG_OP 0x00

// synced with driver

typedef uint32_t nema_tex_format_t;
typedef uint8_t nema_tex_mode_t;
typedef uint8_t nema_blend_mode_t;

typedef enum { NEMA_NOTEX, NEMA_TEX0, NEMA_TEX1, NEMA_TEX2, NEMA_TEX3 } nema_tex_t;

#define NEMA_REG_OP 0x00
#define NEMA_REG_SIG 0X01

#define NEMA_REG_OP_SUBMIT_COMMAND 1
#define NEMA_REG_OP_SUBMIT_COMMAND_LIST 2
#define NEMA_REG_OP_SIGNAL_RECEIVED 3

#define NEMA_OP_BIND_TEX 1
#define NEMA_OP_SET_CLIP 2
#define NEMA_OP_SET_BLEND_BLIT 3
#define NEMA_OP_SET_BLEND_FILL 4
#define NEMA_OP_SET_CONST_COLOR 5
#define NEMA_OP_DRAW_LINE 6
#define NEMA_OP_BLIT 7
#define NEMA_OP_FILL_RECT 8
#define NEMA_OP_DRAW_RECT 9
#define NEMA_OP_FILL_RECT_ROUNDED 10
#define NEMA_OP_DRAW_RECT_ROUNDED 11

#define NEMA_FILTER_PS 0
#define NEMA_RGBA8888 0
#define NEMA_A8 1

#define NEMA_BL_SIMPLE 0x1
#define NEMA_BL_MASK 0x2
#define NEMA_BL_OPA 0x4

// =================

typedef struct {
  int x;
  int y;
  uint32_t w;
  uint32_t h;
} nema_region_t;

struct NEMAState {
  SysBusDevice parent_obj;
  MemoryRegion fb_mem;
  AddressSpace fb_mem_as;
  MemoryRegion io_mem;
  MemoryRegion cl_mem;
  AddressSpace cl_mem_as;

  AddressSpace system_itcm_as;

  qemu_irq inst_processed;
  uint8_t addr_a[4];
  uint8_t addr_b[4];
  nema_cmdlist_t cl;

  bool has_clip_set;
  pixman_region16_t pixman_clip_region;
  nema_region_t nema_clip_region;
};

#endif
