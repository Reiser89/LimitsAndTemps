#pragma once

// Single source of the app version: used by the exe's version resource (app.rc), the tray menu and --dump.
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 8
#define APP_VERSION_PATCH 0
#define APP_VERSION_STR "0.8.0"
#ifndef RC_INVOKED
#define APP_VERSION_W L"" APP_VERSION_STR
#endif
