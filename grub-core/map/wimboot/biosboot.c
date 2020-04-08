/*
 * Copyright (C) 2012 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file
 *
 * Main entry point
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wimboot.h>
#include <peloader.h>
#include <int13.h>
#include <vfat.h>
#include <lznt1.h>
#include <xca.h>
#include <wimpatch.h>
#include <wimfile.h>

/** Start of our image (defined by linker) */
extern char _start[];

/** End of our image (defined by linker) */
extern char _end[];

/** Command line */
char *cmdline;

/** initrd */
void *initrd;

/** Length of initrd */
size_t initrd_len;

/** bootmgr.exe path within WIM */
static const wchar_t bootmgr_path[] = L"\\Windows\\Boot\\PXE\\bootmgr.exe";

/** bootmgr.exe file */
static struct vdisk_file *bootmgr;

/** Minimal length of embedded bootmgr.exe */
#define BOOTMGR_MIN_LEN 16384

/** Memory regions */
enum {
	WIMBOOT_REGION = 0,
	PE_REGION,
	INITRD_REGION,
	NUM_REGIONS
};

/**
 * Wrap interrupt callback
 *
 * @v params		Parameters
 */
static void call_interrupt_wrapper ( struct bootapp_callback_params *params ) {
	uint16_t *attributes;

	/* Handle/modify/pass-through interrupt as required */
	if ( params->vector.interrupt == 0x13 ) {

		/* Intercept INT 13 calls for the emulated drive */
		emulate_int13 ( params );

	} else if ( ( params->vector.interrupt == 0x10 ) &&
		    ( params->ax == 0x4f01 ) &&
		    ( ! cmdline_gui ) ) {

		/* Mark all VESA video modes as unsupported */
		attributes = REAL_PTR ( params->es, params->di );
		call_interrupt ( params );
		*attributes &= ~0x0001;

	} else {

		/* Pass through interrupt */
		call_interrupt ( params );
	}
}

/** Real-mode callback functions */
static struct bootapp_callback_functions callback_fns = {
	.call_interrupt = call_interrupt_wrapper,
	.call_real = call_real,
};

/** Real-mode callbacks */
static struct bootapp_callback callback = {
	.fns = &callback_fns,
};

/** Boot application descriptor set */
static struct {
	/** Boot application descriptor */
	struct bootapp_descriptor bootapp;
	/** Boot application memory descriptor */
	struct bootapp_memory_descriptor memory;
	/** Boot application memory descriptor regions */
	struct bootapp_memory_region regions[NUM_REGIONS];
	/** Boot application entry descriptor */
	struct bootapp_entry_descriptor entry;
	struct bootapp_entry_wtf1_descriptor wtf1;
	struct bootapp_entry_wtf2_descriptor wtf2;
	struct bootapp_entry_wtf3_descriptor wtf3;
	struct bootapp_entry_wtf3_descriptor wtf3_copy;
	/** Boot application callback descriptor */
	struct bootapp_callback_descriptor callback;
	/** Boot application pointless descriptor */
	struct bootapp_pointless_descriptor pointless;
} __attribute__ (( packed )) bootapps = {
	.bootapp = {
		.signature = BOOTAPP_SIGNATURE,
		.version = BOOTAPP_VERSION,
		.len = sizeof ( bootapps ),
		.arch = BOOTAPP_ARCH_I386,
		.memory = offsetof ( typeof ( bootapps ), memory ),
		.entry = offsetof ( typeof ( bootapps ), entry ),
		.xxx = offsetof ( typeof ( bootapps ), wtf3_copy ),
		.callback = offsetof ( typeof ( bootapps ), callback ),
		.pointless = offsetof ( typeof ( bootapps ), pointless ),
	},
	.memory = {
		.version = BOOTAPP_MEMORY_VERSION,
		.len = sizeof ( bootapps.memory ),
		.num_regions = NUM_REGIONS,
		.region_len = sizeof ( bootapps.regions[0] ),
		.reserved_len = sizeof ( bootapps.regions[0].reserved ),
	},
	.entry = {
		.signature = BOOTAPP_ENTRY_SIGNATURE,
		.flags = BOOTAPP_ENTRY_FLAGS,
	},
	.wtf1 = {
		.flags = 0x11000001,
		.len = sizeof ( bootapps.wtf1 ),
		.extra_len = ( sizeof ( bootapps.wtf2 ) +
			       sizeof ( bootapps.wtf3 ) ),
	},
	.wtf3 = {
		.flags = 0x00000006,
		.len = sizeof ( bootapps.wtf3 ),
		.boot_partition_offset = ( VDISK_VBR_LBA * VDISK_SECTOR_SIZE ),
		.xxx = 0x01,
		.mbr_signature = VDISK_MBR_SIGNATURE,
	},
	.wtf3_copy = {
		.flags = 0x00000006,
		.len = sizeof ( bootapps.wtf3 ),
		.boot_partition_offset = ( VDISK_VBR_LBA * VDISK_SECTOR_SIZE ),
		.xxx = 0x01,
		.mbr_signature = VDISK_MBR_SIGNATURE,
	},
	.callback = {
		.callback = &callback,
	},
	.pointless = {
		.version = BOOTAPP_POINTLESS_VERSION,
	},
};

/**
 * Test if a paragraph is empty
 *
 * @v pgh		Paragraph
 * @ret is_empty	Paragraph is empty (all zeroes)
 */
static int is_empty_pgh ( const void *pgh ) {
	const uint32_t *dwords = pgh;

	return ( ( dwords[0] | dwords[1] | dwords[2] | dwords[3] ) == 0 );
}

/**
 * Main entry point
 *
 */
grub_size_t
wimboot_boot (void)
{
  size_t padded_len;
  void *raw_pe;
  struct loaded_pe pe;

	/* Read bootmgr.exe into memory */
	if ( ! bootmgr )
		die ( "FATAL: no bootmgr.exe\n" );
	if ( bootmgr->read == read_file ) {
		raw_pe = bootmgr->opaque;
	} else {
		padded_len = ( ( bootmgr->len + PAGE_SIZE - 1 ) &
			       ~( PAGE_SIZE -1 ) );
		raw_pe = ( initrd - padded_len );
		bootmgr->read ( bootmgr, raw_pe, 0, bootmgr->len );
	}

	/* Load bootmgr.exe into memory */
	if ( load_pe ( raw_pe, bootmgr->len, &pe ) != 0 )
		die ( "FATAL: Could not load bootmgr.exe\n" );

	/* Complete boot application descriptor set */
	bootapps.bootapp.pe_base = pe.base;
	bootapps.bootapp.pe_len = pe.len;
	bootapps.regions[WIMBOOT_REGION].start_page = page_start ( _start );
	bootapps.regions[WIMBOOT_REGION].num_pages = page_len ( _start, _end );
	bootapps.regions[PE_REGION].start_page = page_start ( pe.base );
	bootapps.regions[PE_REGION].num_pages =
		page_len ( pe.base, ( pe.base + pe.len ) );
	bootapps.regions[INITRD_REGION].start_page = page_start ( initrd );
	bootapps.regions[INITRD_REGION].num_pages =
		page_len ( initrd, initrd + initrd_len );

	/* Jump to PE image */
	DBG ( "Entering bootmgr.exe with parameters at %p\n", &bootapps );
	if ( cmdline_pause )
		pause();
	pe.entry ( &bootapps.bootapp );
	die ( "FATAL: bootmgr.exe returned\n" );
}