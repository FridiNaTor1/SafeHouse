#pragma once

// Development commands of the animation runtime (animtest.cpp).
int AnimTestCommand(const char* pszWorld, int argc, char** argv);
int AnimRefCommand(const char* pszWorld, const char* pszA, const char* pszB, int argc, char** argv);  // animref.cpp
int AnimPoseCommand(const char* pszWorld, const char* pszRam, int argc, char** argv);  // animref.cpp
int AnimRunCommand(const char* pszWorld, const char* pszA, const char* pszB, int argc, char** argv);  // animref.cpp
