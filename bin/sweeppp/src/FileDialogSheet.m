// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The macOS save and open dialogs as sheets on the main window.
//
// NFD runs the panel as a window of its own, which with torn-off panels is
// free to open anywhere. A sheet hangs from the window it belongs to, which
// is where a Mac user looks for it. Run modally all the same, so the caller
// gets an answer back the way NFD gives one.

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdlib.h>
#include <string.h>

/// 1 for a chosen path, written to `*out` and freed with `free`; 0 for cancel.
static int runSheet(NSSavePanel* panel, NSWindow* window, char** out) {
    __block NSModalResponse response = NSModalResponseCancel;
    [panel beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse result) {
                    response = result;
                    [NSApp stopModal];
                  }];
    [NSApp runModalForWindow:panel];
    [window makeKeyAndOrderFront:nil];

    if (response != NSModalResponseOK || panel.URL == nil) {
        return 0;
    }
    const char* path = panel.URL.path.UTF8String;
    *out = path != NULL ? strdup(path) : NULL;
    return *out != NULL ? 1 : 0;
}

static void prepare(NSSavePanel* panel, const char* directory, const char* extension) {
    if (directory != NULL && directory[0] != '\0') {
        panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:directory]
                                        isDirectory:YES];
    }
    if (extension != NULL && extension[0] != '\0') {
        UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:extension]];
        if (type != nil) {
            panel.allowedContentTypes = @[ type ];
        }
    }
}

int sweepppSaveSheet(void* window, const char* directory, const char* name, const char* extension,
                     char** out) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        prepare(panel, directory, extension);
        if (name != NULL) {
            panel.nameFieldStringValue = [NSString stringWithUTF8String:name];
        }
        return runSheet(panel, (__bridge NSWindow*)window, out);
    }
}

int sweepppOpenSheet(void* window, const char* directory, const char* extension, char** out) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        prepare(panel, directory, extension);
        return runSheet(panel, (__bridge NSWindow*)window, out);
    }
}
