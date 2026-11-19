#pragma once

// Forward decl for Objective-C entry point. MacEntry.mm instantiates the app
// delegate and starts [NSApp run]. Everything else lives inside MacApp.mm.

#ifdef __OBJC__
@class MacAppDelegate;
#endif

int MacAppRun(int argc, const char* argv[]);
