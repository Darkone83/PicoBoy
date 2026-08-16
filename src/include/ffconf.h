/*---------------------------------------------------------------------------/
/  PicoBoy FatFs (R0.16) configuration
/
/  Single SPI SD volume. Long filenames ON (ROM names need them), code page 437,
/  read+write (for .srm / save states), no on-device format, no exFAT, no RTC.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* must match FF_DEFINED in ff.h */

/*---- Function Configurations ----*/
#define FF_FS_READONLY	0	/* need write for .srm and save states */
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		0	/* card is formatted on a PC */
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/*---- Locale and Namespace Configurations ----*/
#define FF_CODE_PAGE	437	/* U.S. */
#define FF_USE_LFN		1	/* 1: enabled, static working buffer (BSS); FS used only from core0 */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0	/* 0: ANSI/OEM (char* paths in CP437) */
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0	/* absolute paths only (/roms/...) */
#define FF_PATH_DEPTH	16

/*---- Drive/Volume Configurations ----*/
#define FF_VOLUMES		1
#define FF_STR_VOLUME_ID	0
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512	/* SD = fixed 512-byte sectors */
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

/*---- System Configurations ----*/
#define FF_FS_TINY		0
#define FF_FS_EXFAT		0	/* off: keeps code small; FAT32 cards (<=32GB) only */
#define FF_FS_NORTC		1	/* no clock: stamp files with the fixed date below */
#define FF_FS_CRTIME	0
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2025
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0