#pragma once

#include <string>

// --parse-splice / --eval-splice (splicecheck.cpp); dirMaps: the parse maps (build/loadmaps).
int ParseSpliceCommand(const std::string& arg, const std::string& dirMaps, bool fVerbose);
int EvalSpliceCommand(const std::string& arg, const std::string& dirMaps, bool fVerbose, const std::string& pathDump);
int SpliceSelfTestCommand();
