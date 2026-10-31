#pragma once

#if defined(_WIN32)
#define OPENMEDIA_DLL_EXPORT __declspec(dllexport)
#define OPENMEDIA_DLL_IMPORT __declspec(dllimport)
#else
#define OPENMEDIA_DLL_EXPORT __attribute__((visibility("default")))
#define OPENMEDIA_DLL_IMPORT __attribute__((visibility("default")))
#endif

#if defined(OPENMEDIA_DYNAMIC_IMPORT)
#define OPENMEDIA_ABI OPENMEDIA_DLL_IMPORT
#elif defined(OPENMEDIA_DYNAMIC)
#define OPENMEDIA_ABI OPENMEDIA_DLL_EXPORT
#else
#define OPENMEDIA_ABI
#endif

#define OM_ENUM(name, type) \
  typedef type name;        \
  enum name##_
