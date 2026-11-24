#include "screen.h"
#include "main_thread.h"

/// The macOS screen for the HDMI output: a borderless window covering a second display, or the Mac's own where it is asked to, in the mode the driver picks.
///
/// The code gates read C++ alone, so this Objective-C++ file holds the screen and nothing more: the loop, the scaler and the handover it serves live in platform_host_hdmi.cpp, where they are measured.
///
/// @technical
///
/// ## AppKit lives on the main thread
///
/// AppKit creates and draws windows only on the process's main thread, so main_host.cpp runs MoonLight's loop on a thread of its own and hands the main thread to `runMainThread`. That loop runs the run loop, which serves the blocks this file queues on the main queue, and pumps AppKit's events once the first window made it an application. The output thread never waits on the main thread, so a stop cannot deadlock on it.
///
/// ## A frame is an image the layer shows
///
/// The output thread draws into its own buffer; `present` copies it into a CGImage and hands that to the window's layer on the main queue, which scales nothing, the image being the display's own size. A frame still waiting for the main thread is replaced rather than queued behind.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>
#include <vector>

namespace mm::platform {

namespace {

std::atomic<bool> appStarted{false};   // NSApplication made, so the main loop pumps its events

struct Mac {
    CGDirectDisplayID display = 0;
    CGDisplayModeRef ownMode = nullptr;   // the display's mode before the output changed it
    NSWindow* window = nil;               // main thread only
    std::vector<uint8_t> pixels;
    uint16_t width = 0, height = 0;
    std::atomic<bool> framePending{false};
    IOPMAssertionID awake = kIOPMNullAssertionID;   // holds the display awake while the output shows
    std::atomic<bool> own{false};         // the display is the Mac's own
    std::atomic<bool> dismissed{false};   // Esc pressed on it, until the loop takes it
};
Mac& m = *new Mac;   // never destroyed: exit() runs static destructors while the main queue may still run a block reading it

// The display's mode for the driver's size at the nearest refresh, in device pixels and not a scaled HiDPI mode.
CGDisplayModeRef pickMode(CGDirectDisplayID display, const HdmiMode& want) {
    NSDictionary* options = @{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes : @YES};
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, (__bridge CFDictionaryRef)options);
    if (!modes) return nullptr;
    CGDisplayModeRef best = nullptr;
    double bestGap = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        if (CGDisplayModeGetPixelWidth(mode) != want.width || CGDisplayModeGetPixelHeight(mode) != want.height) continue;
        if (CGDisplayModeGetWidth(mode) != want.width) continue;   // a HiDPI mode draws points, not the pixels the sending card counts
        const double rate = CGDisplayModeGetRefreshRate(mode);
        const double gap = rate > want.hz ? rate - want.hz : want.hz - rate;
        if (!best || gap < bestGap) { best = mode; bestGap = gap; }
    }
    if (best) CGDisplayModeRetain(best);
    CFRelease(modes);
    return best;
}

}  // namespace

}  // namespace mm::platform

/// The output's window: borderless, yet able to take the keyboard, so Esc on the Mac's own display reaches it.
@interface MoonLightOutputWindow : NSWindow
@end
@implementation MoonLightOutputWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)cancelOperation:(id)sender { mm::platform::m.dismissed.store(mm::platform::m.own.load()); }   // Esc
@end

namespace mm::platform {

void runMainThread(const std::atomic<bool>& appDone) {
    while (!appDone.load()) {
        @autoreleasepool {
            if (appStarted.load()) {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
                                                       inMode:NSDefaultRunLoopMode dequeue:YES];
                if (event) [NSApp sendEvent:event];
            } else {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
            }
        }
    }
}

namespace screen {

bool open() { return true; }

void close() {}

// The display `screen::pick` chooses among the active ones, a mirror left out.
uint32_t find(uint32_t current, bool own) {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displays, &count) != kCGErrorSuccess) return 0;
    Display list[16];
    size_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (CGDisplayMirrorsDisplay(displays[i]) != kCGNullDirectDisplay) continue;   // a mirror shows what another display does
        list[n++] = {displays[i], CGDisplayIsMain(displays[i]) != 0};
    }
    const uint32_t id = pick(list, n, current, own);
    m.own.store(id && CGDisplayIsMain(id));
    return id;
}

// The display's EDID, which the I/O registry keeps on the display's port, found by the vendor and product codes Core Graphics reads from it. The built-in panel has none.
bool readEdid(uint32_t display, const edid::Size* sizes, size_t n, edid::Monitor& out) {
    const uint32_t vendor = CGDisplayVendorNumber(display) & 0x7fff;   // EDID's manufacturer code leaves the top bit 0; Core Graphics may set it
    const uint32_t model = CGDisplayModelNumber(display);
    io_iterator_t it = 0;
    if (IORegistryCreateIterator(MACH_PORT_NULL, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS) return false;
    bool parsed = false;
    while (io_registry_entry_t entry = IOIteratorNext(it)) {
        if (CFTypeRef value = IORegistryEntryCreateCFProperty(entry, CFSTR("EDID"), kCFAllocatorDefault, 0)) {
            if (CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) >= static_cast<CFIndex>(edid::kBlockBytes)) {
                const uint8_t* b = CFDataGetBytePtr((CFDataRef)value);
                const bool twoBlocks = CFDataGetLength((CFDataRef)value) >= static_cast<CFIndex>(2 * edid::kBlockBytes);
                if (((b[8] << 8 | b[9]) & 0x7fff) == vendor && static_cast<uint32_t>(b[10] | b[11] << 8) == model)
                    parsed = edid::parse(b, twoBlocks ? b + edid::kBlockBytes : nullptr, sizes, n, out);
            }
            CFRelease(value);
        }
        IOObjectRelease(entry);
        if (parsed) break;
    }
    IOObjectRelease(it);
    return parsed;
}

bool up(uint32_t display, const HdmiMode& want, HdmiMode& running) {
    m.display = display;
    if (CGDisplayModeRef mode = pickMode(display, want)) {
        m.ownMode = CGDisplayCopyDisplayMode(display);
        if (CGDisplaySetDisplayMode(display, mode, nullptr) != kCGErrorSuccess) { CGDisplayModeRelease(m.ownMode); m.ownMode = nullptr; }
        CGDisplayModeRelease(mode);
    }
    CGDisplayModeRef now = CGDisplayCopyDisplayMode(display);
    if (!now) { down(); return false; }
    m.width = static_cast<uint16_t>(CGDisplayModeGetPixelWidth(now));   // device pixels, which a HiDPI mode has twice its points of
    m.height = static_cast<uint16_t>(CGDisplayModeGetPixelHeight(now));
    running = {m.width, m.height, static_cast<uint8_t>(CGDisplayModeGetRefreshRate(now) + 0.5)};   // 0 for a panel that names no rate
    CGDisplayModeRelease(now);
    if (m.width == 0 || m.height == 0) { down(); return false; }
    m.pixels.assign(static_cast<size_t>(m.width) * m.height * 4, 0);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!appStarted.load()) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // no Dock icon or menu bar for a window that only shows
            [NSApp finishLaunching];
            appStarted.store(true);
        }
        NSScreen* target = nil;
        for (NSScreen* s in [NSScreen screens])
            if ([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == display) target = s;
        if (!target) return;
        NSWindow* window = [[MoonLightOutputWindow alloc] initWithContentRect:target.frame styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered defer:NO];
        window.level = NSScreenSaverWindowLevel;   // above the menu bar a second display may carry
        window.backgroundColor = NSColor.blackColor;
        window.releasedWhenClosed = NO;
        window.contentView.wantsLayer = YES;
        window.contentView.layer.contentsGravity = kCAGravityResize;
        window.contentView.layer.magnificationFilter = kCAFilterNearest;
        [window setFrame:target.frame display:YES];
        if (m.own.load()) {   // Esc gives the desktop back, which needs the keyboard here
            [NSApp activateIgnoringOtherApps:YES];
            [window makeKeyAndOrderFront:nil];
        }
        else [window orderFrontRegardless];
        m.window = window;
    });
    // A show has no keyboard to keep the screen saver and display sleep away; down() gives them back.
    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, CFSTR("MoonLight HDMI output"), &m.awake) != kIOReturnSuccess)
        m.awake = kIOPMNullAssertionID;
    std::printf("HDMI: %ux%u window on display %u\n", static_cast<unsigned>(m.width), static_cast<unsigned>(m.height), display);
    return true;
}

Surface back() { return {m.pixels.data(), static_cast<uint32_t>(m.width) * 4, m.width, m.height}; }

// The window scales the screen-sized buffer already, so the loop fills it as before.
bool presentScaled(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) { return false; }

void present() {
    if (m.framePending.exchange(true)) return;   // the main thread has not shown the last one yet: @xref{a-frame-is-an-image-the-layer-shows|replaced}
    CFDataRef data = CFDataCreate(nullptr, m.pixels.data(), static_cast<CFIndex>(m.pixels.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(m.width, m.height, 8, 32, static_cast<size_t>(m.width) * 4, space,
                                     static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little,   // B, G, R, X in memory: XRGB8888
                                     provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (m.window) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];   // a new frame replaces the last at once, not in a fade
            m.window.contentView.layer.contents = (__bridge id)image;
            [CATransaction commit];
        }
        CGImageRelease(image);
        m.framePending.store(false);
    });
}

void down() {
    if (m.awake != kIOPMNullAssertionID) { IOPMAssertionRelease(m.awake); m.awake = kIOPMNullAssertionID; }
    dispatch_async(dispatch_get_main_queue(), ^{
        [m.window orderOut:nil];
        m.window = nil;
    });
    if (m.ownMode) {
        CGDisplaySetDisplayMode(m.display, m.ownMode, nullptr);   // the display's own mode back
        CGDisplayModeRelease(m.ownMode);
        m.ownMode = nullptr;
    }
    m.display = 0;
}

void pump() {}

bool takeDismiss() { return m.dismissed.exchange(false); }

}  // namespace screen

}  // namespace mm::platform
