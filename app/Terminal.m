//
//  Terminal.m
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//  Modified by Owen Allen on 5/7/26
//

#import "Terminal.h"
#import "DelayedUITask.h"
#import "UserPreferences.h"
#include "LinuxInterop.h"
#include "fs/devices.h"
#include "fs/tty.h"

extern struct tty_driver ios_pty_driver;

#if !ISH_LINUX
typedef struct tty *tty_t;
#else
typedef struct linux_tty *tty_t;
#endif

@interface Terminal () <WKScriptMessageHandler> {
#if !ISH_LINUX
    lock_t _dataLock;
    cond_t _dataConsumed;
#endif
}

/* ---------- State ---------- */
@property (nonatomic, assign) BOOL loaded;
@property (nonatomic) tty_t tty;                     // guarded by @synchronized(self) for Linux, lock_t for non‑Linux
@property (nonatomic) NSMutableData *pendingData;    // accessed while holding the appropriate lock
@property (nonatomic, assign) BOOL outputInProgress; // guarded by the same lock as pendingData

/* ---------- Tasks ---------- */
@property (nonatomic) DelayedUITask *refreshTask;
@property (nonatomic) DelayedUITask *scrollToBottomTask;

/* ---------- Misc ---------- */
@property (nonatomic, assign) BOOL applicationCursor;
@property (nonatomic, strong) NSNumber *terminalsKey;
@property (nonatomic, strong) NSUUID *uuid;
@property (nonatomic, assign) BOOL enableVoiceOverAnnounce;

@end

@interface CustomWebView : WKWebView
@end

@implementation CustomWebView
- (BOOL)becomeFirstResponder {
    return [super becomeFirstResponder];
}
- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(copy:) || action == @selector(paste:)) {
        return NO;
    }
    return [super canPerformAction:action withSender:sender];
}
@end

@implementation Terminal
@synthesize webView = _webView;

static const int BUF_SIZE = 1 << 14;

static NSMapTable<NSNumber *, Terminal *> *terminals;
static NSMapTable<NSUUID *, Terminal *> *terminalsByUUID;

/* ---------- Initialisation ---------- */
- (instancetype)initWithType:(int)type number:(int)num {
    @synchronized (Terminal.class) {
        self.terminalsKey = @(dev_make(type, num));
        Terminal *existing = [terminals objectForKey:self.terminalsKey];
        if (existing) return existing;

        if (self = [super init]) {
            self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
            self.refreshTask = [[DelayedUITask alloc] initWithTarget:self
                                                             action:@selector(refresh)];
            self.scrollToBottomTask = [[DelayedUITask alloc] initWithTarget:self
                                                                    action:@selector(scrollToBottom)];
#if !ISH_LINUX
            lock_init(&_dataLock);
            cond_init(&_dataConsumed);
#endif
            [terminals setObject:self forKey:self.terminalsKey];
            self.uuid = [NSUUID UUID];
            [terminalsByUUID setObject:self forKey:self.uuid];
        }
        return self;
    }
}

/* ---------- Web view ---------- */
- (WKWebView *)webView {
    if (_webView == nil) {
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        [config.userContentController addScriptMessageHandler:self name:@"load"];
        [config.userContentController addScriptMessageHandler:self name:@"log"];
        [config.userContentController addScriptMessageHandler:self name:@"sendInput"];
        [config.userContentController addScriptMessageHandler:self name:@"resize"];
        [config.userContentController addScriptMessageHandler:self name:@"propUpdate"];

        CGRect webviewSize = CGRectMake(0, 0, 10000, 10000);
        _webView = [[CustomWebView alloc] initWithFrame:webviewSize
                                           configuration:config];
        if (@available(macOS 13.3, iOS 16.4, tvOS 16.4, *)) {
            _webView.inspectable = YES;
        }
        _webView.scrollView.scrollEnabled = NO;
        NSURL *xtermHtmlFile = [NSBundle.mainBundle URLForResource:@"term"
                                                     withExtension:@"html"];
        [_webView loadFileURL:xtermHtmlFile allowingReadAccessToURL:xtermHtmlFile];
    }
    return _webView;
}

/* ---------- Helper ---------- */
#if !ISH_LINUX
+ (Terminal *)createPseudoTerminal:(struct tty **)tty {
    *tty = pty_open_fake(&ios_pty_driver);
    if (IS_ERR(*tty))
        return nil;
    return (__bridge Terminal *)(*tty)->data;
}
#endif

- (void)setTty:(tty_t)tty {
    @synchronized (self) {
        _tty = tty;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self syncWindowSize];
    });
}

/* ---------- Script messaging ---------- */
- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {

    if ([message.name isEqualToString:@"load"]) {
        self.loaded = YES;
        [self.refreshTask schedule];
        self.enableVoiceOverAnnounce = self.enableVoiceOverAnnounce;
    } else if ([message.name isEqualToString:@"log"]) {
        NSLog(@"%@", message.body);
    } else if ([message.name isEqualToString:@"sendInput"]) {
        if (![message.body isKindOfClass:[NSString class]]) return;
        NSData *data = [message.body dataUsingEncoding:NSUTF8StringEncoding];
        [self sendInput:data];
    } else if ([message.name isEqualToString:@"resize"]) {
        [self syncWindowSize];
    } else if ([message.name isEqualToString:@"propUpdate"]) {
        if (![message.body isKindOfClass:[NSArray class]] || [(NSArray *)message.body count] < 2) return;
        NSArray *pair = (NSArray *)message.body;
        [self setValue:pair[1] forKey:pair[0]];
    }
}

/* ---------- Window size ---------- */
- (void)syncWindowSize {
    [self.webView evaluateJavaScript:@"exports.getSize()"
                   completionHandler:^(NSArray<NSNumber *> *dimensions, NSError *error) {
        if (error || dimensions.count < 2) {
            NSLog(@"Failed to obtain terminal size: %@", error);
            return;
        }
        int cols = dimensions[0].intValue;
        int rows = dimensions[1].intValue;
        if (self.tty == NULL) return;

#if !ISH_LINUX
        lock(&self.tty->lock);
        tty_set_winsize(self.tty, (struct winsize_){ .col = cols, .row = rows });
        unlock(&self.tty->lock);
#else
        async_do_in_workqueue(^{
            if (self->_tty) {
                self->_tty->ops->resize(self->_tty, cols, rows);
            }
        });
#endif
    }];
}

/* ---------- Voice‑Over ---------- */
- (void)setEnableVoiceOverAnnounce:(BOOL)enableVoiceOverAnnounce {
    _enableVoiceOverAnnounce = enableVoiceOverAnnounce;
    NSString *js = [NSString stringWithFormat:
                    @"term.setAccessibilityEnabled(%@)",
                    enableVoiceOverAnnounce ? @"true" : @"false"];
    [self.webView evaluateJavaScript:js completionHandler:nil];
}

/* ---------- Output handling ---------- */
- (int)sendOutput:(const void *)buf length:(int)len {
#if !ISH_LINUX
    lock(&_dataLock);
    if (NSThread.isMainThread && _pendingData.length >= BUF_SIZE) {
        unlock(&_dataLock);
        return 0;
    }

    while (!NSThread.isMainThread && _pendingData.length >= BUF_SIZE) {
        wait_for_ignore_signals(&_dataConsumed, &_dataLock, NULL);
    }

    NSUInteger room = BUF_SIZE - _pendingData.length;
    int written = (int)MIN((NSUInteger)len, room);
    if (written > 0) {
        [_pendingData appendData:[NSData dataWithBytes:buf length:written]];
        [self.refreshTask schedule];
    }
    unlock(&_dataLock);
    return written;
#else
    int written = 0;
    @synchronized (self) {
        int room = [self roomForOutput];
        written = len > room ? room : len;
        if (written > 0) {
            [_pendingData appendData:[NSData dataWithBytes:buf length:written]];
            [self.refreshTask schedule];
        }
    }
    return written;
#endif
}

/* ---------- Linux‑specific output space ---------- */
#if ISH_LINUX
- (int)roomForOutput {
    @synchronized (self) {
        if (_pendingData.length > BUF_SIZE) return 0;
        return BUF_SIZE - (int)_pendingData.length;
    }
}
#endif

/* ---------- Input handling ---------- */
- (void)sendInput:(NSData *)input {
    if (input == nil) return;
    if (self.tty == NULL) return;
#if !ISH_LINUX
    tty_input(self.tty, input.bytes, input.length, 0);
#else
    async_do_in_workqueue(^{
        if (self->_tty) {
            self->_tty->ops->send_input(self->_tty, input.bytes, input.length);
        }
    });
#endif
    [self.webView evaluateJavaScript:@"exports.setUserGesture()" completionHandler:nil];
    [self.scrollToBottomTask schedule];
}

/* ---------- Scrolling ---------- */
- (void)scrollToBottom {
    [self.webView evaluateJavaScript:@"exports.scrollToBottom()" completionHandler:nil];
}

/* ---------- ANSI helper ---------- */
- (NSString *)arrow:(char)direction {
    NSAssert(direction >= 'A' && direction <= 'D', @"Invalid arrow direction: %c", direction);
    if (direction < 'A' || direction > 'D') direction = 'A';
    return [NSString stringWithFormat:@"\x1b%c%c",
            self.applicationCursor ? 'O' : '[', direction];
}

/* ---------- Refresh ---------- */
- (void)refresh {
    if (!self.loaded) return;

    NSData *data;

#if !ISH_LINUX
    lock(&_dataLock);
    if (self.outputInProgress) {
        [self.refreshTask schedule];
        unlock(&_dataLock);
        return;
    }
    data = self.pendingData;
    self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
    self.outputInProgress = YES;
    notify(&self->_dataConsumed);
    unlock(&_dataLock);
#else
    @synchronized (self) {
        if (self.outputInProgress) {
            [self.refreshTask schedule];
            return;
        }
        data = self.pendingData;
        self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
        self.outputInProgress = YES;
        if (self->_tty) {
            async_do_in_irq(^{
                self->_tty->ops->can_output(self->_tty);
            });
        }
    }
#endif

    if (data.length == 0) {
        @synchronized (self) {
            self.outputInProgress = NO;
        }
        return;
    }

    NSString *dataString = [[NSString alloc] initWithData:data
                                                   encoding:NSISOLatin1StringEncoding];
    if (!dataString) {
        dataString = [[NSString alloc] initWithData:data
                                            encoding:NSUTF8StringEncoding];
    }
    if (!dataString) {
        @synchronized (self) {
            self.outputInProgress = NO;
        }
        return;
    }

    dataString = [dataString stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    dataString = [dataString stringByReplacingOccurrencesOfString:@"\r" withString:@"\\r"];
    dataString = [dataString stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
    dataString = [dataString stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];

    NSString *js = [NSString stringWithFormat:@"exports.write(\"%@\")", dataString];
    [self.webView evaluateJavaScript:js
                   completionHandler:^(id result, NSError *error) {
        @synchronized (self) {
            self.outputInProgress = NO;
        }
        if (error) {
            NSLog(@"error sending bytes to the terminal: %@", error);
        }
    }];
}

/* ---------- Argument conversion ---------- */
+ (void)convertCommand:(NSArray<NSString *> *)command
                toArgs:(char *)argv
             limitSize:(size_t)maxSize {
    if (maxSize < 2) return;

    char *p = argv;
    char *end = argv + maxSize;

    for (NSString *cmd in command) {
        const char *c = cmd.UTF8String;
        while (p < end - 2 && (*p = *c) != '\0') {
            p++;
            c++;
        }
        if (p >= end - 2) {
            *p = '\0';
            if (p + 1 < end) *(p + 1) = '\0';
            return;
        }
        *p = '\0';
        p++;
    }

    if (p < end) {
        *p = '\0';
    }
}

/* ---------- Factory helpers ---------- */
+ (Terminal *)terminalWithType:(int)type number:(int)number {
    return [[Terminal alloc] initWithType:type number:number];
}

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid {
    @synchronized (Terminal.class) {
        return [terminalsByUUID objectForKey:uuid];
    }
}

/* ---------- Destruction ---------- */
- (void)destroy {
    tty_t tty = self.tty;
    if (tty != NULL) {
#if !ISH_LINUX
        lock(&tty->lock);
        tty_hangup(tty);
        unlock(&tty->lock);
#else
        tty->ops->hangup(tty);
#endif
    }
    @synchronized (Terminal.class) {
        [terminals removeObjectForKey:self.terminalsKey];
        [terminalsByUUID removeObjectForKey:self.uuid];
    }
}

/* ---------- Class initialisation ---------- */
+ (void)initialize {
    if (self == Terminal.class) {
        terminals = [NSMapTable strongToWeakObjectsMapTable];
        terminalsByUUID = [NSMapTable strongToWeakObjectsMapTable];
    }
}

@end

/* ---------- C bridge (Linux) ---------- */
#if ISH_LINUX
nsobj_t Terminal_terminalWithType_number(int type, int number) {
    return CFBridgingRetain([Terminal terminalWithType:type number:number]);
}
int Terminal_sendOutput_length(nsobj_t _self, const char *data, int size) {
    return [(__bridge Terminal *)_self sendOutput:data length:size];
}
int Terminal_roomForOutput(nsobj_t _self) {
    return [(__bridge Terminal *)_self roomForOutput];
}
void Terminal_setLinuxTTY(nsobj_t _self, struct linux_tty *tty) {
    [(__bridge Terminal *)_self setTty:tty];
}
#endif

/* ---------- iOS tty driver ---------- */
#if !ISH_LINUX
static int ios_tty_init(struct tty *tty) {
    unlock(&ttys_lock);

    void (^init_block)(void) = ^{
        Terminal *terminal = [Terminal terminalWithType:tty->type number:tty->num];
        tty->data = (void *)CFBridgingRetain(terminal);
        terminal.tty = tty;
    };

    if ([NSThread isMainThread]) {
        init_block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), init_block);
    }

    lock(&ttys_lock);
    return 0;
}

static int ios_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    Terminal *terminal = (__bridge Terminal *)tty->data;
    return [terminal sendOutput:buf length:(int)len];
}

static void ios_tty_cleanup(struct tty *tty) {
    Terminal *terminal = CFBridgingRelease(tty->data);
    tty->data = NULL;
    terminal.tty = NULL;
}

struct tty_driver_ops ios_tty_ops = {
    .init    = ios_tty_init,
    .write   = ios_tty_write,
    .cleanup = ios_tty_cleanup,
};
DEFINE_TTY_DRIVER(ios_console_driver, &ios_tty_ops, TTY_CONSOLE_MAJOR, 64);
struct tty_driver ios_pty_driver = {.ops = &ios_tty_ops};
#endif
