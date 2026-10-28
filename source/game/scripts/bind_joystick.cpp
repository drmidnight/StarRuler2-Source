#include "binds.h"
#include "main/references.h"
#include "os/driver.h"

#define JOY_AXIS_MAX 16
#define JOY_BUTTON_MAX 64
//Sticks rest slightly off centre, so small values count as 0
#define STICK_DEADZONE 0.1f

namespace scripts {

enum JoystickButtonState {
	JBS_Off,
	JBS_On,
	JBS_Released,
	JBS_Pressed
};

class Joystick {
public:
	//Gamepad index for the driver, counting from 0
	int index;

	unsigned axisCount;
	float axes[JOY_AXIS_MAX];

	unsigned buttonCount;
	unsigned char buttons[JOY_BUTTON_MAX];

	Joystick(unsigned Index) : index((int)Index - 1), axisCount(0), axes(), buttonCount(0), buttons() {
		poll();
	}

	static float deadZone(float value) {
		if(value >= -STICK_DEADZONE && value <= STICK_DEADZONE)
			return 0.f;
		if(value < 0)
			return -((-value - STICK_DEADZONE) / (1.f - STICK_DEADZONE));
		return (value - STICK_DEADZONE) / (1.f - STICK_DEADZONE);
	}

	bool poll() {
		os::GamepadState state;
		if(index < 0 || !devices.driver->getGamepadState((unsigned)index, state)) {
			axisCount = 0;
			buttonCount = 0;
			return false;
		}

		buttonCount = os::GP_ButtonCount;
		for(unsigned i = 0; i < os::GP_ButtonCount; ++i) {
			auto& button = buttons[i];
			if(state.buttons[i]) {
				if(button == JBS_Pressed)
					button = JBS_On;
				else if(button != JBS_On)
					button = JBS_Pressed;
			}
			else {
				if(button == JBS_Released)
					button = JBS_Off;
				else if(button != JBS_Off)
					button = JBS_Released;
			}
		}

		axisCount = os::GP_AxisCount;
		axes[os::GP_AxisLX] = deadZone(state.axes[os::GP_AxisLX]);
		axes[os::GP_AxisLY] = deadZone(state.axes[os::GP_AxisLY]);
		axes[os::GP_AxisRX] = deadZone(state.axes[os::GP_AxisRX]);
		axes[os::GP_AxisRY] = deadZone(state.axes[os::GP_AxisRY]);
		axes[os::GP_AxisLT] = state.axes[os::GP_AxisLT];
		axes[os::GP_AxisRT] = state.axes[os::GP_AxisRT];
		return true;
	}

	float getAxis(unsigned axis) const {
		if(axis < JOY_AXIS_MAX)
			return axes[axis];
		else
			return 0;
	}

	unsigned char getButton(unsigned id) const {
		if(id < JOY_BUTTON_MAX)
			return buttons[id];
		else
			return JBS_Off;
	}

	bool getPressed(unsigned id) const {
		auto state = getButton(id);
		return state == JBS_On || state == JBS_Pressed;
	}

	bool connected() const {
		os::GamepadState state;
		return index >= 0 && devices.driver->getGamepadState((unsigned)index, state);
	}

	//GLFW has no rumble support
	void setVibration(float lowFreq, float hiFreq) {
	}

	static void construct(void* memory, unsigned index) {
		new(memory) Joystick(index);
	}
};

void RegisterJoystickBinds() {
	ClassBind joystick("Joystick", asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_C, sizeof(Joystick));

	joystick.addConstructor("void f(uint)", asFUNCTION(Joystick::construct));

	joystick.addMethod("bool connected() const", asMETHOD(Joystick,connected));
	joystick.addMethod("bool poll()", asMETHOD(Joystick,poll));
	joystick.addMethod("float get_axis(uint index) const", asMETHOD(Joystick,getAxis));
	joystick.addMethod("uint8 get_button(uint index) const", asMETHOD(Joystick,getButton));
	joystick.addMethod("bool get_pressed(uint index) const", asMETHOD(Joystick,getPressed));
	joystick.addMethod("void setVibration(float low, float high) const", asMETHOD(Joystick,setVibration));

	joystick.addMember("uint axisCount", offsetof(Joystick,axisCount));
	joystick.addMember("uint buttonCount", offsetof(Joystick,buttonCount));

	EnumBind buttonStates("JoystickButtonState");
	buttonStates["JBS_On"] = JBS_On;
	buttonStates["JBS_Off"] = JBS_Off;
	buttonStates["JBS_Pressed"] = JBS_Pressed;
	buttonStates["JBS_Released"] = JBS_Released;

	EnumBind gamepad("GamepadKeys");
	gamepad["GP_A"] = os::GP_A;
	gamepad["GP_B"] = os::GP_B;
	gamepad["GP_X"] = os::GP_X;
	gamepad["GP_Y"] = os::GP_Y;
	gamepad["GP_UP"] = os::GP_Up;
	gamepad["GP_LEFT"] = os::GP_Left;
	gamepad["GP_DOWN"] = os::GP_Down;
	gamepad["GP_RIGHT"] = os::GP_Right;
	gamepad["GP_LB"] = os::GP_LB;
	gamepad["GP_LT"] = os::GP_LT;
	gamepad["GP_L3"] = os::GP_L3;
	gamepad["GP_RB"] = os::GP_RB;
	gamepad["GP_RT"] = os::GP_RT;
	gamepad["GP_R3"] = os::GP_R3;
	gamepad["GP_BACK"] = os::GP_Back;
	gamepad["GP_START"] = os::GP_Start;

	gamepad["GP_AXIS_LX"] = os::GP_AxisLX;
	gamepad["GP_AXIS_LY"] = os::GP_AxisLY;
	gamepad["GP_AXIS_RX"] = os::GP_AxisRX;
	gamepad["GP_AXIS_RY"] = os::GP_AxisRY;
	gamepad["GP_AXIS_LT"] = os::GP_AxisLT;
	gamepad["GP_AXIS_RT"] = os::GP_AxisRT;
}

};
