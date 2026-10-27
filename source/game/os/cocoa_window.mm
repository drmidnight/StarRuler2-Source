#include "os/cocoa_window.h"
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

namespace os {

static NSWindow* nativeWindow = nil;
static id scrollMonitor = nil, magnifyMonitor = nil;
static id becomeKeyObserver = nil, resignKeyObserver = nil;
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
	if(nativeWindow)
		[NSApp setPresentationOptions:NSApplicationPresentationDefault];
	nativeWindow = nil;
}

bool lastScrollWasPrecise() {
	return preciseScroll;
}

static void coverScreen(NSWindow* window, bool inFront) {
	if(inFront) {
		[NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
		[window setLevel:NSMainMenuWindowLevel + 1];
	}
	else {
		[NSApp setPresentationOptions:NSApplicationPresentationDefault];
		[window setLevel:NSNormalWindowLevel];
	}
}

void enterBorderlessFullscreen(GLFWwindow* glfwWindow) {
	NSWindow* window = glfwGetCocoaWindow(glfwWindow);
	NSScreen* screen = [window screen] ? [window screen] : [NSScreen mainScreen];

	coverScreen(window, true);
	[window setFrame:[screen frame] display:YES];

	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	removeObserver(becomeKeyObserver);
	removeObserver(resignKeyObserver);
	becomeKeyObserver = [center addObserverForName:NSWindowDidBecomeKeyNotification object:window
		queue:nil usingBlock:^(NSNotification*) { coverScreen(window, true); }];
	resignKeyObserver = [center addObserverForName:NSWindowDidResignKeyNotification object:window
		queue:nil usingBlock:^(NSNotification*) { coverScreen(window, false); }];
}

};
