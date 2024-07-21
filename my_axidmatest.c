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
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tran Dinh Nam");
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
static int pid = 0;
#define MAGIC_NO	100
#define SET_PID_CMD	_IOW(MAGIC_NO, 1, int)
#define SIG_TEST 44

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
	struct list_head node;
	struct task_struct *task;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	u8 **srcs;
	u8 **dsts;
	enum dma_transaction_type type;
	bool done;
};

struct dmatest_chan {
	struct list_head node;
	struct dma_chan *chan;
	struct list_head threads;
};

/*
 * Ops file function for user space to interact with 
 * character device file.
 */

static int file_open (struct inode * inode, struct file *file) {
	pr_info("/dev/device_axidma opened");
	return 0;
}
static int file_release (struct inode * inode, struct file *file){
	pr_info("/dev/device_axidma opened");
	return 0;	
}
static ssize_t file_write (struct file * file, const char __user * buff, size_t length, loff_t * loff_of) {
	int ret;
	memset(buffer,0, length);
	ret = copy_from_user(source_buffer,buff,length);
	if(ret){
		pr_info("/dev/device_axidma write not succeed\n");
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

struct file_operations axidma_fops = {
	.owner      = THIS_MODULE,
    .read       = file_read,
    .write      = file_write,
    .open       = file_open,
    .release    = file_release,
	.unlocked_ioctl  = file_ioctl,
};

struct axidma_driver {
	struct class* class;
	struct device* device;
	dev_t dev;
	struct cdev cdev;
	struct device* container;
};

struct axidma_driver my_axidma_driver;
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

/* Function for slave transfers
 * Each thread requires 2 channels, one for transmit, and one for receive
 */
static int dmatest_slave_func(void *data)
{
	struct dmatest_slave_thread	*thread = data;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	const char *thread_name;
	unsigned int src_off, dst_off, len;
	unsigned int error_count;
	unsigned int failed_tests = 0;
	unsigned int total_tests = 0;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	enum dma_status status;
	enum dma_ctrl_flags flags;
	int ret;
	int src_cnt;
	int dst_cnt;
	int bd_cnt = XILINX_DMATEST_BD_CNT;
	int i;

	ktime_t	ktime, start, diff;
	ktime_t	filltime = 0;
	ktime_t	comparetime = 0;
	s64 runtime = 0;
	unsigned long long total_len = 0;
	thread_name = current->comm;
	ret = -ENOMEM;


	/* Ensure that all previous reads are complete */
	smp_rmb();
	tx_chan = thread->tx_chan;
	rx_chan = thread->rx_chan;
	dst_cnt = bd_cnt;
	src_cnt = bd_cnt;

	/*
	Alloc a two dimension pointer.
	*/
	thread->srcs = kcalloc(src_cnt + 1, sizeof(u8 *), GFP_KERNEL);
	if (!thread->srcs)
		goto err_srcs;
	/*
	Alloc a matrix for srcs buffer.
	*/
	for (i = 0; i < src_cnt; i++) {
		thread->srcs[i] = kmalloc(test_buf_size, GFP_KERNEL);
		if (!thread->srcs[i])
			goto err_srcbuf;
	}
	thread->srcs[i] = NULL;

	/*
	Alloc a two dimension pointer
	*/
	thread->dsts = kcalloc(dst_cnt + 1, sizeof(u8 *), GFP_KERNEL);
	if (!thread->dsts)
		goto err_dsts;

	/*
	Alloc a matrix for dest buffer.
	*/
	for (i = 0; i < dst_cnt; i++) {
		thread->dsts[i] = kmalloc(test_buf_size, GFP_KERNEL);
		if (!thread->dsts[i])
			goto err_dstbuf;
	}
	thread->dsts[i] = NULL;

	/* 
	Set nice value (Priority) of this thread to 10.
	*/
	set_user_nice(current, 10);

	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	
	ktime = ktime_get();
	/*
	Catch any stop request and check if the test validated greater than iterations.
	*/
	while (!kthread_should_stop() &&
	       !(iterations && total_tests >= iterations)) {
		/*
		Get the dma_device from tx_chan and rx_chan got from dmatest_add_slave_threads
		*/
		struct dma_device *tx_dev = tx_chan->device;
		struct dma_device *rx_dev = rx_chan->device;
		struct dma_async_tx_descriptor *txd = NULL;
		struct dma_async_tx_descriptor *rxd = NULL;
		dma_addr_t dma_srcs[XILINX_DMATEST_BD_CNT];
		dma_addr_t dma_dsts[XILINX_DMATEST_BD_CNT];
		struct completion rx_cmp;
		struct completion tx_cmp;
		unsigned long rx_tmo =
				msecs_to_jiffies(300000); /* RX takes longer */
		unsigned long tx_tmo = msecs_to_jiffies(30000);
		u8 align = 0;
		struct scatterlist tx_sg[XILINX_DMATEST_BD_CNT];
		struct scatterlist rx_sg[XILINX_DMATEST_BD_CNT];

		total_tests++;

		/* honor larger alignment restrictions */
		align = tx_dev->copy_align;
		if (rx_dev->copy_align > align)
			align = rx_dev->copy_align;

		if (1 << align > test_buf_size) {
			pr_err("%u-byte buffer too small for %d-byte alignment\n",
			       test_buf_size, 1 << align);
			break;
		}

	 	/*
		Random a length value and round it to the 2^x value.
		*/
		len = dmatest_random() % test_buf_size + 1;
		len = (len >> align) << align;
		if (!len)
			len = 1 << align;
		/*
		Accumulate the total_len
		*/
		total_len += len;
		src_off = dmatest_random() % (test_buf_size - len + 1);
		dst_off = dmatest_random() % (test_buf_size - len + 1);

		/*
		Align the src_off and dst_off to the 2^x value.
		*/
		src_off = (src_off >> align) << align;
		dst_off = (dst_off >> align) << align;

		start = ktime_get();

		dmatest_init_srcs(thread->srcs, src_off, len);
		dmatest_init_dsts(thread->dsts, dst_off, len);
		diff = ktime_sub(ktime_get(), start);
		/*
		Increase the filltime
		 */
		filltime = ktime_add(filltime, diff);

		for (i = 0; i < src_cnt; i++) {
			u8 *buf = thread->srcs[i] + src_off;
			/*
			Allocate physic address of the subarray from source_off to (source_off + len) of each array in source buffer. 
			*/
			dma_srcs[i] = dma_map_single(tx_dev->dev, buf, len,
						     DMA_MEM_TO_DEV);
		}

		for (i = 0; i < dst_cnt; i++) {
			/*
			Allocate physic address of the subarray from 0 to test_buf_size of each array in dest buffer. 
			*/
			dma_dsts[i] = dma_map_single(rx_dev->dev,
						     thread->dsts[i],
						     test_buf_size,
						     DMA_BIDIRECTIONAL);
		}

		/*
		Init the scatter gather list with 11 elenment 
		*/
		sg_init_table(tx_sg, bd_cnt);
		sg_init_table(rx_sg, bd_cnt);

		for (i = 0; i < bd_cnt; i++) {
			/*
			Init the physic address of tx_channel and rx_channel are source_off and dst_off
			*/
			sg_dma_address(&tx_sg[i]) = dma_srcs[i];
			sg_dma_address(&rx_sg[i]) = dma_dsts[i] + dst_off;
			/*
		 	Init the transfer length for tx and rx channel
			*/
			sg_dma_len(&tx_sg[i]) = len;
			sg_dma_len(&rx_sg[i]) = len;
		}

		rxd = rx_dev->device_prep_slave_sg(rx_chan, rx_sg, bd_cnt,
				DMA_DEV_TO_MEM, flags, NULL);

		txd = tx_dev->device_prep_slave_sg(tx_chan, tx_sg, bd_cnt,
				DMA_MEM_TO_DEV, flags, NULL);

		if (!rxd || !txd) {
			for (i = 0; i < src_cnt; i++)
				dma_unmap_single(tx_dev->dev, dma_srcs[i], len,
						 DMA_MEM_TO_DEV);
			for (i = 0; i < dst_cnt; i++)
				dma_unmap_single(rx_dev->dev, dma_dsts[i],
						 test_buf_size,
						 DMA_BIDIRECTIONAL);
			pr_warn("%s: #%u: prep error with src_off=0x%x ",
				thread_name, total_tests - 1, src_off);
			pr_warn("dst_off=0x%x len=0x%x\n",
				dst_off, len);
			msleep(100);
			failed_tests++;
			continue;
		}
		/*
		Init rx_completion, this used to mark at slave_rx_callback func
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
			pr_warn("%s: #%u: submit error %d/%d with src_off=0x%x ",
				thread_name, total_tests - 1,
				rx_cookie, tx_cookie, src_off);
			pr_warn("dst_off=0x%x len=0x%x\n",
				dst_off, len);
			msleep(100);
			failed_tests++;
			continue;
		}
		dma_async_issue_pending(rx_chan);
		dma_async_issue_pending(tx_chan);

		tx_tmo = wait_for_completion_timeout(&tx_cmp, tx_tmo);

		status = dma_async_is_tx_complete(tx_chan, tx_cookie,
						  NULL, NULL);

		if (tx_tmo == 0) {
			pr_warn("%s: #%u: tx test timed out\n",
				thread_name, total_tests - 1);
			failed_tests++;
			continue;
		} else if (status != DMA_COMPLETE) {
			pr_warn("%s: #%u: tx got completion callback, ",
				thread_name, total_tests - 1);
			pr_warn("but status is \'%s\'\n",
				status == DMA_ERROR ? "error" :
				"in progress");
			failed_tests++;
			continue;
		}

		rx_tmo = wait_for_completion_timeout(&rx_cmp, rx_tmo);
		status = dma_async_is_tx_complete(rx_chan, rx_cookie,
						  NULL, NULL);

		if (rx_tmo == 0) {
			pr_warn("%s: #%u: rx test timed out\n",
				thread_name, total_tests - 1);
			failed_tests++;
			continue;
		} else if (status != DMA_COMPLETE) {
			pr_warn("%s: #%u: rx got completion callback, ",
				thread_name, total_tests - 1);
			pr_warn("but status is \'%s\'\n",
				status == DMA_ERROR ? "error" :
				"in progress");
			failed_tests++;
			continue;
		}

		/*
		Unmap the dest buffer
		*/
		for (i = 0; i < dst_cnt; i++)
			dma_unmap_single(rx_dev->dev, dma_dsts[i],
					 test_buf_size, DMA_BIDIRECTIONAL);

		error_count = 0;
		start = ktime_get();
		pr_debug("%s: verifying source buffer...\n", thread_name);
		error_count += dmatest_verify(thread->srcs, 0, src_off,
				0, PATTERN_SRC, true);
		error_count += dmatest_verify(thread->srcs, src_off,
				src_off + len, src_off,
				PATTERN_SRC | PATTERN_COPY, true);
		error_count += dmatest_verify(thread->srcs, src_off + len,
				test_buf_size, src_off + len,
				PATTERN_SRC, true);

		pr_debug("%s: verifying dest buffer...\n",
			 thread->task->comm);
		error_count += dmatest_verify(thread->dsts, 0, dst_off,
				0, PATTERN_DST, false);
		error_count += dmatest_verify(thread->dsts, dst_off,
				dst_off + len, src_off,
				PATTERN_SRC | PATTERN_COPY, false);
		error_count += dmatest_verify(thread->dsts, dst_off + len,
				test_buf_size, dst_off + len,
				PATTERN_DST, false);
		diff = ktime_sub(ktime_get(), start);
		comparetime = ktime_add(comparetime, diff);

		if (error_count) {
			pr_warn("%s: #%u: %u errors with ",
				thread_name, total_tests - 1, error_count);
			pr_warn("src_off=0x%x dst_off=0x%x len=0x%x\n",
				src_off, dst_off, len);
			failed_tests++;
		} else {
			pr_debug("%s: #%u: No errors with ",
				 thread_name, total_tests - 1);
			pr_debug("src_off=0x%x dst_off=0x%x len=0x%x\n",
				 src_off, dst_off, len);
		}
	}

	ktime = ktime_sub(ktime_get(), ktime);
	ktime = ktime_sub(ktime, comparetime);
	ktime = ktime_sub(ktime, filltime);
	runtime = ktime_to_us(ktime);

	ret = 0;
	for (i = 0; thread->dsts[i]; i++)
		kfree(thread->dsts[i]);
err_dstbuf:
	kfree(thread->dsts);
err_dsts:
	for (i = 0; thread->srcs[i]; i++)
		kfree(thread->srcs[i]);
err_srcbuf:
	kfree(thread->srcs);
err_srcs:
	pr_notice("%s: terminating after %u tests, %u failures %llu iops %llu KB/s (status %d)\n",
		  thread_name, total_tests, failed_tests,
		  dmatest_persec(runtime, total_tests),
		  dmatest_KBs(runtime, total_len), ret);

	thread->done = true;
	wake_up(&thread_wait);

	return ret;
}

static void dmatest_cleanup_channel(struct dmatest_chan *dtc)
{
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
	kfree(dtc);
}

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

	thread->tx_chan = tx_chan;
	thread->rx_chan = rx_chan;
	thread->type = (enum dma_transaction_type)DMA_SLAVE;

	/* Ensure that all previous writes are complete */
	smp_wmb();
	/*
	Create a task_struct with the function executed is dmatest_slave_func
	and input parameter is (void*) thread pointer
	*/
	thread->task = kthread_run(dmatest_slave_func, thread, "%s-%s",
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

static int dmatest_add_slave_channels(struct dma_chan *tx_chan,
				      struct dma_chan *rx_chan)
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

	tx_dtc->chan = tx_chan;
	rx_dtc->chan = rx_chan;
	INIT_LIST_HEAD(&tx_dtc->threads);
	INIT_LIST_HEAD(&rx_dtc->threads);

	dmatest_add_slave_threads(tx_dtc, rx_dtc);
	thread_count += 1;

	pr_info("dmatest: Started %u threads using %s %s\n",
		thread_count, dma_chan_name(tx_chan), dma_chan_name(rx_chan));

	/*
	Add two dmatest_chan to the list dmatest_channels.
	 */
	list_add_tail(&tx_dtc->node, &dmatest_channels);
	list_add_tail(&rx_dtc->node, &dmatest_channels);
	nr_channels += 2;

	if (iterations)
		wait_event(thread_wait, !is_threaded_test_run(tx_dtc, rx_dtc));
	/*
	 * Send signal to the user space process 
	*/
	// find the task with that pid
	if(pid < 0){
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
    	return -ENODEV;
	}
	return 0;
}

#define AXIDMA_FILE "file_axidma"
#define AXIDMA_CLASS "class_axidma"
#define AXIDMA_DEVICE "device_axidma"
static int xilinx_axidmatest_probe(struct platform_device *pdev)
{
	struct dma_chan *chan, *rx_chan;
	int err;
	pid = -1;
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
	chan = dma_request_chan(&pdev->dev, "axidma0");
	if (IS_ERR(chan)) {
		err = PTR_ERR(chan);
		if (err != -EPROBE_DEFER)
			pr_err("xilinx_dmatest: No Tx channel\n");
		return err;
	}

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

	err = dmatest_add_slave_channels(chan, rx_chan);
	if (err) {
		pr_err("xilinx_dmatest: Unable to add channels\n");
		goto free_rx;
	}

	return 0;

free_rx:
	dma_release_channel(rx_chan);
free_tx:
	dma_release_channel(chan);

	return err;
}

static int xilinx_axidmatest_remove(struct platform_device *pdev)
{
	struct dmatest_chan *dtc, *_dtc;
	struct dma_chan *chan;

	list_for_each_entry_safe(dtc, _dtc, &dmatest_channels, node) {
		list_del(&dtc->node);
		chan = dtc->chan;
		dmatest_cleanup_channel(dtc);
		pr_info("xilinx_dmatest: dropped channel %s\n",
			dma_chan_name(chan));
		dmaengine_terminate_all(chan);
		dma_release_channel(chan);
	}
	return 0;
}

static const struct of_device_id xilinx_axidmatest_of_ids[] = {
	{ .compatible = "xlnx,axi-dma-test-1.00.a",},
	{}
};

static struct platform_driver xilinx_axidmatest_driver = {
	.driver = {
		.name = "xilinx_axidmatest",
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
