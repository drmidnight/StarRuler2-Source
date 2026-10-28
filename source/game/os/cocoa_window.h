#pragma once

struct GLFWwindow;

//All access to the native macOS window goes through here
namespace os {

//The window's NSWindow
void* getNativeWindow(GLFWwindow* window);

//Starts tracking trackpad input that GLFW does not report: whether scrolling has
//precise deltas, and pinch and rotate steps, which go to onGesture
void attachCocoaWindow(GLFWwindow* window, void (*onGesture)(int type, double x, double y));
void detachCocoaWindow();

//Whether the last scroll event came from a trackpad or other device with precise deltas
bool lastScrollWasPrecise();

//Covers the window's screen with the window, above the menu bar and dock, without
//changing the display mode. The window steps back while another application is in front.
void enterBorderlessFullscreen(GLFWwindow* window);
void leaveBorderlessFullscreen();

};
