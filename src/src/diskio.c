/*-----------------------------------------------------------------------*/
/* PicoBoy: FatFs <-> sd.c (SPI SD) glue. Single fixed volume, pdrv 0.    */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "sd.h"

#define PDRV_SD  0

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != PDRV_SD) return STA_NOINIT;
    return sd_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != PDRV_SD) return STA_NOINIT;
    if (sd_ready()) return 0;          // already up from an earlier sd_init()
    sd_info_t info;
    return sd_init(&info) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != PDRV_SD)  return RES_PARERR;
    if (!sd_ready())      return RES_NOTRDY;
    return sd_read_blocks(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != PDRV_SD)  return RES_PARERR;
    if (!sd_ready())      return RES_NOTRDY;
    return sd_write_blocks(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != PDRV_SD) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;                                  // writes block until the card finishes
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = (LBA_t)sd_sector_count();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;                             // erase block unknown -> 1
            return RES_OK;
        default:
            return RES_PARERR;
    }
}