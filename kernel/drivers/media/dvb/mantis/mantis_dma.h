

#ifndef __MANTIS_DMA_H
#define __MANTIS_DMA_H

extern int mantis_dma_init(struct mantis_pci *mantis);
extern int mantis_dma_exit(struct mantis_pci *mantis);
extern void mantis_dma_start(struct mantis_pci *mantis);
extern void mantis_dma_stop(struct mantis_pci *mantis);
extern void mantis_dma_xfer(unsigned long data);

#endif /* __MANTIS_DMA_H */
