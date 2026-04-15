/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Tracks PIDs registered by the user-space supervisor, checks RSS on a
 * periodic timer, logs a soft-limit warning once per entry, and sends SIGKILL
 * on hard-limit breach. Exposes /dev/container_monitor with two ioctls.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1 (done): Linked-list node for each monitored container.
 * ============================================================== */
struct monitored_entry {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    bool soft_warned;
    struct list_head list;
};

/* ==============================================================
 * TODO 2 (done): Global list + lock.
 *
 * We use a mutex (not a spinlock) because the protected critical sections
 * include get_task_mm() / mmput() in get_rss_bytes(), which may sleep.
 * Holding a spinlock across a sleeping call would be incorrect.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * RSS Helper (provided) - returns bytes, or -1 if task gone.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---- event helpers ---- */
static void log_soft_limit_event(const char *container_id, pid_t pid,
                                 unsigned long limit_bytes, long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id, pid_t pid,
                         unsigned long limit_bytes, long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ==============================================================
 * TODO 3 (done): Timer callback - periodic RSS check.
 *
 * Iterate safely with list_for_each_entry_safe so we can remove an entry
 * mid-loop. On hard-limit breach or when the task has exited, remove and
 * free the node while still holding the mutex.
 * ============================================================== */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *e, *tmp;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(e->pid);

        if (rss < 0) {
            /* task no longer exists - prune the stale entry */
            printk(KERN_INFO
                   "[container_monitor] pruning exited pid=%d container=%s\n",
                   e->pid, e->container_id);
            list_del(&e->list);
            kfree(e);
            continue;
        }

        if ((unsigned long)rss >= e->hard_limit_bytes) {
            kill_process(e->container_id, e->pid, e->hard_limit_bytes, rss);
            list_del(&e->list);
            kfree(e);
            continue;
        }

        if (!e->soft_warned && (unsigned long)rss >= e->soft_limit_bytes) {
            log_soft_limit_event(e->container_id, e->pid,
                                 e->soft_limit_bytes, rss);
            e->soft_warned = true;
        }
    }
    mutex_unlock(&list_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_entry *e, *tmp;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* ensure the container_id is NUL-terminated regardless of user input */
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *node;

        /* basic validation */
        if (req.pid <= 0)
            return -EINVAL;
        if (req.hard_limit_bytes == 0 ||
            req.soft_limit_bytes > req.hard_limit_bytes)
            return -EINVAL;

        printk(KERN_INFO
               "[container_monitor] register container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        node = kzalloc(sizeof(*node), GFP_KERNEL);
        if (!node)
            return -ENOMEM;
        node->pid = req.pid;
        node->soft_limit_bytes = req.soft_limit_bytes;
        node->hard_limit_bytes = req.hard_limit_bytes;
        node->soft_warned = false;
        strscpy(node->container_id, req.container_id,
                sizeof(node->container_id));

        mutex_lock(&list_lock);
        /* reject duplicate pid registrations */
        list_for_each_entry(e, &monitored_list, list) {
            if (e->pid == req.pid) {
                mutex_unlock(&list_lock);
                kfree(node);
                return -EEXIST;
            }
        }
        list_add_tail(&node->list, &monitored_list);
        mutex_unlock(&list_lock);
        return 0;
    }

    /* MONITOR_UNREGISTER */
    printk(KERN_INFO
           "[container_monitor] unregister container=%s pid=%d\n",
           req.container_id, req.pid);

    mutex_lock(&list_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, list) {
        if (e->pid == req.pid) {
            list_del(&e->list);
            kfree(e);
            mutex_unlock(&list_lock);
            return 0;
        }
    }
    mutex_unlock(&list_lock);
    return -ENOENT;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitored_entry *e, *tmp;

    del_timer_sync(&monitor_timer);

    /* free all remaining list entries - no leaks on unload */
    mutex_lock(&list_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&list_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
MODULE_AUTHOR("Kshitij Gupta, Vijay");
