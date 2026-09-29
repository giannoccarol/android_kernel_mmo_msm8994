/*
 * Copyright 2014  Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "internal.h"

static DEFINE_MUTEX(pmsg_lock);
#define PMSG_MAX_BOUNCE_BUFFER_SIZE (2*PAGE_SIZE)

/*
 * ⚠ The bounce buffer is allocated ONCE, not per write.
 *
 * This used to be a vmalloc()/vfree() pair inside write_pmsg().  /dev/pmsg0 is
 * liblog's second transport -- every log line in every process is written here
 * as well as to logd -- and a typical line is ~100 bytes, so each one took a
 * whole vmalloc: alloc_vmap_area, an rbtree insert, page-table setup, TLB and
 * cache maintenance, then __purge_vmap_area_lazy on the way out.  Profiled on
 * this device during a 4K30 recording (simpleperf, whole system):
 *
 *     write_pmsg         22.77% of ALL cpu cycles (inclusive)
 *     compat_sys_writev  28.27%
 *
 * with the vmap churn and its DMA cache maintenance the top kernel symbols on
 * the entire machine.  Removing it took sustained 4K recording from ~20 fps to
 * 29.6 fps at the same junction temperature.  See camera-lab/live-csi-plan.md
 * S144 and S146.
 *
 * pmsg_lock already serialises the whole copy loop, so one shared buffer is
 * safe.  It is kmalloc'd (8 KB, order-1) at registration.
 *
 * Upstream solved the same problem differently -- it deleted the bounce buffer
 * altogether by adding a ->write_buf_user callback that __copy_from_user()s
 * straight into the persistent-ram zone.  That is the better fix in principle,
 * but this zone is mapped by persistent_ram_iomap() with ioremap_wc() (the
 * region is linux,remove-completely, so pfn_valid() is false and the vmap path
 * is not taken), and every other writer reaches it through memcpy_toio().
 * Keeping the bounce buffer leaves that access pattern exactly as it was and
 * costs one ~100-byte, cache-resident memcpy per line, which is nothing beside
 * what was removed.
 */
static char *pmsg_buffer;

static ssize_t write_pmsg(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	size_t i;

	if (!count)
		return 0;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	mutex_lock(&pmsg_lock);

	if (unlikely(!pmsg_buffer)) {
		mutex_unlock(&pmsg_lock);
		return -ENOMEM;
	}

	for (i = 0; i < count; ) {
		size_t c = min(count - i, (size_t)PMSG_MAX_BOUNCE_BUFFER_SIZE);
		u64 id;
		long ret;

		ret = __copy_from_user(pmsg_buffer, buf + i, c);
		if (unlikely(ret != 0)) {
			mutex_unlock(&pmsg_lock);
			return -EFAULT;
		}
		psinfo->write_buf(PSTORE_TYPE_PMSG, 0, &id, 0, pmsg_buffer, c,
				  psinfo);

		i += c;
	}

	mutex_unlock(&pmsg_lock);
	return count;
}

static const struct file_operations pmsg_fops = {
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
	.write		= write_pmsg,
};

static struct class *pmsg_class;
static int pmsg_major;
#define PMSG_NAME "pmsg"
#undef pr_fmt
#define pr_fmt(fmt) PMSG_NAME ": " fmt

static char *pmsg_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0220;
	return NULL;
}

void pstore_register_pmsg(void)
{
	struct device *pmsg_device;

	pmsg_buffer = kmalloc(PMSG_MAX_BOUNCE_BUFFER_SIZE, GFP_KERNEL);
	if (!pmsg_buffer) {
		pr_err("failed to allocate bounce buffer\n");
		goto err;
	}

	pmsg_major = register_chrdev(0, PMSG_NAME, &pmsg_fops);
	if (pmsg_major < 0) {
		pr_err("register_chrdev failed\n");
		goto err_chrdev;
	}

	pmsg_class = class_create(THIS_MODULE, PMSG_NAME);
	if (IS_ERR(pmsg_class)) {
		pr_err("device class file already in use\n");
		goto err_class;
	}
	pmsg_class->devnode = pmsg_devnode;

	pmsg_device = device_create(pmsg_class, NULL, MKDEV(pmsg_major, 0),
					NULL, "%s%d", PMSG_NAME, 0);
	if (IS_ERR(pmsg_device)) {
		pr_err("failed to create device\n");
		goto err_device;
	}
	return;

err_device:
	class_destroy(pmsg_class);
err_class:
	unregister_chrdev(pmsg_major, PMSG_NAME);
err_chrdev:
	kfree(pmsg_buffer);
	pmsg_buffer = NULL;
err:
	return;
}
