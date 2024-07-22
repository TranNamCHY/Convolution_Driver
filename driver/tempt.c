// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * XILINX AXI DMA and MCDMA Engine test module
 *
 * Copyright (C) 2010 Xilinx, Inc. All rights reserved.
 *
 * Based on Atmel DMA Test Client
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
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
#include <asm/siginfo.h>    //siginfo
#include <linux/rcupdate.h> //rcu_read_lock
#include <linux/sched.h>    //find_task_by_pid_type
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/list.h>
#include "paremeter.h"
#include "max_pooling.h"
#include "covid2.h"
#include "conv2d.h"
static unsigned int test_buf_size = 16384;
module_param(test_buf_size, uint, 0444);
MODULE_PARM_DESC(test_buf_size, "Size of the memcpy test buffer");

static unsigned int iterations = 5;
module_param(iterations, uint, 0444);
MODULE_PARM_DESC(iterations,
		 "Iterations before stopping test (default: infinite)");

char config_data[1024];
struct kernel_siginfo info;
struct task_struct *t;
static int pid = -1;
#define MAGIC_NO	100
#define SET_PID_CMD	_IOW(MAGIC_NO, 1, int)
#define SIG_TEST 44

/*
 * Ops file function for user space to interact with 
 * character device file.
 */

static int file_open (struct inode * inode, struct file *file);
static int file_release (struct inode * inode, struct file *file);
static ssize_t file_write (struct file * file, const char __user * buff, size_t length, loff_t * loff_of);
static ssize_t file_read (struct file * file,char __user * buff, size_t length, loff_t * loff_of);
static long file_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

 struct file_operations axidma_fops = {
	.owner      = THIS_MODULE,
    .read       = file_read,
    .write      = file_write,
    .open       = file_open,
    .release    = file_release,
	.unlocked_ioctl  = file_ioctl,
};

u8* source_buffer;
u8* dest_buffer;
int source_length;
int dest_length;
struct axidma_driver {
	struct class* class;
	struct device* device;
	dev_t dev;
	struct cdev cdev;
	struct device* container;
};

struct axidma_driver my_axidma_driver;

/*
 * Initialization patterns. All bytes in the source buffer has bit 7
 * set, all bytes in the destination buffer has bit 7 cleared.
 *
 * Bit 6 is set for all bytes which are to be copied by the DMA
 * engine. Bit 5 is set for all bytes which are to be overwritten by
 * the DMA engine.
 *
 * The remaining bits are the inverse of a counter which increments by
 * one for each byte address.
 */

#define PATTERN_SRC		0x80
#define PATTERN_DST		0x00
#define PATTERN_COPY		0x40
#define PATTERN_OVERWRITE	0x20
#define PATTERN_COUNT_MASK	0x1f

#define XILINX_DMATEST_BD_CNT	11

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

static unsigned long long dmatest_persec(s64 runtime, unsigned int val)
{
	unsigned long long per_sec = 1000000;

	if (runtime <= 0)
		return 0;

	/* drop precision until runtime is 32-bits */
	while (runtime > UINT_MAX) {
		runtime >>= 1;
		per_sec <<= 1;
	}

	per_sec *= val;
	do_div(per_sec, runtime);
	return per_sec;
}

static unsigned long long dmatest_KBs(s64 runtime, unsigned long long len)
{
	return dmatest_persec(runtime, len >> 10);
}

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

static unsigned long dmatest_random(void)
{
	unsigned long buf;

	get_random_bytes(&buf, sizeof(buf));
	return buf;
}

static void dmatest_init_srcs(u8 **bufs, unsigned int start, unsigned int len)
{
	unsigned int i;
	u8 *buf;

	for (; (buf = *bufs); bufs++) {
		for (i = 0; i < start; i++)
			buf[i] = PATTERN_SRC | (~i & PATTERN_COUNT_MASK);
		for ( ; i < start + len; i++)
			buf[i] = PATTERN_SRC | PATTERN_COPY
				| (~i & PATTERN_COUNT_MASK);
		for ( ; i < test_buf_size; i++)
			buf[i] = PATTERN_SRC | (~i & PATTERN_COUNT_MASK);
	}
}

static void dmatest_init_dsts(u8 **bufs, unsigned int start, unsigned int len)
{
	unsigned int i;
	u8 *buf;

	for (; (buf = *bufs); bufs++) {
		for (i = 0; i < start; i++)
			buf[i] = PATTERN_DST | (~i & PATTERN_COUNT_MASK);
		for ( ; i < start + len; i++)
			buf[i] = PATTERN_DST | PATTERN_OVERWRITE
				| (~i & PATTERN_COUNT_MASK);
		for ( ; i < test_buf_size; i++)
			buf[i] = PATTERN_DST | (~i & PATTERN_COUNT_MASK);
	}
}

static void dmatest_mismatch(u8 actual, u8 pattern, unsigned int index,
			     unsigned int counter, bool is_srcbuf)
{
	u8 diff = actual ^ pattern;
	u8 expected = pattern | (~counter & PATTERN_COUNT_MASK);
	const char *thread_name = current->comm;

	if (is_srcbuf)
		pr_warn("%s: srcbuf[0x%x] overwritten! Expected %02x, got %02x\n",
			thread_name, index, expected, actual);
	else if ((pattern & PATTERN_COPY) &&
		 (diff & (PATTERN_COPY | PATTERN_OVERWRITE)))
		pr_warn("%s: dstbuf[0x%x] not copied! Expected %02x, got %02x\n",
			thread_name, index, expected, actual);
	else if (diff & PATTERN_SRC)
		pr_warn("%s: dstbuf[0x%x] was copied! Expected %02x, got %02x\n",
			thread_name, index, expected, actual);
	else
		pr_warn("%s: dstbuf[0x%x] mismatch! Expected %02x, got %02x\n",
			thread_name, index, expected, actual);
}

static unsigned int dmatest_verify(u8 **bufs, unsigned int start,
				   unsigned int end, unsigned int counter,
				   u8 pattern, bool is_srcbuf)
{
	unsigned int i;
	unsigned int error_count = 0;
	u8 actual;
	u8 expected;
	u8 *buf;
	unsigned int counter_orig = counter;

	for (; (buf = *bufs); bufs++) {
		counter = counter_orig;
		for (i = start; i < end; i++) {
			actual = buf[i];
			expected = pattern | (~counter & PATTERN_COUNT_MASK);
			if (actual != expected) {
				if (error_count < 32)
					dmatest_mismatch(actual, pattern, i,
							 counter, is_srcbuf);
				error_count++;
			}
			counter++;
		}
	}

	if (error_count > 32)
		pr_warn("%s: %u errors suppressed\n",
			current->comm, error_count - 32);

	return error_count;
}

static void dmatest_slave_tx_callback(void *completion)
{
	complete(completion);
}

static void dmatest_slave_rx_callback(void *completion)
{
	complete(completion);
}

static int my_dmatest_slave_func(void *data)
{
	struct dmatest_slave_thread	*thread = data;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	const char *thread_name;
	unsigned int total_tests = 0;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags;
	int ret;
	int i;
	thread_name = current->comm;
	ret = -ENOMEM;

	/* Ensure that all previous reads are complete */
	smp_rmb();
	tx_chan = thread->tx_chan;
	rx_chan = thread->rx_chan;
	/* 
	 * Set nice value (Ptest_my_dmatest_slave_funcriority) of this thread to 10.
	 */
	set_user_nice(current, 10);

	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
		/*
		 * Get the dma_device from tx_chan and rx_chan got from dmatest_add_slave_threads
		 */
		struct dma_device *tx_dev = tx_chan->device;
		struct dma_device *rx_dev = rx_chan->device;
		struct dma_async_tx_descriptor *txd = NULL;
		struct dma_async_tx_descriptor *rxd = NULL;
        dma_addr_t dma_srcs;
		dma_addr_t dma_dsts;
		struct completion rx_cmp;
		struct completion tx_cmp;
		unsigned long rx_tmo = msecs_to_jiffies(1000); /* RX takes longer */
		unsigned long tx_tmo = msecs_to_jiffies(1000);
		u8 align = 0;
		struct scatterlist tx_sg[1];
		struct scatterlist rx_sg[1];
		/* honor larger alignment restrictions */
		align = tx_dev->copy_align;
		if (rx_dev->copy_align > align)
			align = rx_dev->copy_align;

		if ((1 << align) > test_buf_size) {
			pr_err("%u-byte buffer too small for %d-byte alignment\n",
			       test_buf_size, 1 << align);
		}

		/*
		 * Init a test case.
		 */
		for(i=0;i<4096;i++){
            source_buffer[i] =  i%255;
        }
		/*
		 * Physic address of source buffer.
		 */
		dma_srcs = dma_map_single(tx_dev->dev, 
							 source_buffer, 
							 test_buf_size,
						     DMA_MEM_TO_DEV);

		/*
		 * Physic address of destination buffer.
		 */	
        dma_dsts = dma_map_single(rx_dev->dev,
						     dest_buffer,
						     test_buf_size,
						     DMA_BIDIRECTIONAL);
		/*
		 * Init the scatter gather list with 11 elenment 
		 */
		sg_init_table(tx_sg, 1);
		sg_init_table(rx_sg, 1);
		sg_dma_address(&tx_sg[0]) = dma_srcs;
		sg_dma_address(&rx_sg[0]) = dma_dsts;

		/*
		 * Init the transfer length for tx and rx channel
		 */
		sg_dma_len(&tx_sg[0]) = 3600;
		sg_dma_len(&rx_sg[0]) = 3600;

		rxd = rx_dev->device_prep_slave_sg(rx_chan, rx_sg, 1,
				DMA_DEV_TO_MEM, flags, NULL);

		txd = tx_dev->device_prep_slave_sg(tx_chan, tx_sg, 1,
				DMA_MEM_TO_DEV, flags, NULL);

		if (!rxd || !txd) {
			pr_info("Error with device_prep_slave_sg at line: %d\n",__LINE__);
			ret = -1;
			goto error;
		}

		/*
		 * Init rx_completion, this used to mark at slave_rx_callback func
		 */
		init_completion(&rx_cmp);
		rxd->callback = dmatest_slave_rx_callback;
		rxd->callback_param = &rx_cmp;
		rx_cookie = rxd->tx_submit(rxd);

		init_completion(&tx_cmp);
		txd->callback = dmatest_slave_tx_callback;
		txd->callback_param = &tx_cmp;
		tx_cookie = txd->tx_submit(txd);

		if (dma_submit_error(rx_cookie) ||
		    dma_submit_error(tx_cookie)) {
			pr_info("Error with submit function at line: %d\n",__LINE__);
			ret = -1;
			goto error;
		}

		dma_async_issue_pending(rx_chan);
		dma_async_issue_pending(tx_chan);

		tx_tmo = wait_for_completion_timeout(&tx_cmp, tx_tmo);
		status = dma_async_is_tx_complete(tx_chan, tx_cookie,
						  NULL, NULL);
		if (tx_tmo == 0) {
			pr_info("Timeout with tx transfer request at line: %d\n",__LINE__);
		} else if (status != DMA_COMPLETE) {
			pr_warn("%s: tx got completion callback,",
				thread_name);
			pr_warn("but status is \'%s\'\n",
				status == DMA_ERROR ? "error" :
				"in progress");
		}

		rx_tmo = wait_for_completion_timeout(&rx_cmp, rx_tmo);
		status = dma_async_is_tx_complete(rx_chan, rx_cookie,
						  NULL, NULL);
		if (rx_tmo == 0) {
			pr_info("Timeout with rx transfer request at line: %d\n",__LINE__);
		} else if (status != DMA_COMPLETE) {
			pr_warn("%s: rx got completion callback, ",
				thread_name);
			pr_warn("but status is \'%s\'\n",
				status == DMA_ERROR ? "error" :
				"in progress");
		}

error:
		/*
		 * Unmap the dest and sourcess buffer
		 */
		dma_unmap_single(tx_dev->dev, dma_srcs, 
						 test_buf_size,
						 DMA_MEM_TO_DEV);
		dma_unmap_single(rx_dev->dev, dma_dsts,
						 test_buf_size,
						 DMA_BIDIRECTIONAL);

	thread->done = true;
    /*
     * Notify for the dmatest_add_slave_channels that the thread has finished.
     */
	wake_up(&thread_wait);

	return ret;
}

static int test_my_dmatest_slave_func(void *data)
{
	struct dmatest_slave_thread	*thread = data;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	const char *thread_name;
	unsigned int total_tests = 0;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags;
	int ret;
	int i;
	thread_name = current->comm;
	ret = 0;
	/* Ensure that all previous reads are complete */
	smp_rmb();
	tx_chan = thread->tx_chan;
	rx_chan = thread->rx_chan;
	/* 
	 * Set nice value (Priority) of this thread to 10.
	 */
	set_user_nice(current, 10);

	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
		/*
		 * Get the dma_device from tx_chan and rx_chan got from dmatest_add_slave_threads
		 */
		struct dma_device *tx_dev = tx_chan->device;
		struct dma_device *rx_dev = rx_chan->device;
		struct dma_async_tx_descriptor *txd = NULL;
		struct dma_async_tx_descriptor *rxd = NULL;
        dma_addr_t dma_srcs;
		dma_addr_t dma_dsts;
		struct completion rx_cmp;
		struct completion tx_cmp;
		unsigned long rx_tmo = msecs_to_jiffies(5000); /* RX takes longer */
		unsigned long tx_tmo = msecs_to_jiffies(5000);
		u8 align = 0;
		struct scatterlist tx_sg[1];
		struct scatterlist rx_sg[1];
		unsigned int len;
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
		len = source_length;
		dma_srcs = dma_map_single(tx_dev->dev,source_buffer,len,DMA_MEM_TO_DEV);
		dma_dsts = dma_map_single(rx_dev->dev,dest_buffer,len,DMA_BIDIRECTIONAL);
		
		sg_init_table(tx_sg,1);
		sg_init_table(rx_sg,1);

		sg_dma_address(&tx_sg[0]) = dma_srcs;
		sg_dma_address(&rx_sg[0]) = dma_dsts;
		 
		sg_dma_len(&tx_sg[0]) = len;
		sg_dma_len(&rx_sg[0]) = len;
		pr_info("Len: %d\n",len);
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
		//pr_info("Tx ma_chan and Rx dma_chan at address: %x, %x\n",(unsigned int)tx_chan,(unsigned int)rx_chan);
		/*
		 * Init a test case.
		 */
		/*
		 * Emulate a test case.
		 */
		/* for(i=0;i<source_length;i++){
			dest_buffer[i] = source_buffer[i];
		} */
		/*
		 * Physic address of source buffer.
		 */
 error:
	thread->done = true;
    /*
     * Notify for the dmatest_add_slave_channels that the thread has finished.
     */
	dma_unmap_single(tx_dev->dev,dma_srcs,len,DMA_MEM_TO_DEV);
	dma_unmap_single(rx_dev->dev,dma_dsts,len,DMA_BIDIRECTIONAL);
	wake_up(&thread_wait);
	if(pid < 0){
		pr_info("Pid was not initialized !");
		return ret;
	}
	rcu_read_lock();
	t = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (t != NULL) {
    rcu_read_unlock();      
    if (send_sig_info(SIG_TEST, &info, t) < 0) {
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

static int dmatest_add_slave_threads(struct dmatest_chan *tx_dtc,
				     struct dmatest_chan *rx_dtc)
{
	struct dmatest_slave_thread *thread;
	struct dma_chan *tx_chan = tx_dtc->chan;
	struct dma_chan *rx_chan = rx_dtc->chan;
	int ret;


	thread = kzalloc(sizeof(struct dmatest_slave_thread), GFP_KERNEL);
	if (!thread) {
		pr_warn("dmatest: No memory for slave thread %s-%s\n",
			dma_chan_name(tx_chan), dma_chan_name(rx_chan));
	}

    /*
    Store the tx_chan and rx_chan for the my_dmatest_slave_func func.
    */
	thread->tx_chan = tx_chan;
	thread->rx_chan = rx_chan;
	thread->type = (enum dma_transaction_type)DMA_SLAVE;
	thread->done = false;
	/* Ensure that all previous writes are complete */
	smp_wmb();
	/*
	Create a task_struct with the function executed is dmatest_slave_func
	and input parameter is (void*) thread pointer
	*/
	thread->task = kthread_run(my_dmatest_slave_func, thread, "%s-%s",
				   dma_chan_name(tx_chan),
				   dma_chan_name(rx_chan));
	ret = PTR_ERR(thread->task);
	if (IS_ERR(thread->task)) {
		pr_warn("dmatest: Failed to run thread %s-%s\n",
			dma_chan_name(tx_chan), dma_chan_name(rx_chan));
		kfree(thread);
		return ret;
	}

	/* srcbuf and dstbuf are allocated by the thread itself */
	/* 
	Increase the reference count of the given task_struct.
	*/
	get_task_struct(thread->task);
	/*
	Add the node of thread to the node list tx_dtc->threads
	*/
	list_add_tail(&thread->node, &tx_dtc->threads);

	/* Added one thread with 2 channels */
	return 1;
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
	thread->task = kthread_run(test_my_dmatest_slave_func, thread, "%s-%s",
				   dma_chan_name(thread->tx_chan),
				   dma_chan_name(thread->rx_chan));
	ret = PTR_ERR(thread->task);
	if (IS_ERR(thread->task)) {
		pr_warn("dmatest: Failed to run thread %s-%s\n",
			dma_chan_name(thread->tx_chan), dma_chan_name(thread->rx_chan));
		//kfree(thread);
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
	unsigned int thread_count = 0;
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

	//my_dmatest_add_slave_threads(tx_dtc, rx_dtc);
	//thread_count += 1;

	//pr_info("dmatest: Started %u threads using %s %s\n",
	//	thread_count, dma_chan_name(tx_chan), dma_chan_name(rx_chan));

	/*
	Add two dmatest_chan to the list dmatest_channels.
	 */
	list_add_tail(&tx_dtc->node, &dmatest_channels);
	list_add_tail(&rx_dtc->node, &dmatest_channels);
	nr_channels += 2;

    /*
    Wait util all thread created by dmatest_add_slave_threads finish.
    */
	//wait_event(thread_wait, !is_threaded_test_run(tx_dtc, rx_dtc));

	/*
	 * Send signal to the user space process 
	 */
	// find the task with that pid

	/* if(pid < 0){
		pr_info("Pid was not initialized !");
		return 0;
	}
	rcu_read_lock();
	t = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
	if (t != NULL) {
    rcu_read_unlock();      
    if (send_sig_info(SIG_TEST, &info, t) < 0) {
			pr_info("send_sig_info error\n");
		}
	} else {
    	pr_info("pid_task error\n");
    	rcu_read_unlock();
    	return -1;
	} */

	return 0;
}

#define AXIDMA_FILE "file_axidma"
#define AXIDMA_CLASS "class_axidma"
#define AXIDMA_DEVICE "device_axidma"

static int xilinx_axidmatest_probe(struct platform_device *pdev)
{
    nr_channels  = 0;
	struct dma_chan *tx_chan, *rx_chan;
	int chan_num = 0;
	int err;
	int ret;
	unsigned long time_tempt;
	pid = -1;
	source_buffer = kmalloc(test_buf_size,GFP_KERNEL);
	if(!source_buffer){
		pr_info("Unable to allocate for source_buffer");
		return -1;
	}
	dest_buffer = kmalloc(test_buf_size,GFP_KERNEL);
	if(!dest_buffer){
		pr_info("Unable to allocat for dest_buffer");	
		return -1;
	}
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
	my_axidma_driver.container = &pdev->dev;

	/*
	Get the tx_channel created by dma platform driver xilinx_dma.c
	*/
    memset(source_buffer,0,test_buf_size); 
    memset(dest_buffer,0,test_buf_size);
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
	pr_info("Find the rx dma_chan created by platform driver, address: %x\n",(unsigned int)rx_chan);
	
	err = my_dmatest_add_slave_channels(tx_chan, rx_chan,chan_num);
	
	if (err) {
		pr_err("xilinx_dmatest: Unable to add channels\n");
		goto free_rx;
	}
	/*
	 * Init a test case for driver.
	 */
	
	/*
	source_length = 3123;
	int i;
	for(i=0;i<source_length;i++){
		source_buffer[i] = i%128;
	}
	time_tempt = jiffies;
	my_dmatest_add_slave_threads(0);
	ret = wait_event_timeout(thread_wait,my_is_threaded_test_run(0),msecs_to_jiffies(5000));
	if(ret == 0 ){
		pr_info("Time out in file_write function");
		return -1;
	}
	pr_info("It take: %d minitimes",jiffies_to_msecs(jiffies - time_tempt));
	pr_info("Check value from dest_buffer: \n");
	for(i=0;i<10;i++){
		pr_info("%u\n",dest_buffer[i]);
	}
	pr_info("....\n");
	pr_info("%u\n",dest_buffer[source_length-1])
	for(i=0;i<source_leng;
	pr_info("%u\n",dest_buffer[source_length]);


	memset(dest_buffer,0,source_length);
	time_tempt = jiffies;
	my_dmatest_add_slave_threads(0);
	ret = wait_event_timeout(thread_wait,my_is_threaded_test_run(0),msecs_to_jiffies(5000));
	if(ret == 0 ){
		pr_info("Time out in file_write function");
		return -1;
	}
	pr_info("It take: %d minitimes",jiffies_to_msecs(jiffies - time_tempt));
	pr_info("Check value from dest_buffer: \n");
	for(i=0;i<10;i++){
		pr_info("%u\n",dest_buffer[i]);
	}
	pr_info("....\n");
	pr_info("%u\n",dest_buffer[source_length-1]);
	pr_info("%u\n",dest_buffer[source_length]);
	*/
	/*
	 *  End test case !.
	 */
	return 0;

free_rx:
	dma_release_channel(rx_chan);
free_tx:
	dma_release_channel(tx_chan);

	return err;
}

static int xilinx_axidmatest_remove(struct platform_device *pdev)
{
	cdev_del(&my_axidma_driver.cdev); 
	device_destroy(my_axidma_driver.class,my_axidma_driver.dev);
	class_destroy(my_axidma_driver.class);
	unregister_chrdev_region(my_axidma_driver.dev,1);
	struct dmatest_chan *dtc, *_dtc;
	struct dma_chan *chan;

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
	kfree(source_buffer);
	kfree(dest_buffer);
	return 0;
}

static const struct of_device_id xilinx_axidmatest_of_ids[] = {
	{ .compatible = "my-xlnx,axi-dma-test-1.00.a",},
	{}
};

static int file_open (struct inode * inode, struct file *file) {
	memset(source_buffer,0,test_buf_size);
	memset(dest_buffer,0,test_buf_size);
	pr_info("/dev/device_axidma opened");
	return 0;
}
static int file_release (struct inode * inode, struct file *file){
	pr_info("/dev/device_axidma closed");
	return 0;	
}
static ssize_t file_write (struct file * file, const char __user * buff, size_t length, loff_t * loff_of) {
	int ret;
	ret = copy_from_user(source_buffer,buff,length);
	source_length = length;
	if(ret){
		pr_info("/dev/device_axidma write not succeed\n");
		return -1;
	}
	my_dmatest_add_slave_threads(0);
	ret = wait_event_timeout(thread_wait,my_is_threaded_test_run(0),msecs_to_jiffies(5000));
	if(ret == 0 ){
		pr_info("Time out in file_write function");
		return -1;
	}
	return length;
}
static ssize_t file_read (struct file * file,char __user * buff, size_t length, loff_t * loff_of){
	if(copy_to_user(buff,dest_buffer,length)){
		pr_info("/dev/device_axidma read not succeed\n");
		return -1;
	}
	return length;
}
static long file_ioctl(struct file *filep, unsigned int cmd, unsigned long arg){
	switch(cmd){ 
		case SET_PID_CMD:
			//memset(config_data, 0, 1024);
			if(copy_from_user(&pid, (int*)arg,sizeof(int))){
				pr_info("error at %d \n",__LINE__);
				return -1;
			};
			printk(KERN_INFO "NamTran %s, %d, pid = %d\n", __func__, __LINE__, pid);
		break;

		default:
			return -1;
	}
	memset(&info, 0 , sizeof(struct kernel_siginfo));
	info.si_signo = SIG_TEST;
	info.si_code = SI_QUEUE;
	info.si_int = 15;

	return 0;
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

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx AXI DMA Test Client");
MODULE_LICENSE("GPL v2");
