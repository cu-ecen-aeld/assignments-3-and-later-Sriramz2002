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
#include <linux/slab.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Sriramkumar"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    filp->private_data = NULL;
    return 0;
}














ssize_t aesd_read(struct file *filp, char __user *buf, size_t cnt, loff_t *f_pos)
{
    ssize_t rb = 0;                         // Total bytes read
    size_t eo = 0;                          // Entry offset within entry
    size_t rc = cnt;                        // Remaining count to read
    size_t bc = 0;                          // Bytes copied so far
    struct aesd_dev *dev = NULL;
    struct aesd_buffer_entry *be = NULL;

    PDEBUG("read %zu bytes with offset %lld", cnt, *f_pos);

    if (!filp || !buf || !f_pos || cnt == 0)
        return -EINVAL;

    dev = filp->private_data;

    // Lock mutex
    if (mutex_lock_interruptible(&dev->aesd_mutex)) {
        PDEBUG("mutex_lock_interruptible failed");
        return -ERESTARTSYS;
    }

    // Read loop: continue reading until buffer exhausted or count satisfied
    while (rc > 0) {
        be = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &eo);
        if (!be || !be->buffptr)
            break; // No more valid entries to read

        size_t bytes = be->size - eo;
        if (bytes > rc)
            bytes = rc;

        if (copy_to_user(buf + bc, be->buffptr + eo, bytes)) {
            PDEBUG("copy_to_user failed");
            rb = -EFAULT;
            goto out;
        }

        *f_pos += bytes;
        bc += bytes;
        rc -= bytes;
    }

    rb = bc; // total bytes successfully read

out:
    mutex_unlock(&dev->aesd_mutex);
    return rb;
}







ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev;
    char *kbuf = NULL, *newline = NULL;
    ssize_t wbytes;
    const char *old_ptr = NULL;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    // Validate input
    if (!filp || !buf || count == 0)
        return -EINVAL;

    // Allocate temporary kernel buffer
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    // Copy from user space
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    // Find newline character
    newline = memchr(kbuf, '\n', count);
    wbytes = newline ? (newline - kbuf + 1) : count;

    dev = filp->private_data;

    // Lock mutex
    if (mutex_lock_interruptible(&dev->aesd_mutex)) {
        kfree(kbuf);
        return -ERESTARTSYS;
    }

    // Reallocate buffer to accommodate new data
    dev->buffer_entry.buffptr = krealloc(dev->buffer_entry.buffptr,
                                         dev->buffer_entry.size + wbytes,
                                         GFP_KERNEL);
    if (!dev->buffer_entry.buffptr) {
        mutex_unlock(&dev->aesd_mutex);
        kfree(kbuf);
        return -ENOMEM;
    }

    // Copy new data into buffer
    memcpy((void *)dev->buffer_entry.buffptr + dev->buffer_entry.size, kbuf, wbytes);
    dev->buffer_entry.size += wbytes;

    // Commit to circular buffer if newline found
    if (newline) {
        old_ptr = aesd_circular_buffer_add_entry(&dev->buffer, &dev->buffer_entry);
        if (old_ptr)
            kfree(old_ptr);
        dev->buffer_entry.buffptr = NULL;
        dev->buffer_entry.size = 0;
    }

    mutex_unlock(&dev->aesd_mutex);
    kfree(kbuf);

    return count;
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
    if (err)
    {
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

    mutex_init(&aesd_device.aesd_mutex);

    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index = 0;
    struct aesd_buffer_entry *entry;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // Referenced from aesd-circular-buffer.h

    AESD_CIRCULAR_BUFFER_FOREACH(entry,&aesd_device.buffer,index) {
        kfree(entry->buffptr);
    }
    mutex_destroy(&aesd_device.aesd_mutex);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);

