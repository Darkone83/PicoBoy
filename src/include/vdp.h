#ifndef _VDP_H_
#define _VDP_H_

/* Display timing (NTSC) */
#define MASTER_CLOCK        (3579545)
#define LINES_PER_FRAME     (262)
#define FRAMES_PER_SECOND   (60)
#define CYCLES_PER_LINE     ((MASTER_CLOCK / FRAMES_PER_SECOND) / LINES_PER_FRAME)

/* VDP context */
typedef struct {
    uint8 vram[0x4000];
    uint8 cram[0x40];
    uint8 reg[0x10];
    uint8 status;
    uint8 latch;
    uint8 pending;
    uint8 buffer;
    uint8 code;
    uint16 addr;
    int ntab;
    int satb;
    int line;
    int left;
    uint8 limit;
} t_vdp;

/*
 * PicoBoy: keep the VDP context inside the shared emulator arena rather than
 * reserving its embedded 16 KiB VRAM permanently in .bss.
 * sms_core.c allocates pb_vdp_ptr before system_init().
 */
extern t_vdp *pb_vdp_ptr;
#define vdp (*pb_vdp_ptr)

/* Function prototypes */
void vdp_init(void);
void vdp_reset(void);
void vdp_ctrl_w(int data);
int vdp_ctrl_r(void);
uint8 vdp_vcounter_r(void);
uint8 vdp_hcounter_r(void);
void vdp_data_w(int data);
int vdp_data_r(void);
void vdp_run(void);

#endif /* _VDP_H_ */