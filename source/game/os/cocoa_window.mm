#include "os/cocoa_window.h"
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

namespace os {

static NSWindow* nativeWindow = nil;
static id scrollMonitor = nil, magnifyMonitor = nil;
static id becomeKeyObserver = nil, resignKeyObserver = nil, moveObserver = nil;
//Whether the window covers its screen, above the menu bar
static bool coveringScreen = false;
static bool preciseScroll = false;

void* getNativeWindow(GLFWwindow* window) {
	return (void*)glfwGetCocoaWindow(window);
}

void attachCocoaWindow(GLFWwindow* window, void (*onMagnify)(double magnification)) {
	nativeWindow = glfwGetCocoaWindow(window);

	//Local monitors see each event before GLFW dispatches it to the window
	scrollMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
		handler:^NSEvent*(NSEvent* event) {
			if([event window] == nativeWindow)
				preciseScroll = [event hasPreciseScrollingDeltas];
			return event;
		}];

	magnifyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
		handler:^NSEvent*(NSEvent* event) {
			if([event window] == nativeWindow && onMagnify)
				onMagnify([event magnification]);
			return event;
		}];
}

static void removeObserver(id& observer) {
	if(observer) {
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
		observer = nil;
	}
}

static void removeMonitor(id& monitor) {
	if(monitor) {
		[NSEvent removeMonitor:monitor];
		monitor = nil;
	}
}

void detachCocoaWindow() {
	removeMonitor(scrollMonitor);
	removeMonitor(magnifyMonitor);
	removeObserver(becomeKeyObserver);
	removeObserver(resignKeyObserver);
	removeObserver(moveObserver);
	coveringScreen = false;
	if(nativeWindow)
		[NSApp setPresentationOptions:NSApplicationPresentationDefault];
	nativeWindow = nil;
}

bool lastScrollWasPrecise() {
	return preciseScroll;
}

static void coverScreen(NSWindow* window, bool inFront) {
	coveringScreen = inFront;
	if(inFront) {
		[NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
		[window setLevel:NSMainMenuWindowLevel + 1];
		//macOS keeps windows below the menu bar, and on screens with a notch the
		//menu bar can still push the window down when it is shown or focused
		NSScreen* screen = [window screen] ? [window screen] : [NSScreen mainScreen];
		if(!NSEqualRects([window frame], [screen frame]))
			[window setFrame:[screen frame] display:YES];
	}
	else {
		[NSApp setPresentationOptions:NSApplicationPresentationDefault];
		[window setLevel:NSNormalWindowLevel];
	}
}

//AppKit moves windows below the menu bar, for example when it slides in at the top of
//the screen. While the window covers the screen it keeps the frame it asked for.
static void keepFrameWhileCovering(NSWindow* window) {
	Class cls = [window class];
	SEL sel = @selector(constrainFrameRect:toScreen:);
	static IMP original = nullptr;
	if(original)
		return;
	original = method_getImplementation(class_getInstanceMethod(cls, sel));
	IMP replacement = imp_implementationWithBlock(^NSRect(id self, NSRect frame, NSScreen* screen) {
		if(coveringScreen && self == nativeWindow)
			return frame;
		return ((NSRect (*)(id, SEL, NSRect, NSScreen*))original)(self, sel, frame, screen);
	});
	//Adds the override to GLFW's window class, or replaces the one it inherits
	if(!class_addMethod(cls, sel, replacement, method_getTypeEncoding(class_getInstanceMethod(cls, sel))))
		method_setImplementation(class_getInstanceMethod(cls, sel), replacement);
}

void enterBorderlessFullscreen(GLFWwindow* glfwWindow) {
	NSWindow* window = glfwGetCocoaWindow(glfwWindow);
	nativeWindow = window;
	keepFrameWhileCovering(window);
	coverScreen(window, true);

	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	removeObserver(becomeKeyObserver);
	removeObserver(resignKeyObserver);
	becomeKeyObserver = [center addObserverForName:NSWindowDidBecomeKeyNotification object:window
		queue:nil usingBlock:^(NSNotification*) { coverScreen(window, true); }];
	resignKeyObserver = [center addObserverForName:NSWindowDidResignKeyNotification object:window
		queue:nil usingBlock:^(NSNotification*) { coverScreen(window, false); }];
	//If the window is moved anyway, put it back
	removeObserver(moveObserver);
	moveObserver = [center addObserverForName:NSWindowDidMoveNotification object:window
		queue:nil usingBlock:^(NSNotification*) {
			if(coveringScreen)
				coverScreen(window, true);
		}];
}

};
