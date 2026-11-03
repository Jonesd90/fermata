#include "WinOwner.h"
#include <cwchar>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace td
{
bool makeOwnedBy (juce::Component& child, juce::Component& owner)
{
#if JUCE_WINDOWS
    auto* pc = child.getPeer();
    auto* po = owner.getPeer();
    if (pc == nullptr || po == nullptr) return false;
    HWND hc = (HWND) pc->getNativeHandle(), ho = (HWND) po->getNativeHandle();
    if (hc == nullptr || ho == nullptr) return false;
    if ((HWND) GetWindowLongPtr (hc, GWLP_HWNDPARENT) != ho)
        SetWindowLongPtr (hc, GWLP_HWNDPARENT, (LONG_PTR) ho);          // for a top-level window this sets its OWNER
    const LONG_PTR ex = GetWindowLongPtr (hc, GWL_EXSTYLE);
    if (ex & WS_EX_APPWINDOW) SetWindowLongPtr (hc, GWL_EXSTYLE, ex & ~(LONG_PTR) WS_EX_APPWINDOW);
    return true;
#else
    juce::ignoreUnused (child, owner);
    return false;
#endif
}

bool isForegroundWindow (juce::Component& c)
{
    auto* pc = c.getPeer();
    if (pc == nullptr) return false;
#if JUCE_WINDOWS
    return GetForegroundWindow() == (HWND) pc->getNativeHandle();
#else
    if (auto* tl = dynamic_cast<juce::TopLevelWindow*> (&c)) return tl->isActiveWindow();
    return pc->isFocused();
#endif
}

void keepTopmost (juce::Component& c)
{
#if JUCE_WINDOWS
    auto* pc = c.getPeer();
    if (pc == nullptr || pc->isMinimised()) return;
    if (auto hc = (HWND) pc->getNativeHandle())
        SetWindowPos (hc, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    juce::ignoreUnused (c);
#endif
}
bool startProgram (const juce::StringArray& args, juce::String& error)
{
    if (args.isEmpty()) { error = "Nothing to start."; return false; }
#if JUCE_WINDOWS
    juce::String cmd;                                                    // quoted the way CommandLineToArgvW reads it back (as juce::ChildProcess does)
    for (auto a : args)
    {
        if (a.containsAnyOf ("\" ")) a = a.replace ("\"", "\\\"").quoted();
        cmd << a << ' ';
    }
    cmd = cmd.trimEnd();
    const wchar_t* wide = cmd.toWideCharPointer();
    std::vector<wchar_t> line (wide, wide + std::wcslen (wide) + 1);     // (CreateProcessW may write into it; wcslen, not length(): a character outside the BMP takes two wchar_ts - 3.27.1)
    STARTUPINFOW si {}; si.cb = sizeof (si);
    PROCESS_INFORMATION pi {};
    if (! CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))      // FALSE: no handles handed on
    {
        error = "Windows could not start " + juce::File (args[0]).getFileName() + " (error " + juce::String ((int) GetLastError()) + ").";
        return false;
    }
    CloseHandle (pi.hThread); CloseHandle (pi.hProcess);
    return true;
#else
    juce::ChildProcess p;
    if (! p.start (args, 0)) { error = "Could not start " + args[0] + "."; return false; }
    return true;                                                         // (the program goes on running when 'p' is let go of)
#endif
}
} // namespace td
