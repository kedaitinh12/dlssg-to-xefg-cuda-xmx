#pragma once

#include "../Log.h"

#include <string_view>

namespace mfg_port
{
	// MFG-Unlocker logs the port as JSON-ish stage/fields pairs, kept here so the copied code stays close to it
	inline void Event(std::string_view stage, std::string_view fields = {})
	{
		if (fields.empty())
			Log::Info("MfgPort {}", stage);
		else
			Log::Info("MfgPort {} {}", stage, fields);
	}
} // namespace mfg_port
