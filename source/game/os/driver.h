#pragma once
#include "os/key_consts.h"
#include <functional>
#include <string>
#include <list>
#include <vector>

namespace os {

enum WindowMode {
	WM_Fullscreen,
	WM_Window,
};

enum KeyAction {
	KA_Pressed  =   1,
	KA_Released =   2,
	KA_Repeated = 1+4,
};

//Buttons in the layout of an Xbox controller
enum GamepadButton {
	GP_A, GP_B, GP_X, GP_Y,
	GP_Up, GP_Left, GP_Down, GP_Right,
	GP_LB, GP_LT, GP_L3,
	GP_RB, GP_RT, GP_R3,
	GP_Back, GP_Start,
	GP_ButtonCount
};

enum GamepadAxis {
	GP_AxisLX, GP_AxisLY,
	GP_AxisRX, GP_AxisRY,
	GP_AxisLT, GP_AxisRT,
	GP_AxisCount
};

struct GamepadState {
	bool buttons[GP_ButtonCount];
	//Sticks from -1 to 1, positive right and up. Triggers from 0 to 1.
	float axes[GP_AxisCount];
};

//Direct manipulation input, from a trackpad and later touch
enum GestureType {
	//x: change in scale, positive when the fingers spread apart
	GT_Pinch,
	//x: degrees turned, positive counter-clockwise
	GT_Rotate,
	//x, y: movement in points, positive right and down
	GT_Pan,
};

struct Gesture {
	GestureType type;
	double x, y;
};

struct WindowData {
	int width, height;
	int redbits, greenbits, bluebits, alphabits;
	int depthbits, stencilbits;
	int aa_samples;
	int refreshRate;
	bool resizable;
	int verticalSync;
	WindowMode mode;
	std::string targetMonitor;
	bool overrideMonitor;
	//Whether the window gets an OpenGL context; other renderers draw into it themselves
	bool openGL;

	WindowData()
	 : width(1024), height(768),
	 redbits(8), greenbits(8), bluebits(8),
	 alphabits(8), depthbits(8), stencilbits(8),
	 aa_samples(4), refreshRate(0), resizable(false),
	 verticalSync(1), mode(WM_Window), overrideMonitor(false), openGL(true) {}
};

enum OSCallback {
	OSC_WindowResize,
	OSC_WindowClose,
	OSC_Key,
	OSC_CharKey,
	OSC_MouseMove,
	OSC_MouseButton,
	OSC_MouseWheel,

	OSC_COUNT
};

template<class Arg0,class Arg1>
struct DriverCallbacks_2 {
	typedef std::function<bool(Arg0,Arg1)> cb;
	std::list<cb> callbacks;

	//Adds the callback to the list, earlier if priority
	void add(cb func, bool priority = false) {
		if(priority)
			callbacks.push_front(func);
		else
			callbacks.push_back(func);
	}

	//Removes a callback previously added to the list
	/*void remove(cb func) {
		callbacks.remove_if([func](cb f) {
			return func.template target<bool(Arg0,Arg1)>() == f.template target<bool(Arg0,Arg1)>();
		});
	}*/

	//Calls each callback in order, stopping if true is returned
	void operator()(Arg0 arg0, Arg1 arg1) {
		for(auto i = callbacks.begin(), end = callbacks.end(); i != end; ++i)
			if((*i)(arg0,arg1))
				break;
	}
};

template<class Arg0>
struct DriverCallbacks_1 {
	typedef std::function<bool(Arg0)> cb;
	std::list<cb> callbacks;

	//Adds the callback to the list, earlier if priority
	void add(cb func, bool priority = false) {
		if(priority)
			callbacks.push_front(func);
		else
			callbacks.push_back(func);
	}

	//Removes a callback previously added to the list
	/*void remove(cb func) {
		callbacks.remove_if([func](cb f) {
			return func.template target<bool(Arg0)>() == f.template target<bool(Arg0)>();
		});
	}*/
	//Calls each callback in order, stopping if true is returned
	void operator()(Arg0 arg0) {
		for(auto i = callbacks.begin(), end = callbacks.end(); i != end; ++i)
			if((*i)(arg0))
				break;
	}
};

struct DriverCallbacks {
	typedef std::function<bool()> cb;
	std::list<cb> callbacks;

	//Adds the callback to the list, earlier if priority
	void add(cb func, bool priority = false) {
		if(priority)
			callbacks.push_front(func);
		else
			callbacks.push_back(func);
	}

	//Removes a callback previously added to the list
	/*void remove(cb func) {
		callbacks.remove_if([func](cb f) {
			return func.template target<bool()>() == f.template target<bool()>();
		});
	}*/

	//Calls each callback in order, stopping if true is returned
	void operator()() {
		for(auto i = callbacks.begin(), end = callbacks.end(); i != end; ++i)
			if((*i)())
				break;
	}
};
	
class OSDriver {
public:
	struct VideoMode {
		unsigned width;
		unsigned height;
		unsigned refresh;
	};

	DriverCallbacks onWindowClose;
	DriverCallbacks_1<unsigned> onCharTyped;
	DriverCallbacks_2<double,double> onScroll;
	DriverCallbacks_1<Gesture> onGesture;
	DriverCallbacks_2<int,int> onResize, onMouseButton, onMouseMoved, onKeyEvent;

	bool shiftKey, altKey, ctrlKey;
	//Command on macOS
	bool superKey;
	//Window size in points, which the UI and mouse use
	int win_width, win_height;
	//Size of the window's framebuffer in pixels, which rendering uses
	int fb_width, fb_height;
	//Framebuffer pixels per window point, 2 on Retina screens
	double pixelDensity() const {
		return win_width > 0 && fb_width > 0 ? (double)fb_width / (double)win_width : 1.0;
	}

	int mouse_x, mouse_y;
	int leftButton, rightButton, middleButton;
	//Whether the last scroll event came from a trackpad or other device with precise deltas
	bool preciseScroll;

	virtual bool systemRandom(unsigned char* buffer, unsigned bytes) = 0;

	virtual void setVerticalSync(int waitFrames) = 0;
	//The native window (an NSWindow on macOS), for renderers that attach to it
	virtual void* getNativeWindow() { return nullptr; }

	virtual void swapBuffers(double minWait_s = 0) = 0;
	//NOTE: If you need to keep the program awake but can't render, use this instead of swapBuffers
	//		Skips the elapsed time with minimal impact on timers
	virtual void handleEvents(unsigned minWait_ms = 100) = 0;

	virtual void createWindow(WindowData& data) = 0;
	virtual void setWindowTitle(const char* str) = 0;
	virtual void setWindowSize(int width, int height) = 0;
	virtual void closeWindow() = 0;
	virtual bool isWindowFocused() = 0;
	virtual bool isWindowMinimized() = 0;
	virtual bool isMouseOver() = 0;
	virtual void flashWindow() = 0;
	virtual bool isFullscreen() = 0;
	//Switches the open window between fullscreen at the desktop resolution, and a
	//window of the given size
	virtual void setFullscreen(bool fullscreen, int windowedWidth, int windowedHeight) = 0;
	virtual void getVideoModes(std::vector<VideoMode>& output) = 0;
	virtual void getMonitorNames(std::vector<std::string>& output) = 0;

	virtual void getDesktopSize(unsigned& width, unsigned& height) = 0;

	virtual void resetTimer() = 0;
	//Returns time since last reset in ms
	virtual int getTime() const = 0;
	//Returns time since last reset in seconds
	virtual double getAccurateTime() const = 0;

	//Returns the current system time for the frame
	virtual double getFrameTime() const = 0;
	
	//Resets the game timer (implied by resetTimer())
	virtual void resetGameTime(double time) = 0;
	//Returns the current game time
	virtual double getGameTime() const = 0;
	//Sets the game speed as a factor of real time
	virtual void setGameSpeed(double factor) = 0;
	//Returns the current game speed
	virtual double getGameSpeed() const = 0;

	virtual unsigned getProcessorCount() = 0;

	virtual void sleep(int milliseconds) = 0;

	virtual unsigned getDoubleClickTime() const = 0;
	virtual void getMousePos(int& x, int& y) = 0;
	virtual void getLastMousePos(int& x, int& y) = 0;
	virtual void setMousePos(int x, int y) = 0;
	virtual void setCursorVisible(bool visible) = 0;
	virtual void setCursorLocked(bool locked) = 0;
	//Sets whether the cursor should currently lock if set to
	virtual void setCursorShouldLock(bool locked) = 0;

	virtual void setClipboard(const std::string& text) = 0;
	virtual std::string getClipboard() = 0;

	//Returns false when no gamepad is connected at index, counting from 0
	virtual bool getGamepadState(unsigned index, GamepadState& state) = 0;
	virtual int getCharForKey(int key) = 0;
	virtual int getKeyForChar(unsigned char chr) = 0;

	OSDriver() : shiftKey(false), altKey(false), ctrlKey(false), superKey(false),
			win_width(0), win_height(0), fb_width(0), fb_height(0), mouse_x(0), mouse_y(0),
			leftButton(false), rightButton(false), middleButton(false), preciseScroll(false) {}
	virtual ~OSDriver() {}
};

};
