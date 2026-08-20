#pragma once

#include <string>

namespace byteback {

std::string safeBasename(const std::string& name);

// destDir/basename, appending _N before extension when the path already exists.
std::string uniqueDestPath(const std::string& destDir, const std::string& name);

// Reject destDir whose lexically-normal form contains "..".
bool destDirIsSafe(const std::string& destDir);

} // namespace byteback
