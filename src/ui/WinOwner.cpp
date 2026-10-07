#include "WinOwner.h"
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
} // namespace td
