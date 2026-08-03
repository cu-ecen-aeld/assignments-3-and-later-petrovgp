/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>      // kmalloc(), kfree()
#include <linux/uaccess.h>   // copy_to_user(), copy_from_user()
#include <linux/mutex.h>
#include <linux/string.h>    // memchr(), memcpy()

#include "aesd-circular-buffer.h"
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Marko Petrov");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    mutex_lock(&dev->lock);

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->buffer,
                *f_pos,
                &entry_offset);

    if (!entry) {
        retval = 0;
        goto out;
    }

    bytes_to_copy = min(count, entry->size - entry_offset);

    if (copy_to_user(buf,
                    entry->buffptr + entry_offset,
                    bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

    out:
    mutex_unlock(&dev->lock);

    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *newbuf;
    struct aesd_buffer_entry old_entry;
    ssize_t retval = count;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    mutex_lock(&dev->lock);

    /* Allocate a new buffer containing previous partial command +
     * the newly written data.
     */
    newbuf = kmalloc(dev->pending_write.size + count, GFP_KERNEL);
    if (!newbuf) {
        retval = -ENOMEM;
        goto unlock;
    }

    /* Copy any previous partial command */
    if (dev->pending_write.size) {
        memcpy(newbuf,
               dev->pending_write.buffptr,
               dev->pending_write.size);
    }

    /* Copy new user data */
    if (copy_from_user(newbuf + dev->pending_write.size,
                       buf,
                       count)) {
        kfree(newbuf);
        retval = -EFAULT;
        goto unlock;
    }

    /* Replace the pending buffer */
    kfree((void *)dev->pending_write.buffptr);

    dev->pending_write.buffptr = newbuf;
    dev->pending_write.size += count;

    /* If no newline yet, keep accumulating */
    if (!memchr(dev->pending_write.buffptr,
                '\n',
                dev->pending_write.size))
    {
        goto unlock;
    }

    /* Complete command received - add to circular buffer */
    old_entry = aesd_circular_buffer_add_entry(
                    &dev->buffer,
                    &dev->pending_write);

    /* Free overwritten entry if buffer wrapped */
    if (old_entry.buffptr)
        kfree((void *)old_entry.buffptr);

    /* Reset pending write buffer */
    dev->pending_write.buffptr = NULL;
    dev->pending_write.size = 0;

    unlock:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);

    mutex_init(&aesd_device.lock);

    aesd_device.pending_write.buffptr = NULL;
    aesd_device.pending_write.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

struct aesd_buffer_entry *entry;

void aesd_cleanup_module(void)
{
    int i;

    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    mutex_lock(&aesd_device.lock);

    /* free all completed entries */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, i) {
        kfree((void *)entry->buffptr);
    }

    /* free unfinished command */
    kfree((void *)aesd_device.pending_write.buffptr);

    mutex_unlock(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
