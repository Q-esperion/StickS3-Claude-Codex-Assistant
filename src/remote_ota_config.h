#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.1.1"
#endif

#ifndef REMOTE_OTA_MANIFEST_URL
#define REMOTE_OTA_MANIFEST_URL ""
#endif
