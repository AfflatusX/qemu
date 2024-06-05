/*
 * This represents the Kyoto virtual board. Currently this is for
 * experiement and learning purpose.
 */

#include "qemu/osdep.h"

#include "exec/address-spaces.h"
#include "hw/arm/armsse.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/char/cmsdk-apb-uart.h"
#include "hw/i2c/arm_sbcon_i2c.h"
#include "hw/misc/mps2-fpgaio.h"
#include "hw/misc/mps2-scc.h"
#include "hw/misc/tz-mpc.h"
#include "hw/misc/tz-msc.h"
#include "hw/misc/unimp.h"
#include "hw/net/lan9118.h"
#include "hw/qdev-clock.h"
#include "hw/rtc/pl031.h"
#include "hw/ssi/pl022.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"

#include "nema.h"
#include "ram_disp.h"
#include "slate_utils.h"
#include "timer.h"

#define TYPE_SLATE_MACHINE MACHINE_TYPE_NAME("slate")
#define IS_ALIAS 1
#define IS_ROM 2
#define MPS3_DDR_SIZE (128 * MiB)
#define S32KCLK_FRQ (32 * 1000)
#define REF_CLK_FRQ (100 * 1000)
#define MPS2TZ_RAM_MAX 5

/*
 * Define the layout of RAM in a board, including which parts are
 * behind which MPCs.
 * mrindex specifies the index into mms->ram[] to use for the backing RAM;
 * -1 means "use the system RAM".
 */
typedef struct RAMInfo {
  const char* name;
  uint32_t base;
  uint32_t size;
  int mpc; /* MPC number, -1 for "not behind an MPC" */
  int mrindex;
  int flags;
} RAMInfo;

static const uint32_t an524_oscclk[] = {
    24000000,
    32000000,
    50000000,
    50000000,
    24576000,
    23750000,
};

static const RAMInfo slate_raminfo[] = {
    {
        .name = "sram",
        .base = 0x01000000,
        .size = 2 * MiB,
        .mpc = 0,
        .mrindex = 1,
    },
    {
        .name = "sram 2",
        .base = 0x21000000,
        .size = 4 * MiB,
        .mpc = -1,
        .mrindex = 3,
    },
    {
        /* We don't model QSPI flash yet; for now expose it as simple ROM */
        .name = "QSPI",
        .base = 0x28000000,
        .size = 8 * MiB,
        .mpc = 1,
        .mrindex = 4,
        .flags = IS_ROM,
    },
    {
        .name = "DDR",
        .base = 0x60000000,
        .size = MPS3_DDR_SIZE,
        .mpc = 2,
        .mrindex = -1,
    },
    {
        .name = NULL,
    },
};

struct SlateMachineClass {
  MachineClass parent;

  bool fpgaio_has_dbgctrl; /* Does FPGAIO have DBGCTRL register? */
  bool fpgaio_has_switches; /* Does FPGAIO have SWITCH register? */
  const RAMInfo* raminfo;
  const uint32_t* oscclk;
  int uart_overflow_irq; /* number of the combined UART overflow IRQ */
  uint32_t apb_periph_frq;
  uint32_t fpgaio_num_leds; /* Number of LEDs in FPGAIO LED0 register */
  uint32_t len_oscclk;
  uint32_t scc_id;
  uint32_t sysclk_frq;
  uint32_t boot_ram_size;
};

struct SlateMachineState {
  MachineState parent;
  ARMv7MState armv7m;
  Clock* sysclk;
  Clock* refclk;
  Clock* s32kclk;
  ARMSSE iotkit;
  SplitIRQ sec_resp_splitter;
  TZPPC ppc[5];
  TZMSC msc[4];
  TZMPC mpc[3];
  qemu_or_irq uart_irq_orgate;
  CMSDKAPBUART uart[6];
  ArmSbconI2CState i2c[5];
  PL022State spi[5];
  MPS2SCC scc;
  MPS2FPGAIO fpgaio;
  UnimplementedDeviceState i2s_audio;
  DeviceState* lan9118;
  UnimplementedDeviceState cldc;
  UnimplementedDeviceState gpio[4];
  PL031State rtc;
  MemoryRegion eth_usb_container;
  UnimplementedDeviceState usb;
  MemoryRegion ram[MPS2TZ_RAM_MAX];

  MemoryRegion nema_gpu_container;
  RamDisplayState ram_disp;
  TimerState host_timer;
  NEMAState nema_gpu;
  uint32_t screen_size;
};

OBJECT_DECLARE_TYPE(SlateMachineState, SlateMachineClass, SLATE_MACHINE)

typedef union PPCExtraData {
  bool i2c_internal;
} PPCExtraData;

typedef MemoryRegion* MakeDevFn(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata);

typedef struct PPCPortInfo {
  const char* name;
  MakeDevFn* devfn;
  void* opaque;
  hwaddr addr;
  hwaddr size;
  int irqs[3]; /* currently no device needs more IRQ lines than this */
  PPCExtraData extradata; /* to pass device-specific info to the devfn */
} PPCPortInfo;

typedef struct PPCInfo {
  const char* name;
  PPCPortInfo ports[TZ_NUM_PORTS];
} PPCInfo;

// static definitions
static void set_default_ram_info(SlateMachineClass* mmc);
static const RAMInfo* find_raminfo_for_mpc(SlateMachineState* mms, int mpc);
static void make_ram_alias(MemoryRegion* mr, const char* name, MemoryRegion* orig, hwaddr base);
static MemoryRegion* mr_for_raminfo(SlateMachineState* mms, const RAMInfo* raminfo);

static void set_default_ram_info(SlateMachineClass* mmc) {
  /*
   * Set mc->default_ram_size and default_ram_id from the
   * information in mmc->raminfo.
   */
  MachineClass* mc = MACHINE_CLASS(mmc);
  const RAMInfo* p;

  for (p = mmc->raminfo; p->name; p++) {
    if (p->mrindex < 0) {
      /* Found the entry for "system memory" */
      mc->default_ram_size = p->size;
      mc->default_ram_id = p->name;
      return;
    }
  }
  g_assert_not_reached();
}

static const RAMInfo* find_raminfo_for_mpc(SlateMachineState* mms, int mpc) {
  SlateMachineClass* mmc = SLATE_MACHINE_GET_CLASS(mms);
  const RAMInfo* p;
  const RAMInfo* found = NULL;

  for (p = mmc->raminfo; p->name; p++) {
    if (p->mpc == mpc && !(p->flags & IS_ALIAS)) {
      /* There should only be one entry in the array for this MPC */
      g_assert(!found);
      found = p;
    }
  }
  /* if raminfo array doesn't have an entry for each MPC this is a bug */
  assert(found);
  return found;
}

/* Create an alias of an entire original MemoryRegion @orig
 * located at @base in the memory map.
 */
static void make_ram_alias(MemoryRegion* mr, const char* name, MemoryRegion* orig, hwaddr base) {
  memory_region_init_alias(mr, NULL, name, orig, 0, memory_region_size(orig));
  memory_region_add_subregion(get_system_memory(), base, mr);
}

static qemu_irq get_sse_irq_in(SlateMachineState* mms, int irqno) {
  /*
   * Convert from "CPU irq number" (as listed in the FPGA image
   * documentation) to the SSE external-interrupt number.
   */
  irqno -= 32;
  return qdev_get_gpio_in_named(DEVICE(&mms->iotkit), "EXP_IRQ", irqno);
}

static void create_non_mpc_ram(SlateMachineState* mms) {
  /*
   * Handle the RAMs which are either not behind MPCs or which are
   * aliases to another MPC.
   */
  const RAMInfo* p;
  SlateMachineClass* mmc = SLATE_MACHINE_GET_CLASS(mms);

  for (p = mmc->raminfo; p->name; p++) {
    if (p->flags & IS_ALIAS) {
      SysBusDevice* mpc_sbd = SYS_BUS_DEVICE(&mms->mpc[p->mpc]);
      MemoryRegion* upstream = sysbus_mmio_get_region(mpc_sbd, 1);
      make_ram_alias(&mms->ram[p->mrindex], p->name, upstream, p->base);
    } else if (p->mpc == -1) {
      /* RAM not behind an MPC */
      MemoryRegion* mr = mr_for_raminfo(mms, p);
      memory_region_add_subregion(get_system_memory(), p->base, mr);
    }
  }
}

static MemoryRegion* mr_for_raminfo(SlateMachineState* mms, const RAMInfo* raminfo) {
  /* Return an initialized MemoryRegion for the RAMInfo. */
  MemoryRegion* ram;

  if (raminfo->mrindex < 0) {
    /* Means this RAMInfo is for QEMU's "system memory" */
    MachineState* machine = MACHINE(mms);
    assert(!(raminfo->flags & IS_ROM));
    return machine->ram;
  }

  ram = &mms->ram[raminfo->mrindex];

  memory_region_init_ram(ram, NULL, raminfo->name, raminfo->size, &error_fatal);
  if (raminfo->flags & IS_ROM) {
    memory_region_set_readonly(ram, true);
  }
  return ram;
}

static uint32_t boot_ram_size(SlateMachineState* mms) {
  /* Return the size of the RAM block at guest address zero */
  const RAMInfo* p;
  SlateMachineClass* mmc = SLATE_MACHINE_GET_CLASS(mms);

  /*
   * Use a per-board specification (for when the boot RAM is in
   * the SSE and so doesn't have a RAMInfo list entry)
   */
  if (mmc->boot_ram_size) {
    return mmc->boot_ram_size;
  }

  for (p = mmc->raminfo; p->name; p++) {
    if (p->base == 0) {
      return p->size;
    }
  }
  g_assert_not_reached();
}

static MemoryRegion* make_eth_usb(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  /*
   * The AN524 makes the ethernet and USB share a PPC port.
   * irqs[] is the ethernet IRQ.
   */
  SysBusDevice* s;
  NICInfo* nd = &nd_table[0];

  memory_region_init(&mms->eth_usb_container, OBJECT(mms), "mps2-tz-eth-usb-container", 0x200000);

  /*
   * In hardware this is a LAN9220; the LAN9118 is software compatible
   * except that it doesn't support the checksum-offload feature.
   */
  qemu_check_nic_model(nd, "lan9118");
  mms->lan9118 = qdev_new(TYPE_LAN9118);
  qdev_set_nic_properties(mms->lan9118, nd);

  s = SYS_BUS_DEVICE(mms->lan9118);
  sysbus_realize_and_unref(s, &error_fatal);
  sysbus_connect_irq(s, 0, get_sse_irq_in(mms, irqs[0]));

  memory_region_add_subregion(&mms->eth_usb_container, 0, sysbus_mmio_get_region(s, 0));

  /* The USB OTG controller is an ISP1763; we don't have a model of it. */
  object_initialize_child(OBJECT(mms), "usb-otg", &mms->usb, TYPE_UNIMPLEMENTED_DEVICE);
  qdev_prop_set_string(DEVICE(&mms->usb), "name", "usb-otg");
  qdev_prop_set_uint64(DEVICE(&mms->usb), "size", 0x100000);
  s = SYS_BUS_DEVICE(&mms->usb);
  sysbus_realize(s, &error_fatal);

  memory_region_add_subregion(&mms->eth_usb_container, 0x100000, sysbus_mmio_get_region(s, 0));

  return &mms->eth_usb_container;
}

static void init_ram_disp(SlateMachineState* mms) {
  SysBusDevice* s;
  object_initialize_child(OBJECT(mms), "ram-disp", &mms->ram_disp, TYPE_RAM_DISPLAY_DEVICE);
  s = SYS_BUS_DEVICE(&mms->ram_disp);
  qdev_prop_set_uint32(DEVICE(&mms->ram_disp), "screensize", mms->screen_size);
  sysbus_realize_and_unref(s, &error_fatal);
  sysbus_connect_irq(s, 0, get_sse_irq_in(mms, 50));
}

static void init_host_timer(SlateMachineState* mms) {
  SysBusDevice* s;
  object_initialize_child(OBJECT(mms), "host-timer", &mms->host_timer, TYPE_TIMER_DEVICE);
  s = SYS_BUS_DEVICE(&mms->host_timer);
  sysbus_realize_and_unref(s, &error_fatal);
  sysbus_connect_irq(s, 0, get_sse_irq_in(mms, 57));

  memory_region_add_subregion(get_system_memory(), 0x22300000, &mms->host_timer.io_mem);
}

static void init_nema_gpu(SlateMachineState* mms) {
  SysBusDevice* s;

  memory_region_init(
      &mms->nema_gpu_container,
      OBJECT(mms),
      "nema-gpu-container",
      NEMA_IO_MEM_SIZE + NEMA_FB_MEM_SIZE + NEMA_CL_MEM_SIZE);
  memory_region_add_subregion(get_system_memory(), NEMA_MEM_START, &mms->nema_gpu_container);

  object_initialize_child(OBJECT(mms), "nema-gpu", &mms->nema_gpu, TYPE_NEMA_DEVICE);
  s = SYS_BUS_DEVICE(&mms->nema_gpu);
  sysbus_realize_and_unref(s, &error_fatal);
  sysbus_connect_irq(s, 0, get_sse_irq_in(mms, 56));
  memory_region_add_subregion(&mms->nema_gpu_container, 0, &mms->nema_gpu.io_mem);
  memory_region_add_subregion(&mms->nema_gpu_container, NEMA_IO_MEM_SIZE, &mms->nema_gpu.fb_mem);
  memory_region_add_subregion(
      &mms->nema_gpu_container, NEMA_IO_MEM_SIZE + NEMA_FB_MEM_SIZE, &mms->nema_gpu.cl_mem);
}

static MemoryRegion* make_rtc(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  PL031State* pl031 = opaque;
  SysBusDevice* s;

  object_initialize_child(OBJECT(mms), name, pl031, TYPE_PL031);
  s = SYS_BUS_DEVICE(pl031);
  sysbus_realize(s, &error_fatal);
  /*
   * The board docs don't give an IRQ number for the PL031, so
   * presumably it is not connected.
   */
  return sysbus_mmio_get_region(s, 0);
}

static MemoryRegion* make_fpgaio(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  MPS2FPGAIO* fpgaio = opaque;
  SlateMachineClass* mmc = SLATE_MACHINE_GET_CLASS(mms);

  object_initialize_child(OBJECT(mms), "fpgaio", fpgaio, TYPE_MPS2_FPGAIO);
  qdev_prop_set_uint32(DEVICE(fpgaio), "num-leds", mmc->fpgaio_num_leds);
  qdev_prop_set_bit(DEVICE(fpgaio), "has-switches", mmc->fpgaio_has_switches);
  qdev_prop_set_bit(DEVICE(fpgaio), "has-dbgctrl", mmc->fpgaio_has_dbgctrl);
  sysbus_realize(SYS_BUS_DEVICE(fpgaio), &error_fatal);
  return sysbus_mmio_get_region(SYS_BUS_DEVICE(fpgaio), 0);
}

static MemoryRegion* make_scc(
    SlateMachineState* ms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  MPS2SCC* scc = opaque;
  DeviceState* sccdev;
  SlateMachineClass* mc = SLATE_MACHINE_GET_CLASS(ms);
  uint32_t i;

  object_initialize_child(OBJECT(ms), "scc", scc, TYPE_MPS2_SCC);
  sccdev = DEVICE(scc);
  qdev_prop_set_uint32(sccdev, "scc-cfg0", 0);
  qdev_prop_set_uint32(sccdev, "scc-cfg4", 0x2);
  qdev_prop_set_uint32(sccdev, "scc-aid", 0x00200008);
  qdev_prop_set_uint32(sccdev, "scc-id", mc->scc_id);
  qdev_prop_set_uint32(sccdev, "len-oscclk", mc->len_oscclk);
  for (i = 0; i < mc->len_oscclk; i++) {
    g_autofree char* propname = g_strdup_printf("oscclk[%u]", i);
    qdev_prop_set_uint32(sccdev, propname, mc->oscclk[i]);
  }
  sysbus_realize(SYS_BUS_DEVICE(scc), &error_fatal);
  return sysbus_mmio_get_region(SYS_BUS_DEVICE(sccdev), 0);
}

static MemoryRegion* make_unimp_dev(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  /* Initialize, configure and realize a TYPE_UNIMPLEMENTED_DEVICE,
   * and return a pointer to its MemoryRegion.
   */
  UnimplementedDeviceState* uds = opaque;

  object_initialize_child(OBJECT(mms), name, uds, TYPE_UNIMPLEMENTED_DEVICE);
  qdev_prop_set_string(DEVICE(uds), "name", name);
  qdev_prop_set_uint64(DEVICE(uds), "size", size);
  sysbus_realize(SYS_BUS_DEVICE(uds), &error_fatal);
  return sysbus_mmio_get_region(SYS_BUS_DEVICE(uds), 0);
}

static MemoryRegion* make_mpc(
    SlateMachineState* ms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  TZMPC* mpc = opaque;
  int i = mpc - &ms->mpc[0];
  MemoryRegion* upstream;
  const RAMInfo* raminfo = find_raminfo_for_mpc(ms, i);
  MemoryRegion* ram = mr_for_raminfo(ms, raminfo);

  object_initialize_child(OBJECT(ms), name, mpc, TYPE_TZ_MPC);
  object_property_set_link(OBJECT(mpc), "downstream", OBJECT(ram), &error_fatal);
  sysbus_realize(SYS_BUS_DEVICE(mpc), &error_fatal);
  /* Map the upstream end of the MPC into system memory */
  upstream = sysbus_mmio_get_region(SYS_BUS_DEVICE(mpc), 1);
  memory_region_add_subregion(get_system_memory(), raminfo->base, upstream);
  /* and connect its interrupt to the IoTKit */
  qdev_connect_gpio_out_named(
      DEVICE(mpc), "irq", 0, qdev_get_gpio_in_named(DEVICE(&ms->iotkit), "mpcexp_status", i));

  /* Return the register interface MR for our caller to map behind the PPC */
  return sysbus_mmio_get_region(SYS_BUS_DEVICE(mpc), 0);
}

static MemoryRegion* make_i2c(
    SlateMachineState* ms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  ArmSbconI2CState* i2c = opaque;
  SysBusDevice* s;

  object_initialize_child(OBJECT(ms), name, i2c, TYPE_ARM_SBCON_I2C);
  s = SYS_BUS_DEVICE(i2c);
  sysbus_realize(s, &error_fatal);

  /*
   * If this is an internal-use-only i2c bus, mark it full
   * so that user-created i2c devices are not plugged into it.
   * If we implement models of any on-board i2c devices that
   * plug in to one of the internal-use-only buses, then we will
   * need to create and plugging those in here before we mark the
   * bus as full.
   */
  if (extradata->i2c_internal) {
    BusState* qbus = qdev_get_child_bus(DEVICE(i2c), "i2c");
    qbus_mark_full(qbus);
  }

  return sysbus_mmio_get_region(s, 0);
}

static MemoryRegion* make_uart(
    SlateMachineState* mms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  /* The irq[] array is tx, rx, combined, in that order */
  SlateMachineClass* mmc = SLATE_MACHINE_GET_CLASS(mms);
  CMSDKAPBUART* uart = opaque;
  int i = uart - &mms->uart[0];
  SysBusDevice* s;
  DeviceState* orgate_dev = DEVICE(&mms->uart_irq_orgate);

  object_initialize_child(OBJECT(mms), name, uart, TYPE_CMSDK_APB_UART);
  qdev_prop_set_chr(DEVICE(uart), "chardev", serial_hd(i));
  qdev_prop_set_uint32(DEVICE(uart), "pclk-frq", mmc->apb_periph_frq);
  sysbus_realize(SYS_BUS_DEVICE(uart), &error_fatal);
  s = SYS_BUS_DEVICE(uart);
  sysbus_connect_irq(s, 0, get_sse_irq_in(mms, irqs[0]));
  sysbus_connect_irq(s, 1, get_sse_irq_in(mms, irqs[1]));
  sysbus_connect_irq(s, 2, qdev_get_gpio_in(orgate_dev, i * 2));
  sysbus_connect_irq(s, 3, qdev_get_gpio_in(orgate_dev, i * 2 + 1));
  sysbus_connect_irq(s, 4, get_sse_irq_in(mms, irqs[2]));
  return sysbus_mmio_get_region(SYS_BUS_DEVICE(uart), 0);
}

static MemoryRegion* make_spi(
    SlateMachineState* ms,
    void* opaque,
    const char* name,
    hwaddr size,
    const int* irqs,
    const PPCExtraData* extradata) {
  /*
   * The AN505 has five PL022 SPI controllers.
   * One of these should have the LCD controller behind it; the others
   * are connected only to the FPGA's "general purpose SPI connector"
   * or "shield" expansion connectors.
   * Note that if we do implement devices behind SPI, the chip select
   * lines are set via the "MISC" register in the MPS2 FPGAIO device.
   */
  PL022State* spi = opaque;
  SysBusDevice* s;

  object_initialize_child(OBJECT(ms), name, spi, TYPE_PL022);
  sysbus_realize(SYS_BUS_DEVICE(spi), &error_fatal);
  s = SYS_BUS_DEVICE(spi);
  sysbus_connect_irq(s, 0, get_sse_irq_in(ms, irqs[0]));
  return sysbus_mmio_get_region(s, 0);
}

static void slate_common_init(MachineState* machine) {
  SlateMachineState* ms = SLATE_MACHINE(machine);
  SlateMachineClass* mc = SLATE_MACHINE_GET_CLASS(machine);

  if (ms->screen_size == 0) {
    error_report("machine screen_size not set.");
    exit(EXIT_FAILURE);
  }

  printf("screen size is: %u x %u\n", ms->screen_size, ms->screen_size);

  MemoryRegion* system_memory = get_system_memory();
  DeviceState* iotkitdev;
  DeviceState* dev_splitter;
  const PPCInfo* ppcs;
  int num_ppcs;
  int i;

  /* These clocks don't need migration because they are fixed-frequency */
  ms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
  clock_set_hz(ms->sysclk, mc->sysclk_frq);
  ms->s32kclk = clock_new(OBJECT(machine), "S32KCLK");
  clock_set_hz(ms->s32kclk, S32KCLK_FRQ);
  ms->refclk = clock_new(OBJECT(machine), "REFCLK");
  clock_set_hz(ms->refclk, REF_CLK_FRQ);

  object_initialize_child(OBJECT(machine), TYPE_IOTKIT, &ms->iotkit, TYPE_SSE300);
  iotkitdev = DEVICE(&ms->iotkit);
  object_property_set_link(OBJECT(&ms->iotkit), "memory", OBJECT(system_memory), &error_abort);
  qdev_prop_set_uint32(iotkitdev, "EXP_NUMIRQ", 96);
  qdev_prop_set_uint32(iotkitdev, "init-svtor", 0x00000000);
  qdev_prop_set_uint32(iotkitdev, "SRAM_ADDR_WIDTH", 21);
  qdev_connect_clock_in(iotkitdev, "MAINCLK", ms->sysclk);
  qdev_connect_clock_in(iotkitdev, "S32KCLK", ms->s32kclk);

  // SLATE customization
  qdev_prop_set_bit(iotkitdev, "HAS_REF_CLK", true);
  qdev_prop_set_uint32(iotkitdev, "ITCM_SIZE", 2 * MiB);
  qdev_connect_clock_in(iotkitdev, "REFCLK", ms->refclk);

  sysbus_realize(SYS_BUS_DEVICE(&ms->iotkit), &error_fatal);

  /* The sec_resp_cfg output from the IoTKit must be split into multiple
   * lines, one for each of the PPCs we create here, plus one per MSC.
   */
  object_initialize_child(
      OBJECT(machine), "sec-resp-splitter", &ms->sec_resp_splitter, TYPE_SPLIT_IRQ);
  object_property_set_int(
      OBJECT(&ms->sec_resp_splitter),
      "num-lines",
      ARRAY_SIZE(ms->ppc) + ARRAY_SIZE(ms->msc),
      &error_fatal);
  qdev_realize(DEVICE(&ms->sec_resp_splitter), NULL, &error_fatal);
  dev_splitter = DEVICE(&ms->sec_resp_splitter);
  qdev_connect_gpio_out_named(iotkitdev, "sec_resp_cfg", 0, qdev_get_gpio_in(dev_splitter, 0));

  /*
   * The overflow IRQs for all UARTs are ORed together.
   * Tx, Rx and "combined" IRQs are sent to the NVIC separately.
   * Create the OR gate for this: it has one input for the TX overflow
   * and one for the RX overflow for each UART we might have.
   * (If the board has fewer than the maximum possible number of UARTs
   * those inputs are never wired up and are treated as always-zero.)
   */
  object_initialize_child(OBJECT(ms), "uart-irq-orgate", &ms->uart_irq_orgate, TYPE_OR_IRQ);
  object_property_set_int(
      OBJECT(&ms->uart_irq_orgate), "num-lines", 2 * ARRAY_SIZE(ms->uart), &error_fatal);
  qdev_realize(DEVICE(&ms->uart_irq_orgate), NULL, &error_fatal);
  qdev_connect_gpio_out(DEVICE(&ms->uart_irq_orgate), 0, get_sse_irq_in(ms, mc->uart_overflow_irq));

  /* Most of the devices in the FPGA are behind Peripheral Protection
   * Controllers. The required order for initializing things is:
   *  + initialize the PPC
   *  + initialize, configure and realize downstream devices
   *  + connect downstream device MemoryRegions to the PPC
   *  + realize the PPC
   *  + map the PPC's MemoryRegions to the places in the address map
   *    where the downstream devices should appear
   *  + wire up the PPC's control lines to the IoTKit object
   */

  const PPCInfo an547_ppcs[] = {
      {
          .name = "apb_ppcexp0",
          .ports =
              {
                  {"ssram-mpc", make_mpc, &ms->mpc[0], 0x57000000, 0x1000},
                  {"qspi-mpc", make_mpc, &ms->mpc[1], 0x57001000, 0x1000},
                  {"ddr-mpc", make_mpc, &ms->mpc[2], 0x57002000, 0x1000},
              },
      },
      {
          .name = "apb_ppcexp1",
          .ports =
              {
                  {"i2c0",
                   make_i2c,
                   &ms->i2c[0],
                   0x49200000,
                   0x1000,
                   {},
                   {.i2c_internal = true /* touchscreen */}},
                  {"i2c1",
                   make_i2c,
                   &ms->i2c[1],
                   0x49201000,
                   0x1000,
                   {},
                   {.i2c_internal = true /* audio conf */}},
                  {"spi0", make_spi, &ms->spi[0], 0x49202000, 0x1000, {53}},
                  {"spi1", make_spi, &ms->spi[1], 0x49203000, 0x1000, {54}},
                  {"spi2", make_spi, &ms->spi[2], 0x49204000, 0x1000, {55}},
                  {"i2c2",
                   make_i2c,
                   &ms->i2c[2],
                   0x49205000,
                   0x1000,
                   {},
                   {.i2c_internal = false /* shield 0 */}},
                  {"i2c3",
                   make_i2c,
                   &ms->i2c[3],
                   0x49206000,
                   0x1000,
                   {},
                   {.i2c_internal = false /* shield 1 */}},
                  {/* port 7 reserved */},
                  {"i2c4",
                   make_i2c,
                   &ms->i2c[4],
                   0x49208000,
                   0x1000,
                   {},
                   {.i2c_internal = true /* DDR4 EEPROM */}},
              },
      },
      {
          .name = "apb_ppcexp2",
          .ports =
              {
                  {"scc", make_scc, &ms->scc, 0x49300000, 0x1000},
                  {"i2s-audio", make_unimp_dev, &ms->i2s_audio, 0x49301000, 0x1000},
                  {"fpgaio", make_fpgaio, &ms->fpgaio, 0x49302000, 0x1000},
                  {"uart0", make_uart, &ms->uart[0], 0x49303000, 0x1000, {33, 34, 43}},
                  {"uart1", make_uart, &ms->uart[1], 0x49304000, 0x1000, {35, 36, 44}},
                  {"uart2", make_uart, &ms->uart[2], 0x49305000, 0x1000, {37, 38, 45}},
                  {"uart3", make_uart, &ms->uart[3], 0x49306000, 0x1000, {39, 40, 46}},
                  {"uart4", make_uart, &ms->uart[4], 0x49307000, 0x1000, {41, 42, 47}},
                  {"uart5", make_uart, &ms->uart[5], 0x49308000, 0x1000, {125, 126, 127}},

                  {/* port 9 reserved */},
                  {"clcd", make_unimp_dev, &ms->cldc, 0x4930a000, 0x1000},
                  {"rtc", make_rtc, &ms->rtc, 0x4930b000, 0x1000},
              },
      },
      {
          .name = "ahb_ppcexp0",
          .ports =
              {
                  {"gpio0", make_unimp_dev, &ms->gpio[0], 0x41100000, 0x1000},
                  {"gpio1", make_unimp_dev, &ms->gpio[1], 0x41101000, 0x1000},
                  {"gpio2", make_unimp_dev, &ms->gpio[2], 0x41102000, 0x1000},
                  {"gpio3", make_unimp_dev, &ms->gpio[3], 0x41103000, 0x1000},
                  {"eth-usb", make_eth_usb, NULL, 0x41400000, 0x200000, {49}},
              },
      },
  };

  ppcs = an547_ppcs;
  num_ppcs = ARRAY_SIZE(an547_ppcs);

  for (i = 0; i < num_ppcs; i++) {
    const PPCInfo* ppcinfo = &ppcs[i];
    TZPPC* ppc = &ms->ppc[i];
    DeviceState* ppcdev;
    int port;
    char* gpioname;

    object_initialize_child(OBJECT(machine), ppcinfo->name, ppc, TYPE_TZ_PPC);
    ppcdev = DEVICE(ppc);

    for (port = 0; port < TZ_NUM_PORTS; port++) {
      const PPCPortInfo* pinfo = &ppcinfo->ports[port];
      MemoryRegion* mr;
      char* portname;

      if (!pinfo->devfn) {
        continue;
      }

      mr =
          pinfo->devfn(ms, pinfo->opaque, pinfo->name, pinfo->size, pinfo->irqs, &pinfo->extradata);
      portname = g_strdup_printf("port[%d]", port);
      object_property_set_link(OBJECT(ppc), portname, OBJECT(mr), &error_fatal);
      g_free(portname);
    }

    sysbus_realize(SYS_BUS_DEVICE(ppc), &error_fatal);

    for (port = 0; port < TZ_NUM_PORTS; port++) {
      const PPCPortInfo* pinfo = &ppcinfo->ports[port];

      if (!pinfo->devfn) {
        continue;
      }
      sysbus_mmio_map(SYS_BUS_DEVICE(ppc), port, pinfo->addr);

      gpioname = g_strdup_printf("%s_nonsec", ppcinfo->name);
      qdev_connect_gpio_out_named(
          iotkitdev, gpioname, port, qdev_get_gpio_in_named(ppcdev, "cfg_nonsec", port));
      g_free(gpioname);
      gpioname = g_strdup_printf("%s_ap", ppcinfo->name);
      qdev_connect_gpio_out_named(
          iotkitdev, gpioname, port, qdev_get_gpio_in_named(ppcdev, "cfg_ap", port));
      g_free(gpioname);
    }

    gpioname = g_strdup_printf("%s_irq_enable", ppcinfo->name);
    qdev_connect_gpio_out_named(
        iotkitdev, gpioname, 0, qdev_get_gpio_in_named(ppcdev, "irq_enable", 0));
    g_free(gpioname);
    gpioname = g_strdup_printf("%s_irq_clear", ppcinfo->name);
    qdev_connect_gpio_out_named(
        iotkitdev, gpioname, 0, qdev_get_gpio_in_named(ppcdev, "irq_clear", 0));
    g_free(gpioname);
    gpioname = g_strdup_printf("%s_irq_status", ppcinfo->name);
    qdev_connect_gpio_out_named(ppcdev, "irq", 0, qdev_get_gpio_in_named(iotkitdev, gpioname, 0));
    g_free(gpioname);

    qdev_connect_gpio_out(dev_splitter, i, qdev_get_gpio_in_named(ppcdev, "cfg_sec_resp", 0));
  }

  create_unimplemented_device("FPGA NS PC", 0x48007000, 0x1000);

  create_unimplemented_device("U55 timing adapter 0", 0x48102000, 0x1000);
  create_unimplemented_device("U55 timing adapter 1", 0x48103000, 0x1000);

  create_non_mpc_ram(ms);

  init_ram_disp(ms);
  init_nema_gpu(ms);
  init_host_timer(ms);

  // this allows host to access itcm region of guest
  // all readonly flash content is written there (i.e. image assets)
  address_space_init(&ms->nema_gpu.system_itcm_as, &ms->iotkit.itcm, "itcm_as");

  armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, boot_ram_size(ms));
}

static void slate_class_init(ObjectClass* oc, void* data) {
  MachineClass* mc = MACHINE_CLASS(oc);
  SlateMachineClass* smc = SLATE_MACHINE_CLASS(oc);

  uintptr_t offs = OFFSET_OF(SlateMachineState, screen_size);
  object_class_property_add(oc, "screensize", "uint32", get_uint32, set_uint32, NULL, (void*)offs);
  object_class_property_set_description(
      oc, "screensize", "the display screen size of the machine.");

  mc->init = slate_common_init;
  mc->desc = "Slate with cortex-m55";
  mc->default_cpus = 1;
  mc->min_cpus = mc->default_cpus;
  mc->max_cpus = mc->default_cpus;
  mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m55");

  smc->scc_id = 0x41055470;
  smc->sysclk_frq = 50 * 1000 * 1000; /* 100MHz */
  smc->apb_periph_frq = 25 * 1000 * 1000; /* 25MHz */
  smc->oscclk = an524_oscclk; /* same as AN524 */
  smc->len_oscclk = ARRAY_SIZE(an524_oscclk);
  smc->fpgaio_num_leds = 10;
  smc->fpgaio_has_switches = true;
  smc->fpgaio_has_dbgctrl = true;
  smc->uart_overflow_irq = 48;
  smc->raminfo = slate_raminfo;
  smc->boot_ram_size = 16 * MiB;

  set_default_ram_info(smc);
}

static const TypeInfo slate_info = {
    .name = TYPE_SLATE_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SlateMachineState),
    .class_size = sizeof(SlateMachineClass),
    .class_init = slate_class_init,
};

static void slate_machine_init(void) {
  type_register_static(&slate_info);
}

type_init(slate_machine_init);
