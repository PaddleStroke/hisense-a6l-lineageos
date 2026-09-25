/* SPDX-License-Identifier: GPL-2.0 */
/* A6L (kvoice): shared between the a6l_ipa_bam fork of bam_dma and ipa-legacy. Not in mainline dmaengine.h (7.2 uses
 * bits 0..9 of enum dma_ctrl_flags). Sireesh Kodali's IPA v2 series defined DMA_PREP_IMM_CMD in dmaengine.h. */
#ifndef A6L_IPA_BAM_H
#define A6L_IPA_BAM_H
#ifndef DMA_PREP_IMM_CMD
#define DMA_PREP_IMM_CMD	(1 << 15)
#endif
#endif
