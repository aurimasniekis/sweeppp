// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// The macOS half of the snapshot clipboard.
//
// Objective-C rather than Objective-C++, and a C entry point rather than a
// class: the project enables OBJC alongside C and C++ for the file dialog's
// sake, and adding OBJCXX to carry three statements would be a language
// enabled for one file. Nothing here needs C++.

#import <AppKit/AppKit.h>
#include <stdbool.h>
#include <stddef.h>

bool sweepppCopyPngToPasteboard(const unsigned char* png, unsigned long bytes) {
    if (png == NULL || bytes == 0) {
        return false;
    }

    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:png length:(NSUInteger)bytes];

        // PNG, not TIFF. Every macOS application that accepts a pasted image
        // reads PNG, it is what the buffer already is, and writing the bytes
        // straight through avoids a decode and a re-encode that could only
        // lose something.
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        return [pasteboard setData:data forType:NSPasteboardTypePNG];
    }
}
