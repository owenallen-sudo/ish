//
//  LinuxTTY.c
//  libiSHLinux
//
//  Created by Theodore Dubois on 8/15/21.
//

#include "LinuxInterop.h"
#include <linux/bug.h>
#include <linux/console.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/slab.h>

static void nslog_console_write(struct console *console, const char *data, unsigned len) {
    ConsoleLog(data, len);
}

static struct console nslog_console = {
    .name = "nslog",
    .write = nslog_console_write,
    .flags = CON_PRINTBUFFER|CON_ANYTIME,
    .index = -1,
};

static __init int nslog_init(void) {
    register_console(&nslog_console);
    return 0;
}
device_initcall(nslog_init);

struct ios_tty {
    struct linux_tty linux_tty;
    struct tty_port port;
};

#define NUM_TTYS 6
static struct tty_driver *ios_tty_driver;
static struct ios_tty ios_ttys[NUM_TTYS];

static int ios_tty_port_activate(struct tty_port *port, struct tty_struct *tty) {
    BUG_ON(port != &ios_ttys[tty->index].port);
    nsobj_t terminal = Terminal_terminalWithType_number(TTY_MAJOR, tty->index);
    if (!terminal)
        return -ENOMEM;
    port->client_data = (void *) terminal;
    Terminal_setLinuxTTY(port->client_data, &container_of(port, struct ios_tty, port)->linux_tty);
    return 0;
}

static void ios_tty_port_destruct(struct tty_port *port) {
    nsobj_t client_data = port->client_data;
    if (client_data) {
        async_do_in_ios(^{
            objc_put(client_data);
            async_do_in_irq(^{
                tty_port_put(port);
            });
        });
    }
}

static struct tty_port_operations ios_tty_port_ops = {
    .activate = ios_tty_port_activate,
    .destruct = ios_tty_port_destruct,
};

static void ios_tty_cb_can_output(struct linux_tty *linux_tty) {
    struct ios_tty *tty = container_of(linux_tty, struct ios_tty, linux_tty);
    tty_port_tty_wakeup(&tty->port);
}

static void ios_tty_cb_send_input(struct linux_tty *linux_tty, const char *data, size_t length) {
    struct ios_tty *tty = container_of(linux_tty, struct ios_tty, linux_tty);
    if (length > 0) {
        tty_insert_flip_string(&tty->port, data, length);
        tty_flip_buffer_push(&tty->port);
    }
}

static void ios_tty_cb_resize(struct linux_tty *linux_tty, int cols, int rows) {
    struct ios_tty *tty = container_of(linux_tty, struct ios_tty, linux_tty);
    struct tty_struct *tty_struct = tty_port_tty_get(&tty->port);
    if (tty_struct) {
        struct winsize ws = {
            .ws_row = rows,
            .ws_col = cols,
        };
        tty_do_resize(tty_struct, &ws);
        tty_kref_put(tty_struct);
    }
}

static void ios_tty_cb_hangup(struct linux_tty *linux_tty) {
}

static struct linux_tty_callbacks ios_tty_callbacks = {
    .can_output = ios_tty_cb_can_output,
    .send_input = ios_tty_cb_send_input,
    .resize = ios_tty_cb_resize,
    .hangup = ios_tty_cb_hangup,
};

static int ios_tty_open(struct tty_struct *tty, struct file *filp) {
    return tty_port_open(tty->port, tty, filp);
}

static int ios_tty_write(struct tty_struct *tty, const unsigned char *data, int len) {
    if (!tty || !tty->port || !tty->port->client_data || len <= 0)
        return 0;
    return Terminal_sendOutput_length(tty->port->client_data, data, len);
}

static unsigned int ios_tty_write_room(struct tty_struct *tty) {
    if (!tty || !tty->port || !tty->port->client_data)
        return 0;
    return Terminal_roomForOutput(tty->port->client_data);
}

static struct tty_operations ios_tty_ops = {
    .open = ios_tty_open,
    .write = ios_tty_write,
    .write_room = ios_tty_write_room,
};

static int ios_tty_console_setup(struct console *console, char *what) {
    nsobj_t terminal = Terminal_terminalWithType_number(TTY_MAJOR, 1);
    if (!terminal)
        return -ENOMEM;
    if (console->data)
        objc_put(console->data);
    console->data = (void *) terminal;
    return 0;
}

static void ios_tty_console_write(struct console *console, const char *data, unsigned len) {
    nsobj_t tty = console->data;
    if (!tty || !data || len == 0)
        return;
    
    while (len) {
        const char *newline = memchr(data, '\n', len);
        if (newline != NULL) {
            size_t chunk_len = newline - data;
            Terminal_sendOutput_length(tty, data, chunk_len);
            Terminal_sendOutput_length(tty, "\r\n", 2);
            len -= chunk_len + 1;
            data = newline + 1;
        } else {
            Terminal_sendOutput_length(tty, data, len);
            len = 0;
        }
    }
}

static struct tty_driver *ios_tty_console_device(struct console *console, int *index) {
    *index = console->index;
    return ios_tty_driver;
}

static struct console ios_tty_console = {
    .name = "tty",
    .setup = ios_tty_console_setup,
    .write = ios_tty_console_write,
    .device = ios_tty_console_device,
    .flags = CON_PRINTBUFFER|CON_ANYTIME,
    .index = -1,
};

static __init int ios_tty_init(void) {
    for (int i = 0; i < NUM_TTYS; i++) {
        ios_ttys[i].linux_tty.ops = &ios_tty_callbacks;
        tty_port_init(&ios_ttys[i].port);
        ios_ttys[i].port.ops = &ios_tty_port_ops;
    }

    ios_tty_driver = tty_alloc_driver(NUM_TTYS, TTY_DRIVER_REAL_RAW | TTY_DRIVER_RESET_TERMIOS);
    if (!ios_tty_driver) {
        panic("ios tty: failed to tty_alloc_driver");
    }
    
    ios_tty_driver->driver_name = "ios";
    ios_tty_driver->name = "tty";
    ios_tty_driver->name_base = 1;
    ios_tty_driver->major = TTY_MAJOR;
    ios_tty_driver->minor_start = 1;
    ios_tty_driver->type = TTY_DRIVER_TYPE_CONSOLE;
    ios_tty_driver->subtype = SYSTEM_TYPE_CONSOLE;
    ios_tty_driver->init_termios = tty_std_termios;
    tty_set_operations(ios_tty_driver, &ios_tty_ops);

    for (int i = 0; i < NUM_TTYS; i++) {
        tty_port_link_device(&ios_ttys[i].port, ios_tty_driver, i);
    }

    if (tty_register_driver(ios_tty_driver)) {
        panic("ios tty: failed to tty_register_driver");
    }

    register_console(&ios_tty_console);

    return 0;
}
device_initcall(ios_tty_init);
