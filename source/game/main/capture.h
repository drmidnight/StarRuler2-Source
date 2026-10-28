#pragma once
#include <string>

//Visual parity captures: --capture <folder> renders fixed views to PNG files in
//<folder>, then quits. Without --load it captures the main menu; with it, the
//client script scripts/client/capture.as walks through views of the save.
//The game stays paused throughout, so animation driven by game time holds still.
namespace capture {

//The folder captures go to, empty when not capturing
extern std::string folder;

inline bool active() {
	return !folder.empty();
}

//Whether streaming textures, meshes and images have all finished loading
bool resourcesLoaded();

//Writes the next rendered frame to <name>.png
void request(const std::string& name);

//Called after a frame renders and before it is presented
void afterRender();

//Called once per menu frame, to capture the main menu
void tickMenu();

//Ends the capture run and quits
void finish();

};
