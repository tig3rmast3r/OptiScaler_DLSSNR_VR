#pragma once
// Compile the production provenance observer, with no application Config/PCH.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#define LOG_WARN(...) static_cast<void>(0)
#define LOG_INFO(...) static_cast<void>(0)
