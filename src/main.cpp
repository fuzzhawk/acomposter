// acomposter entry point.

#include "app/Application.h"
#include "core/Utf.h"
#include "platform/FileDialog.h"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Apartment threaded, because the shell dialogs and a good number of plugin
    // editors expect an STA on the thread that owns the windows.
    const HRESULT comResult = ::CoInitializeEx(nullptr,
                                               COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool ownsCom = SUCCEEDED(comResult);

    // OLE on top of it, once, for the life of the process: DoDragDrop needs it
    // and dragging a file out is a thing that can happen at any moment, so
    // initialising it per drag would only tear the layer down again between
    // drags. It sits on the same apartment, so the flags above still apply.
    const bool ownsOle = SUCCEEDED(::OleInitialize(nullptr));

    int exitCode = 0;
    {
        acm::app::Application application;

        if (!application.initialise()) {
            acm::platform::messageDialog(nullptr, "acomposter could not start",
                                         application.errorText(), true);
            exitCode = 1;
        } else {
            exitCode = application.run();
        }
        // The Application's destructor shuts everything down in order; it must
        // run before COM is torn down, hence the scope.
    }

    if (ownsOle) ::OleUninitialize();
    if (ownsCom) ::CoUninitialize();
    return exitCode;
}
