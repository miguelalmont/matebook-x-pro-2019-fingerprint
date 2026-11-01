// SPDX-License-Identifier: GPL-2.0-only
/*
 * Goodix GXFP51B7 EC-mailbox transport (Huawei MateBook X Pro 2019).
 *
 * Adapted from the GXFP5130 transport of Void755/gxfpmoc. The /dev/gxfp uAPI is
 * unchanged (see include/uapi/linux/gxfp.h).
 *
 * Transport differences from the GXFP5130:
 *   - 32 KiB MMIO window (not 4 KiB): request at 0x0000, response at 0x1000.
 *   - The host-to-device doorbell is _DSM function 2, which runs
 *     HWEC.ECMD(0xF0) in the Embedded Controller, instead of a GPIO pulse.
 *   - The device-to-host notification is still a GpioInt.
 *   - There is no "read-done" GPIO: ACPI only declares one GpioIo and one
 *     GpioInt, so no acknowledgement is strobed.
 *   - The window holds the bare MP frame (0xA0...), without the 8-byte eSPI
 *     wrapper (0xF0...) of the GXFP5130.
 *
 * The GpioInt is level-triggered and has no usable device acknowledgement. _DSM
 * functions 3 and 4 (ECMD 0xF1/0xF2) are not advertised by function 0, and their
 * guard checks \_SB.PCI0.LPCB.H_EC.ECMD, a name absent from the DSDT, so they are
 * no-ops. Without an ack the IRQ thread runs many times per response (about 131
 * for a 0xA8). This only costs CPU: the EC leaves a sequence of frames in the
 * window, the driver queues them all and userspace filters by command. Masking
 * the IRQ or deduplicating here was tried on hardware and does not work: masking
 * suppresses the firings that carry the response, and this driver does not know
 * which frame is the expected one.
 *
 * This driver only moves bytes and sends nothing on its own, but it carries
 * whatever userspace hands it. Malformed frames or production/firmware commands
 * (0x3 FDT, 0xE PROD, 0xF UPFW) could leave the sensor unusable.
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/align.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/poll.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include "uapi/linux/gxfp.h"

#define GXFP_DRV_NAME		"gxfp"

/* GXFP51B7 MMIO window layout. */
#define GXFP_WINDOW_MIN		0x8000u	/* 32 KiB */
#define GXFP_TX_OFFSET		0x0000u	/* host-to-EC request ring */
#define GXFP_RX_OFFSET		0x1000u	/* EC-to-host response ring */
#define GXFP_RX_REGION		(GXFP_WINDOW_MIN - GXFP_RX_OFFSET) /* 0x7000 */
/* gxfp_enqueue always copies GXFP_RX_MAX-sized slots from rx_buf: the region must
 * be an exact multiple so that every chunk has GXFP_RX_MAX readable bytes. */
static_assert(GXFP_RX_REGION % GXFP_RX_MAX == 0,
	      "GXFP_RX_REGION must be a multiple of GXFP_RX_MAX");

/* Device _DSM: cc58b68a-4479-4893-a8bb-961209db59e5 */
static const guid_t gxfp_dsm_guid =
	GUID_INIT(0xcc58b68a, 0x4479, 0x4893,
		  0xa8, 0xbb, 0x96, 0x12, 0x09, 0xdb, 0x59, 0xe5);
#define GXFP_DSM_REV		0
#define GXFP_DSM_FN_DOORBELL	2	/* ECMD(0xF0): tells the EC about a TX */

/* Maximum MP frame length; matches include/gxfp/proto/mp_proto.h. */
#define GXFP_MP_LEN_MAX		0x6C00u

#define GXFP_RX_FIFO_SIZE	SZ_256K

/* Settle time after publishing a TX and ringing the doorbell. */
#define GXFP_DOORBELL_SETTLE_US	200

struct gxfp {
	struct device *dev;
	acpi_handle handle;
	struct miscdevice misc;

	void __iomem *window;
	size_t window_size;

	struct gpio_desc *gpio_reset;	/* OutputOnly GpioIo, optional */
	int irq;
	unsigned int irq_flags;

	u8 *tx_buf;			/* GXFP_TX_MAX */
	u8 *rx_buf;			/* GXFP_RX_REGION (whole-region read) */
	u8 *rx_read_buf;		/* GXFP_RX_RECORD_SIZE */
	void *rx_fifo_buf;

	struct mutex open_lock;
	struct mutex tx_lock;
	struct mutex rx_read_lock;

	spinlock_t rx_lock;
	struct kfifo rx_fifo;
	wait_queue_head_t rx_wait;

	bool rx_fifo_ready;
	bool rx_overflow;
	atomic_t queued_records;
	bool opened;
	bool removed;
	bool suspended;
};

/* --------------------------------------------------------------------------
 * Mailbox access
 */

static void gxfp_write_tx(struct gxfp *g, size_t len)
{
	size_t offset;

	for (offset = 0; offset < len; offset += sizeof(u64))
		writeq(get_unaligned_le64(g->tx_buf + offset),
		       g->window + GXFP_TX_OFFSET + offset);
}

static void gxfp_read_rx(struct gxfp *g, size_t len)
{
	size_t offset;

	len = ALIGN(min_t(size_t, len, GXFP_RX_REGION), sizeof(u64));
	for (offset = 0; offset < len; offset += sizeof(u64))
		put_unaligned_le64(readq(g->window + GXFP_RX_OFFSET + offset),
				   g->rx_buf + offset);
	/* Complete the mailbox snapshot before parsing it. */
	rmb();
}

/*
 * Host-to-device doorbell: evaluates _DSM function 2. In ASL this runs
 * \_SB.PCI0.LPCB.HWEC.ECMD(0xF0), which makes the EC process the frame just
 * copied into the window.
 */
static int gxfp_doorbell(struct gxfp *g)
{
	union acpi_object *obj;

	obj = acpi_evaluate_dsm(g->handle, &gxfp_dsm_guid, GXFP_DSM_REV,
				GXFP_DSM_FN_DOORBELL, NULL);
	if (!obj)
		return -EIO;
	ACPI_FREE(obj);
	return 0;
}

/* --------------------------------------------------------------------------
 * Interrupt: the EC left a response at 0x1000 and raised the GpioInt.
 */

static size_t gxfp_rx_total_len(const u8 *rx)
{
	u16 mp_len;

	/* MP header: [type<<4][len LE16][checksum]. total = 4 + len. */
	mp_len = (u16)rx[1] | ((u16)rx[2] << 8);
	if (mp_len == 0 || mp_len >= GXFP_MP_LEN_MAX)
		return 0;
	if ((u8)(rx[0] + rx[1] + rx[2]) != rx[3])
		return 0;
	return 4u + (size_t)mp_len;
}

/*
 * Queues one record. @data must point to GXFP_RX_MAX readable bytes: a full slot
 * is always copied to the FIFO, and header.len says how many bytes are valid.
 * This holds because the caller splits rx_buf, whose size (GXFP_RX_REGION) is an
 * exact multiple of GXFP_RX_MAX.
 */
static void gxfp_enqueue(struct gxfp *g, const u8 *data, u32 len, u64 ts)
{
	struct gxfp_rx_record header;

	memset(&header, 0, sizeof(header));
	header.len = len;
	header.timestamp_ns = ts;

	if (g->rx_fifo_ready &&
	    kfifo_avail(&g->rx_fifo) >= GXFP_RX_RECORD_SIZE) {
		kfifo_in(&g->rx_fifo, &header, sizeof(header));
		kfifo_in(&g->rx_fifo, data, GXFP_RX_MAX);
		atomic_inc(&g->queued_records);
	} else {
		g->rx_overflow = true;
		kfifo_reset(&g->rx_fifo);
		atomic_set(&g->queued_records, 0);
	}
}

static irqreturn_t gxfp_irq_thread(int irq, void *data)
{
	struct gxfp *g = data;
	unsigned long flags;
	u64 ts = ktime_get_ns();
	size_t total, done;
	bool queue = !READ_ONCE(g->removed) && !READ_ONCE(g->suspended);

	(void)irq;

	/* Full snapshot of the response ring. */
	gxfp_read_rx(g, GXFP_RX_REGION);

	if (!queue)
		return IRQ_HANDLED;

	/*
	 * Userspace reassembles the response from GXFP_RX_MAX-sized records, so
	 * split it into chunks of that size. If the MP header is invalid, deliver
	 * one raw record so that it can be inspected.
	 *
	 * Every snapshot is queued and userspace filters by command: see the note
	 * on the missing acknowledgement at the top of this file.
	 */
	total = gxfp_rx_total_len(g->rx_buf);
	if (total == 0 || total > GXFP_RX_REGION)
		total = GXFP_RX_MAX;

	spin_lock_irqsave(&g->rx_lock, flags);
	for (done = 0; done < total; done += GXFP_RX_MAX) {
		u32 chunk = (u32)min_t(size_t, GXFP_RX_MAX, total - done);

		gxfp_enqueue(g, g->rx_buf + done, chunk, ts);
		if (g->rx_overflow)
			break;
	}
	spin_unlock_irqrestore(&g->rx_lock, flags);

	wake_up_interruptible(&g->rx_wait);
	return IRQ_HANDLED;
}

/* --------------------------------------------------------------------------
 * Character device (same uAPI as gxfpmoc)
 */

static int gxfp_open(struct inode *inode, struct file *file)
{
	struct miscdevice *m = file->private_data;
	struct gxfp *g = container_of(m, struct gxfp, misc);

	(void)inode;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	guard(mutex)(&g->open_lock);

	if (READ_ONCE(g->removed))
		return -ENODEV;
	if (g->opened)
		return -EBUSY;

	g->opened = true;
	file->private_data = g;
	return 0;
}

static int gxfp_release(struct inode *inode, struct file *file)
{
	struct gxfp *g = file->private_data;

	(void)inode;

	if (!g)
		return 0;

	guard(mutex)(&g->open_lock);
	g->opened = false;
	return 0;
}

static ssize_t gxfp_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct gxfp *g = file->private_data;
	size_t tx_len;
	int ret;

	(void)ppos;

	if (!g)
		return -ENODEV;
	if (count == 0 || count > GXFP_TX_MAX)
		return -EMSGSIZE;

	guard(mutex)(&g->tx_lock);

	if (READ_ONCE(g->removed))
		return -ENODEV;
	if (READ_ONCE(g->suspended))
		return -EHOSTDOWN;

	memset(g->tx_buf, 0, GXFP_TX_MAX);
	if (copy_from_user(g->tx_buf, buf, count))
		return -EFAULT;

	tx_len = ALIGN(count, sizeof(u64));
	gxfp_write_tx(g, tx_len);
	/* Publish every qword before ringing the doorbell. */
	wmb();

	ret = gxfp_doorbell(g);
	if (ret)
		return ret;
	fsleep(GXFP_DOORBELL_SETTLE_US);

	return count;
}

static ssize_t gxfp_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct gxfp *g = file->private_data;
	unsigned long flags;
	int ret;

	(void)ppos;

	if (!g)
		return -ENODEV;
	if (count < GXFP_RX_RECORD_SIZE)
		return -EMSGSIZE;

	ret = mutex_lock_interruptible(&g->rx_read_lock);
	if (ret)
		return ret;

	for (;;) {
		spin_lock_irqsave(&g->rx_lock, flags);

		if (READ_ONCE(g->removed)) {
			spin_unlock_irqrestore(&g->rx_lock, flags);
			ret = -ENODEV;
			break;
		}
		if (READ_ONCE(g->suspended)) {
			spin_unlock_irqrestore(&g->rx_lock, flags);
			ret = -EHOSTDOWN;
			break;
		}
		if (!g->rx_fifo_ready) {
			spin_unlock_irqrestore(&g->rx_lock, flags);
			ret = -ENODEV;
			break;
		}
		if (g->rx_overflow) {
			g->rx_overflow = false;
			spin_unlock_irqrestore(&g->rx_lock, flags);
			ret = -ENOBUFS;
			break;
		}

		if (atomic_read(&g->queued_records) > 0) {
			if (kfifo_out(&g->rx_fifo, g->rx_read_buf,
				      GXFP_RX_RECORD_SIZE) !=
			    GXFP_RX_RECORD_SIZE) {
				kfifo_reset(&g->rx_fifo);
				atomic_set(&g->queued_records, 0);
				spin_unlock_irqrestore(&g->rx_lock, flags);
				ret = -EIO;
				break;
			}
			atomic_dec(&g->queued_records);
			spin_unlock_irqrestore(&g->rx_lock, flags);

			if (copy_to_user(buf, g->rx_read_buf,
					 GXFP_RX_RECORD_SIZE))
				ret = -EFAULT;
			else
				ret = GXFP_RX_RECORD_SIZE;
			break;
		}

		spin_unlock_irqrestore(&g->rx_lock, flags);

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}

		ret = wait_event_interruptible(g->rx_wait,
					       atomic_read(&g->queued_records) > 0 ||
					       !READ_ONCE(g->rx_fifo_ready) ||
					       READ_ONCE(g->rx_overflow) ||
					       READ_ONCE(g->removed) ||
					       READ_ONCE(g->suspended));
		if (ret)
			break;
	}

	mutex_unlock(&g->rx_read_lock);
	return ret;
}

static __poll_t gxfp_poll(struct file *file, poll_table *wait)
{
	struct gxfp *g = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	if (!g)
		return EPOLLERR;

	poll_wait(file, &g->rx_wait, wait);

	spin_lock_irqsave(&g->rx_lock, flags);
	if (READ_ONCE(g->removed))
		mask = EPOLLERR | EPOLLHUP;
	else if (READ_ONCE(g->suspended))
		mask = EPOLLERR;
	else if (READ_ONCE(g->rx_overflow))
		mask = EPOLLIN | EPOLLERR;
	else if (g->rx_fifo_ready && atomic_read(&g->queued_records) > 0)
		mask = EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&g->rx_lock, flags);

	return mask;
}

static long gxfp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct gxfp *g = file->private_data;
	unsigned long flags;

	(void)arg;

	if (!g)
		return -ENODEV;

	switch (cmd) {
	case GXFP_IOCTL_FLUSH_RX:
		if (READ_ONCE(g->removed))
			return -ENODEV;
		spin_lock_irqsave(&g->rx_lock, flags);
		if (g->rx_fifo_ready)
			kfifo_reset(&g->rx_fifo);
		atomic_set(&g->queued_records, 0);
		g->rx_overflow = false;
		spin_unlock_irqrestore(&g->rx_lock, flags);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations gxfp_fops = {
	.owner = THIS_MODULE,
	.open = gxfp_open,
	.release = gxfp_release,
	.read = gxfp_read,
	.write = gxfp_write,
	.poll = gxfp_poll,
	.unlocked_ioctl = gxfp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = noop_llseek,
};

/* --------------------------------------------------------------------------
 * probe / remove / PM
 */

static int gxfp_probe(struct platform_device *pdev)
{
	struct gxfp *g;
	struct resource *res;
	struct gpio_desc *reset;
	int ret;

	g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->dev = &pdev->dev;
	g->handle = ACPI_HANDLE(&pdev->dev);
	if (!g->handle)
		return dev_err_probe(&pdev->dev, -ENODEV, "no ACPI handle\n");

	mutex_init(&g->open_lock);
	mutex_init(&g->tx_lock);
	mutex_init(&g->rx_read_lock);
	spin_lock_init(&g->rx_lock);
	init_waitqueue_head(&g->rx_wait);
	atomic_set(&g->queued_records, 0);
	platform_set_drvdata(pdev, g);

	/* Check that _DSM advertises function 2 (doorbell). */
	if (!acpi_check_dsm(g->handle, &gxfp_dsm_guid, GXFP_DSM_REV,
			    BIT(GXFP_DSM_FN_DOORBELL)))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "_DSM does not support the doorbell function\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing mailbox MMIO window\n");
	if (resource_size(res) < GXFP_WINDOW_MIN)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "window too small: %pa\n",
				     &res->start);
	g->window_size = resource_size(res);

	/*
	 * The window lives in a firmware-reserved region, where
	 * request_mem_region may fail, so use devm_ioremap (no reservation)
	 * rather than devm_ioremap_resource.
	 */
	g->window = devm_ioremap(&pdev->dev, res->start, g->window_size);
	if (!g->window)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "could not map the MMIO window\n");

	g->tx_buf = devm_kzalloc(&pdev->dev, GXFP_TX_MAX, GFP_KERNEL);
	g->rx_buf = devm_kzalloc(&pdev->dev, GXFP_RX_REGION, GFP_KERNEL);
	g->rx_read_buf = devm_kzalloc(&pdev->dev, GXFP_RX_RECORD_SIZE, GFP_KERNEL);
	if (!g->tx_buf || !g->rx_buf || !g->rx_read_buf)
		return -ENOMEM;

	g->rx_fifo_buf = devm_kzalloc(&pdev->dev, GXFP_RX_FIFO_SIZE, GFP_KERNEL);
	if (!g->rx_fifo_buf)
		return -ENOMEM;
	ret = kfifo_init(&g->rx_fifo, g->rx_fifo_buf, GXFP_RX_FIFO_SIZE);
	if (ret)
		return ret;
	g->rx_fifo_ready = true;

	/* GpioIo (reset/attention): optional, the transport does not need it. */
	reset = devm_gpiod_get_index(&pdev->dev, NULL, 0, GPIOD_ASIS);
	if (!IS_ERR(reset))
		g->gpio_reset = reset;

	/* GpioInt -> platform IRQ, created by the ACPI core. */
	g->irq = platform_get_irq(pdev, 0);
	if (g->irq < 0)
		return dev_err_probe(&pdev->dev, g->irq,
				     "could not get the IRQ (GpioInt)\n");

	ret = devm_request_threaded_irq(&pdev->dev, g->irq, NULL,
					gxfp_irq_thread,
					IRQF_ONESHOT,
					GXFP_DRV_NAME, g);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "could not register IRQ %d\n", g->irq);

	g->misc.minor = MISC_DYNAMIC_MINOR;
	g->misc.name = GXFP_DRV_NAME;
	g->misc.fops = &gxfp_fops;
	g->misc.parent = &pdev->dev;
	g->misc.mode = 0600;
	ret = misc_register(&g->misc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "could not register the misc device\n");

	dev_info(&pdev->dev,
		 "GXFP51B7 mailbox: %zu KiB window, IRQ %d, /dev/%s\n",
		 g->window_size / 1024, g->irq, GXFP_DRV_NAME);
	return 0;
}

static void gxfp_remove(struct platform_device *pdev)
{
	struct gxfp *g = platform_get_drvdata(pdev);
	unsigned long flags;

	if (!g)
		return;

	mutex_lock(&g->open_lock);
	WRITE_ONCE(g->removed, true);
	mutex_unlock(&g->open_lock);

	misc_deregister(&g->misc);

	spin_lock_irqsave(&g->rx_lock, flags);
	if (g->rx_fifo_ready)
		kfifo_reset(&g->rx_fifo);
	atomic_set(&g->queued_records, 0);
	g->rx_fifo_ready = false;
	g->rx_overflow = false;
	spin_unlock_irqrestore(&g->rx_lock, flags);

	wake_up_interruptible_all(&g->rx_wait);
}

static int gxfp_suspend(struct device *dev)
{
	struct gxfp *g = dev_get_drvdata(dev);
	unsigned long flags;

	if (!g || READ_ONCE(g->removed))
		return 0;

	WRITE_ONCE(g->suspended, true);
	disable_irq(g->irq);

	spin_lock_irqsave(&g->rx_lock, flags);
	if (g->rx_fifo_ready)
		kfifo_reset(&g->rx_fifo);
	atomic_set(&g->queued_records, 0);
	g->rx_overflow = false;
	spin_unlock_irqrestore(&g->rx_lock, flags);

	wake_up_interruptible_all(&g->rx_wait);
	return 0;
}

static int gxfp_resume(struct device *dev)
{
	struct gxfp *g = dev_get_drvdata(dev);

	if (!g || READ_ONCE(g->removed))
		return 0;

	WRITE_ONCE(g->suspended, false);
	enable_irq(g->irq);
	wake_up_interruptible_all(&g->rx_wait);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gxfp_pm_ops, gxfp_suspend, gxfp_resume);

static const struct acpi_device_id gxfp_acpi_ids[] = {
	{ "GXFP51B7", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, gxfp_acpi_ids);

static struct platform_driver gxfp_driver = {
	.driver = {
		.name = GXFP_DRV_NAME "51b7",
		.acpi_match_table = gxfp_acpi_ids,
		.pm = pm_sleep_ptr(&gxfp_pm_ops),
	},
	.probe = gxfp_probe,
	.remove = gxfp_remove,
};
module_platform_driver(gxfp_driver);

MODULE_DESCRIPTION("Goodix GXFP51B7 EC mailbox transport (adapted from gxfpmoc)");
MODULE_LICENSE("GPL");
