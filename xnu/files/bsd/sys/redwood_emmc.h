/*
 * The X6826B's eMMC: the system image's partition as a block device, read as the kernel asks for it.
 * See bsd/dev/redwood_emmc.c.
 */
#ifndef _SYS_REDWOOD_EMMC_H_
#define _SYS_REDWOOD_EMMC_H_

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

__BEGIN_DECLS
int redwood_emmc_root(dev_t *root);        /* emmc_root=1: the device, set up on the first call; 0 or an errno */
void redwood_emmc_devfs(void);             /* /dev/emmc0, /dev/remmc0 once devfs is up */
bool redwood_emmc_var_on(void);            /* /dev/remmc_var takes writes (emmc_write=1, the window checked) */
void redwood_emmc_var_stop(void);          /* no more writes, none in flight: before going back to the loader */
bool redwood_emmc_nv_find(const char *name, uint64_t *lba, uint64_t *blocks);   /* r1_md=5: an NV partition from the GPT, read only */
bool redwood_emmc_nv_read(uint64_t lba, uint32_t blocks, void *dst);           /* r1_md=5: blocks IN from an NV partition; there is no write */
__END_DECLS

#endif /* _SYS_REDWOOD_EMMC_H_ */
