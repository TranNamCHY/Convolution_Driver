#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched/task.h>
#include <linux/dma/xilinx_dma.h>
#include <asm/siginfo.h>   
#include <linux/rcupdate.h> 
#include <linux/sched.h> 
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/random.h>
#include "paremeter.h"
#include "max_pooling.h"
#include "covid2.h"
#include "conv2d.h"
#include "utility.h"
#include <linux/mm.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
/*
 * This define was used to create character file.
*/

#define AXIDMA_FILE "file_axidma"
#define AXIDMA_CLASS "class_axidma"
#define AXIDMA_DEVICE "device_axidma"

/*
 * This structure was used to create character file.
*/
struct axidma_driver {
	struct class* class;
	struct device* device;
	dev_t dev;
	struct cdev cdev;
	struct device* container;
};

struct axidma_driver my_axidma_driver;
#define MAX_DEST_BUFFER 4 * 100 * 100
#define MAX_SRC_BUFFER 100 * 100
#define KERNEL_LEN 3 * 3
static unsigned int test_buf_size = MAX_DEST_BUFFER;
module_param(test_buf_size, uint, 0444);
MODULE_PARM_DESC(test_buf_size, "Size of the memcpy test buffer");


/*
 * Used to sending interrupt for the userspace application.
*/
char config_data[1024];
struct kernel_siginfo info;
struct task_struct *t;
static int pid = -1;
int sig_num = -1;
unsigned int pre_cmd;
uint16_t image_width;
uint16_t image_height;
/*
 * Buffer and dma_address used for dma operation.
*/
struct dma_memory {
	void* kernel_buffer; 
	dma_addr_t dma_address;
	int size;
};
void* coherent_source_buffer; 
void* coherent_dest_buffer;
void* weight;
struct page* page_weight;
dma_addr_t mapped_source_buffer;
dma_addr_t mapped_dest_buffer;
struct dma_chan *force_tx_chan;
struct dma_chan *force_rx_chan;

/*
 * Macro used for usespace application
*/
unsigned int pre_cmd;
#define MAGIC_NO	100
#define SET_PID_CMD	_IOW(MAGIC_NO, 1, int)
#define PRE_SRC_BUFF _IOW(MAGIC_NO, 2, int)
#define PRE_KERNEL_BUFF _IOW(MAGIC_NO, 3, int)
#define PRE_DEST_BUFF _IOW(MAGIC_NO, 4, int)
#define SET_IMAGE_HEIGHT_WIDTH _IOW(MAGIC_NO, 5, int)
#define START_CACULATE _IOW(MAGIC_NO, 6, int)
#define FORCE_START_CACULATE _IOW(MAGIC_NO, 7, int)
#define SIG_TEST 44

/*
 * Declare some struct variable for bare metal driver. 
 */
#define DMA_CONV_BASEADDR XPAR_AXI_DMA_0_BASEADDR
#define CONV_BASE_ADDRESS XPAR_CONV_UNIT_0_S_AXI_BASEADDR
#define DMA_MAXP_BASEADDR XPAR_AXI_DMA_1_BASEADDR
#define MAX_POOLING_BASE_ADDRESS XPAR_MAX_POOLING_0_S_AXI_BASEADDR
#define IMAGE_BUFF_BASEADDR XPAR_IMAGE_BUFFER_0_BASEADDR
#define MY_AXI_FIFO_BASEADDR AXI_FIFO_BASEADDR

/*
 * Data structure for Baremetal module.
*/
Con2D_Type Conv2D;
Image_type Image_buf;
Fifo_type Axi_Fifo;

/*
 * This two buffer was used in single dma map api
*/

struct dmatest_slave_thread {
	struct list_head node; // To store the entity to the list_head threads of dmatest_chan entity
	struct task_struct *task;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	u8 **srcs;
	u8 **dsts;
	enum dma_transaction_type type;
	bool done;
};

struct dmatest_chan {
	struct list_head node; // This node to add each dmatest_chan entity to the list_headd dmatest_channels.
	struct dma_chan *chan; 
	struct list_head threads; // This list_head store threads created to do the trasnfer request for a dmatest_channels.
	int chan_num;
	int thread_num;
	char name[20];
};

/*
 * These are protected by dma_list_mutex since they're only used by
 * the DMA filter function callback
 */
static DECLARE_WAIT_QUEUE_HEAD(thread_wait);
static LIST_HEAD(dmatest_channels);
static unsigned int nr_channels;
/*
 * Reset the operation of axi fifo module
*/
void fifo_init_reset(uint32_t threshold){
	int timeout = 2000;
	unsigned long mark_time;
	uint32_t offset;
	offset = offsetof(Fifo_type,Threshold);
	if(Axi_Fifo.BaseAddress  == NULL){
		pr_info("Detect NULL address of BaseAddress at line: %d\n",__LINE__);
		return ;
	}
	// Reset fifo, set reset register = 1 and check if counter been set to 0.
	write_and_check_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Reset)-offset,1u << 0,1u << 0,timeout,__LINE__);
    // Check the counter register has been reset.
	mark_time = jiffies;
	while(((jiffies_to_msecs(jiffies - mark_time)) < timeout) && (read_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Counter)-offset) != 0)){}
	// Disable Reset 
	write_and_check_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Reset)-offset,0,1u << 0,timeout,__LINE__);
	// Write to threshold register
	write_and_check_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Threshold) - offset, threshold, 0xFFFFFFFF, timeout, __LINE__);
}

/* 
 * Get the counter value from counter register of axi fifo module.
*/
uint32_t get_counter(void){
	uint32_t offset = offsetof(Fifo_type,Threshold);
	return read_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Counter) - offset);
}

/*
 * Get the threshold value from threshold registere of axi fifo module.
*/
uint32_t get_threshold(void){
	uint32_t offset = offsetof(Fifo_type,Threshold);
	return read_register(Axi_Fifo.BaseAddress,offsetof(Fifo_type,Threshold) - offset);
}

void conv2d_initial_fixed_paras(void) {
    uint32_t data = 0;
	int timeout = 2000;
	uint32_t offset =  offsetof(Con2D_Type,Status);
	if(Conv2D.BaseAddress ==  NULL){
		pr_info("Detect NULL address of BaseAddress at line: %d\n",__LINE__);
		return ;
	}
    //Con2D->IAddr = 0;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,IAddr)-offset,0,0xFFFFFFFF,timeout,__LINE__);

    /*
     * Note: No need to turn on and off between times we put the parameter in Address_and_Coeff
     * Because both the weight and address be transmitted at the same time
     */
    //Con2D->Control |= C_WE_IN;  // allow write to weight
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,C_WE_IN,1u << 1,timeout,__LINE__);

    data = 0;
    data |= K_ADDR;
    data |= K << COEFF_IN_LOW;
    //Con2D->Address_and_Coeff = data;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Address_and_Coeff)-offset,data,0xFFFFFFFF,timeout,__LINE__);

    data = 0;
    data |= K2_MINUS1_ADDR;
    data |= K2_MINUS1 << COEFF_IN_LOW;
    //Con2D->Address_and_Coeff = data;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Address_and_Coeff)-offset,data,0xFFFFFFFF,timeout,__LINE__);

    data = 0;
    data |= K2_MINUSK_ADDR;
    data |= K2_MINUSK << COEFF_IN_LOW;
    //Con2D->Address_and_Coeff = data;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Address_and_Coeff)-offset,data,0xFFFFFFFF,timeout,__LINE__);

	//Con2D->Control &= ~C_WE_IN;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,0u << 1,1u << 1,timeout,__LINE__);

}

void image_buffer_reset(void) {
	uint32_t offset;
	int timeout;
	offset =  offsetof(Image_type,Reset);
	timeout = 2000;
	//Image_buf->Reset = 0;
	write_and_check_register(Image_buf.BaseAddress,offsetof(Image_type,Reset)-offset,0u,0xFFFFFFFF,timeout,__LINE__);
	//Image_buf->Reset |= IMAGE_BUFF_RESET;
	write_and_check_register(Image_buf.BaseAddress,offsetof(Image_type,Reset)-offset,IMAGE_BUFF_RESET,1u << 0,timeout,__LINE__);
}

void conv2d_reset(void) {
	int timeout = 2000;
	uint32_t offset =  offsetof(Con2D_Type,Status);
    // reset all control paras
    //Con2D->Control = 0;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,0,0xFFFFFFFF,timeout,__LINE__);
    // Then turn on reset bit (negative reset occurs at previous command)
    //Con2D->Control |= RESET;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,1u << 0,RESET,timeout,__LINE__);
}

void conv2d_change_weight(int8_t* weight) {
	int timeout = 2000;
	uint8_t i;
	uint32_t offset =  offsetof(Con2D_Type,Status);
	if(weight == NULL){
		pr_info("Weight buffer has not been initialized");
		return;
	}
    //Con2D->Control |= C_WE_IN;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,C_WE_IN,1u << 1,timeout,__LINE__);
    for (i = 0; i <= K2_MINUS1; i++) {
        uint32_t data = 0;
        data |= (WEIGHT_STR_ADDR + i);
        data |= weight[i] << COEFF_IN_LOW;
        //Con2D->Address_and_Coeff = data;
		write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Address_and_Coeff)-offset,data,0xFFFFFFFF,timeout,__LINE__);
    }
    //Con2D->Control &= ~C_WE_IN;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,0u << 1,1u << 1,timeout,__LINE__);
}

void conv2d_start(void) {
	int timeout = 2000;
	uint32_t offset =  offsetof(Con2D_Type,Status);
    // Enable InputDmac & Convolution
    //Con2D->Control |= INPUTDMAC_START;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,INPUTDMAC_START,1u << 2,timeout,__LINE__);
    //Con2D->Control |= CONV2D_START;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,CONV2D_START,1u << 3,timeout,__LINE__);
    //Con2D->Control &= ~INPUTDMAC_START;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,0u << 2,1u << 2,timeout,__LINE__);
	//Con2D->Control &= ~CONV2D_START;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,Control)-offset,0u << 3,1u << 3,timeout,__LINE__);
}

void conv2d_change_size_paras(con2d_size_paras paras) {
	int timeout;
    uint32_t data;
	uint32_t offset;
	offset = offsetof(Con2D_Type,Status);
	if(Conv2D.BaseAddress ==  NULL){
		pr_info("Detect NULL address of BaseAddress at line: %d\n",__LINE__);
		return ;
	}
	data = 0;
	timeout = 2000;
    // set BCR register
    data |= paras.B;
    data |= paras.C << C_LOW;
    data |= paras.R << R_LOW;
    //Con2D->BCR = data;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,BCR)-offset,data,0xFFFFFFFF,timeout,__LINE__);
    // set M^2 - 1
    //Con2D->M2minus1 = paras.M2minus1;
	write_and_check_register(Conv2D.BaseAddress,offsetof(Con2D_Type,M2minus1)-offset,paras.M2minus1,0xFFFFFFFF,timeout,__LINE__);
}

/*
 * Ops file function for user space to interact with 
 * character device file.
 */
static int file_open (struct inode * inode, struct file *file);
static int file_release (struct inode * inode, struct file *file);
static ssize_t file_write (struct file * file, const char __user * buff, size_t length, loff_t * loff_of);
static ssize_t file_read (struct file * file,char __user * buff, size_t length, loff_t * loff_of);
static long file_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);
static int file_mmap(struct file *file, struct vm_area_struct *vma);
static void axidma_vma_close(struct vm_area_struct *vma);
struct file_operations axidma_fops = {
	.owner      = THIS_MODULE,
    .read       = file_read,
    .write      = file_write,
    .open       = file_open,
    .release    = file_release,
	.unlocked_ioctl  = file_ioctl,
	.mmap = file_mmap,
};

// The VMA operations for the AXI DMA device
static const struct vm_operations_struct axidma_vm_ops = {
    .close = axidma_vma_close,
};

static bool is_threaded_test_run(struct dmatest_chan *tx_dtc,
				 struct dmatest_chan *rx_dtc)
{
	struct dmatest_slave_thread *thread;
	int ret = false;
    /*
    Iter all dmatest_slave_thread elemnents of the
     struct list of tx and rx dmatest_chan and check if the bool done have been set to true.
    */
	list_for_each_entry(thread, &tx_dtc->threads, node) {
		if (!thread->done)
			ret = true;
	}

	list_for_each_entry(thread, &rx_dtc->threads, node) {
		if (!thread->done)
			ret = true;
	}
	return ret;
}

static bool my_is_threaded_test_run(int chan_num)
{
	struct dmatest_slave_thread *thread = NULL;
	struct dmatest_chan *dtc, *_dtc;
	struct dmatest_chan *wanted_tx_dtc = NULL;
	int flag = 0;
	/*
 	 * Find the tx_dtc channel to check the corresrponding threads.
 	 */
	list_for_each_entry_safe(dtc, _dtc, &dmatest_channels, node) {
		if(strstr(dtc->name,"tx_channel") != NULL && dtc->chan_num == chan_num && &dtc->node != &dmatest_channels){
			wanted_tx_dtc = dtc;
			flag = 1;
			break;
		} 
	}
	if(!flag){
		return true;
	}
	/*
	 * Iter through the thread list of thread.
	 */
	list_for_each_entry(thread, &wanted_tx_dtc->threads, node) {
		if(!thread->done && &thread->node != &wanted_tx_dtc->threads)
			return false;
	}
	return true;
}

static void dmatest_slave_tx_callback(void *completion)
{
	//dump_stack();
	complete(completion);
}

static void dmatest_slave_rx_callback(void *completion)
{
	//dump_stack();
	complete(completion);
}

static int force_coherent_test_my_dmatest_slave_func(void)
{
	//unsigned long mark_time = jiffies;
	struct dma_chan *tx_chan = force_tx_chan;
	struct dma_chan *rx_chan = force_rx_chan;
	const char *thread_name;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags;
	int ret = 0;
	struct dma_device *tx_dev = tx_chan->device;
	struct dma_device *rx_dev = rx_chan->device;
	struct dma_async_tx_descriptor *txd = NULL;
	struct dma_async_tx_descriptor *rxd = NULL;
	struct completion rx_cmp;
	struct completion tx_cmp;
	con2d_size_paras default_paras;
	unsigned long rx_tmo = msecs_to_jiffies(5000); /* RX takes longer */
	unsigned long tx_tmo = msecs_to_jiffies(5000);
	u8 align = 0;
	struct scatterlist tx_sg[1];
	struct scatterlist rx_sg[1];
	//thread_name = current->comm;
	/* pr_info("Kernel filter: ");
	for(i = 0; i < 3; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < 3; j++){
			printk(KERN_CONT  "%d ", tempt_kernel_buffer[i*3 + j]);
		}
		printk(KERN_CONT "]\n");
	}
	pr_info("Source buffer:");
	for(i = 0; i < image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < image_width; j++){
			printk(KERN_CONT  "%d ", tempt_source_buffer[i * image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */
	/* Ensure that all previous reads are complete */
	smp_rmb();
	/* 
	 * Set nice value (Priority) of this thread to 10.
	 */
	conv2d_initial_fixed_paras();
	
	/*
	 * Set up paramerter for conv module.
 	 */
	default_paras.B = image_height - 2;
	default_paras.C = image_width;
	default_paras.R = 3;
	default_paras.M2minus1 = (image_width - 2)*(image_height - 2) -1;
	conv2d_change_size_paras(default_paras);
	conv2d_reset();
	image_buffer_reset();
	conv2d_change_weight((int8_t*) weight);
	conv2d_start();
	fifo_init_reset((image_width - 2) * (image_height - 2) - 1);
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
		/*
		 * Get the dma_device from tx_chan and rx_chan got from dmatest_add_slave_threads
		 */
		/* honor larger alignment restrictions */
		align = tx_dev->copy_align;
		//pr_info("Align value: %d, %d\n",align,rx_dev->copy_align);
		if (rx_dev->copy_align > align)
			align = rx_dev->copy_align;

		if ((1 << align) > test_buf_size) {
			pr_err("%u-byte buffer too small for %d-byte alignment\n",
			       test_buf_size, 1 << align);
			ret = -1; 
			goto error;
		}
		/*
		 * !!! Note: No need to ensure the source_length divisible by 4 anymore
		 */
		//len = (source_length >> align) << align;
		//dma_srcs = dma_map_single(tx_dev->dev,source_buffer,len,DMA_MEM_TO_DEV);
		//dma_dsts = dma_map_single(rx_dev->dev,dest_buffer,dest_length,DMA_BIDIRECTIONAL);
		
		sg_init_table(tx_sg,1);
		sg_init_table(rx_sg,1);

		sg_dma_address(&tx_sg[0]) = mapped_source_buffer;
		sg_dma_address(&rx_sg[0]) = mapped_dest_buffer;
		 
		sg_dma_len(&tx_sg[0]) =  image_height * image_width;
		sg_dma_len(&rx_sg[0]) = (image_height - 2) * (image_width -2 ) * 4;
		//pr_info("Source Len: %d\n",len);
		//pr_info("Dest_length: %d\n",dest_length);
		rxd = rx_dev->device_prep_slave_sg(rx_chan,rx_sg,1,DMA_DEV_TO_MEM,flags,NULL);
		txd = tx_dev->device_prep_slave_sg(tx_chan,tx_sg,1,DMA_MEM_TO_DEV,flags,NULL);
		
		if(!rxd || !txd){
			///dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
			//dma_unmap_single(rx_dev->dev,dma_dsts,len,DMA_BIDIRECTIONAL);
			pr_info("Unable with device_prep_slave_sg function, At line: %d\n",__LINE__);
			ret = -1;
			goto error;
		}
		
		init_completion(&rx_cmp);
		rxd->callback = dmatest_slave_rx_callback;
		rxd->callback_param = &rx_cmp;
		rx_cookie = rxd->tx_submit(rxd);

		init_completion(&tx_cmp);
		txd->callback = dmatest_slave_tx_callback;
		txd->callback_param = &tx_cmp;
		tx_cookie = txd->tx_submit(txd);
		if(dma_submit_error(rx_cookie) || 
		   dma_submit_error(tx_cookie)) {
			pr_info("Error with dma_submit_error function, At line: %d\n",__LINE__);
			ret = -1;
			goto error;
		   }
		
		dma_async_issue_pending(rx_chan);
		dma_async_issue_pending(tx_chan);

		tx_tmo = wait_for_completion_timeout(&tx_cmp,tx_tmo);
		status = dma_async_is_tx_complete(tx_chan,tx_cookie,NULL,NULL);

		if(tx_tmo == 0){
			pr_info("Tx transfer timed out\n");
			ret = -1;
			goto error;
		}
		else if(status != DMA_COMPLETE) {
			pr_info("Got the tx call back, but status is: %s",status == DMA_ERROR ? "error" : "in progress");
			ret = -1;
			goto error;
		}

		rx_tmo = wait_for_completion_timeout(&rx_cmp,rx_tmo);
		status = dma_async_is_tx_complete(rx_chan,rx_cookie,NULL,NULL);

		if(rx_tmo == 0){
			pr_info("Tx transfer timed out\n");
			ret = -1;
			goto error;
		} 
		else if(status != DMA_COMPLETE) {
			pr_info("Got the rx call back, but status is: %s",status == DMA_ERROR ? "error" : "in progress");
			ret = -1;
			goto error;
		}
		/* for(i=0;i<30;i++){
			printk(KERN_CONT  "%d ", tempt_dest_buffer[i]);
		}
		for(i = 0; i < (image_height - 2); i++){
			printk(KERN_INFO "[ ");
			for(j = 0; j < (image_width -2); j++){
				printk(KERN_CONT  "%d ", tempt_dest_buffer[i*(image_width-2) + j]);
			}
			printk(KERN_CONT "]\n");
		} */
	//pr_info("Slave thread take: %d minisecond", jiffies_to_msecs(jiffies - mark_time));
 error:
	return ret;
}

static int tempt_force_coherent_test_my_dmatest_slave_func(void)
{

	int ret = 0;
	int i,j,k,q;
	int8_t* tempt_source_buffer;
	int32_t * tempt_dest_buffer;
	int8_t* tempt_kernel_buffer;
	u32 dest_image_height;
	u32 dest_image_width;
	tempt_source_buffer = (int8_t*)coherent_source_buffer;
	if(tempt_source_buffer == NULL){
		pr_info("tempt_source_buffer NULL value");
		goto end;
	}
	/* pr_info("Source buffer: ");
	for(i = 0; i < image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < image_width; j++){
			printk(KERN_CONT  "%d ", tempt_source_buffer[i*image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */

	tempt_dest_buffer = (int32_t*)coherent_dest_buffer;
	if(tempt_dest_buffer == NULL){
		pr_info("tempt_dest_buffer NULL value");
		goto end;
	}
	dest_image_height = image_height -3 + 1;
	dest_image_width = image_width - 3 + 1;

	tempt_kernel_buffer = (int8_t*)weight;
	if(tempt_kernel_buffer == NULL){
		pr_info("tempt_kernel_buffer NULL value");
		goto end;
	}
	
	/* pr_info("Kernel buffer: ");
	for(k = 0; k < 3; k++){
		printk(KERN_INFO "[ ");
		for(q = 0; q < 3; q++){
			printk(KERN_CONT " %d ", tempt_kernel_buffer[k*3 + q]);
		}
		printk(KERN_CONT "]\n");
	} */

	for(i = 0; i < dest_image_height; i++){
		for(j = 0; j < dest_image_width; j++){
			tempt_dest_buffer[i * dest_image_height + j] = 0;
			for(k = 0; k < 3; k++){
				for(q = 0; q < 3; q++){
					tempt_dest_buffer[i * dest_image_height + j] += tempt_kernel_buffer[k*3 + q] * tempt_source_buffer[((i) + k ) * image_height + (j) + q];
				}
			}
		}
	}

	/* pr_info("Dest buffer: ");
	for(i = 0; i < dest_image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < dest_image_width; j++){
			printk(KERN_CONT  "%d ", tempt_dest_buffer[i * dest_image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */
		
    /*
     * Notify for the dmatest_add_slave_channels that the thread has finished.
     */
	 
	//dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
	//dma_unmap_single(rx_dev->dev,dma_dsts,dest_length,DMA_BIDIRECTIONAL);
end:
	return 0;
}

static int tempt_coherent_test_my_dmatest_slave_func(void *data)
{
	unsigned long mark_time = jiffies;
	struct dmatest_slave_thread	*thread = data;
	struct dma_chan *tx_chan = thread->tx_chan;
	struct dma_chan *rx_chan = thread->rx_chan;
	const char *thread_name;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags;
	int ret = 0;
	struct dma_device *tx_dev = tx_chan->device;
	struct dma_device *rx_dev = rx_chan->device;
	struct dma_async_tx_descriptor *txd = NULL;
	struct dma_async_tx_descriptor *rxd = NULL;
	struct completion rx_cmp;
	struct completion tx_cmp;
	con2d_size_paras default_paras;
	int i = 0;
	int j = 0;
	int8_t* tempt_kernel_buffer = (int8_t*)(weight);
	int8_t* tempt_source_buffer = (int8_t*)(coherent_source_buffer);
	int32_t* tempt_dest_buffer = (int32_t*)(coherent_dest_buffer);
	unsigned long rx_tmo = msecs_to_jiffies(5000); /* RX takes longer */
	unsigned long tx_tmo = msecs_to_jiffies(5000);
	u8 align = 0;
	struct scatterlist tx_sg[1];
	struct scatterlist rx_sg[1];
	thread_name = current->comm;
	/* pr_info("Kernel filter: ");
	for(i = 0; i < 3; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < 3; j++){
			printk(KERN_CONT  "%d ", tempt_kernel_buffer[i*3 + j]);
		}
		printk(KERN_CONT "]\n");
	}
	pr_info("Source buffer:");
	for(i = 0; i < image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < image_width; j++){
			printk(KERN_CONT  "%d ", tempt_source_buffer[i * image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */
	/* Ensure that all previous reads are complete */
	smp_rmb();
	/* 
	 * Set nice value (Priority) of this thread to 10.
	 */
	set_user_nice(current, 10);
	
	conv2d_initial_fixed_paras();
	
	/*
	 * Set up paramerter for conv module.
 	 */
	default_paras.B = image_height - 2;
	default_paras.C = image_width;
	default_paras.R = 3;
	default_paras.M2minus1 = (image_width - 2)*(image_height - 2) -1;
	conv2d_change_size_paras(default_paras);
	conv2d_reset();
	image_buffer_reset();
	conv2d_change_weight((int8_t*) weight);
	conv2d_start();
	fifo_init_reset((image_width - 2) * (image_height - 2) - 1);
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
		/*
		 * Get the dma_device from tx_chan and rx_chan got from dmatest_add_slave_threads
		 */
		/* honor larger alignment restrictions */
		align = tx_dev->copy_align;
		//pr_info("Align value: %d, %d\n",align,rx_dev->copy_align);
		if (rx_dev->copy_align > align)
			align = rx_dev->copy_align;

		if ((1 << align) > test_buf_size) {
			pr_err("%u-byte buffer too small for %d-byte alignment\n",
			       test_buf_size, 1 << align);
			ret = -1; 
			goto error;
		}
		/*
		 * !!! Note: No need to ensure the source_length divisible by 4 anymore
		 */
		//len = (source_length >> align) << align;
		//dma_srcs = dma_map_single(tx_dev->dev,source_buffer,len,DMA_MEM_TO_DEV);
		//dma_dsts = dma_map_single(rx_dev->dev,dest_buffer,dest_length,DMA_BIDIRECTIONAL);
		
		sg_init_table(tx_sg,1);
		sg_init_table(rx_sg,1);

		sg_dma_address(&tx_sg[0]) = mapped_source_buffer;
		sg_dma_address(&rx_sg[0]) = mapped_dest_buffer;
		 
		sg_dma_len(&tx_sg[0]) =  image_height * image_width;
		sg_dma_len(&rx_sg[0]) = (image_height - 2) * (image_width -2 ) * 4;
		//pr_info("Source Len: %d\n",len);
		//pr_info("Dest_length: %d\n",dest_length);
		rxd = rx_dev->device_prep_slave_sg(rx_chan,rx_sg,1,DMA_DEV_TO_MEM,flags,NULL);
		txd = tx_dev->device_prep_slave_sg(tx_chan,tx_sg,1,DMA_MEM_TO_DEV,flags,NULL);
		
		if(!rxd || !txd){
			///dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
			//dma_unmap_single(rx_dev->dev,dma_dsts,len,DMA_BIDIRECTIONAL);
			pr_info("Unable with device_prep_slave_sg function, At line: %d\n",__LINE__);
			ret = -1;
			goto error;
		}
		
		init_completion(&rx_cmp);
		rxd->callback = dmatest_slave_rx_callback;
		rxd->callback_param = &rx_cmp;
		rx_cookie = rxd->tx_submit(rxd);

		init_completion(&tx_cmp);
		txd->callback = dmatest_slave_tx_callback;
		txd->callback_param = &tx_cmp;
		tx_cookie = txd->tx_submit(txd);

		if(dma_submit_error(rx_cookie) || 
		   dma_submit_error(tx_cookie)) {
			pr_info("Error with dma_submit_error function, At line: %d\n",__LINE__);
			ret = -1;
			goto error;
		   }
		
		dma_async_issue_pending(rx_chan);
		dma_async_issue_pending(tx_chan);

		tx_tmo = wait_for_completion_timeout(&tx_cmp,tx_tmo);
		status = dma_async_is_tx_complete(tx_chan,tx_cookie,NULL,NULL);

		if(tx_tmo == 0){
			pr_info("Tx transfer timed out\n");
			ret = -1;
			goto error;
		}
		else if(status != DMA_COMPLETE) {
			pr_info("Got the tx call back, but status is: %s",status == DMA_ERROR ? "error" : "in progress");
			ret = -1;
			goto error;
		}

		rx_tmo = wait_for_completion_timeout(&rx_cmp,rx_tmo);
		status = dma_async_is_tx_complete(rx_chan,rx_cookie,NULL,NULL);

		if(rx_tmo == 0){
			pr_info("Tx transfer timed out\n");
			ret = -1;
			goto error;
		} 
		else if(status != DMA_COMPLETE) {
			pr_info("Got the rx call back, but status is: %s",status == DMA_ERROR ? "error" : "in progress");
			ret = -1;
			goto error;
		}
		/* for(i=0;i<30;i++){
			printk(KERN_CONT  "%d ", tempt_dest_buffer[i]);
		}
		for(i = 0; i < (image_height - 2); i++){
			printk(KERN_INFO "[ ");
			for(j = 0; j < (image_width -2); j++){
				printk(KERN_CONT  "%d ", tempt_dest_buffer[i*(image_width-2) + j]);
			}
			printk(KERN_CONT "]\n");
		} */
	pr_info("Slave thread take: %d minisecond", jiffies_to_msecs(jiffies - mark_time));
 error:
	thread->done = true;
	put_task_struct(thread->task);
    /*
     * Notify for the dmatest_add_slave_channels that the thread has finished.
     */
	 
	//dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
	//dma_unmap_single(rx_dev->dev,dma_dsts,dest_length,DMA_BIDIRECTIONAL);
	wake_up(&thread_wait);
	if(pid < 0){
		pr_info("Pid was not initialized !");
		return ret;
	}
	rcu_read_lock();
	t = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (t != NULL) {
    rcu_read_unlock();      
    if (send_sig_info(sig_num, &info, t) < 0) {
			pr_info("send_sig_info error\n");
			return -1;
		}
		return 0;
	} else {
    	pr_info("pid_task error\n");
    	rcu_read_unlock();
    	return -1;
	}

	return ret;
}

static int coherent_test_my_dmatest_slave_func(void *data)
{
	struct dmatest_slave_thread	*thread = data;
	int ret = 0;
	int i,j,k,q;
	int8_t* tempt_source_buffer;
	int32_t * tempt_dest_buffer;
	int8_t* tempt_kernel_buffer;
	u32 dest_image_height;
	u32 dest_image_width;
	tempt_source_buffer = (int8_t*)coherent_source_buffer;
	if(tempt_source_buffer == NULL){
		pr_info("tempt_source_buffer NULL value");
		goto end;
	}
	/* pr_info("Source buffer: ");
	for(i = 0; i < image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < image_width; j++){
			printk(KERN_CONT  "%d ", tempt_source_buffer[i*image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */

	tempt_dest_buffer = (int32_t*)coherent_dest_buffer;
	if(tempt_dest_buffer == NULL){
		pr_info("tempt_dest_buffer NULL value");
		goto end;
	}
	dest_image_height = image_height -3 + 1;
	dest_image_width = image_width - 3 + 1;

	tempt_kernel_buffer = (int8_t*)weight;
	if(tempt_kernel_buffer == NULL){
		pr_info("tempt_kernel_buffer NULL value");
		goto end;
	}
	
	/* pr_info("Kernel buffer: ");
	for(k = 0; k < 3; k++){
		printk(KERN_INFO "[ ");
		for(q = 0; q < 3; q++){
			printk(KERN_CONT " %d ", tempt_kernel_buffer[k*3 + q]);
		}
		printk(KERN_CONT "]\n");
	} */

	for(i = 0; i < dest_image_height; i++){
		for(j = 0; j < dest_image_width; j++){
			tempt_dest_buffer[i * dest_image_height + j] = 0;
			for(k = 0; k < 3; k++){
				for(q = 0; q < 3; q++){
					tempt_dest_buffer[i * dest_image_height + j] += tempt_kernel_buffer[k*3 + q] * tempt_source_buffer[((i) + k ) * image_height + (j) + q];
				}
			}
		}
	}

	/* pr_info("Dest buffer: ");
	for(i = 0; i < dest_image_height; i++){
		printk(KERN_INFO "[ ");
		for(j = 0; j < dest_image_width; j++){
			printk(KERN_CONT  "%d ", tempt_dest_buffer[i * dest_image_height + j]);
		}
		printk(KERN_CONT "]\n");
	} */
		
    /*
     * Notify for the dmatest_add_slave_channels that the thread has finished.
     */
	 
	//dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
	//dma_unmap_single(rx_dev->dev,dma_dsts,dest_length,DMA_BIDIRECTIONAL);
end:
	thread->done = true;
	wake_up(&thread_wait);
	if(pid < 0){
		pr_info("Pid was not initialized !");
		return ret;
	}
	rcu_read_lock();
	t = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (t != NULL) {
    rcu_read_unlock();      
    if (send_sig_info(sig_num, &info, t) < 0) {
			pr_info("send_sig_info error\n");
			return -1;
		}
		return 0;
	} else {
    	pr_info("pid_task error\n");
    	rcu_read_unlock();
    	return -1;
	}
	put_task_struct(thread->task);
	return ret;
}

static void dmatest_cleanup_channel(struct dmatest_chan *dtc)
{
	struct dmatest_slave_thread *thread;
	struct dmatest_slave_thread *_thread;
	int ret;

	/*
	// Iter the list_head threads of dmatest_chan dtc.
	*/
	list_for_each_entry_safe(thread, _thread, &dtc->threads, node) {
		/*
		// Stop the thread created by the correspond dmatest_chan channel.
		*/
		ret = kthread_stop(thread->task);
		pr_debug("dmatest: thread %s exited with status %d\n",
			 thread->task->comm, ret);
		/*
		// Delete this thread from list_head threads of dmatest_chan channel.
		*/
		list_del(&thread->node);
		/*
		// Decrease the reference count of task struct structure
		*/
		put_task_struct(thread->task);
		kfree(thread);
	}
	kfree(dtc);
}

/*
 * Jus need to use this func with tx_dtc channel.
 */
static void dmatest_clean_all_threads(struct dmatest_chan *dtc){
	struct dmatest_slave_thread *thread;
	struct dmatest_slave_thread *_thread;
	int ret;

	list_for_each_entry_safe(thread, _thread, &dtc->threads, node) {
		ret = kthread_stop(thread->task);
		pr_debug("dmatest: thread %s exited with status %d\n",
			 thread->task->comm, ret);
		list_del(&thread->node);
		put_task_struct(thread->task);
		kfree(thread);
	}
}

/*
 * This function should be used by file_write
 */
static int my_dmatest_add_slave_threads(int chan_num)
{
	struct dmatest_slave_thread *thread = NULL;
	struct dmatest_slave_thread *_thread = NULL;
	struct dmatest_chan *dtc, *_dtc;
	struct dmatest_chan *wanted_tx_dtc = NULL;
	struct dmatest_chan *wanted_rx_dtc = NULL;
	int ret = 0;
	/*
 	 * Add thread to transmit and receive for the tx_dtc channel
 	 */
	list_for_each_entry_safe(dtc, _dtc, &dmatest_channels, node) {
		if(strstr(dtc->name,"tx_channel") != NULL && dtc->chan_num == chan_num && &dtc->node != &dmatest_channels){
			pr_info("Found tx dmatest channel number: %d\n",chan_num);
			wanted_tx_dtc = dtc;
		} 
		if(strstr(dtc->name,"rx_channel") != NULL && dtc->chan_num == chan_num && &dtc->node != &dmatest_channels){
			pr_info("Found rx dmatest channel number: %d\n",chan_num);
			wanted_rx_dtc = dtc;
		} 
	}
	if(!wanted_rx_dtc || !wanted_tx_dtc) { 
		pr_info("Cound not find tx or rx dmatest channel\n");
		return -1;
	}
	
	/* int check_num_thread = 0;
	list_for_each_entry(thread,&wanted_tx_dtc->threads,node){
		if(&thread->node != &wanted_tx_dtc->threads)
			check_num_thread++;
	} 
	pr_info("Number of available thread: %d\n",check_num_thread); */
	/*
	 * Iter over the thread list of tx_dtc  channel.
	 */
	if(wanted_tx_dtc->thread_num != 0){
		list_for_each_entry_safe(thread, _thread, &wanted_tx_dtc->threads, node) {
		/* 
		 * Check if the elements is not the head element. 
		 */
		if(thread != NULL && &thread->node != &wanted_tx_dtc->threads){
				pr_info("Found a avaiable thread in channels of tx_dtc channel, Address: %x\n.", (unsigned int)thread);
				break;
			}
		}
	}
	if(wanted_tx_dtc->thread_num == 0){
		pr_info("Found no avaiable thread, need create one !");
		thread = kmalloc(sizeof(struct dmatest_slave_thread),GFP_KERNEL);
		if(!thread){ 
			pr_info("Allocate a thread failed !");
			kfree(thread);
			return -1;
		}
		list_add_tail(&thread->node, &wanted_tx_dtc->threads);
		wanted_tx_dtc->thread_num++;
    /*
     * Store the tx_chan and rx_chan for the my_dmatest_slave_func func.
     */
	thread->tx_chan = wanted_tx_dtc->chan;
	thread->rx_chan = wanted_rx_dtc->chan;
	thread->type = (enum dma_transaction_type)DMA_SLAVE;
	pr_info("Found a avaiable thread in channels of tx_dtc channel, Address: %x\n.",(unsigned int)thread);
	}	

	thread->done = false;

	/* Ensure that all previous writes are complete */
	smp_wmb();
	/*
	 * Create a task_struct with the function executed is dmatest_slave_func
	 * and input parameter is (void*) thread pointer
	 */
	ktime_t start, end;
	start = ktime_get();
	thread->task = kthread_run(coherent_test_my_dmatest_slave_func, thread, "%s-%s",
				   dma_chan_name(thread->tx_chan),
				   dma_chan_name(thread->rx_chan));
	end = ktime_get();
	s64 time_diff_ns = ktime_to_ns(ktime_sub(end, start));
	unsigned long tempt = time_diff_ns;
	pr_info("Execution time: %ld ns\n", tempt);

	ret = PTR_ERR(thread->task);
	if (IS_ERR(thread->task)) {
		pr_warn("dmatest: Failed to run thread %s-%s\n",
			dma_chan_name(thread->tx_chan), dma_chan_name(thread->rx_chan));
		return ret;
	}

	/* srcbuf and dstbuf are allocated by the thread itself */
	/* 
	 * Increase the reference count of the given task_struct.
	 */
	get_task_struct(thread->task);
	/*
	 * Add the node of thread to the node list tx_dtc->threads
	 */

	/* Added one thread with 2 channels */
	return 1;
}

/*
 * This function just push two dmatest channel to the list head dmatest_channels.
 */
static int my_dmatest_add_slave_channels(struct dma_chan *tx_chan,
				      struct dma_chan *rx_chan,int chan_num)
{
	struct dmatest_chan *tx_dtc;
	struct dmatest_chan *rx_dtc;
	tx_dtc = kmalloc(sizeof(struct dmatest_chan), GFP_KERNEL);
	if (!tx_dtc) {
		pr_warn("dmatest: No memory for tx %s\n",
			dma_chan_name(tx_chan));
		return -ENOMEM;
	}

	rx_dtc = kmalloc(sizeof(struct dmatest_chan), GFP_KERNEL);
	if (!rx_dtc) {
		pr_warn("dmatest: No memory for rx %s\n",
			dma_chan_name(rx_chan));
		return -ENOMEM;
	}
	tx_dtc->chan_num = chan_num;
	rx_dtc->chan_num = chan_num;
	tx_dtc->thread_num = 0;
	rx_dtc->thread_num = 0;
	memset(tx_dtc->name,0,20);
	memset(rx_dtc->name,0,20);
	strncpy(tx_dtc->name,"tx_channel",strlen("tx_channel")); 
	strncpy(rx_dtc->name,"rx_channel",strlen("rx_channel"));
	rx_dtc->chan = rx_chan;
	tx_dtc->chan = tx_chan;
	/*
	 * Init the thread list of two dmatest chan
	 */
	INIT_LIST_HEAD(&tx_dtc->threads);
	INIT_LIST_HEAD(&rx_dtc->threads);
	/*
	Add two dmatest_chan to the list dmatest_channels.
	 */
	list_add_tail(&tx_dtc->node, &dmatest_channels);
	list_add_tail(&rx_dtc->node, &dmatest_channels);
	nr_channels += 2;

	return 0;
}

static int xilinx_axidmatest_probe(struct platform_device *pdev)
{
	struct dma_chan *tx_chan, *rx_chan;
	int chan_num = 0;
	int err;
	int ret;
	u64 dma_mask;
	pid = -1;
	nr_channels  = 0;
    dma_mask = DMA_BIT_MASK(8 * sizeof(dma_addr_t));
    ret = dma_set_coherent_mask(&pdev->dev, dma_mask);
    if (ret < 0) {
        pr_info("Unable to set the DMA coherent mask.\n");
        return ret;
    } 
	
	/* coherent_source_buffer = dma_alloc_coherent(&pdev->dev,MAX_SRC_BUFFER,&mapped_source_buffer,GFP_KERNEL);
	if(coherent_source_buffer == NULL){ 
		pr_info("Unable to allocate for coherent_source_buffer");
		return -1;
	} */
	coherent_source_buffer = NULL;

	/* coherent_dest_buffer = dma_alloc_coherent(&pdev->dev,MAX_DEST_BUFFER,&mapped_dest_buffer,GFP_KERNEL);
	if(coherent_dest_buffer == NULL){
		pr_info("Unable to allocate for coherent_dest_buffer");
		return -1;
	} */
	coherent_dest_buffer = NULL;
	weight = NULL;

	/* source_buffer = kmalloc(test_buf_size,GFP_KERNEL);
	if(!source_buffer){
		pr_info("Unable to allocate for source_buffer");
		return -1;
	} */
	/* dest_buffer = kmalloc(test_buf_size,GFP_KERNEL);
	if(!dest_buffer){
		pr_info("Unable to allocat for dest_buffer");	
		return -1;
	} */
	if(alloc_chrdev_region(&my_axidma_driver.dev, 0, 1,AXIDMA_FILE) < 0){
			pr_info("Can not allocate major number\n");
			return -1;
	}
	cdev_init(&my_axidma_driver.cdev,&axidma_fops);
	cdev_add(&my_axidma_driver.cdev,my_axidma_driver.dev,1);
	my_axidma_driver.class = class_create(THIS_MODULE,AXIDMA_CLASS);
	if(IS_ERR(my_axidma_driver.class)) {
		pr_info("Unsuccessfully create class !");
		return -ENOMEM;
	}
	my_axidma_driver.device = device_create(my_axidma_driver.class,NULL,my_axidma_driver.dev,NULL,AXIDMA_DEVICE);
	/*
	 * Link the platform device to the axidma_driver entity.
	*/
	my_axidma_driver.container = NULL;
	my_axidma_driver.container = &pdev->dev;

	/*
	Get the tx_channel created by dma platform driver xilinx_dma.c
	*/
    /* memset(source_buffer,0,test_buf_size); 
    memset(dest_buffer,0,test_buf_size); */
	/*
	Get the tx_channel created by dma platform driver xilinx_dma.c
	*/
	tx_chan = dma_request_chan(&pdev->dev, "axidma0");
	if (IS_ERR(tx_chan)) {
		err = PTR_ERR(tx_chan);
		if (err != -EPROBE_DEFER)
			pr_err("xilinx_dmatest: No Tx channel\n");
		return err;
	}
	force_tx_chan = tx_chan;
	pr_info("Find the tx dma_chan created by platform driver, address: %x\n",(unsigned int)tx_chan);
	/*
	Get the rx_channel created by dma platform driver xilinx_dma.c
	*/
	rx_chan = dma_request_chan(&pdev->dev, "axidma1");
	if (IS_ERR(rx_chan)) {
		err = PTR_ERR(rx_chan);
		if (err != -EPROBE_DEFER)
			pr_err("xilinx_dmatest: No Rx channel\n");
		goto free_tx;
	}
	force_rx_chan = rx_chan;
	pr_info("Find the rx dma_chan created by platform driver, address: %x\n",(unsigned int)rx_chan);

	
	err = my_dmatest_add_slave_channels(tx_chan, rx_chan,chan_num);
	
	if (err) {
		pr_err("xilinx_dmatest: Unable to add channels\n");
		goto free_rx;
	}
	Conv2D.BaseAddress = NULL;
	Conv2D.BaseAddress = (void*)ioremap(CONV_BASE_ADDRESS,100);
	if(Conv2D.BaseAddress == NULL){
		pr_info("Error with ioremap func at line: %d\n",__LINE__);
		return -1;
	}
	Image_buf.BaseAddress = NULL;
	Image_buf.BaseAddress = (void*)ioremap(IMAGE_BUFF_BASEADDR,100);
	if(Image_buf.BaseAddress == NULL){
		pr_info("Error with ioremap func at line: %d\n",__LINE__);
		return -1;
	}
	Axi_Fifo.BaseAddress = NULL;
	Axi_Fifo.BaseAddress = (void*)ioremap(MY_AXI_FIFO_BASEADDR,100);
	if(Axi_Fifo.BaseAddress == NULL){
		pr_info("Error with ioremap func at line: %d\n",__LINE__);
		return -1;
	}
	return 0;

free_rx:
	dma_release_channel(rx_chan);
	return err;
free_tx:
	dma_release_channel(tx_chan);
	return err;
}

static int xilinx_axidmatest_remove(struct platform_device *pdev)
{
	struct dmatest_chan *dtc, *_dtc;
	struct dma_chan *chan;
	cdev_del(&my_axidma_driver.cdev); 
	device_destroy(my_axidma_driver.class,my_axidma_driver.dev);
	class_destroy(my_axidma_driver.class);
	unregister_chrdev_region(my_axidma_driver.dev,1);
	/*
	Iter through the dmatest_channels list_head
	*/
	list_for_each_entry_safe(dtc, _dtc, &dmatest_channels, node) {
		/*
		Delete the dmatest_chan entity from list_head dmatest_channels
		*/
		list_del(&dtc->node);
		chan = dtc->chan;
		/*
		// Free the related thread and free allocation.
		*/
		dmatest_cleanup_channel(dtc);
		pr_info("xilinx_dmatest: dropped channel %s\n",
			dma_chan_name(chan));
		dmaengine_terminate_all(chan);
		dma_release_channel(chan);
	}
	if(Conv2D.BaseAddress != NULL){
		iounmap(Conv2D.BaseAddress);
	}
	if(Image_buf.BaseAddress != NULL){
		iounmap(Image_buf.BaseAddress);
	}
	return 0;
}

static const struct of_device_id xilinx_axidmatest_of_ids[] = {
	{ .compatible = "my-xlnx,axi-dma-test-1.00.a",},
	{}
};

static int file_open (struct inode * inode, struct file *file) {
	file->private_data  = NULL;
	pr_info("/dev/device_axidma opened");
	return 0;
}
static int file_release (struct inode * inode, struct file *file){
	pr_info("/dev/device_axidma closed");
	return 0;	
}
static ssize_t file_write (struct file * file, const char __user * buff, size_t length, loff_t * loff_of) {
	return length;
}
static ssize_t file_read (struct file * file,char __user * buff, size_t length, loff_t * loff_of){
	return length;
}

static long file_ioctl(struct file *filep, unsigned int cmd, unsigned long arg){
	pre_cmd = cmd;
	ktime_t start, end;
	start = ktime_get();
	int* tempt_coherent_source_buffer;
	s64 time_diff_ns;
	int i = 0;
	int result;
	unsigned long tempt;
	switch(cmd){ 
		case SET_PID_CMD:
			//memset(config_data, 0, 1024);
			/* if(copy_from_user(&pid, (int*)arg,sizeof(int))){
				pr_info("error at %d \n",__LINE__);
				return -1;
			}; */
			pid = (int)(arg);
			if(pid < 0){
				pr_info("Invalid pid value.");
				return -1;
			}
			memset(&info, 0 , sizeof(struct kernel_siginfo));
			info.si_signo = SIG_TEST;
			info.si_code = SI_QUEUE;
			info.si_int = 15;
			printk(KERN_INFO "NamTran %s, %d, pid = %d\n", __func__, __LINE__, pid);
		break;
		
		case SET_IMAGE_HEIGHT_WIDTH:
			image_height = (uint16_t)((arg &  0xFFFF0000) >> 16);
			image_width = (uint16_t)(arg & 0x0000FFFF);
			//pr_info("Image height: %d, image_width: %d",image_height,image_width);
		break;

		/* case START_CACULATE: 
			sig_num = (int)arg;
			pr_info("Got sig_num value: %d\n",sig_num);
			info.si_signo = sig_num;
			if (image_width == 0 || image_height == 0){
				pr_info("Image width and image height were not set");
				return -1;
			}
			start = ktime_get();
			my_dmatest_add_slave_threads(0);	
			end = ktime_get();
			time_diff_ns = ktime_to_ns(ktime_sub(end, start));
			tempt = time_diff_ns;
			pr_info("Execution time of my_dmatest_add_slave_threads: %ld ns\n", tempt);
		break; */

		case START_CACULATE:
			/* if (image_width == 0 || image_height == 0){
				pr_info("Image width and image height were not set");
				return -1;
			}*/
			start = ktime_get();
			result = force_coherent_test_my_dmatest_slave_func();
			end = ktime_get();
			time_diff_ns = ktime_to_ns(ktime_sub(end, start));
			pr_info("Execution time of force_coherent_test_my_dmatest_slave_func: %ld ns\n", time_diff_ns);
			/* end = ktime_get();
			time_diff_ns = ktime_to_ns(ktime_sub(end, start));
			pr_info("Ioctl take: %lld",time_diff_ns); */
			return result;
		break;

		/* case TEST_MMAP:
			pr_info("TEST_MMAP for soucce_buffer has been called: ");
			tempt_coherent_source_buffer = (int*)coherent_source_buffer;
			for(i=0;i<10;i++){
				tempt_coherent_source_buffer[i] = i;s
			}
		break; */ 
		default:
			return 0;
	}

	return 0;
}

static int file_mmap(struct file *file, struct vm_area_struct *vma){
	int ret;
	size_t size;
	unsigned long pfn;
	void* tempt_kern_buff;
	struct dma_memory* src_buff_mem;
	struct dma_memory* dst_buff_mem;
	dma_addr_t tempt_dma_addr;
	size = vma->vm_end - vma->vm_start;
	switch(pre_cmd){ 
		case PRE_SRC_BUFF:
			if(coherent_source_buffer != NULL){
				pr_info("coherent_source_buffer has been allocated !");
				goto unknown_error;
			}
			coherent_source_buffer = dma_alloc_coherent(my_axidma_driver.container, size, &mapped_source_buffer,GFP_KERNEL);
			if(coherent_source_buffer == NULL){ 
				pr_info("Unable to allocate for coherent_source_buffer");
				return -1;
			}
			ret = dma_mmap_coherent(my_axidma_driver.container, vma, coherent_source_buffer, mapped_source_buffer , size);
			tempt_dma_addr = mapped_source_buffer;
			tempt_kern_buff = coherent_source_buffer;
			if(ret < 0 ){
				pr_info("Error with dma_mmap_coherent function of source buffer");
				goto free_dma_region;
			}
			src_buff_mem = kmalloc(sizeof(struct dma_memory), GFP_KERNEL);
			if(src_buff_mem == NULL){
				pr_info("Error with src_buff_mem allocate");
				goto free_dma_region;
			}
			src_buff_mem->kernel_buffer = coherent_source_buffer;
			src_buff_mem->size = size;
			src_buff_mem->dma_address = mapped_source_buffer; 
			vma->vm_private_data =  (void*)src_buff_mem;
			pr_info("Successfully mmap for source_buffer");
		break;

		case PRE_DEST_BUFF:
			if(coherent_dest_buffer != NULL){
				pr_info("coherent_dest_buffer has been allocated !");
				goto unknown_error;
			}
			coherent_dest_buffer = dma_alloc_coherent(my_axidma_driver.container, size ,&mapped_dest_buffer,GFP_KERNEL);
			if(coherent_dest_buffer == NULL){
				pr_info("Unable to allocate for coherent_dest_buffer");
				return -1;
			}
			ret = dma_mmap_coherent(my_axidma_driver.container, vma, coherent_dest_buffer, mapped_dest_buffer , size);
			tempt_dma_addr = mapped_dest_buffer;
			tempt_kern_buff = coherent_dest_buffer;
			if(ret < 0){
				pr_info("Error with dma_mmap_coherent function of dest buffer");
				goto free_dma_region;
			}
			dst_buff_mem = kmalloc(sizeof(struct dma_memory), GFP_KERNEL);
			if(dst_buff_mem == NULL){
				pr_info("Error with src_buff_mem allocate");
				goto free_dma_region;
			}
			dst_buff_mem->kernel_buffer = coherent_dest_buffer;
			dst_buff_mem->size = size;
			dst_buff_mem->dma_address = mapped_dest_buffer;
			vma->vm_private_data =  (void*)dst_buff_mem;
			pr_info("Successfully mmap for dest_buffer");
		break;

		case PRE_KERNEL_BUFF:
			page_weight = alloc_pages(GFP_KERNEL,(KERNEL_LEN >> PAGE_SHIFT) + 1);
			if(page_weight == NULL){
				pr_info("Error with page_weight");
				goto unknown_error;
			}
			weight = page_address(page_weight);
			if(weight == NULL){
				__free_pages(page_weight, (KERNEL_LEN >> PAGE_SHIFT) + 1);
				goto unknown_error;
			}
			pfn = page_to_pfn(page_weight);
			ret =  remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
			if( ret < 0 ){
				pr_err("Could not map the addres areas\n");
				__free_pages(page_weight, (KERNEL_LEN >> PAGE_SHIFT) + 1);
				return ret;
			}
			vma->vm_private_data = (void*)(page_weight);
			pr_info("Successfully mmap for kernel_buffer");
		break;
		
		default:
			pr_info("Unknown which kind of buffer you want to mmap.");
			return -1;
	}
	vma->vm_ops = &axidma_vm_ops;
	return 0;
free_dma_region:
	dma_free_coherent(my_axidma_driver.container, size, tempt_kern_buff, tempt_dma_addr);
	return -2;
unknown_error:
	return -1;
}

static void axidma_vma_close(struct vm_area_struct *vma) {
	pr_info("vma_close was called");
	void* tempt = NULL;
	tempt = vma->vm_private_data;
	if((struct page*)(tempt) == page_weight){
		if(weight != NULL){
			__free_pages(page_weight, (KERNEL_LEN >> PAGE_SHIFT) + 1);
			weight = NULL;
		}
	}
	else {
		struct dma_memory* tempt_dma_mem;
		tempt_dma_mem = (struct dma_memory*)(tempt);
		dma_free_coherent(my_axidma_driver.container, tempt_dma_mem->size, tempt_dma_mem->kernel_buffer, tempt_dma_mem->dma_address);
		if(tempt_dma_mem->kernel_buffer == coherent_source_buffer){
			coherent_source_buffer = NULL;
		}
		if(tempt_dma_mem->kernel_buffer == coherent_dest_buffer){
			coherent_dest_buffer = NULL;
		}
		kfree(tempt_dma_mem);
	}
}


static struct platform_driver xilinx_axidmatest_driver = {
	.driver = {
		.name = "my-xilinx_axidmatest",
		.of_match_table = xilinx_axidmatest_of_ids,
	},
	.probe = xilinx_axidmatest_probe,
	.remove = xilinx_axidmatest_remove,
};

static int __init axidma_init(void)
{
	return platform_driver_register(&xilinx_axidmatest_driver);
}
late_initcall(axidma_init);

static void __exit axidma_exit(void)
{
	platform_driver_unregister(&xilinx_axidmatest_driver);
}
module_exit(axidma_exit)

MODULE_AUTHOR("TranNamCHY");
MODULE_DESCRIPTION("Driver for maxpooling and conv2d");
MODULE_LICENSE("GPL");
