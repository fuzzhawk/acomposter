// acomposter entry point.

#include "app/Application.h"
#include "core/Utf.h"
#include "platform/FileDialog.h"

#include <windows.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // Apartment threaded, because the shell dialogs and a good number of plugin
    // editors expect an STA on the thread that owns the windows.
    const HRESULT comResult = ::CoInitializeEx(nullptr,
                                               COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool ownsCom = SUCCEEDED(comResult);

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

    if (ownsCom) ::CoUninitialize();
    return exitCode;
}
