//////////////////////////////////////////////////
// rBoot open source boot loader for ESP8266.
// Copyright 2015 Richard A Burton
// richardaburton@gmail.com
// See license.txt for license terms.
// Changes for rboot4lcm by HomeACcessoryKid@gmail.com (c)2020
/////////////////////////////////////////////////////////////

#include "rboot-private.h"
#include <rboot-hex2a.h>

#ifndef UART_CLK_FREQ
// reset apb freq = 2x crystal freq: http://esp8266-re.foogod.com/wiki/Serial_UART
#define UART_CLK_FREQ	(26000000 * 2)
#endif

static uint32_t check_image(uint32_t readpos) {

	uint8_t buffer[BUFFER_SIZE];
	uint8_t sectcount;
	uint8_t sectcurrent;
	uint8_t chksum = CHKSUM_INIT;
	uint32_t loop;
	uint32_t remaining;
	uint32_t romaddr;

	rom_header_new *header = (rom_header_new*)buffer;
	section_header *section = (section_header*)buffer;

	if (readpos == 0 || readpos == 0xffffffff) {
		return 0;
	}

	// read rom header
	if (SPIRead(readpos, header, sizeof(rom_header_new)) != 0) {
		return 0;
	}

	// check header type
	if (header->magic == ROM_MAGIC) {
		// old type, no extra header or irom section to skip over
		romaddr = readpos;
		readpos += sizeof(rom_header);
		sectcount = header->count;
	} else if (header->magic == ROM_MAGIC_NEW1 && header->count == ROM_MAGIC_NEW2) {
		// new type, has extra header and irom section first
		romaddr = readpos + header->len + sizeof(rom_header_new);
#ifdef BOOT_IROM_CHKSUM
		// we will set the real section count later, when we read the header
		sectcount = 0xff;
		// just skip the first part of the header
		// rest is processed for the chksum
		readpos += sizeof(rom_header);
#else
		// skip the extra header and irom section
		readpos = romaddr;
		// read the normal header that follows
		if (SPIRead(readpos, header, sizeof(rom_header)) != 0) {
			return 0;
		}
		sectcount = header->count;
		readpos += sizeof(rom_header);
#endif
	} else {
		return 0;
	}

	// test each section
	for (sectcurrent = 0; sectcurrent < sectcount; sectcurrent++) {

		// read section header
		if (SPIRead(readpos, section, sizeof(section_header)) != 0) {
			return 0;
		}
		readpos += sizeof(section_header);

		// get section address and length
		remaining = section->length;

		while (remaining > 0) {
			// work out how much to read, up to BUFFER_SIZE
			uint32_t readlen = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
			// read the block
			if (SPIRead(readpos, buffer, readlen) != 0) {
				return 0;
			}
			// increment next read position
			readpos += readlen;
			// decrement remaining count
			remaining -= readlen;
			// add to chksum
			for (loop = 0; loop < readlen; loop++) {
				chksum ^= buffer[loop];
			}
		}

#ifdef BOOT_IROM_CHKSUM
		if (sectcount == 0xff) {
			// just processed the irom section, now
			// read the normal header that follows
			if (SPIRead(readpos, header, sizeof(rom_header)) != 0) {
				return 0;
			}
			sectcount = header->count + 1;
			readpos += sizeof(rom_header);
		}
#endif
	}

	// round up to next 16 and get checksum
	readpos = readpos | 0x0f;
	if (SPIRead(readpos, buffer, 1) != 0) {
		return 0;
	}

	// compare calculated and stored checksums
	if (buffer[0] != chksum) {
		return 0;
	}

	return romaddr;
}

#if defined (BOOT_GPIO_ENABLED) || defined(BOOT_GPIO_SKIP_ENABLED)

#if BOOT_GPIO_NUM > 16
#error "Invalid BOOT_GPIO_NUM value (disable BOOT_GPIO_ENABLED to disable this feature)"
#endif

// sample gpio code for gpio16
#define ETS_UNCACHED_ADDR(addr) (addr)
#define READ_PERI_REG(addr) (*((volatile uint32_t *)ETS_UNCACHED_ADDR(addr)))
#define WRITE_PERI_REG(addr, val) (*((volatile uint32_t *)ETS_UNCACHED_ADDR(addr))) = (uint32_t)(val)
#define PERIPHS_RTC_BASEADDR				0x60000700
#define REG_RTC_BASE  PERIPHS_RTC_BASEADDR
#define RTC_GPIO_OUT							(REG_RTC_BASE + 0x068)
#define RTC_GPIO_ENABLE							(REG_RTC_BASE + 0x074)
#define RTC_GPIO_IN_DATA						(REG_RTC_BASE + 0x08C)
#define RTC_GPIO_CONF							(REG_RTC_BASE + 0x090)
#define PAD_XPD_DCDC_CONF						(REG_RTC_BASE + 0x0A0)
static uint32_t get_gpio16(void) {
	// set output level to 1
	WRITE_PERI_REG(RTC_GPIO_OUT, (READ_PERI_REG(RTC_GPIO_OUT) & (uint32_t)0xfffffffe) | (uint32_t)(1));

	// read level
	WRITE_PERI_REG(PAD_XPD_DCDC_CONF, (READ_PERI_REG(PAD_XPD_DCDC_CONF) & 0xffffffbc) | (uint32_t)0x1);	// mux configuration for XPD_DCDC and rtc_gpio0 connection
	WRITE_PERI_REG(RTC_GPIO_CONF, (READ_PERI_REG(RTC_GPIO_CONF) & (uint32_t)0xfffffffe) | (uint32_t)0x0);	//mux configuration for out enable
	WRITE_PERI_REG(RTC_GPIO_ENABLE, READ_PERI_REG(RTC_GPIO_ENABLE) & (uint32_t)0xfffffffe);	//out disable

	return (READ_PERI_REG(RTC_GPIO_IN_DATA) & 1);
}

// support for "normal" GPIOs (other than 16)
#define REG_GPIO_BASE            0x60000300
#define GPIO_IN_ADDRESS          (REG_GPIO_BASE + 0x18)
#define GPIO_ENABLE_OUT_ADDRESS  (REG_GPIO_BASE + 0x0c)
#define REG_IOMUX_BASE           0x60000800
#define IOMUX_PULLUP_MASK        (1<<7)
#define IOMUX_FUNC_MASK          0x0130
const uint8_t IOMUX_REG_OFFS[] = {0x34, 0x18, 0x38, 0x14, 0x3c, 0x40, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x04, 0x08, 0x0c, 0x10};
const uint8_t IOMUX_GPIO_FUNC[] = {0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30};

static int get_gpio(int gpio_num) {
	// disable output buffer if set
	uint32_t old_out = READ_PERI_REG(GPIO_ENABLE_OUT_ADDRESS);
	uint32_t new_out = old_out & ~ (1<<gpio_num);
	WRITE_PERI_REG(GPIO_ENABLE_OUT_ADDRESS, new_out);

	// set GPIO function, enable soft pullup
	uint32_t iomux_reg = REG_IOMUX_BASE + IOMUX_REG_OFFS[gpio_num];
	uint32_t old_iomux = READ_PERI_REG(iomux_reg);
	uint32_t gpio_func = IOMUX_GPIO_FUNC[gpio_num];
	uint32_t new_iomux = (old_iomux & ~IOMUX_FUNC_MASK) | gpio_func | IOMUX_PULLUP_MASK;
	WRITE_PERI_REG(iomux_reg, new_iomux);

	// allow soft pullup to take effect if line was floating
	ets_delay_us(10);
	int result = READ_PERI_REG(GPIO_IN_ADDRESS) & (1<<gpio_num);

	// set iomux & GPIO output mode back to initial values
	WRITE_PERI_REG(iomux_reg, old_iomux);
	WRITE_PERI_REG(GPIO_ENABLE_OUT_ADDRESS, old_out);
	return (result ? 1 : 0);
}

// return '1' if we should do a gpio boot
static int perform_gpio_boot(rboot_config *romconf) {
	if (romconf->mode & MODE_GPIO_ROM == 0) {
		return 0;
	}

	// pin low == GPIO boot
	if (BOOT_GPIO_NUM == 16) {
		return (get_gpio16() == 0);
	} else {
		return (get_gpio(BOOT_GPIO_NUM) == 0);
	}
}

#endif

#ifdef BOOT_RTC_ENABLED
uint32_t system_rtc_mem(int32_t addr, void *buff, int32_t length, uint32_t mode) {

    int32_t blocks;

    // validate reading a user block
    if (addr < 64) return 0;
    if (buff == 0) return 0;
    // validate 4 byte aligned
    if (((uint32_t)buff & 0x3) != 0) return 0;
    // validate length is multiple of 4
    if ((length & 0x3) != 0) return 0;

    // check valid length from specified starting point
    if (length > (0x300 - (addr * 4))) return 0;

    // copy the data
    for (blocks = (length >> 2) - 1; blocks >= 0; blocks--) {
        volatile uint32_t *ram = ((uint32_t*)buff) + blocks;
        volatile uint32_t *rtc = ((uint32_t*)0x60001100) + addr + blocks;
		if (mode == RBOOT_RTC_WRITE) {
			*rtc = *ram;
		} else {
			*ram = *rtc;
		}
    }

    return 1;
}
#endif

#ifdef BOOT_BAUDRATE
static enum rst_reason get_reset_reason(void) {

	// reset reason is stored @ offset 0 in system rtc memory
	volatile uint32_t *rtc = (uint32_t*)0x60001100;

	return *rtc;
}
#endif

#if defined(BOOT_CONFIG_CHKSUM) || defined(BOOT_RTC_ENABLED)
// calculate checksum for block of data
// from start up to (but excluding) end
static uint8_t calc_chksum(uint8_t *start, uint8_t *end) {
	uint8_t chksum = CHKSUM_INIT;
	while(start < end) {
		chksum ^= *start;
		start++;
	}
	return chksum;
}
#endif

#ifndef BOOT_CUSTOM_DEFAULT_CONFIG
// populate the user fields of the default config
// created on first boot or in case of corruption
static uint8_t default_config(rboot_config *romconf, uint32_t flashsize) {
	romconf->count = 2;
	romconf->roms[0] = SECTOR_SIZE * (BOOT_CONFIG_SECTOR + 1);
	romconf->roms[1] = (flashsize / 2) + (SECTOR_SIZE * (BOOT_CONFIG_SECTOR + 1));
#ifdef BOOT_GPIO_ENABLED
	romconf->mode = MODE_GPIO_ROM;
#endif
#ifdef BOOT_GPIO_SKIP_ENABLED
	romconf->mode = MODE_GPIO_SKIP;
#endif
}
#endif

#ifdef OTA_MAIN_SECTOR  //rboot4lcm sets this to control a led. Identical to the GPIO defines before
#define ETS_UNCACHED_ADDR(addr) (addr)
#define READ_PERI_REG(addr) (*((volatile uint32_t *)ETS_UNCACHED_ADDR(addr)))
#define WRITE_PERI_REG(addr, val) (*((volatile uint32_t *)ETS_UNCACHED_ADDR(addr))) = (uint32_t)(val)
#define REG_GPIO_BASE           0x60000300          // a register that defines all pins output value
#define GPIO_ENABLE_OUT_ADDRESS (REG_GPIO_BASE+0x0c)// a register that defines pin in/output mode
#define REG_IOMUX_BASE          0x60000800
#define IOMUX_FUNC_MASK         0x0130
#endif

// prevent this function being placed inline with main
// to keep main's stack size as small as possible
// don't mark as static or it'll be optimised out when
// using the assembler stub
uint32_t NOINLINE find_image(void) {

	uint8_t flag;
	uint32_t loadAddr;
	uint32_t flashsize;
	int32_t romToBoot;
	uint8_t updateConfig = 0;
	uint8_t buffer[SECTOR_SIZE];
#ifdef BOOT_GPIO_ENABLED
	uint8_t gpio_boot = 0;
#endif
#if defined (BOOT_GPIO_ENABLED) || defined(BOOT_GPIO_SKIP_ENABLED)
	uint8_t sec;
#endif
#ifdef BOOT_RTC_ENABLED
	rboot_rtc_data rtc;
	uint8_t temp_boot = 0;
#endif

	rboot_config *romconf = (rboot_config*)buffer;
	rom_header *header = (rom_header*)buffer;

#ifdef BOOT_BAUDRATE
	// soft reset doesn't reset PLL/divider, so leave as configured
	if (!(get_reset_reason()==REASON_SOFT_RESTART || get_reset_reason()==REASON_SOFT_WDT_RST)) {
		uart_div_modify( 0, UART_CLK_FREQ / BOOT_BAUDRATE);
	}
#endif

/* --------------------------------------------
Assumptions for the storage of start- and continue-bits
they will be stored in at the end of BOOT_CONFIG_SECTOR after with the rboot parameters and user parameters
the define BOOT_BITS_ADDR indicates where the bits are stored, first half for continue-bits, last half for start-bits
they should be multiples of 8bytes which will assure that the start of the start_bits is at a 32bit address
the last byte will contain the amount of open continue-bits and is a signal for reflash of this sector
  --------------------------------------------- */
#define RBOOT_SIZE BOOT_BITS_ADDR-BOOT_CONFIG_SECTOR*SECTOR_SIZE
#define LAST_ADDR (BOOT_CONFIG_SECTOR+1)*SECTOR_SIZE
#define FIELD_SIZE (LAST_ADDR-BOOT_BITS_ADDR)/2
    uint32_t start_bits, continue_bits, help_bits, count;

    //read last byte of BOOT_CONFIG_SECTOR to see if we need to reflash it if we ran out of status bits
    //Important to do it ASAP since it reduces the chance of being interupted by a power cycle
    loadAddr=BOOT_BITS_ADDR+FIELD_SIZE;
    SPIRead(LAST_ADDR-4, &count, 4);
    if (count<33) { //default value is 0xffffffff
        SPIRead(BOOT_CONFIG_SECTOR * SECTOR_SIZE, buffer, RBOOT_SIZE);
        SPIEraseSector(BOOT_CONFIG_SECTOR);
        SPIWrite(BOOT_CONFIG_SECTOR * SECTOR_SIZE, buffer, RBOOT_SIZE);
        start_bits=(uint32_t)~0>>count; //clear start-bits based on value of count
        SPIWrite(loadAddr,&start_bits,4);
    }

#if defined BOOT_DELAY_MICROS && BOOT_DELAY_MICROS > 0
	// delay to slow boot (help see messages when debugging)
	ets_delay_us(BOOT_DELAY_MICROS);
#endif

    ets_printf("\r\nrBoot4LCM v1.0.0\r\n");
    if (count<33)  ets_printf("reformatted start_bits field: %08x count: %d\n",start_bits,count);
    //find the beginning of start-bit-range
    do {SPIRead(loadAddr,&start_bits,4);
        if (start_bits) ets_printf("         %08x @ %04x\n",start_bits,loadAddr);
        loadAddr+=4;
    } while (!start_bits && loadAddr<LAST_ADDR); //until a non-zero value
    loadAddr-=4; //return to the address where start_bits was read
    
    SPIRead(loadAddr-FIELD_SIZE,&continue_bits,4);
    if (continue_bits!=~0 || loadAddr-FIELD_SIZE<=BOOT_BITS_ADDR) ets_printf("         %08x @ %04x",continue_bits,loadAddr-FIELD_SIZE);
    count=1;
    help_bits=~start_bits&continue_bits; //collect the bits that are not in start_bits
    while (help_bits) {help_bits&=(help_bits-1);count++;} //count the bits using Brian Kernighan’s Algorithm
    if (continue_bits==~0 && loadAddr-FIELD_SIZE>BOOT_BITS_ADDR) {
        SPIRead(loadAddr-FIELD_SIZE-4,&help_bits,4); //read the previous word
         ets_printf("%08x ffffffff @ %04x",help_bits,loadAddr-FIELD_SIZE-4);
        while (help_bits) {help_bits&=(help_bits-1);count++;} //count more bits
    }
     ets_printf(" => count: %d\n",count);
    
    //clear_start_bit();
    if (loadAddr<LAST_ADDR-4) {
        start_bits>>=1; //clear leftmost 1-bit
        SPIWrite(loadAddr,&start_bits,4);
    } else { //reflash this sector because we reached the end (encode count in last byte and do in next cycle)
        SPIWrite(LAST_ADDR-4,&count,4);
    }
    
    int led_pin=0,polarity=0,led_valid=0;
	SPIRead(BOOT_CONFIG_SECTOR * SECTOR_SIZE, romconf, sizeof(rboot_config)); // only to read the LED value and polarity
	int led_info=romconf->unused[1];
	//encoding: bit7 MUST be 0, bit6 MUST be 1, bit 5 don't care, bit 4 polarity, bits 3-0 led gpio pin number
	if (!(led_info&(1<<7)) && !((~led_info)&(1<<6))) {
	    polarity=led_info&0x10; led_pin=led_info&0x0f;
	    if ( (led_pin!=0 && led_pin<6) || led_pin>11 ) led_valid=1; //do not allow pins 0 and 6-11
        ets_printf("led_pin=%d,  polarity=%d,  led_valid=%d\n",led_pin,polarity>>4,led_valid);
	}

    uint32_t old_dir,new_dir,iomux_reg,old_iomux,gpio_func,new_iomux,old_out,new_out;
    if (count>1 && count<16) { //some devices have trouble to find the right timing for a power cycle so delay*3
        if (led_valid) {
            ets_printf("LED ON");
            //Support for LED driving //make the gpio pin an output
            old_dir = READ_PERI_REG(GPIO_ENABLE_OUT_ADDRESS);
            new_dir = old_dir | (1<<led_pin);
            WRITE_PERI_REG(GPIO_ENABLE_OUT_ADDRESS, new_dir);
            //set the function to be GPIO, it might not be by default // e.g. PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U,FUNC_GPIO2); 
            const uint8_t IOMUX_REG_OFFS[]= {0x34, 0x18, 0x38, 0x14, 0x3c, 0x40, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x04, 0x08, 0x0c, 0x10};
            const uint8_t IOMUX_GPIO_FUNC[]={0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30};
            iomux_reg = REG_IOMUX_BASE + IOMUX_REG_OFFS[led_pin];
            old_iomux = READ_PERI_REG(iomux_reg);
            gpio_func = IOMUX_GPIO_FUNC[led_pin];
            new_iomux = (old_iomux & ~IOMUX_FUNC_MASK) | gpio_func ;
            WRITE_PERI_REG(iomux_reg, new_iomux);
            //define the right led array value to apply for led=on
            old_out = READ_PERI_REG(REG_GPIO_BASE);
            new_out = polarity?old_out|(1<<led_pin):old_out&~(1<<led_pin);
            WRITE_PERI_REG(REG_GPIO_BASE,new_out); //and switch on the LED
        }
        ets_delay_us(2*BOOT_CYCLE_DELAY_MICROS); //additional two delay cycles
    }
    if (count<16) ets_delay_us(BOOT_CYCLE_DELAY_MICROS);
//==================================================//if we powercycle, this is where it stops!
    if (led_valid && count>1 && count<16) {
      	ets_printf(" and OFF\n");
        // set level, iomux & GPIO output mode back to initial values
        WRITE_PERI_REG(REG_GPIO_BASE,old_out); WRITE_PERI_REG(iomux_reg,old_iomux); WRITE_PERI_REG(GPIO_ENABLE_OUT_ADDRESS,old_dir);
    }

    help_bits=0; //clear_all_continue_bits
    if (loadAddr<LAST_ADDR-4) {
        if (continue_bits==~0 && loadAddr-FIELD_SIZE>BOOT_BITS_ADDR) SPIWrite(loadAddr-FIELD_SIZE-4,&help_bits,4);
        SPIWrite(loadAddr-FIELD_SIZE,&start_bits,4);
    } else { //reflash this sector because we reached the end (encode ZERO in last byte and do in next cycle)
        SPIWrite(LAST_ADDR-4,&help_bits,4);
    }
/* --------------------------------------------
   End of rboot4lcm key code. count is used further down for choosing rom and stored in rtc for ota-main to interpret
   --------------------------------------------- */

	// read rom header
	SPIRead(0, header, sizeof(rom_header));

	// print and get flash size
	ets_printf("Flash Size:   ");
	flag = header->flags2 >> 4;
	if (flag == 0) {
		ets_printf("4 Mbit\r\n");
		flashsize = 0x80000;
	} else if (flag == 1) {
		ets_printf("2 Mbit\r\n");
		flashsize = 0x40000;
	} else if (flag == 2) {
		ets_printf("8 Mbit\r\n");
		flashsize = 0x100000;
	} else if (flag == 3 || flag == 5) {
		ets_printf("16 Mbit\r\n");
#ifdef BOOT_BIG_FLASH
		flashsize = 0x200000;
#else
		flashsize = 0x100000; // limit to 8Mbit
#endif
	} else if (flag == 4 || flag == 6) {
		ets_printf("32 Mbit\r\n");
#ifdef BOOT_BIG_FLASH
		flashsize = 0x400000;
#else
		flashsize = 0x100000; // limit to 8Mbit
#endif
	} else if (flag == 8) {
		ets_printf("64 Mbit\r\n");
#ifdef BOOT_BIG_FLASH
		flashsize = 0x800000;
#else
		flashsize = 0x100000; // limit to 8Mbit
#endif
	} else if (flag == 9) {
		ets_printf("128 Mbit\r\n");
#ifdef BOOT_BIG_FLASH
		flashsize = 0x1000000;
#else
		flashsize = 0x100000; // limit to 8Mbit
#endif
	} else {
		ets_printf("unknown\r\n");
		// assume at least 4mbit
		flashsize = 0x80000;
	}

	// print spi mode
	ets_printf("Flash Mode:   ");
	if (header->flags1 == 0) {
		ets_printf("QIO\r\n");
	} else if (header->flags1 == 1) {
		ets_printf("QOUT\r\n");
	} else if (header->flags1 == 2) {
		ets_printf("DIO\r\n");
	} else if (header->flags1 == 3) {
		ets_printf("DOUT\r\n");
	} else {
		ets_printf("unknown\r\n");
	}

	// print spi speed
	ets_printf("Flash Speed:  ");
	flag = header->flags2 & 0x0f;
	if (flag == 0) ets_printf("40 MHz\r\n");
	else if (flag == 1) ets_printf("26.7 MHz\r\n");
	else if (flag == 2) ets_printf("20 MHz\r\n");
	else if (flag == 0x0f) ets_printf("80 MHz\r\n");
	else ets_printf("unknown\r\n");

	// print enabled options
#ifdef BOOT_BIG_FLASH
	ets_printf("rBoot Option: Big flash\r\n");
#endif
#ifdef BOOT_CONFIG_CHKSUM
	ets_printf("rBoot Option: Config chksum\r\n");
#endif
#ifdef BOOT_GPIO_ENABLED
	ets_printf("rBoot Option: GPIO rom mode (%d)\r\n", BOOT_GPIO_NUM);
#endif
#ifdef BOOT_GPIO_SKIP_ENABLED
	ets_printf("rBoot Option: GPIO skip mode (%d)\r\n", BOOT_GPIO_NUM);
#endif
#ifdef BOOT_RTC_ENABLED
	ets_printf("rBoot Option: RTC data\r\n");
#endif
#ifdef BOOT_IROM_CHKSUM
	ets_printf("rBoot Option: irom chksum\r\n");
#endif
	ets_printf("\r\n");

	// read boot config
	SPIRead(BOOT_CONFIG_SECTOR * SECTOR_SIZE, buffer, SECTOR_SIZE);
	// fresh install or old version?
	if (romconf->magic != BOOT_CONFIG_MAGIC || romconf->version != BOOT_CONFIG_VERSION
#ifdef BOOT_CONFIG_CHKSUM
		|| romconf->chksum != calc_chksum((uint8_t*)romconf, (uint8_t*)&romconf->chksum)
#endif
		) {
		// create a default config for a standard 2 rom setup
		ets_printf("Writing default boot config.\r\n");
		ets_memset(romconf, 0x00, sizeof(rboot_config));
		romconf->magic = BOOT_CONFIG_MAGIC;
		romconf->version = BOOT_CONFIG_VERSION;
		default_config(romconf, flashsize);
#ifdef BOOT_CONFIG_CHKSUM
		romconf->chksum = calc_chksum((uint8_t*)romconf, (uint8_t*)&romconf->chksum);
#endif
		// write new config sector
		SPIEraseSector(BOOT_CONFIG_SECTOR);
		SPIWrite(BOOT_CONFIG_SECTOR * SECTOR_SIZE, buffer, SECTOR_SIZE);
	}

	// try rom selected in the config, unless overriden by gpio/temp boot
	romToBoot = romconf->current_rom;

#ifdef BOOT_RTC_ENABLED
	// if rtc data enabled, check for valid data
	if (system_rtc_mem(RBOOT_RTC_ADDR, &rtc, sizeof(rboot_rtc_data), RBOOT_RTC_READ) &&
		(rtc.chksum == calc_chksum((uint8_t*)&rtc, (uint8_t*)&rtc.chksum))) {

		if (rtc.next_mode & MODE_TEMP_ROM) {
			if (rtc.temp_rom >= romconf->count) {
				ets_printf("Invalid temp rom selected.\r\n");
				return 0;
			}
			ets_printf("Booting temp rom.\r\n");
			temp_boot = 1;
			romToBoot = rtc.temp_rom;
		}
	}
#endif

#if defined(BOOT_GPIO_ENABLED) || defined (BOOT_GPIO_SKIP_ENABLED)
	if (perform_gpio_boot(romconf)) {
#if defined(BOOT_GPIO_ENABLED)
		if (romconf->gpio_rom >= romconf->count) {
			ets_printf("Invalid GPIO rom selected.\r\n");
			return 0;
		}
		ets_printf("Booting GPIO-selected rom.\r\n");
		romToBoot = romconf->gpio_rom;
		gpio_boot = 1;
#elif defined(BOOT_GPIO_SKIP_ENABLED)
		romToBoot = romconf->current_rom + 1;
		if (romToBoot >= romconf->count) {
			romToBoot = 0;
		}
		romconf->current_rom = romToBoot;
#endif
		updateConfig = 1;
		if (romconf->mode & MODE_GPIO_ERASES_SDKCONFIG) {
			ets_printf("Erasing SDK config sectors before booting.\r\n");
			for (sec = 1; sec < 5; sec++) {
				SPIEraseSector((flashsize / SECTOR_SIZE) - sec);
			}
		}
	}
#endif

	// check valid rom number
	// gpio/temp boots will have already validated this
	if (romconf->current_rom >= romconf->count) {
		// if invalid rom selected try rom 0
		ets_printf("Invalid rom selected, defaulting to 0.\r\n");
		romToBoot = 0;
		romconf->current_rom = 0;
		updateConfig = 1;
	}

    if (count>COUNT4USER) romToBoot = 1; //rboot4lcm takes priority over tmpboot and gpio
    
	// check rom is valid
	loadAddr = check_image(romconf->roms[romToBoot]);

#ifdef BOOT_GPIO_ENABLED
	if (gpio_boot && loadAddr == 0) {
		// don't switch to backup for gpio-selected rom
		ets_printf("GPIO boot rom (%d) is bad.\r\n", romToBoot);
		return 0;
	}
#endif
#ifdef BOOT_RTC_ENABLED
	if (temp_boot && loadAddr == 0) {
		// don't switch to backup for temp rom
		ets_printf("Temp boot rom (%d) is bad.\r\n", romToBoot);
		// make sure rtc temp boot mode doesn't persist
		rtc.next_mode = MODE_STANDARD;
		rtc.chksum = calc_chksum((uint8_t*)&rtc, (uint8_t*)&rtc.chksum);
		system_rtc_mem(RBOOT_RTC_ADDR, &rtc, sizeof(rboot_rtc_data), RBOOT_RTC_WRITE);
		return 0;
	}
#endif

	// check we have a good rom
	while (loadAddr == 0) {
		ets_printf("Rom %d at %x is bad.\r\n", romToBoot, romconf->roms[romToBoot]);
		// for normal mode try each previous rom
		// until we find a good one or run out
		updateConfig = 1;
		romToBoot--;
		if (romToBoot < 0) romToBoot = romconf->count - 1;
		if (romToBoot == romconf->current_rom) {
			// tried them all and all are bad!
			ets_printf("No good rom available.\r\n");
			return 0;
		}
		loadAddr = check_image(romconf->roms[romToBoot]);
	}

#ifndef OTA_MAIN_SECTOR  //for rboot4LCM OTA never rewrite
	// re-write config, if required
	if (updateConfig) {
		ets_printf("Re-writing config with Rom %d.\r\n",romToBoot);
		romconf->current_rom = romToBoot;
#ifdef BOOT_CONFIG_CHKSUM
		romconf->chksum = calc_chksum((uint8_t*)romconf, (uint8_t*)&romconf->chksum);
#endif
		SPIEraseSector(BOOT_CONFIG_SECTOR);
		SPIWrite(BOOT_CONFIG_SECTOR * SECTOR_SIZE, buffer, SECTOR_SIZE);
	}
#endif

#ifdef BOOT_RTC_ENABLED
	// set rtc boot data for app to read
	rtc.magic = RBOOT_RTC_MAGIC;
	rtc.next_mode = MODE_STANDARD;
	rtc.last_mode = MODE_STANDARD;
	if (temp_boot) rtc.last_mode |= MODE_TEMP_ROM;
#ifdef BOOT_GPIO_ENABLED
	if (gpio_boot) rtc.last_mode |= MODE_GPIO_ROM;
#endif
	rtc.last_rom = romToBoot;
	rtc.temp_rom = count; //we (ab)use this field to convey the count value on the way out
	rtc.chksum = calc_chksum((uint8_t*)&rtc, (uint8_t*)&rtc.chksum);
	system_rtc_mem(RBOOT_RTC_ADDR, &rtc, sizeof(rboot_rtc_data), RBOOT_RTC_WRITE);
#endif

	ets_printf("Booting rom %d at %x, load addr %x.\r\n", romToBoot, romconf->roms[romToBoot], loadAddr);
	// copy the loader to top of iram
	ets_memcpy((void*)_text_addr, _text_data, _text_len);
	// return address to load from
	return loadAddr;

}

#ifdef BOOT_NO_ASM

// small stub method to ensure minimum stack space used
void call_user_start(void) {
	uint32_t addr;
	stage2a *loader;

	addr = find_image();
	if (addr != 0) {
		loader = (stage2a*)entry_addr;
		loader(addr);
	}
}

#else

// assembler stub uses no stack space
// works with gcc
void call_user_start(void) {
	__asm volatile (
		"mov a15, a0\n"          // store return addr, hope nobody wanted a15!
		"call0 find_image\n"     // find a good rom to boot
		"mov a0, a15\n"          // restore return addr
		"bnez a2, 1f\n"          // ?success
		"ret\n"                  // no, return
		"1:\n"                   // yes...
		"movi a3, entry_addr\n"  // get pointer to entry_addr
		"l32i a3, a3, 0\n"       // get value of entry_addr
		"jx a3\n"                // now jump to it
	);
}

#endif
