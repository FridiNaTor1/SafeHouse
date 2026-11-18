#pragma once

// The readable scripts' commands (splicedump.cpp): --load-world W --dump-splice DIR, --splice-names-check DIR.

#include <map>
#include <string>

struct SW;

// the world's name for an object (its group's name; its class when it is not the group's model): the namer's $names
std::string SpliceObjectName(const SW* psw, int oid);
// (before the world loads) the parts read by their paths in the installation, the groups' scripts texts kept
void InstallSpliceDumpReader(std::map<int, std::string>* pmptext);
// the kept texts as readable files in dir; 1 when one failed
int DumpSplice(const SW* psw, const std::map<int, std::string>& mptext, const std::string& dir);
// every Scripts.splice under the installation printed readable and read back (and the tables' self-test)
int SpliceNamesCheck(const std::string& dirInstall);
