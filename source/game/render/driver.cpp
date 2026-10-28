#include "render/driver.h"
#include "main/references.h"

namespace render {

Texture* RenderDriver::createTexture() {
	return devices.render->createEmptyTexture();
}

Texture* RenderDriver::createCubemap() {
	return devices.render->createEmptyCubemap();
}

};
