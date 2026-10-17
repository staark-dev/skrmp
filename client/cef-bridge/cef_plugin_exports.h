#pragma once

// samp-cef client plugin entry points.
// samp-cef discovers and calls these C exports in loaded interface DLLs.
// See cef-interface example at commit f14fe4fa for the contract.

extern "C" {
    // Called by samp-cef with its API vtable after CEF is initialized.
    __declspec(dllexport) void cef_initialize(void* api);

    // Called every SA:MP main loop tick.
    __declspec(dllexport) void cef_samp_mainloop();

    // Called when samp-cef is shutting down.
    __declspec(dllexport) void cef_quit();
}
