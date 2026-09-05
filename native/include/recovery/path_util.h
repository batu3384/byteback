#pragma once

#include <string>

namespace byteback {

std::string safeBasename(const std::string& name);

// Sanitized relative directory from an FS path; "" means "no subdirectory".
std::string safeRelativeDir(const std::string& fsPath);

// destDir + "/" + relDir (relDir empty -> destDir unchanged).
std::string joinDestDir(const std::string& destDir, const std::string& relDir);

// destDir/basename, appending _N before extension when the path already exists.
std::string uniqueDestPath(const std::string& destDir, const std::string& name);

// Reject destDir whose lexically-normal form contains "..".
bool destDirIsSafe(const std::string& destDir);

} // namespace byteback
