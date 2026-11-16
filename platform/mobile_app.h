#pragma once
// The phone / tablet apps' bridge: Android (android/android_app.h) or iOS (ios/ios_app.h). Both offer the same calls
// for what the shared code needs (programs run in the app, the document picker, the touch pad, haptics); code that
// does the same on both uses cane::mobile and CANE_MOBILE.
#if defined(__ANDROID__) || defined(CANE_ANDROID)
#include "android/android_app.h"
#ifndef CANE_MOBILE
#define CANE_MOBILE 1
#endif
namespace cane { namespace mobile = android; }
#elif defined(CANE_IOS)
#include "ios/ios_app.h"
#ifndef CANE_MOBILE
#define CANE_MOBILE 1
#endif
namespace cane { namespace mobile = ios; }
#endif
