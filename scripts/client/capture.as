//Steps through fixed views of a loaded save for visual parity captures, when the
//game was started with --capture (see source/game/main/capture.h). Each view is
//captured once everything has loaded and the view has had time to settle.
import tabs.Tab;
import Tab@ newTab(Tab@ tab) from "tabs.tabbar";
import void switchToTab(Tab@ tab) from "tabs.tabbar";
import void closeTab(Tab@ tab) from "tabs.tabbar";
import Tab@ createGalaxyTab(Object@ focusOn) from "tabs.GalaxyTab";
import Tab@ createGalaxyTab(vec3d pos) from "tabs.GalaxyTab";
import void zoomTabBy(Tab@ tab, double factor) from "tabs.GalaxyTab";
import void openOverlay(Tab@ tab, Object@ obj) from "tabs.GalaxyTab";
import Tab@ createDesignEditorTab(const Design@ dsg) from "tabs.DesignEditorTab";
import Tab@ createPlanetsTab() from "tabs.PlanetsTab";

//Frames a view gets after everything has loaded, for cameras and fades to settle
const uint SETTLE_FRAMES = 90;

const array<string> VIEWS = {"galaxy", "system", "planet", "fleet", "design", "planets"};

uint view = 0;
uint settled = 0;
bool viewOpen = false;
Tab@ viewTab;

//The player's biggest flagship, which shows the most ship detail
Ship@ findFlagship() {
	Ship@ best;
	for(uint i = 0, cnt = playerEmpire.objectCount; i < cnt; ++i) {
		Ship@ ship = cast<Ship>(playerEmpire.objects[i]);
		if(ship is null || !ship.hasLeaderAI)
			continue;
		if(best is null || ship.blueprint.design.size > best.blueprint.design.size)
			@best = ship;
	}
	return best;
}

//Opens the tab for a view, or returns null when the save has nothing to show for it
Tab@ openView(const string& name) {
	Object@ home = playerEmpire.Homeworld;
	Ship@ flagship = findFlagship();

	if(name == "galaxy") {
		Tab@ tab = createGalaxyTab(vec3d());
		newTab(tab);
		zoomTabBy(tab, 50.0);
		return tab;
	}
	if(name == "system" && home !is null)
		return newTab(createGalaxyTab(home));
	if(name == "planet" && home !is null) {
		Tab@ tab = newTab(createGalaxyTab(home));
		zoomTabBy(tab, 0.3);
		openOverlay(tab, home);
		return tab;
	}
	if(name == "fleet" && flagship !is null) {
		Tab@ tab = newTab(createGalaxyTab(flagship));
		zoomTabBy(tab, 0.3);
		return tab;
	}
	if(name == "design" && flagship !is null)
		return newTab(createDesignEditorTab(flagship.blueprint.design));
	if(name == "planets")
		return newTab(createPlanetsTab());
	return null;
}

void tick(double time) {
	if(!isCapturing || view >= VIEWS.length)
		return;

	if(!viewOpen) {
		if(viewTab !is null)
			closeTab(viewTab);
		@viewTab = openView(VIEWS[view]);
		if(viewTab is null) {
			print("Capture: nothing to show for " + VIEWS[view]);
			++view;
			if(view >= VIEWS.length)
				finishCapture();
			return;
		}
		switchToTab(viewTab);
		viewOpen = true;
		settled = 0;
		return;
	}

	if(!resourcesLoaded) {
		settled = 0;
		return;
	}

	++settled;
	if(settled == SETTLE_FRAMES) {
		captureFrame(VIEWS[view]);
	}
	else if(settled > SETTLE_FRAMES) {
		//The capture was written at the end of the previous frame
		viewOpen = false;
		++view;
		if(view >= VIEWS.length)
			finishCapture();
	}
}
