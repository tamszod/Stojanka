# Third-party code

Copied into the repository (not installed by vcpkg) because the installer must stay a single
file with no DLLs next to it.

| Folder | What | Version | License |
|---|---|---|---|
| `webview/` | [webview](https://github.com/webview/webview), the all-in-one header `core/include/webview/webview.h`. Shows the installer's HTML interface with the system browser engine (Edge WebView2 on Windows, WebKit on macOS) and has its own WebView2 loader. | 0.12.0 | MIT |
| `webview2/` | `WebView2.h` from Microsoft's [Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2) NuGet package (`build/native/include`). Windows only. | 1.0.3485.44 | BSD-3-Clause style (see LICENSE.txt) |

To update, replace the files with the same files from a newer release and rebuild.
