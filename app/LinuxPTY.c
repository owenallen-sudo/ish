//
// LinuxPTY.c
// libiSHLinux
//
// Created by Theodore Dubois on 12/30/21.

#include <linux/init.h>
#include <linux/namei.h>
#include <linux/errname.h>
#include <linux/kthread.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/syscalls.h>
#include <linux/init_syscalls.h>
#include <linux/init_task.h>
#include <linux/termios.h>
#include <linux/fcntl.h>
#include <linux/vmalloc.h>
#include <linux/fdtable.h>
#include <linux/kref.h>
#include <uapi/linux/mount.h>
#include "LinuxInterop.h"

static struct path ptmx_path;

struct ios_pty_wq {
    struct ios_pty *pty;
    struct wait_queue_entry wq;
    struct wait_queue_head *head;
};

struct ios_pty {
    struct kref refcount;  // Reference counting to prevent use-after-free
    dev_t pts_rdev;
    struct file *ptm;
    nsobj_t terminal;
    struct linux_tty linux_tty;
    
    // Use dynamic allocation for wait queues to avoid hardcoded limit
    struct ios_pty_wq *wqs;
    int n_wqs;
    int wqs_capacity;
    
    poll_table pt;
    spinlock_t lock;
    
    struct work_struct poll_cb_work;
    struct work_struct output_work;
    atomic_t cleanup_started;  // Prevent double cleanup
};

static struct ios_pty *ios_pty_get(struct ios_pty *pty) {
    if (pty)
        kref_get(&pty->refcount);
    return pty;
}

static void ios_pty_release(struct kref *ref) {
    struct ios_pty *pty = container_of(ref, struct ios_pty, refcount);
    
    for (int i = 0; i < pty->n_wqs; i++)
        remove_wait_queue(pty->wqs[i].head, &pty->wqs[i].wq);
    
    kvfree(pty->wqs);
    fput(pty->ptm);
    nsobj_t terminal = pty->terminal;
    Terminal_setLinuxTTY(terminal, NULL);
    objc_put(terminal);
    
    kfree(pty);
}

// Safe put with NULL check
static void ios_pty_put(struct ios_pty *pty) {
    if (pty)
        kref_put(&pty->refcount, ios_pty_release);
}

static void ios_pty_output_work(struct work_struct *output_work) {
    struct ios_pty *pty = container_of(output_work, struct ios_pty, output_work);
    
    if (atomic_read(&pty->cleanup_started))
        return;
   
    pty = ios_pty_get(pty);
    if (!pty)
        return;
    
    char *buf = kvmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buf) {
        printk(KERN_WARNING "ios: failed to allocate buffer for pty output\n");
        ios_pty_put(pty);
        return;
    }
    
    ssize_t size;
    int iterations = 0;
    const int MAX_ITERATIONS = 100;  // Prevent infinite loops
    
    for (;;) {
        if (++iterations > MAX_ITERATIONS) {
            printk(KERN_DEBUG "ios: pty output work exceeded max iterations\n");
            break;
        }
        
        size_t room = Terminal_roomForOutput(pty->terminal);
        if (room == 0) {
            // This is normal - just exit, work will be scheduled again when room is available
            break;
        }
        
        size = kernel_read(pty->ptm, buf, room, NULL);
        if (size < 0) {
            if (size != -EAGAIN)
                printk(KERN_WARNING "ios: pty read failed: %s\n", errname(size));
            break;
        }
        
        if (size == 0) {
            // EOF reached
            break;
        }
        
        int sent = Terminal_sendOutput_length(pty->terminal, buf, size);
        if (sent != size) {
            printk(KERN_WARNING "ios: dropped %ld bytes of pty output\n", size - sent);
            break;
        }
    }
    
    kvfree(buf);
    ios_pty_put(pty);
}

static void ios_pty_cleanup(struct ios_pty *pty) {
    // Prevent double cleanup
    if (atomic_cmpxchg(&pty->cleanup_started, 0, 1) != 0)
        return;
    
    cancel_work_sync(&pty->poll_cb_work);
    cancel_work_sync(&pty->output_work);
    ios_pty_put(pty);
}

static void ios_pty_cb_can_output(struct linux_tty *linux_tty) {
    struct ios_pty *pty = container_of(linux_tty, struct ios_pty, linux_tty);
    
    if (atomic_read(&pty->cleanup_started))
        return;
    
    schedule_work(&pty->output_work);
}

static void ios_pty_cb_send_input(struct linux_tty *linux_tty, const char *data, size_t length) {
    struct ios_pty *pty = container_of(linux_tty, struct ios_pty, linux_tty);
    
    if (atomic_read(&pty->cleanup_started))
        return;
    
    // Get reference to protect against concurrent cleanup
    pty = ios_pty_get(pty);
    if (!pty)
        return;
    
    spin_lock(&pty->lock);
    struct file *ptm = pty->ptm;
    fget(ptm);  // Increment file reference count
    spin_unlock(&pty->lock);
    
    ssize_t written = kernel_write(ptm, data, length, NULL);
    fput(ptm);
    
    if (written < 0)
        printk(KERN_WARNING "ios: pty input failed: %s\n", errname(written));
    else if (written != (ssize_t)length)
        printk(KERN_WARNING "ios: dropped %ld bytes of pty input\n", length - written);
    
    ios_pty_put(pty);
}

static void ios_pty_cb_resize(struct linux_tty *linux_tty, int cols, int rows) {
    struct ios_pty *pty = container_of(linux_tty, struct ios_pty, linux_tty);
    
    if (atomic_read(&pty->cleanup_started))
        return;
    
    pty = ios_pty_get(pty);
    if (!pty)
        return;
    
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
    };
    
    spin_lock(&pty->lock);
    struct file *ptm = pty->ptm;
    fget(ptm);
    spin_unlock(&pty->lock);
    
    vfs_ioctl(ptm, TIOCSWINSZ, (unsigned long) &ws);
    fput(ptm);
    
    ios_pty_put(pty);
}

static void ios_pty_cb_hangup(struct linux_tty *linux_tty) {
    struct ios_pty *pty = container_of(linux_tty, struct ios_pty, linux_tty);
    ios_pty_cleanup(pty);
}

static struct linux_tty_callbacks ios_pty_callbacks = {
    .can_output = ios_pty_cb_can_output,
    .send_input = ios_pty_cb_send_input,
    .resize = ios_pty_cb_resize,
    .hangup = ios_pty_cb_hangup,
};

static void ios_pty_poll_cb_work(struct work_struct *work) {
    struct ios_pty *pty = container_of(work, struct ios_pty, poll_cb_work);
    
    if (atomic_read(&pty->cleanup_started))
        return;
    
    pty = ios_pty_get(pty);
    if (!pty)
        return;
    
    spin_lock(&pty->lock);
    struct file *ptm = pty->ptm;
    fget(ptm);
    spin_unlock(&pty->lock);
    
    __poll_t events = vfs_poll(ptm, NULL);
    fput(ptm);
    
    if (events & EPOLLIN)
        ios_pty_output_work(&pty->output_work);
    
    if (events & EPOLLHUP) {
        ios_pty_cleanup(pty);
        ios_pty_put(pty);
        return;
    }
    
    ios_pty_put(pty);
}

static int ptm_callback(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key) {
    struct ios_pty_wq *pty_wq = container_of(wq_entry, struct ios_pty_wq, wq);
    struct ios_pty *pty = pty_wq->pty;
    
    if (atomic_read(&pty->cleanup_started))
        return 0;
    
    schedule_work(&pty->poll_cb_work);
    return 0;
}

static int poll_callback_grow_array(struct ios_pty *pty) {
    int new_capacity = pty->wqs_capacity * 2;
    if (new_capacity < 8)
        new_capacity = 8;
    
    struct ios_pty_wq *new_wqs = kvmalloc(sizeof(*new_wqs) * new_capacity, GFP_KERNEL);
    if (!new_wqs)
        return -ENOMEM;
    
    if (pty->wqs) {
        memcpy(new_wqs, pty->wqs, sizeof(*pty->wqs) * pty->n_wqs);
        kvfree(pty->wqs);
    }
    
    pty->wqs = new_wqs;
    pty->wqs_capacity = new_capacity;
    return 0;
}

static void poll_callback(struct file *file, wait_queue_head_t *whead, poll_table *pt) {
    struct ios_pty *pty = container_of(pt, struct ios_pty, pt);
    
    spin_lock(&pty->lock);
    
    if (pty->n_wqs >= pty->wqs_capacity) {
        int err = poll_callback_grow_array(pty);
        if (err) {
            spin_unlock(&pty->lock);
            printk(KERN_WARNING "ios: failed to grow poll callback array\n");
            return;
        }
    }
    
    struct ios_pty_wq *pty_wq = &pty->wqs[pty->n_wqs++];
    pty_wq->pty = pty;
    pty_wq->head = whead;
    init_waitqueue_func_entry(&pty_wq->wq, ptm_callback);
    
    spin_unlock(&pty->lock);
    
    add_wait_queue(whead, &pty_wq->wq);
}

struct file *ios_pty_open(nsobj_t *terminal_out) {
    struct file *ptm_file = dentry_open(&ptmx_path, O_RDWR, current_cred());
    if (IS_ERR(ptm_file))
        return ptm_file;
    
    int lock_pty = 0;
    int err = vfs_ioctl(ptm_file, TIOCSPTLCK, (unsigned long) &lock_pty);
    if (err < 0) {
        printk(KERN_WARNING "ios: failed to unlock pty: %s\n", errname(err));
        fput(ptm_file);
        return ERR_PTR(err);
    }
    
    spin_lock(&ptm_file->f_lock);
    ptm_file->f_flags |= O_NONBLOCK;
    spin_unlock(&ptm_file->f_lock);
    
    // Get the slave side of the PTY
    int fd = vfs_ioctl(ptm_file, TIOCGPTPEER, O_RDWR);
    if (fd < 0) {
        fput(ptm_file);
        return ERR_PTR(fd);
    }
    
    struct file *pts_file = fget(fd);
    close_fd(fd);
    
    // Allocate PTY structure
    struct ios_pty *pty = kzalloc(sizeof(*pty), GFP_KERNEL);
    if (pty == NULL) {
        fput(pts_file);
        fput(ptm_file);
        return ERR_PTR(-ENOMEM);
    }
    
    // Initialize kref (starts at 1)
    kref_init(&pty->refcount);
    spin_lock_init(&pty->lock);
    atomic_set(&pty->cleanup_started, 0);
    
    // Allocate initial wait queue array
    pty->wqs_capacity = 4;
    pty->wqs = kvmalloc(sizeof(*pty->wqs) * pty->wqs_capacity, GFP_KERNEL);
    if (!pty->wqs) {
        kfree(pty);
        fput(pts_file);
        fput(ptm_file);
        return ERR_PTR(-ENOMEM);
    }
    pty->n_wqs = 0;
    
    pty->ptm = ptm_file;
    
    INIT_WORK(&pty->poll_cb_work, ios_pty_poll_cb_work);
    INIT_WORK(&pty->output_work, ios_pty_output_work);
    
    pty->pts_rdev = pts_file->f_inode->i_rdev;
    pty->terminal = Terminal_terminalWithType_number(MAJOR(pty->pts_rdev), MINOR(pty->pts_rdev));
    
    if (!pty->terminal) {
        printk(KERN_WARNING "Failed to create terminal\n");
        kvfree(pty->wqs);
        kfree(pty);
        fput(pts_file);
        fput(ptm_file);
        return ERR_PTR(-ENOMEM);
    }
    
    pty->linux_tty.ops = &ios_pty_callbacks;
    Terminal_setLinuxTTY(pty->terminal, &pty->linux_tty);
    *terminal_out = pty->terminal;
    
    init_poll_funcptr(&pty->pt, poll_callback);
    __poll_t revents = vfs_poll(pty->ptm, &pty->pt);
    
    if (revents && pty->n_wqs > 0)
        ptm_callback(&pty->wqs[pty->n_wqs-1].wq, 0, 0, NULL);
    
    return pts_file;
}

static __init int ios_pty_init(void) {
    init_mkdir("/dev/pts", 0755);
    int err = do_mount("devpts", "/dev/pts", "devpts", MS_SILENT, NULL);
