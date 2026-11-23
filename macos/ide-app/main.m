// macos/ide-app/main.m — "Nihil IDE.app": the page of the IDE in a window of its own (WebKit),
// with the Nihil icon in the Dock and in Cmd+Tab. The IDE itself is ide/server.nihil.
//
//   Nihil IDE.app/Contents/MacOS/nihil-ide [URL]
//
// With no URL it uses http://127.0.0.1:$NIHIL_IDE_PORT/ (8730). If nothing answers there it starts
// the server itself (`nihil ide --no-window` of the installation the app lives in, at
// libexec/nihil/Nihil IDE.app, or that its copy in ~/Applications names) and stops it when the app quits; when `nihil ide` started the
// server, it stays the terminal's. Built by macos/ide-app/build.sh.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

// no web view right-click menu (Reload, Back, Inspect Element…): the page has its own
@interface IDEWebView : WKWebView
@end

@implementation IDEWebView
static NSEvent *lastMouseDown;
- (void)mouseDown:(NSEvent *)event {
    lastMouseDown = event;
    [super mouseDown:event];
}
- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event {
    [menu removeAllItems];                      // the page shows its own menu (ide/static/index.html)
    [super willOpenMenu:menu withEvent:event];
}
@end

@interface App : NSObject <NSApplicationDelegate, NSWindowDelegate, WKUIDelegate, WKNavigationDelegate, WKScriptMessageHandler>
@property (strong) NSWindow *window;
@property (strong) WKWebView *web;
@property (strong) NSURL *url;
@property (strong) NSTask *server;
@end

static NSString *token_of(NSString *html) {
    NSRange r = [html rangeOfString:@"name=\"nihil-token\" content=\""];
    if (r.location == NSNotFound) return @"";
    NSUInteger from = r.location + r.length;
    NSRange end = [html rangeOfString:@"\"" options:0 range:NSMakeRange(from, html.length - from)];
    return end.location == NSNotFound ? @"" : [html substringWithRange:NSMakeRange(from, end.location - from)];
}

static NSString *fetch(NSURL *url) {
    __block NSString *text = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 1;
    [[[NSURLSession sessionWithConfiguration:cfg] dataTaskWithURL:url
        completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
            if (!e && d && [(NSHTTPURLResponse *)r statusCode] == 200)
                text = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
            dispatch_semaphore_signal(done);
        }] resume];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    return text;
}

@implementation App

// one menu only, "Nihil IDE": the shortcuts of Edit, View and Window are in it as hidden items
// (the keys work, the menu bar does not show them)
- (void)menus {
    NSMenu *bar = [NSMenu new];
    NSMenu *app = [NSMenu new];
    [app addItemWithTitle:@"About Nihil IDE" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Hide Nihil IDE" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItemWithTitle:@"Quit Nihil IDE" action:@selector(terminate:) keyEquivalent:@"q"];
    NSArray *hidden = @[
        @[@"Undo", @"undo:", @"z"], @[@"Redo", @"redo:", @"Z"], @[@"Cut", @"cut:", @"x"], @[@"Copy", @"copy:", @"c"],
        @[@"Paste", @"paste:", @"v"], @[@"Select All", @"selectAll:", @"a"], @[@"Reload", @"reload:", @"r"],
        @[@"Minimize", @"performMiniaturize:", @"m"], @[@"Close", @"performClose:", @"w"],
        @[@"Full Screen", @"toggleFullScreen:", @"f"]];
    for (NSArray *h in hidden) {
        NSMenuItem *item = [app addItemWithTitle:h[0] action:NSSelectorFromString(h[1]) keyEquivalent:h[2]];
        if ([h[1] isEqual:@"toggleFullScreen:"])
            item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
        item.hidden = YES;
        item.allowsKeyEquivalentWhenHidden = YES;
    }
    NSMenuItem *top = [NSMenuItem new];
    top.submenu = app;
    [bar addItem:top];
    NSApp.mainMenu = bar;
}

- (void)reload:(id)sender { [self.web loadRequest:[NSURLRequest requestWithURL:self.url]]; }

// the server: the one already answering at the URL, or one of our own
- (BOOL)serve {
    if (fetch(self.url)) return YES;
    // inside the installation (libexec/nihil/Nihil IDE.app), or the copy in ~/Applications made by
    // the installer, which writes the installation's place in Resources/nihil-home
    NSString *home = [[[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent]
                         stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    NSString *mark = [[NSBundle mainBundle] pathForResource:@"nihil-home" ofType:nil];
    NSString *marked = mark ? [[NSString stringWithContentsOfFile:mark encoding:NSUTF8StringEncoding error:nil]
                                  stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] : nil;
    if (marked.length) home = marked;
    NSString *nihil = [home stringByAppendingPathComponent:@"bin/nihil"];
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:nihil]) return NO;
    NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *log = [logs stringByAppendingPathComponent:@"nihil-ide.log"];
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment] mutableCopy];
    env[@"NIHIL_IDE_PORT"] = [self.url.port stringValue] ?: @"8730";
    self.server = [NSTask new];
    self.server.executableURL = [NSURL fileURLWithPath:nihil];
    self.server.arguments = @[@"ide", @"--no-window"];
    self.server.environment = env;
    self.server.standardOutput = self.server.standardError = [NSFileHandle fileHandleForWritingAtPath:log];
    if (![self.server launchAndReturnError:nil]) { self.server = nil; return NO; }
    for (int i = 0; i < 50; i++) {
        if (fetch(self.url)) return YES;
        [NSThread sleepForTimeInterval:0.2];
    }
    return NO;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    [self menus];
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    NSString *port = [[NSProcessInfo processInfo] environment][@"NIHIL_IDE_PORT"] ?: @"8730";
    NSString *u = nil;
    for (NSString *a in [args subarrayWithRange:NSMakeRange(1, args.count - 1)])
        if ([a hasPrefix:@"http://"]) u = a;
    self.url = [NSURL URLWithString:u ?: [NSString stringWithFormat:@"http://127.0.0.1:%@/", port]];

    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    // the page's right-click menu asks for cut/copy/paste/select all here: the system's commands,
    // so pasting needs no permission (the clipboard API would ask each time)
    [cfg.userContentController addScriptMessageHandler:self name:@"nihil"];
    self.web = [[IDEWebView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 820) configuration:cfg];
    self.web.UIDelegate = self;
    self.web.navigationDelegate = self;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 820)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                  | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Nihil IDE";
    // no title bar: the page's header (42 px) is the top of the window, the window's buttons inside it
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.titlebarAppearsTransparent = YES;
    self.window.backgroundColor = [NSColor colorWithSRGBRed:0x1d / 255.0 green:0x1f / 255.0 blue:0x27 / 255.0 alpha:1];
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.delegate = self;
    self.window.contentView = self.web;
    self.window.frameAutosaveName = @"NihilIDE";
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self placeButtons];
    [NSApp activateIgnoringOtherApps:YES];

    if ([self serve]) {
        [self reload:nil];
    } else {
        [self.web loadHTMLString:@"<body style='background:#16171d;color:#d7dae0;font:14px system-ui;padding:40px'>"
            "<h2>The Nihil IDE server is not running</h2><p>Start it in a terminal with <code>nihil ide</code>"
            " (log: ~/Library/Logs/nihil-ide.log).</p></body>" baseURL:nil];
    }
}

// `nihil ide` again (or a click on the Dock icon): a new server has a new token, so reload then
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible {
    [self.window makeKeyAndOrderFront:nil];
    [self.web evaluateJavaScript:@"(document.querySelector('meta[name=nihil-token]')||{}).content||''"
        completionHandler:^(id current, NSError *e) {
            NSString *page = fetch(self.url);
            if (page && ![token_of(page) isEqualToString:current ?: @""]) [self reload:nil];
        }];
    return NO;
}

// the window's buttons centered in the page's header (42 px high), 14 px from the left
- (void)placeButtons {
    NSButton *close = [self.window standardWindowButton:NSWindowCloseButton];
    NSView *bar = close.superview.superview;              // the title bar's container
    if (!bar || (self.window.styleMask & NSWindowStyleMaskFullScreen)) return;
    CGFloat header = 42;
    NSRect r = bar.frame;
    r.size.height = header;
    r.origin.y = NSHeight(self.window.frame) - header;
    bar.frame = r;
    NSWindowButton kinds[] = {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton};
    for (int i = 0; i < 3; i++) {
        NSButton *b = [self.window standardWindowButton:kinds[i]];
        NSRect f = b.frame;
        f.origin.x = 14 + i * 20;
        f.origin.y = (header - NSHeight(f)) / 2;
        [b setFrameOrigin:f.origin];
    }
}
- (void)windowDidResize:(NSNotification *)n { [self placeButtons]; }
- (void)windowDidBecomeKey:(NSNotification *)n { [self placeButtons]; }
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    [self.web evaluateJavaScript:@"document.documentElement.classList.add('full-screen')" completionHandler:nil];
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    [self.web evaluateJavaScript:@"document.documentElement.classList.remove('full-screen')" completionHandler:nil];
    [self placeButtons];
}

- (void)userContentController:(WKUserContentController *)c didReceiveScriptMessage:(WKScriptMessage *)message {
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSString *win = message.body[@"window"];
    if ([win isEqual:@"drag"] && lastMouseDown && ([NSEvent pressedMouseButtons] & 1)) {
        [self.window performWindowDragWithEvent:lastMouseDown];
        return;
    }
    if ([win isEqual:@"zoom"]) { [self.window performZoom:nil]; return; }
    NSString *edit = message.body[@"edit"];
    NSDictionary *actions = @{@"cut": @"cut:", @"copy": @"copy:", @"paste": @"paste:", @"selectAll": @"selectAll:"};
    if ([edit isKindOfClass:[NSString class]] && actions[edit])
        [NSApp sendAction:NSSelectorFromString(actions[edit]) to:nil from:self];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)applicationWillTerminate:(NSNotification *)n {
    if (self.server.running) [self.server terminate];
}

// links that leave the IDE open in the browser
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    NSURL *u = action.request.URL;
    BOOL local = [u.scheme isEqualToString:@"about"] || ([u.host isEqualToString:self.url.host] && [u.port isEqual:self.url.port]);
    if (!local && action.navigationType == WKNavigationTypeLinkActivated) {
        [[NSWorkspace sharedWorkspace] openURL:u];
        decide(WKNavigationActionPolicyCancel);
        return;
    }
    decide(WKNavigationActionPolicyAllow);
}

- (WKWebView *)webView:(WKWebView *)web createWebViewWithConfiguration:(WKWebViewConfiguration *)c
    forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)f {
    if (action.request.URL) [[NSWorkspace sharedWorkspace] openURL:action.request.URL];
    return nil;
}

- (void)webView:(WKWebView *)web runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))done {
    NSAlert *a = [NSAlert new];
    a.messageText = message;
    [a runModal];
    done();
}

- (void)webView:(WKWebView *)web runJavaScriptConfirmPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))done {
    NSAlert *a = [NSAlert new];
    a.messageText = message;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    done([a runModal] == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView *)web runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
    defaultText:(NSString *)text initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString *))done {
    NSAlert *a = [NSAlert new];
    a.messageText = prompt;
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = text ?: @"";
    a.accessoryView = field;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    done([a runModal] == NSAlertFirstButtonReturn ? field.stringValue : nil);
}

@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        App *delegate = [App new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
