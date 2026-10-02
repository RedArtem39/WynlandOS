#ifndef WYNLAND_HDA_H
#define WYNLAND_HDA_H

#include <wynland/types.h>

/* Intel High Definition Audio (drivers/sound/hda.c), seen by programs as
   an OSS /dev/dsp: write() 16-bit PCM, ioctl()s for format/rate/channels
   and buffer state -- what GStreamer's osssink speaks. */

#define DEV_DSP 0xFFFFFFC0   /* VfsNode.first_cluster sentinel for /dev/dsp */

void    hda_init(void);
bool    hda_present(void);
void    hda_tick(void);                       /* timer IRQ, every 10 ms */
int64_t hda_dsp_write(const void *buf, uint32_t len);
int64_t hda_dsp_ioctl(uint64_t request, uint64_t argp);
void    hda_dsp_open(void);

#endif
