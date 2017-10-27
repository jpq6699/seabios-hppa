// Glue code for parisc architecture
//
// Copyright (C) 2017  Helge Deller <deller@gmx.de>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // GET_BDA
#include "bregs.h" // struct bregs
#include "hw/pic.h" // enable_hwirq
#include "output.h" // debug_enter
#include "stacks.h" // call16_int
#include "string.h" // memset
#include "util.h" // serial_setup
#include "malloc.h" // malloc
#include "hw/serialio.h" // qemu_debug_port
#include "hw/ata.h"
#include "hw/rtc.h"
#include "fw/paravirt.h" // PlatformRunningOn
#include "parisc/hppa_hardware.h" // DINO_UART_BASE
#include "parisc/pdc.h"
#include "parisc/b160l.h"

int HaveRunPost;
u8 ExtraStack[BUILD_EXTRA_STACK_SIZE+1] __aligned(8);
u8 *StackPos;
u8 __VISIBLE parisc_stack[16*1024] __aligned(64);

u8 BiosChecksum;

char zonefseg_start, zonefseg_end;  // SYMBOLS
char varlow_start, varlow_end, final_varlow_start;
char final_readonly_start;
char code32flat_start, code32flat_end;
char zonelow_base;

void mtrr_setup(void) { }
void mathcp_setup(void) { }
void smp_setup(void) { }
void bios32_init(void) { }
void yield_toirq(void) { }
void farcall16(struct bregs *callregs) { }
void farcall16big(struct bregs *callregs) { }

void cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	eax = ebx = ecx = edx = 0;
}

void wrmsr_smp(u32 index, u64 val) { }

#define ARG0 arg[7-0]
#define ARG1 arg[7-1]
#define ARG2 arg[7-2]
#define ARG3 arg[7-3]
#define ARG4 arg[7-4]
#define ARG5 arg[7-5]
#define ARG6 arg[7-6]
#define ARG7 arg[7-7]

/*********** IODC ******/

static struct drive_s *boot_drive;

static struct pdc_iodc *iodc_list[] = { PARISC_IODC_LIST };
static unsigned long hpa_list[] = { PARISC_HPA_LIST };

#define SERIAL_TIMEOUT 20
unsigned long parisc_serial_in(char *c, unsigned long maxchars)
{
	const portaddr_t addr = DINO_UART_HPA+0x800;
	unsigned long end = timer_calc(SERIAL_TIMEOUT);
	unsigned long count = 0;
	while (count < maxchars) {
		u8 lsr = inb(addr+SEROFF_LSR);
		if (lsr & 0x01) {
			// Success - can read data
			*c++ = inb(addr+SEROFF_DATA);
			count++;
		}
	        if (timer_check(end))
			break;
	}
	return count;
}

int __VISIBLE parisc_iodc_entry(unsigned int *arg)
{
	unsigned long hpa = ARG0;
	unsigned long option = ARG1;
	unsigned long spa = ARG2;
	unsigned long layers = ARG3;
	unsigned long *result = (unsigned long *)ARG4;
	int ret, len;
	char *c;
	struct disk_op_s disk_op;

#if 0
	dprintf(0, "\nIODC option start #%ld : hpa=%lx spa=%lx layers=%lx ", option, hpa, spa, layers);
	dprintf(0, "result=%p arg5=0x%x arg6=0x%x arg7=0x%x\n", result, ARG5, ARG6, ARG7);
#endif
	/* console I/O */
	if (hpa == DINO_UART_HPA || hpa == LASI_UART_HPA)
	switch (option) {
	case ENTRY_IO_COUT: /* console output */
		c = (char*)ARG6;
		result[0] = len = ARG7;
		while (len--)
			printf("%c", *c++);
		return PDC_OK;
	case ENTRY_IO_CIN: /* console input, with 5 seconds timeout */
		c = (char*)ARG6;
		result[0] = parisc_serial_in(c, ARG7);
		return PDC_OK;
	}

	/* boot medium I/O */
	if (hpa == IDE_HPA)
	switch (option) {
	case ENTRY_IO_BOOTIN: /* boot medium IN */
		disk_op.drive_fl = boot_drive;
		disk_op.buf_fl = (void*)ARG6;
		disk_op.command = CMD_READ;
		disk_op.count = (ARG7 / disk_op.drive_fl->blksize);
		disk_op.lba = (ARG5 / disk_op.drive_fl->blksize);
		ret = process_op(&disk_op);
		// dprintf(0, "\nBOOT IO res %d count = %d\n", ret, ARG7);
		result[0] = ARG7;
		if (ret)
			return PDC_ERROR;
		return PDC_OK;
	}

	dprintf(0, "\nIODC option #%ld called: hpa=%lx spa=%lx layers=%lx ", option, hpa, spa, layers);
	dprintf(0, "result=%p arg5=%x arg6=%x arg7=%x\n", result, ARG5, ARG6, ARG7);

	hlt();
	return PDC_BAD_OPTION;
}

/*********** PDC *******/

/* https://codereview.stackexchange.com/questions/38275/convert-between-date-time-and-time-stamp-without-using-standard-library-routines */

static unsigned short days[4][12] =
{
    {   0,  31,  60,  91, 121, 152, 182, 213, 244, 274, 305, 335},
    { 366, 397, 425, 456, 486, 517, 547, 578, 609, 639, 670, 700},
    { 731, 762, 790, 821, 851, 882, 912, 943, 974,1004,1035,1065},
    {1096,1127,1155,1186,1216,1247,1277,1308,1339,1369,1400,1430},
};

static inline int rtc_from_bcd(int a)
{
	return ((a >> 4) * 10) + (a & 0x0f);
}

static unsigned long seconds_since_1970(void)
{
	#define SECONDS_2000_JAN_1 946684800
	/* assumption: only dates between 01/01/2000 and 31/12/2099 */
	unsigned int second = rtc_from_bcd(rtc_read(CMOS_RTC_SECONDS));
	unsigned int minute = rtc_from_bcd(rtc_read(CMOS_RTC_MINUTES));
	unsigned int hour   = rtc_from_bcd(rtc_read(CMOS_RTC_HOURS));
	unsigned int day    = rtc_from_bcd(rtc_read(CMOS_RTC_DAY_MONTH)) - 1;
	unsigned int month  = rtc_from_bcd(rtc_read(CMOS_RTC_MONTH)) - 1;
	unsigned int year   = rtc_from_bcd(rtc_read(CMOS_RTC_YEAR));
	return (((year/4*(365*4+1)+days[year%4][month]+day)*24+hour)*60+minute)
			*60+second + SECONDS_2000_JAN_1;
}

char *pdc_name(unsigned long num)
{
	#define DO(x) if (num == x) return #x;
	DO(PDC_POW_FAIL)
	DO(PDC_CHASSIS)
	DO(PDC_PIM)
	DO(PDC_MODEL)
	DO(PDC_CACHE)
	DO(PDC_HPA)
	DO(PDC_COPROC)
	DO(PDC_IODC)
	DO(PDC_TOD)
	DO(PDC_STABLE)
	DO(PDC_NVOLATILE)
	DO(PDC_ADD_VALID)
	DO(PDC_INSTR)
	DO(PDC_PROC)
	DO(PDC_BLOCK_TLB)
	DO(PDC_TLB)
	DO(PDC_MEM)
	DO(PDC_PSW)
	DO(PDC_SYSTEM_MAP)
	DO(PDC_SOFT_POWER)
	DO(PDC_MEM_MAP)
	DO(PDC_EEPROM)
	DO(PDC_NVM)
	DO(PDC_SEED_ERROR)
	DO(PDC_IO)
	DO(PDC_BROADCAST_RESET)
	DO(PDC_LAN_STATION_ID)
	DO(PDC_CHECK_RANGES)
	DO(PDC_NV_SECTIONS)
	DO(PDC_PERFORMANCE)
	DO(PDC_SYSTEM_INFO)
	DO(PDC_RDR)
	DO(PDC_INTRIGUE)
	DO(PDC_STI)
	DO(PDC_PCI_INDEX)
	DO(PDC_INITIATOR)
	DO(PDC_LINK)
	return "UNKNOWN!";
}

int __VISIBLE parisc_pdc_entry(unsigned int *arg)
{
	static unsigned long psw_defaults = PDC_PSW_ENDIAN_BIT;
	static unsigned long cache_info[] = { PARISC_PDC_CACHE_INFO };
	static unsigned long model[] = { PARISC_PDC_MODEL };
	static const char model_str[] = PARISC_MODEL;

	unsigned long proc = ARG0;
	unsigned long option = ARG1;
	unsigned long *result = (unsigned long *)ARG2;
#if 0
	dprintf(0, "\nSeaBIOS: Start PDC proc %s(%d) option %d result=%x ARG3=%x ", pdc_name(ARG0), ARG0, ARG1, ARG2, ARG3);
	dprintf(0, "ARG4=%x ARG5=%x ARG6=%x ARG7=%x\n", ARG4, ARG5, ARG6, ARG7);
#endif
	switch (proc) {
	case PDC_POW_FAIL:
		break;
	case PDC_CHASSIS: /* chassis functions */
		// dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_CHASSIS function %d %x %x %x %x\n", ARG1, ARG2, ARG3, ARG4, ARG5);
		return PDC_BAD_PROC;
	case PDC_PIM:
		break;
	case PDC_MODEL: /* model information */
		switch (option) {
		case PDC_MODEL_INFO:
			memcpy(result, model, sizeof(model));
			return PDC_OK;
		case PDC_MODEL_VERSIONS:
			if (ARG3 == 0) {
				result[0] =  PARISC_PDC_VERSION;
				return PDC_OK;
			}
			return -4; // invalid c_index
		case PDC_MODEL_SYSMODEL:
			result[0] = sizeof(model_str) - 1;
			strtcpy((char *)ARG4, model_str, sizeof(model_str));
			return PDC_OK;
		case PDC_MODEL_CPU_ID:
			result[0] = PARISC_PDC_CPUID;
			return PDC_OK;
		case PDC_MODEL_CAPABILITIES:
			result[0] = PARISC_PDC_CAPABILITIES;
			return PDC_OK;
		}
		dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_MODEL function %d %x %x %x %x\n", ARG1, ARG2, ARG3, ARG4, ARG5);
		return PDC_BAD_OPTION;
	case PDC_CACHE:
		switch (option) {
		case PDC_CACHE_INFO:
			memcpy(result, cache_info, sizeof(cache_info));
			return PDC_OK;
		}
		dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_CACHE function %d %x %x %x %x\n", ARG1, ARG2, ARG3, ARG4, ARG5);
		return PDC_BAD_OPTION;
	case PDC_HPA:
		break;
	case PDC_COPROC:
		switch (option) {
		case PDC_COPROC_CFG:
			memset(result, 0, 32 * sizeof(unsigned long));
			result[0] = 1; // XXX: num_cpus
			result[1] = 1;
			result[17] = 1; // Revision
			result[18] = 19; // Model
			return PDC_OK;
		}
		return PDC_BAD_OPTION;
	case PDC_IODC: /* Call IODC functions */
		switch (option) {
		case 0:			// Get entry point
			switch (ARG3) {
			case DINO_UART_HPA:
				if (ARG4 == 0) {
					memcpy((void*)ARG5, &iodc_data_hpa_fff83000, ARG6);
					*result = ARG6;
					return PDC_OK;
				}
				if (ARG4 == 3) { // Search next (correct ??, write to ARG7 too?)
					result[0] = 0;
					result[1] = CL_DUPLEX;
					result[2] = 0;
					result[3] = 0;
					return PDC_OK;
					// return PDC_NE_BOOTDEV;
				}
				break;
			case IDE_HPA:
				if (ARG4 == 0) {
					memcpy((void*)ARG5, &iodc_data_hpa_fff8c000, ARG6); // FIXME !!!
					*result = ARG6;
					return PDC_OK;
				}
				if (ARG4 == 3) { // Search next (correct ??, write to ARG7 too?)
					result[0] = 0;
					result[1] = CL_RANDOM;
					result[2] = 0;
					result[3] = 0;
					return PDC_OK;
					// return PDC_NE_BOOTDEV;
				}
				break;
			}
		}
		dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_IODC function %ld ARG3=%x ARG4=%x ARG5=%x\n", option, ARG3, ARG4, ARG5);
		return PDC_BAD_OPTION;
	case PDC_TOD:	/* Time of day */
		switch (option) {
		case PDC_TOD_READ:
			result[0] = seconds_since_1970();
			result[1] = result[2] = result[3] = 0;
			return PDC_OK;
		}
		dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_TOD function %ld ARG3=%x ARG4=%x ARG5=%x\n", option, ARG3, ARG4, ARG5);
		return PDC_BAD_OPTION;
	case PDC_STABLE:
		return PDC_BAD_OPTION;
	case PDC_NVOLATILE:
		return PDC_BAD_PROC;
	case PDC_ADD_VALID:
		dprintf(0, "\n\nSeaBIOS: Unimplemented PDC_ADD_VALID function %ld ARG2=%x\n", option, ARG2);
		return PDC_OK;
	case PDC_INSTR:
		return PDC_BAD_PROC;
	case PDC_CONFIG:	/* Obsolete */
		return PDC_BAD_PROC;
	case PDC_BLOCK_TLB:	/* not needed on virtual machine */
		return PDC_BAD_PROC;
	case PDC_TLB:		/* not used on Linux. Maybe on HP-UX? */
		return PDC_BAD_PROC;
	case PDC_MEM:		/* replaced by PDC_SYSTEM_MAP, might be needed for 64-bit */
		dprintf(0, "\n\nSeaBIOS: Check PDC_MEM option %ld ARG3=%x ARG4=%x ARG5=%x\n", option, ARG3, ARG4, ARG5);
		return PDC_BAD_PROC;
	case PDC_PSW:	/* Get/Set default System Mask  */
		if (option > PDC_PSW_SET_DEFAULTS)
			return PDC_BAD_OPTION;
		/* FIXME: For 64bit support enable PDC_PSW_WIDE_BIT too! */
		if (option == PDC_PSW_MASK)
			*result = PDC_PSW_ENDIAN_BIT;
		if (option == PDC_PSW_GET_DEFAULTS)
			*result = psw_defaults;
		if (option == PDC_PSW_SET_DEFAULTS) {
			// if ((ARG2 & PDC_PSW_ENDIAN_BIT) == 0)
			psw_defaults = ARG2;
		}
		return PDC_OK;
	case PDC_SYSTEM_MAP:
		dprintf(0, "\n\nSeaBIOS: PDC_SYSTEM_MAP function %ld ARG3=%x ARG4=%x ARG5=%x\n", option, ARG3, ARG4, ARG5);
		switch (option) {
		case PDC_FIND_MODULE:
			return PDC_OK;
		case PDC_FIND_ADDRESS:
		case PDC_TRANSLATE_PATH:
			return PDC_OK;
		}
		break;
	}

	dprintf(0, "\nSeaBIOS: Unimplemented PDC proc %s(%d) option %d result=%x ARG3=%x ",
			pdc_name(ARG0), ARG0, ARG1, ARG2, ARG3);
	dprintf(0, "ARG4=%x ARG5=%x ARG6=%x ARG7=%x\n", ARG4, ARG5, ARG6, ARG7);

	hlt();
	return PDC_BAD_PROC;
}

/*********** BOOT MENU *******/

extern struct drive_s *select_parisc_boot_drive(void);

/* size of I/O block used in HP firmware */
#define FW_BLOCKSIZE    2048

int parisc_boot_menu(unsigned char **iplstart, unsigned char **iplend)
{
	int ret;
	unsigned int *target = (void *)0x60000; // bug in palo: 0xa0000 crashes.
	struct disk_op_s disk_op = {
		.buf_fl = target,
		.command = CMD_SEEK,
		.count = 0,
		.lba = 0,
	};

	boot_drive = select_parisc_boot_drive();
	disk_op.drive_fl = boot_drive;
	if (boot_drive == NULL) {
		printf("SeaBIOS: No boot device.\n");
		return 0;
	}

	/* seek to beginning of disc/CD */
	ret = process_op(&disk_op);
	// printf("DISK_SEEK returned %d\n", ret);
	if (ret)
		return 0;

	/* read boot sector of disc/CD */
	target[0] = 0xabcd;
	disk_op.command = CMD_READ;
	// printf("blocksize is %d\n", disk_op.drive_fl->blksize);
	disk_op.count = (FW_BLOCKSIZE / disk_op.drive_fl->blksize);
	disk_op.lba = 0;
	ret = process_op(&disk_op);
	// printf("DISK_READ(count=%d) = %d\n", disk_op.count, ret);
	if (ret)
		return 0;

	unsigned int ipl_addr = be32_to_cpu(target[0xf0/sizeof(int)]); /* offset 0xf0 in bootblock */
	unsigned int ipl_size = be32_to_cpu(target[0xf4/sizeof(int)]);
	unsigned int ipl_entry= be32_to_cpu(target[0xf8/sizeof(int)]);

	/* check LIF header of bootblock */
	// printf("boot magic is 0x%x (should be 0x8000)\n", target[0]>>16);
	// printf("ipl  start at 0x%x, size %d, entry 0x%x\n", ipl_addr, ipl_size, ipl_entry);

	// TODO: check ipl values, out of range, ... ?

	/* seek to beginning of IPL */
	disk_op.command = CMD_SEEK;
	disk_op.count = 0; // (ipl_size / disk_op.drive_fl->blksize);
	disk_op.lba = (ipl_addr / disk_op.drive_fl->blksize);
	ret = process_op(&disk_op);
	// printf("DISK_SEEK to IPL returned %d\n", ret);

	/* read IPL */
	disk_op.command = CMD_READ;
	disk_op.count = (ipl_size / disk_op.drive_fl->blksize);
	disk_op.lba = (ipl_addr / disk_op.drive_fl->blksize);
	ret = process_op(&disk_op);
	// printf("DISK_READ IPL returned %d\n", ret);

	// printf("First word at %p is 0x%x\n", target, target[0]);

	/* execute IPL */
	// TODO: flush D- and I-cache, not needed in emulation ?
	*iplstart = *iplend = (unsigned char *) target;
	*iplstart += ipl_entry;
	*iplend += ipl_size;
	return 1;
}


/*********** MAIN *******/

extern char pdc_entry;
extern char iodc_entry;

static const struct pz_device mem_cons_boot = {
	.hpa = DINO_UART_HPA,
	.iodc_io = (unsigned long) &iodc_entry,
	.cl_class = CL_DUPLEX,
};

static const struct pz_device mem_boot_boot = {
	.dp.flags = PF_AUTOBOOT,
	.hpa = IDE_HPA,
	.iodc_io = (unsigned long) &iodc_entry,
	.cl_class = CL_RANDOM,
};

static const struct pz_device mem_kbd_boot = {
	.hpa = DINO_UART_HPA,
	.iodc_io = (unsigned long) &iodc_entry,
	// .cl_class = CL_DUPLEX,
	.cl_class = CL_KEYBD,
};

void hlt(void)
{
    printf("SeaBIOS issued HALT SYSTEM.\n\n");
    asm volatile("\t.word 0xffffffff": : :"memory");
}

void reset(void)
{
	hlt(); // TODO: Reset the machine
}


#define PAGE0 ((volatile struct zeropage *) 0UL)

void __VISIBLE start_parisc_firmware(unsigned long ram_size,
	unsigned long linux_kernel_entry,
	unsigned long cmdline,
	unsigned long initrd_start,
	unsigned long initrd_end)
{
	unsigned int cpu_hz;
	unsigned char *iplstart, *iplend;

	/* Initialize PAGE0 */
	memset((void*)PAGE0, 0, sizeof(*PAGE0));
	PAGE0->memc_cont = ram_size;
	PAGE0->memc_phsize = ram_size;
	PAGE0->mem_free = 4*4096; // 16k ??
	PAGE0->mem_hpa = CPU_HPA; // /* HPA of boot-CPU */
	PAGE0->mem_pdc = (unsigned long) &pdc_entry;
	PAGE0->mem_10msec = CPU_CLOCK_MHZ*(1000000ULL/100);

	PAGE0->imm_max_mem = ram_size;
	memcpy((void*)&(PAGE0->mem_cons), &mem_cons_boot, sizeof(mem_cons_boot));
	memcpy((void*)&(PAGE0->mem_boot), &mem_boot_boot, sizeof(mem_boot_boot));
//	memcpy((void*)&(PAGE0->mem_kbd),  &mem_kbd_boot, sizeof(mem_kbd_boot));

	malloc_preinit();
	if (iodc_list[0] && hpa_list[0]) // avoid warning
		iplstart = 0;

	// set Qemu serial debug port
	DebugOutputPort = PORT_SERIAL1;
	// PlatformRunningOn = PF_QEMU;  // emulate runningOnQEMU()

	cpu_hz = 100 * PAGE0->mem_10msec; /* Hz of this PARISC */
	printf("\n");
	printf("PARISC SeaBIOS Firmware, 1 x PA7300LC (PCX-L2) at %d.%06d MHz, %lu MB RAM.\n",
		cpu_hz / 1000000, cpu_hz % 1000000,
		ram_size/1024/1024);

	// mdelay(1000); // test: "wait 1 second"
	// test endianess functions
	// printf("0xdeadbeef %x %x\n", cpu_to_le16(0xdeadbeef),cpu_to_le32(0xdeadbeef));
	// printf("0xdeadbeef %x %x\n", le16_to_cpu(0xefbe),le32_to_cpu(0xefbeadde));

#if 0
	printf("SeaBIOS: Current date %d-%d-%d\n", rtc_from_bcd(rtc_read(CMOS_RTC_YEAR)),
		rtc_from_bcd(rtc_read(CMOS_RTC_MONTH)),
		rtc_from_bcd(rtc_read(CMOS_RTC_DAY_MONTH)));
	printf("SeaBIOS: Current seconds %lu\n", seconds_since_1970());
#endif

	// handle_post();
	serial_debug_preinit();
	debug_banner();
	// maininit();
	qemu_preinit();
	// coreboot_preinit();

	pci_setup();
	serial_setup();
	block_setup();

	printf("\n");

	/* directly start Linux kernel if it was given on qemu command line. */
	if (linux_kernel_entry) {
		void (*start_kernel)(unsigned long mem_free, unsigned long cmdline,
			unsigned long rdstart, unsigned long rdend);

		printf("Starting Linux kernel which was loaded by qemu.\n\n");
		start_kernel = (void *) linux_kernel_entry;
		start_kernel(PAGE0->mem_free, cmdline, initrd_start, initrd_end);
		hlt(); /* this ends the emulator */
	}

	/* check for bootable drives, and load and start IPL bootloader if possible */
	if (parisc_boot_menu(&iplstart, &iplend)) {
		void (*start_ipl)(long interactive, long mem_free);

		printf("\nStarting IPL boot code from boot medium.\n\n");
		start_ipl = (void *) iplstart;
		start_ipl(0, (long)iplend); // first parameter: 1=interactive, 0=non-interactive
		hlt(); /* this ends the emulator */
	}

	hlt(); /* this ends the emulator */
}
