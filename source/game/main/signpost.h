#pragma once

//Marks the rest of the enclosing scope as a named interval in the Points of
//Interest track of Instruments on macOS:
//  SIGNPOST_INTERVAL("Render");
//  SIGNPOST_INTERVAL("Script tick", "%{public}s", name);
//The name and format must be string literals. Elsewhere the macro does nothing.
//While Instruments is not recording, each interval costs a couple of checks.

#ifdef __APPLE__
#include <os/signpost.h>

namespace signpost {
	inline os_log_t log() {
		static os_log_t handle = os_log_create("org.starruler2.opensource", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
		return handle;
	}

	template<class F>
	struct ScopeEnd {
		F end;
		~ScopeEnd() { end(); }
	};
	template<class F>
	ScopeEnd(F) -> ScopeEnd<F>;
};

#define SIGNPOST_CONCAT2(a, b) a##b
#define SIGNPOST_CONCAT(a, b) SIGNPOST_CONCAT2(a, b)
#define SIGNPOST_INTERVAL(name, ...) \
	const os_signpost_id_t SIGNPOST_CONCAT(_signpostId, __LINE__) = os_signpost_id_generate(signpost::log()); \
	os_signpost_interval_begin(signpost::log(), SIGNPOST_CONCAT(_signpostId, __LINE__), name __VA_OPT__(,) __VA_ARGS__); \
	signpost::ScopeEnd SIGNPOST_CONCAT(_signpostEnd, __LINE__){[id = SIGNPOST_CONCAT(_signpostId, __LINE__)] { \
		os_signpost_interval_end(signpost::log(), id, name); \
	}}
#else
#define SIGNPOST_INTERVAL(name, ...)
#endif
