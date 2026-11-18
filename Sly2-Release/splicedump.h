#pragma once

// The readable scripts' commands (splicedump.cpp): --load-world W --dump-splice DIR, --splice-names-check DIR.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

struct SW;

// the world's name for an object (its group's name; its class when it is not the group's model): the namer's $names.
// Not loaded here: another place's record, an object inside a record, a state machine's state ("group SM state K"),
// and (fOtherEpisodes) another episode's object
std::string SpliceObjectName(const SW* psw, int oid, bool fOtherEpisodes = true);
// The objects inside other objects' records (a group's Objects.bin: the children a top-level record carries, which the
// world only makes when an instance is placed): oid -> where it is. Found by their record headers ({k, filters, cid,
// oid, n, size}: a plausible class and a size inside the parent's record); the scripts' oids not in the world are
// named by them ("f_repair_truck ALO").
struct SPLICECHILDREC
{
    int igrp = -1;
    int cid = 0;
    int oidRoot = 0;
    int cidRoot = 0;
};
void SpliceIndexChildRecords(int igrp, const uint8_t* pb, std::size_t cb, std::map<int, SPLICECHILDREC>& mp);
// a child record's name ("group class"; empty: none)
std::string SpliceChildName(const std::map<int, SPLICECHILDREC>& mp, int oid, const std::string& groupName);
// the classes whose objects run a module's handlers (an ASEG's / SM's: their runners ASEGA / SMA too)
#include <vector>
std::vector<int> SpliceRuntimeClasses(std::vector<int> acid);
// where the world's event words come from (round 3): its clips' timed script events (ASEG event kind 12): "group clip
// at T s" for a word, empty when no clip raises it (SPLICENAMECTX::pfnEventWord)
#include <functional>
std::function<std::string(int word)> SpliceEventWordSource(const SW* psw);
// (before the world loads) the parts read by their paths in the installation, the groups' scripts texts kept
void InstallSpliceDumpReader(std::map<int, std::string>* pmptext);
// the kept texts as readable files in dir; 1 when one failed
int DumpSplice(const SW* psw, const std::map<int, std::string>& mptext, const std::string& dir);
// every Scripts.splice under the installation printed readable and read back (and the tables' self-test)
int SpliceNamesCheck(const std::string& dirInstall);
// --splice-compile FILE: a scripts file (the converter's text or a readable one) compiled: the converter's text of
// what it compiles to on stdout, its diagnostics (no world) on stderr; 1 when it does not compile
int SpliceCompileCommand(const std::string& path);
