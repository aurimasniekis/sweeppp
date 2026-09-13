// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Opening a document from the Finder, on macOS.
//
// Every other desktop hands an application the path as an argument, which
// main() already handles. macOS does not: it delivers the document to the
// NSApplication delegate, and the association declared in Info.plist is worth
// nothing without one that listens.
//
// GLFW installs a delegate of its own -- GLFWApplicationDelegate -- and it
// implements no document method, so the event is answered by nobody and the
// Finder reports that the document could not be opened. That is glfw#1024, and
// the way out is to add the method to GLFW's delegate class rather than to
// replace the delegate: window closing, screen changes and termination all
// route through it, and taking it over to gain one method would break them.
//
// The timing is the other half, and it is what made a cold start behave
// differently from a warm one. A document that *starts* the application is
// dispatched during GLFW's own initialisation, before any code of ours that
// waits for a window could run -- measured here at 0.22 s, against a handler
// installed at 0.24 s. The class, though, is registered when libglfw is
// loaded, long before an instance of it exists, so the method can be added
// before the window is created and be in place when the event lands.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

static void (*g_openDocument)(const char*) = NULL;

/// Documents that arrived before there was a window to open them in.
///
/// The start-up case, every time: the event lands while the plugins are still
/// being discovered and the radio is not open yet.
static NSMutableArray<NSString*>* g_pending = nil;

static void deliverPath(NSString* path) {
    if (path == nil) {
        return;
    }
    if (g_openDocument != NULL) {
        g_openDocument([path fileSystemRepresentation]);
        return;
    }

    if (g_pending == nil) {
        g_pending = [[NSMutableArray alloc] init];
    }
    [g_pending addObject:path];
}

static BOOL openFileMethod(id self_, SEL cmd, NSApplication* application, NSString* name) {
    (void)self_;
    (void)cmd;
    (void)application;
    deliverPath(name);
    return YES;
}

static void openFilesMethod(id self_, SEL cmd, NSApplication* application,
                            NSArray<NSString*>* names) {
    (void)self_;
    (void)cmd;
    for (NSString* name in names) {
        deliverPath(name);
    }

    // Required: without a reply the launch is left waiting on an answer that
    // never comes.
    [application replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

/// Adds both document methods to a delegate class, if it does not have them.
///
/// class_addMethod leaves an existing implementation alone, so this is safe to
/// call again and safe on a class that grew one of its own.
static BOOL adoptDelegateClass(Class delegateClass) {
    if (delegateClass == Nil) {
        return NO;
    }
    class_addMethod(delegateClass, @selector(application:openFile:), (IMP)openFileMethod, "c@:@@");
    class_addMethod(delegateClass, @selector(application:openFiles:), (IMP)openFilesMethod,
                    "v@:@@");
    return YES;
}

/// Puts the document methods in place, and -- once there is somewhere to open
/// one -- registers `handler` and hands over anything that arrived meanwhile.
///
/// `handler` may be NULL, which is how this is called before the window
/// exists: what matters then is only that the methods are on the class before
/// GLFW's delegate is asked to open anything. Call it again with the real
/// callback when there is one.
///
/// Returns false when there was no delegate class to adopt, which would leave
/// a cold start with nowhere to deliver a document.
///
/// `handler` runs on the main thread from inside the event pump, so it may
/// touch anything a frame may.
bool sweepppInstallOpenDocumentHandler(void (*handler)(const char*)) {
    // By name, because the instance does not exist yet on the call that
    // matters. Also whatever delegate is actually installed by the time of a
    // later call, so a GLFW that renamed the class still works once the window
    // is up -- it would only lose a document that arrived before that.
    BOOL adopted = adoptDelegateClass(objc_getClass("GLFWApplicationDelegate"));

    id delegate = NSApp != nil ? [NSApp delegate] : nil;
    if (delegate != nil) {
        adopted = adoptDelegateClass(object_getClass(delegate)) || adopted;
    }

    if (handler == NULL) {
        return adopted ? true : false;
    }

    g_openDocument = handler;
    for (NSString* path in g_pending) {
        handler([path fileSystemRepresentation]);
    }
    [g_pending removeAllObjects];
    return adopted ? true : false;
}
