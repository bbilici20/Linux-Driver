/* Prototype module for second mandatory DM510 assignment */
#ifndef __KERNEL__
#  define __KERNEL__
#endif
#ifndef MODULE
#  define MODULE
#endif

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>	
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/wait.h>
//#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
//#include <asm/system.h>
#include <asm/switch_to.h>
#include <linux/cdev.h>

/* Prototypes - this would normally go in a .h file */
static int dm510_open( struct inode*, struct file* );
static int dm510_release( struct inode*, struct file* );
static ssize_t dm510_read( struct file*, char*, size_t, loff_t* );
static ssize_t dm510_write( struct file*, const char*, size_t, loff_t* );
long dm510_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

#define DEVICE_NAME "dm510_dev" /* Dev name as it appears in /proc/devices */
#define MAJOR_NUMBER 255
#define MIN_MINOR_NUMBER 0
#define MAX_MINOR_NUMBER 1

#define init_MUTEX(_m) sema_init(_m,1)
#define BUFFERSIZE 0
#define MAXREADERS 1

#define DEVICE_COUNT 2
static int scull_p_nr_devs = DEVICE_COUNT;
static struct scull_pipe *scull_p_devices;
int scull_p_buffer =  10000;	/* buffer size */
dev_t scull_p_devno = MKDEV(MAJOR_NUMBER, MIN_MINOR_NUMBER);	/* Our first device number */
int max_readers = 10;
/* end of what really should have been in a .h file */

static int scull_p_fasync(int fd, struct file *filp, int mode);
static int spacefree(struct scull_pipe *dev);

/* file operations struct */
static struct file_operations dm510_fops = {
	.owner   = THIS_MODULE,
	.read    = dm510_read,
	.write   = dm510_write,
	.open    = dm510_open,
	.release = dm510_release,
        .unlocked_ioctl   = dm510_ioctl
};

struct scull_pipe {
        wait_queue_head_t inq, outq;       /* read and write queues */
        char *buffer, *end;                /* begin of buf, end of buf */
        int buffersize;                    /* used in pointer arithmetic */
        char *rp, *wp;                     /* where to read, where to write */
        int nreaders, nwriters;            /* number of openings for r/w */
        struct fasync_struct *async_queue; /* asynchronous readers */
        struct mutex mutex;              /* mutual exclusion semaphore */
        struct cdev cdev;                  /* Char device structure */
		struct scull_pipe *other_pipe;  /*to use the other devices buffer*/
};


static void scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
	int err, devno = scull_p_devno + index;
    
	cdev_init(&dev->cdev, &dm510_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scullpipe%d", err, index);
}

/* called when module is loaded */
int dm510_init_module( void ) {

	/* initialization code belongs here */
	int i, result;

    result = register_chrdev_region(scull_p_devno, scull_p_nr_devs * 2, "scullp");
    if (result < 0) {
        printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
        return 0;
    }
    scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
    if (scull_p_devices == NULL) {
        unregister_chrdev_region(scull_p_devno, scull_p_nr_devs * 2);
        return 0;
    }
    memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));

    for (i = 0; i < scull_p_nr_devs; i++) {
        init_waitqueue_head(&(scull_p_devices[i].inq));
        init_waitqueue_head(&(scull_p_devices[i].outq));
        mutex_init(&scull_p_devices[i].mutex);
        scull_p_setup_cdev(scull_p_devices + i, i);
    }

    // Set the pointer to the other device
    for (i = 0; i < scull_p_nr_devs; i++) {
        scull_p_devices[i].other_pipe = &scull_p_devices[(i + 1) % scull_p_nr_devs];
    }

    printk(KERN_INFO "DM510: Hello from your device!\n");
    return 0;
}

/* Called when module is unloaded */
void dm510_cleanup_module( void ) {

	/* clean up code belongs here */
	int i;

#ifdef SCULL_DEBUG
	remove_proc_entry("scullpipe", NULL);
#endif

	if (!scull_p_devices)
		return; /* nothing else to release */

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
	}
	kfree(scull_p_devices);
	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
	scull_p_devices = NULL; /* pedantic */

	printk(KERN_INFO "DM510: Module unloaded.\n");
}


/* Called when a process tries to open the device file */
static int dm510_open( struct inode *inode, struct file *filp ) {
	
	/* device claiming code belongs here */
	struct scull_pipe *dev;

    dev = container_of(inode->i_cdev, struct scull_pipe, cdev);

    /* Ensure mutual exclusion while opening the device */
    if (!mutex_trylock(&dev->mutex))
        return -EBUSY;

    /* Check if the device is already opened for writing */
    if (filp->f_mode & FMODE_WRITE && (dev->nwriters)) {
        mutex_unlock(&dev->mutex);
        printk("The device is already open for writing!");
        return -EBUSY; // Device already open for writing or reading
    }

    filp->private_data = dev;

    if (!dev->buffer) {
        /* allocate the buffer */
        dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
        if (!dev->buffer) {
            mutex_unlock(&dev->mutex);
            return -ENOMEM;
        }
    }
    dev->buffersize = scull_p_buffer;
    dev->end = dev->buffer + dev->buffersize;
    dev->rp = dev->wp = dev->buffer; /* rd and wr from the beginning */

    /* Use f_mode, not f_flags: it's cleaner (fs/open.c tells why) */
    if (filp->f_mode & FMODE_READ)
        dev->nreaders++;
    if (filp->f_mode & FMODE_WRITE)
        dev->nwriters++;

    mutex_unlock(&dev->mutex);

	return 0;
}


/* Called when a process closes the device file. */
static int dm510_release( struct inode *inode, struct file *filp ) {

	/* device release code belongs here */
	struct scull_pipe *dev = filp->private_data;

	/* remove this filp from the asynchronously notified filp's */
	scull_p_fasync(-1, filp, 0);
	mutex_lock(&dev->mutex);
	if (filp->f_mode & FMODE_READ)
		dev->nreaders--;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters--;
	if (dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL; /* the other fields are not checked on open */
	}
	mutex_unlock(&dev->mutex);

	return 0;
}


/* Called when a process, which already opened the dev file, attempts to read from it. */
static ssize_t dm510_read( struct file *filp,
    char *buf,      /* The buffer to fill with data     */
    size_t count,   /* The max number of bytes to read  */
    loff_t *f_pos )  /* The offset in the file           */
{
	
	/* read code belongs here */
	struct scull_pipe *dev = filp->private_data;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	while (dev->rp == dev->wp) { /* nothing to read */
		mutex_unlock(&dev->mutex); /* release the lock */
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		printk("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		/* otherwise loop, but first reacquire the lock */
		if (mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	/* ok, data is there, return something */
	if (dev->wp > dev->rp)
		count = min(count, (size_t)(dev->wp - dev->rp));
	else /* the write pointer has wrapped, return data up to dev->end */
		count = min(count, (size_t)(dev->end - dev->rp));
	if (copy_to_user(buf, dev->rp, count)) {
		mutex_unlock (&dev->mutex);
		return -EFAULT;
	}
	dev->rp += count;
	if (dev->rp == dev->end)
		dev->rp = dev->buffer; /* wrapped */
	mutex_unlock (&dev->mutex);

	/* finally, awake any writers and return */
	wake_up_interruptible(&dev->outq);
	printk("\"%s\" did read %li bytes\n",current->comm, (long)count);

	return count;
}

static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
	while (spacefree(dev) == 0) { /* full */
		DEFINE_WAIT(wait);
		
		mutex_unlock(&dev->mutex);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		printk("\"%s\" writing: going to sleep\n",current->comm);
		prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		if (mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	return 0;
}	

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

/* Called when a process writes to dev file */
static ssize_t dm510_write( struct file *filp,
    const char *buf,/* The buffer to get data from      */
    size_t count,   /* The max number of bytes to write */
    loff_t *f_pos )  /* The offset in the file           */
{

	/* write code belongs here */	
	struct scull_pipe *dev = filp->private_data;
    struct scull_pipe *other_dev = dev->other_pipe;
    int result;

    if (!other_dev) {
        /* No other device is assigned */
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&other_dev->mutex))
        return -ERESTARTSYS;

    /* Make sure there's space to write */
    result = scull_getwritespace(other_dev, filp);
    if (result) {
        mutex_unlock(&other_dev->mutex);
        return result;
    }

    /* OK, space is there, accept something */
    count = min(count, (size_t)spacefree(other_dev));
    if (other_dev->wp >= other_dev->rp)
        count = min(count, (size_t)(other_dev->end - other_dev->wp)); /* to end-of-buf */
    else /* the write pointer has wrapped, fill up to rp-1 */
        count = min(count, (size_t)(other_dev->rp - other_dev->wp - 1));
    printk("Going to accept %li bytes to %p from %p\n", (long)count, other_dev->wp, buf);
    if (copy_from_user(other_dev->wp, buf, count)) {
        mutex_unlock(&other_dev->mutex);
        return -EFAULT;
    }
    other_dev->wp += count;
    if (other_dev->wp == other_dev->end)
        other_dev->wp = other_dev->buffer; /* wrapped */
    mutex_unlock(&other_dev->mutex);

    /* Finally, awake any reader */
    wake_up_interruptible(&other_dev->inq);  /* blocked in read() and select() */

    /* And signal asynchronous readers */
    if (other_dev->async_queue)
        kill_fasync(&other_dev->async_queue, SIGIO, POLL_IN);
    printk("\"%s\" did write %li bytes\n", current->comm, (long)count);
    return count;
	
}

/* called by system call icotl */ 
long dm510_ioctl( 
    struct file *filp, 
    unsigned int cmd,   /* command passed from the user */
    unsigned long arg ) /* argument of the command */
{
	/* ioctl code belongs here */
	int retval = 0;
    int new_value;
    if(cmd!=0 && cmd!=1){
        return -EFAULT;
    }
    else if(arg<0){
        return -EFAULT;
    }
    switch (cmd) {
        case BUFFERSIZE:
            if (copy_from_user(&new_value, (int __user *)arg, sizeof(int))) {
                return -EFAULT;
            }
            scull_p_buffer = new_value;
            printk(KERN_INFO "DM510: Buffer size adjusted to %d\n", scull_p_buffer);
            break;

        case MAXREADERS:
            if (copy_from_user(&new_value, (int __user *)arg, sizeof(int))) {
                return -EFAULT;
            }
            max_readers = new_value; //update max readers
            printk(KERN_INFO "DM510: Max readers set to %d\n", new_value);
            break;

        default:
            return -ENOTTY; // Unsupported ioctl command
    }
	printk(KERN_INFO "DM510: ioctl called.\n");

	return retval;
}

static int scull_p_fasync(int fd, struct file *filp, int mode)
{
	struct scull_pipe *dev = filp->private_data;

	return fasync_helper(fd, filp, mode, &dev->async_queue);
}

module_init( dm510_init_module );
module_exit( dm510_cleanup_module );

MODULE_AUTHOR( "...Beril Bilici" );
MODULE_LICENSE( "GPL" );
