#include "main/capture.h"
#include "main/references.h"
#include "main/logging.h"
#include "main/tick.h"
#include "render/driver.h"
#include "image.h"
#include "files.h"

namespace capture {

std::string folder;
static std::string pending;
static unsigned menuFrames = 0;

bool resourcesLoaded() {
	return !devices.library.hasQueuedTextures() && !devices.library.hasQueuedMeshes()
		&& !devices.library.hasQueuedImages();
}

void request(const std::string& name) {
	pending = name;
}

void afterRender() {
	if(!active())
		return;

	//Capture keeps the game paused, so views hold still between builds
	if(game_running && devices.driver->getGameSpeed() != 0.0)
		devices.driver->setGameSpeed(0.0);

	if(pending.empty())
		return;

	Image* screen = devices.render->getScreen(0, 0, devices.driver->fb_width, devices.driver->fb_height);
	std::string filename = path_join(folder, pending + ".png");
	if(saveImage(screen, filename.c_str(), true))
		print("Captured %s", filename.c_str());
	else
		error("Could not write capture %s", filename.c_str());
	delete screen;
	pending.clear();
}

void tickMenu() {
	if(!active())
		return;

	//Give the menu time to load its images and settle its animations
	if(!resourcesLoaded()) {
		menuFrames = 0;
		return;
	}
	++menuFrames;
	if(menuFrames == 120)
		request("menu");
	else if(menuFrames == 122)
		finish();
}

void finish() {
	print("Capture finished");
	game_state = GS_Quit;
}

};
