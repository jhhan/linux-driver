/*******************************************************************************
*
*  NetFPGA-10G http://www.netfpga.org
*
*  File:
*        nf10_user.c
*
*  Project:
*
*
*  Author:
*        Hwanju Kim
*
*  Description:
*	 This module provides user-level access interface for AXI registers and
*	 direct access for data path. Note that the current direct access
*	 by user-level app is done by making buffers permanently to be
*	 mapped by the app. So, it is a responsibility of the app to copy
*	 a received buffer to its own user buffer if packet processing takes
*	 time lagging behind the packet arrival rate. The kernel-user interface
*	 is minimalistic for now.
*
*	 This code is initially developed for the Network-as-a-Service (NaaS) project.
*	 (under development in https://github.com/NetFPGA-NewNIC/linux-driver)
*        
*
*  Copyright notice:
*        Copyright (C) 2014 University of Cambridge
*
*  Licence:
*        This file is part of the NetFPGA 10G development base package.
*
*        This file is free code: you can redistribute it and/or modify it under
*        the terms of the GNU Lesser General Public License version 2.1 as
*        published by the Free Software Foundation.
*
*        This package is distributed in the hope that it will be useful, but
*        WITHOUT ANY WARRANTY; without even the implied warranty of
*        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*        Lesser General Public License for more details.
*
*        You should have received a copy of the GNU Lesser General Public
*        License along with the NetFPGA source package.  If not, see
*        http://www.gnu.org/licenses/.
*
*/

#include "nf10.h"
#include "nf10_user.h"
#include <linux/sched.h>
#include <linux/poll.h>

/* AXI host completion buffer size: 1st 8B for read and 2nd 8B for write */
#define AXI_COMPLETION_SIZE		16
#define AXI_COMPLETION_READ_ADDR	112
#define AXI_COMPLETION_WRITE_ADDR	176
#define AXI_READ_ADDR			64
#define AXI_WRITE_ADDR			128
#define axi_read_completion(adapter)	(u64 *)(adapter->axi_completion_kern_addr)
#define axi_write_completion(adapter)	(u64 *)(adapter->axi_completion_kern_addr + 0x8)
/* return codes via upper 32bit of completion buffer */
#define AXI_COMPLETION_WAIT		0x0
#define AXI_COMPLETION_OKAY		0x1
#define AXI_COMPLETION_NACK		0x2
#define axi_completion_stat(completion)	(u32)(completion >> 32)
#define axi_completion_data(completion)	(completion & ((1ULL << 32) - 1))

static dev_t devno;
static struct class *dev_class;
static struct mutex axi_mutex;
DEFINE_SPINLOCK(user_lock);

static int nf10_open(struct inode *n, struct file *f)
{
	struct nf10_adapter *adapter = (struct nf10_adapter *)container_of(
					n->i_cdev, struct nf10_adapter, cdev);
	if (adapter->user_ops == NULL) {
		netif_err(adapter, drv, default_netdev(adapter),
				"no user_ops is set\n");
		return -EINVAL;
	}
	f->private_data = adapter;
	return 0;
}

static int nf10_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct nf10_adapter *adapter = f->private_data;
	unsigned long pfn;
	unsigned long size;
	int err = 0;
	
	/* page alignment check */
	if ((vma->vm_start & ~PAGE_MASK) || (vma->vm_end & ~PAGE_MASK)) {
		netif_err(adapter, drv, default_netdev(adapter),
			  "not aligned vaddrs (vm_start=%lx vm_end=%lx)", 
			  vma->vm_start, vma->vm_end);
		return -EINVAL;
	}
	/* mmap requires user_ops->get_pfn */
	if (!adapter->user_ops->get_pfn)
		return -EINVAL;

	size = vma->vm_end - vma->vm_start;

	if ((pfn = adapter->user_ops->get_pfn(adapter, size)) == 0) {
		netif_err(adapter, drv, default_netdev(adapter),
			  "failed to get pfn (nr_user_mmap=%u)\n",
			  adapter->nr_user_mmap);
		return -EINVAL;
	}
	/* mmap pfn to the requested user virtual address space */
	err = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);

	netif_dbg(adapter, drv, default_netdev(adapter),
		  "mmapped [%d] err=%d va=%p pfn=%lx size=%lu\n",
		  adapter->nr_user_mmap, err, (void *)vma->vm_start, pfn, size);

	/* nr_user_mmap is used by user_ops->get_pfn to locate a right kernel
	 * memory area */
	if (!err)
		adapter->nr_user_mmap++;

	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,4,0)
#define pt_key	_key
#else
#define pt_key	key
#endif
static unsigned int nf10_poll(struct file *f, poll_table *wait)
{
	struct nf10_adapter *adapter = f->private_data;
	unsigned int mask = 0;
	unsigned long events = wait ? wait->pt_key : POLLIN | POLLOUT | POLLERR;

	/* XXX: do we need to immediately return if wait is NULL?
	 * in old kernel versions, NULL wait is passed when timeout expires,
	 * but right after following it, a valid wait is passed */

	spin_lock_bh(&user_lock);
	/* UF_[RX|TX]_PENDING is set by nf10_user_callback from NAPI poll
	 * handler with IRQ being disabled */
	if (events & (POLLIN | POLLRDNORM)) {
		/* poll requested for rx */
		poll_wait(f, &adapter->user_rx_wq, wait);
		if (adapter->user_flags & UF_RX_PENDING) {
			adapter->user_flags &= ~UF_RX_PENDING;
			mask |= (POLLIN | POLLRDNORM);
		}
	}
	if (events & (POLLOUT | POLLWRNORM)) {
		/* poll requested for tx */
		poll_wait(f, &adapter->user_tx_wq, wait);
		if (adapter->user_flags & UF_TX_PENDING) {
			adapter->user_flags &= ~UF_TX_PENDING;
			mask |= (POLLOUT | POLLWRNORM);
		}
	}
	/* mask == 0 means it will be sleeping waiting for events,
	 * so if irq is disabled, enable again before sleeping */
	if (!mask && (adapter->user_flags & UF_IRQ_DISABLED)) {
		netif_dbg(adapter, intr, default_netdev(adapter),
			  "enable irq before sleeping (events=%lx)\n", events);
		/* if poll is requested for tx, it waits for tx buffer
		 * availability, so needs to sync user gc address */
		if (events & (POLLOUT | POLLWRNORM))
			adapter->user_flags |= UF_GC_ADDR_SYNC;
		adapter->user_flags &= ~UF_IRQ_DISABLED;
		nf10_enable_irq(adapter);
	}
	spin_unlock_bh(&user_lock);

	netif_dbg(adapter, intr, default_netdev(adapter),
		  "nf10_poll events=%lx mask=%x flags=%x\n",
		  events, mask, adapter->user_flags);
	return mask;
}

/* this threshold is for safety to avoid infinite loop in case AXI interface
 * does not respond */
#define AXI_LOOP_THRESHOLD	100000000
static int write_axi(struct nf10_adapter *adapter, u64 addr_val)
{
	volatile u64 *completion = axi_write_completion(adapter);
	u32 r;
	unsigned long loop = 0;

	/* init -> write addr & val -> poll stat -> return stat */
	*completion = 0;
	wmb();
	writeq(addr_val, adapter->bar0 + AXI_WRITE_ADDR);
	while ((r = axi_completion_stat(*completion)) == AXI_COMPLETION_WAIT) {
		if (++loop >= AXI_LOOP_THRESHOLD) {
			r = AXI_COMPLETION_NACK;
			break;
		}
	}
	netif_dbg(adapter, drv, default_netdev(adapter),
		  "%s: addr=%llx val=%llx r=%d (loop=%lu)\n",
		  __func__, addr_val >> 32, addr_val & 0xffffffff, r, loop);

	return r;
}

static int read_axi(struct nf10_adapter *adapter, u64 addr, u64 *val)
{
	volatile u64 *completion = axi_read_completion(adapter);
	u32 r;
	unsigned long loop = 0;

	/* init -> write addr -> poll stat -> return val & stat */
	*completion = 0;
	wmb();
	writeq(addr, adapter->bar0 + AXI_READ_ADDR);
	while ((r = axi_completion_stat(*completion)) == AXI_COMPLETION_WAIT) {
		if (++loop >= AXI_LOOP_THRESHOLD) {
			r= AXI_COMPLETION_NACK;
			break;
		}
	}
	*val = axi_completion_data(*completion);
	netif_dbg(adapter, drv, default_netdev(adapter),
		  "%s: addr=%llx val=%llx r=%d (loop=%lu)\n",
		  __func__, addr, *val, r, loop);

	return r;
}

static int check_axi(int ret)
{
	BUG_ON(ret == AXI_COMPLETION_WAIT);
	/* let user know returning EFAULT if nacked */
	if (ret == AXI_COMPLETION_NACK) {
		pr_err("Error: AXI write request gets NACK\n");
		return -EFAULT;
	}
	return 0;
}

static long nf10_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct nf10_adapter *adapter = (struct nf10_adapter *)f->private_data;

	switch(cmd) {
	case NF10_IOCTL_CMD_READ_STAT:
		/* nothing to do: this placeholder is for compatability
		 * it was used for debugging purpose of the previous dma */
		break;
	case NF10_IOCTL_CMD_WRITE_REG:
#ifdef CONFIG_OSNT
	case NF10_IOCTL_CMD_WRITE_REG_PY:   /* compat w/ OSNT python apps */
#endif
	{
		/* wraxi */
		u32 ret;
		u64 addr_val = (u64)arg;
#ifdef CONFIG_OSNT
		if (cmd == NF10_IOCTL_CMD_WRITE_REG_PY)
			addr_val = *((u64 *)arg);
#endif
		mutex_lock(&axi_mutex);
		ret = check_axi(write_axi(adapter, addr_val));
		mutex_unlock(&axi_mutex);
		if (ret)
			return ret;	/* error */
		break;
	}
	case NF10_IOCTL_CMD_READ_REG:
	{
		/* rdaxi */
		u64 addr, val;
		u32 ret;
		if (copy_from_user(&addr, (void __user *)arg, 8)) {
			pr_err("Error: failed to copy AXI read addr\n");
			return -EFAULT;
		}
		mutex_lock(&axi_mutex);
		ret = check_axi(read_axi(adapter, addr, &val));
		mutex_unlock(&axi_mutex);
		if (ret)
			return ret;	/* error */
		val |= (addr << 32);	/* for compatability with older rdaxi */
		if (copy_to_user((void __user *)arg, &val, 8)) {
			pr_err("Error: failed to copy AXI read val\n");
			return -EFAULT;
		}
		break;
	}
	case NF10_IOCTL_CMD_INIT:
	{
		unsigned long ret = 0;

		if (adapter->user_flags) {
			pr_err("Error: nf10 user stack in use\n");
			return -EBUSY;
		}
		adapter->nr_user_mmap = 0;
		adapter->user_flags |= (arg & UF_ON_MASK);
		/* when initialized, IRQ is disabled by default, and enabled
		 * when exiting or before poll() call sleeps */
		adapter->user_flags |= UF_IRQ_DISABLED;
		nf10_disable_irq(adapter);
		if (adapter->user_ops->init) {
			ret = adapter->user_ops->init(adapter, arg);
			if (copy_to_user((void __user *)arg, &ret, sizeof(u64)))
				return -EFAULT;
		}
		netif_dbg(adapter, drv, default_netdev(adapter),
			  "user init: flags=%x ret=%lu\n",
			  adapter->user_flags, ret);
		break;
	}
	case NF10_IOCTL_CMD_EXIT:
	{
		unsigned long ret = 0;

		if (adapter->user_ops->exit) {
			ret = adapter->user_ops->exit(adapter, arg);
			if (copy_to_user((void __user *)arg, &ret, sizeof(u64)))
				return -EFAULT;
		}
		adapter->nr_user_mmap = 0;
		/* IRQ is re-enabled with user gc address synced */
		adapter->user_flags = UF_GC_ADDR_SYNC;
		nf10_enable_irq(adapter);
		netif_dbg(adapter, drv, default_netdev(adapter),
			  "user exit: flags=%x ret=%lu\n",
			  adapter->user_flags, ret);
		break;
	}
	case NF10_IOCTL_CMD_PREPARE_RX:
		/* arg conveys slot index of rx lbuf */
		if (!adapter->user_ops->prepare_rx_buffer)
			return -ENOTSUPP;
		netif_dbg(adapter, drv, default_netdev(adapter),
			  "user-driven lbuf preparation: i=%lu\n", arg);
		adapter->user_ops->prepare_rx_buffer(adapter, arg);
		break;
	case NF10_IOCTL_CMD_XMIT:
	{
		if (!adapter->user_ops->start_xmit)
			return -ENOTSUPP;
		/* arg conveys reference (index) and size of user tx lbuf */
		ret = adapter->user_ops->start_xmit(adapter, arg);
		break;
	}
	default:
		return -EINVAL;
	}
	return ret;
}

static int nf10_release(struct inode *n, struct file *f)
{
	f->private_data = NULL;
	return 0;
}

static struct file_operations nf10_fops = {
	.owner = THIS_MODULE,
	.open = nf10_open,
	.mmap = nf10_mmap,
	.poll = nf10_poll,
	.unlocked_ioctl = nf10_ioctl,
	.release = nf10_release
};

int nf10_init_fops(struct nf10_adapter *adapter)
{
	int err;

	/* create /dev/NF10_DRV_NAME char device as user-kernel interface */
	if ((err = alloc_chrdev_region(&devno, 0, 1, NF10_DRV_NAME))) {
		netif_err(adapter, probe, default_netdev(adapter),
				"failed to alloc chrdev\n");
		return err;
	}
	cdev_init(&adapter->cdev, &nf10_fops);
	adapter->cdev.owner = THIS_MODULE;
	adapter->cdev.ops = &nf10_fops;
	if ((err = cdev_add(&adapter->cdev, devno, 1))) {
		netif_err(adapter, probe, default_netdev(adapter),
			  "failed to add cdev\n");
		return err;
	}

	dev_class = class_create(THIS_MODULE, NF10_DRV_NAME);
	device_create(dev_class, NULL, devno, NULL, NF10_DRV_NAME);

	/* alloc completion buffer for AXI register interface */
	adapter->axi_completion_kern_addr = pci_alloc_consistent(adapter->pdev,
			AXI_COMPLETION_SIZE, &adapter->axi_completion_dma_addr);
	if (adapter->axi_completion_kern_addr == NULL) {
		pr_err("Error: failed to alloc axi completion buffer."
		       "       note that axi interface won't work.");
		return -ENOMEM;
	}
	writeq(adapter->axi_completion_dma_addr,
	       adapter->bar0 + AXI_COMPLETION_READ_ADDR);
	writeq(adapter->axi_completion_dma_addr + 0x8,
	       adapter->bar0 + AXI_COMPLETION_WRITE_ADDR);

	mutex_init(&axi_mutex);

	return 0;
}

int nf10_remove_fops(struct nf10_adapter *adapter)
{
	device_destroy(dev_class, devno);
	class_unregister(dev_class);
	class_destroy(dev_class);
	cdev_del(&adapter->cdev);
	unregister_chrdev_region(devno, 1);

	if (adapter->axi_completion_kern_addr != NULL)
		pci_free_consistent(adapter->pdev, AXI_COMPLETION_SIZE,
				adapter->axi_completion_kern_addr,
				adapter->axi_completion_dma_addr);

	mutex_destroy(&axi_mutex);
	return 0;
}

/**
 * nf10_user_callback - called by NAPI poll loop when a tx or rx event occurs
 * @adapter: associated adapter structure
 * @rx: 1 if rx or 0 if tx
 *
 * Returns true if a relevant process is initialized, false otherwise
 **/
bool nf10_user_callback(struct nf10_adapter *adapter, int rx)
{
	u32 user_flags;
	wait_queue_head_t *this_q, *other_q;
	u64 poll_flags;

	/* check if user process is initialized for rx or tx */
	if ((rx  && !(adapter->user_flags & UF_RX_ON)) ||
	    (!rx && !(adapter->user_flags & UF_TX_ON)))
		return false;

	/* now we have user process that wants rx or tx */
	if (rx) {
		user_flags = UF_RX_PENDING;
		this_q  = &adapter->user_rx_wq;
		other_q = &adapter->user_tx_wq;
		poll_flags = POLLIN | POLLRDNORM | POLLRDBAND;
	}
	else {	/* tx */
		user_flags = UF_TX_PENDING;
		this_q  = &adapter->user_tx_wq;
		other_q = &adapter->user_rx_wq;
		poll_flags = POLLOUT | POLLWRNORM | POLLWRBAND;
	}
	netif_dbg(adapter, drv, default_netdev(adapter),
		  "try to wake up user process for %s\n", rx ? "RX" : "TX");

	spin_lock_bh(&user_lock);
	adapter->user_flags |= user_flags;
	/* avoid requesting IRQ disabling when any process is waiting in the
	 * other queue. without this check, the waiting process may never
	 * wake up. otherwise request NAPI loop to exit with IRQ disabled */
	if (!waitqueue_active(other_q))
		adapter->user_flags |= UF_IRQ_DISABLED;
	wake_up_interruptible_poll(this_q, poll_flags);
	spin_unlock_bh(&user_lock);

	return true;
}
