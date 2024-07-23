#ifndef AXIDMA_H_
#define AXIDMA_H_

// Kernel dependencies

/******************************************************************/
/* Definitions for driver AXIDMA */
#define XPAR_XAXIDMA_NUM_INSTANCES 2

/* Definitions for peripheral AXI_DMA_0 */
#define XPAR_AXI_DMA_0_DEVICE_ID 0
#define XPAR_AXI_DMA_0_BASEADDR 0x40400000
#define XPAR_AXI_DMA_0_HIGHADDR 0x4040FFFF
#define XPAR_AXI_DMA_0_SG_INCLUDE_STSCNTRL_STRM 0
#define XPAR_AXI_DMA_0_INCLUDE_MM2S_DRE 0
#define XPAR_AXI_DMA_0_INCLUDE_S2MM_DRE 0
#define XPAR_AXI_DMA_0_INCLUDE_MM2S 1
#define XPAR_AXI_DMA_0_INCLUDE_S2MM 1
#define XPAR_AXI_DMA_0_M_AXI_MM2S_DATA_WIDTH 32
#define XPAR_AXI_DMA_0_M_AXI_S2MM_DATA_WIDTH 32
#define XPAR_AXI_DMA_0_INCLUDE_SG 0
#define XPAR_AXI_DMA_0_ENABLE_MULTI_CHANNEL 0
#define XPAR_AXI_DMA_0_NUM_MM2S_CHANNELS 1
#define XPAR_AXI_DMA_0_NUM_S2MM_CHANNELS 1
#define XPAR_AXI_DMA_0_MM2S_BURST_SIZE 16
#define XPAR_AXI_DMA_0_S2MM_BURST_SIZE 16
#define XPAR_AXI_DMA_0_MICRO_DMA 0
#define XPAR_AXI_DMA_0_ADDR_WIDTH 32
#define XPAR_AXI_DMA_0_SG_LENGTH_WIDTH 14


/******************************************************************/
/* Definitions for peripheral AXI_DMA_1 */
#define XPAR_AXI_DMA_1_DEVICE_ID 1
#define XPAR_AXI_DMA_1_BASEADDR 0x40410000
#define XPAR_AXI_DMA_1_HIGHADDR 0x4041FFFF
#define XPAR_AXI_DMA_1_SG_INCLUDE_STSCNTRL_STRM 0
#define XPAR_AXI_DMA_1_INCLUDE_MM2S_DRE 0
#define XPAR_AXI_DMA_1_INCLUDE_S2MM_DRE 0
#define XPAR_AXI_DMA_1_INCLUDE_MM2S 1
#define XPAR_AXI_DMA_1_INCLUDE_S2MM 1
#define XPAR_AXI_DMA_1_M_AXI_MM2S_DATA_WIDTH 32
#define XPAR_AXI_DMA_1_M_AXI_S2MM_DATA_WIDTH 32
#define XPAR_AXI_DMA_1_INCLUDE_SG 0
#define XPAR_AXI_DMA_1_ENABLE_MULTI_CHANNEL 0
#define XPAR_AXI_DMA_1_NUM_MM2S_CHANNELS 1
#define XPAR_AXI_DMA_1_NUM_S2MM_CHANNELS 1
#define XPAR_AXI_DMA_1_MM2S_BURST_SIZE 16
#define XPAR_AXI_DMA_1_S2MM_BURST_SIZE 16
#define XPAR_AXI_DMA_1_MICRO_DMA 0
#define XPAR_AXI_DMA_1_ADDR_WIDTH 32


/******************************************************************/

/* Definitions for driver CONV_UNIT */
#define XPAR_CONV_UNIT_NUM_INSTANCES 1

/* Definitions for peripheral CONV_UNIT_0 */
#define XPAR_CONV_UNIT_0_DEVICE_ID 0
#define XPAR_CONV_UNIT_0_S_AXI_BASEADDR 0x43C00000
#define XPAR_CONV_UNIT_0_S_AXI_HIGHADDR 0x43C0FFFF


/******************************************************************/

/* Definitions for driver MAX_POOLING */
#define XPAR_MAX_POOLING_NUM_INSTANCES 1

/* Definitions for peripheral MAX_POOLING_0 */
#define XPAR_MAX_POOLING_0_DEVICE_ID 0
#define XPAR_MAX_POOLING_0_S_AXI_BASEADDR 0x43C10000
#define XPAR_MAX_POOLING_0_S_AXI_HIGHADDR 0x43C1FFFF


/******************************************************************/

/* Definitions for peripheral IMAGE_BUFFER_0 */
#define XPAR_IMAGE_BUFFER_0_BASEADDR 0x43C20000
#define XPAR_IMAGE_BUFFER_0_HIGHADDR 0x43C2FFFF
#define IMAGE_BUFF_RESET (1<<0)
#endif /* AXIDMA_H_ */