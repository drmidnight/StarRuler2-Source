#include "lighting.h"
#include "render/driver.h"
#include "main/references.h"
#include <set>

namespace render {
namespace light {

PointLight::PointLight() : att_constant(1), att_linear(0), att_quadratic(0) {
}

float PointLight::distanceFrom(const vec3f& pos) {
	return (float)pos.distanceToSQ(position);
}

void setPosition(const vec3f& pos);
void setPosition(const vec3d& pos);

void PointLight::enable(unsigned& lightIndex, const vec3f& offset) {
	LightDesc light = {{position.x + offset.x, position.y + offset.y, position.z + offset.z, 1.f},
		diffuse, specular, att_quadratic};
	devices.render->setLight(lightIndex, &light);
	++lightIndex;
}

vec3f PointLight::getPosition() const {
	return position;
}

float PointLight::getRadius() const {
	return radius;
}


NodePointLight::NodePointLight(scene::Node* follow) : followNode(follow) {
	follow->grab();
}

NodePointLight::~NodePointLight() {
	if(followNode)
		followNode->drop();
	followNode = nullptr;
}

vec3f NodePointLight::getPosition() const {
	if(!followNode)
		return position;
	return vec3f(followNode->abs_position);
}

float NodePointLight::getRadius() const {
	if(!followNode)
		return radius;
	return followNode->abs_scale;
}

//TODO: This is probably out of sync by one frame
void NodePointLight::enable(unsigned& lightIndex, const vec3f& offset) {
	if(!followNode) {
		lightIndex += 1;
		return;
	}
	if(followNode->queuedDelete || !followNode->parent) {
		followNode->drop();
		followNode = nullptr;
		lightIndex += 1;
		return;
	}
	const auto& v = followNode->abs_position;
	position.x = (float)v.x;
	position.y = (float)v.y;
	position.z = (float)v.z;
	PointLight::enable(lightIndex, offset);
}

std::set<LightSource*> lights;

void registerLight(LightSource* light) {
	lights.insert(light);
}

void unregisterLight(LightSource* light) {
	lights.erase(light);
}

void destroyLights() {
	for(auto it = lights.begin(); it != lights.end(); ++it)
		delete *it;
	lights.clear();
}

void resetLights() {
	for(unsigned i = 1; i < MAX_LIGHTS; ++i)
		devices.render->setLight(i, nullptr);

	LightDesc light = {{0.f, 0.f, 0.f, 1.f}, Colorf(), Colorf(), 0.f};
	devices.render->setLight(0, &light);
}

//Finds up to <maximum> nearest lights to <from>
unsigned findNearestLights(const vec3f& from, LightSource** pLights, unsigned maximum) {
	unsigned found = 0;
	float distances[MAX_LIGHTS];
	if(maximum > MAX_LIGHTS)
		maximum = MAX_LIGHTS;

	for(auto i = lights.begin(), end = lights.end(); i != end; ++i) {
		float dist = (*i)->distanceFrom(from);

		for(unsigned d = 0; d < found; ++d) {
			if(distances[d] > dist) {
				for(unsigned x = found-1; x > d; --x) {
					distances[x] = distances[x-1];
					pLights[x] = pLights[x-1];
				}
				distances[d] = dist;
				pLights[d] = *i;
				goto nextLight;
			}
		}

		if(found < maximum) {
			pLights[found] = *i;
			distances[found] = dist;
			++found;
		}

		nextLight:;
	}

	return found;
}

};
};
