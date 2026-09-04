#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter_windows.h>
#include <windows.h>

#include <algorithm>

#include "flutter_window.h"
#include "utils.h"

namespace {

// 竖窗的【期望】尺寸(逻辑像素)。实际尺寸会被 ClampToWorkArea 按显示器工作区夹取。
//
// 480x960 的取值理由:
//   - 宽 480 够放"标签 + 数值 + 单位"三段而不换行(三通道原始计数最长 8 位带负号);
//   - 高 960 在 1080p 屏上减去任务栏与标题栏后仍放得下;
//   - 1:2 比例在多数缩放比下都不会变成怪比例。
constexpr int kDesiredWidthLogical = 480;
constexpr int kDesiredHeightLogical = 960;

// 标题栏 + 边框大致占掉的逻辑高度。夹取时要把它算进去, 否则客户区虽然放得下,
// 加上非客户区之后仍然超出工作区。
constexpr int kNonClientHeightLogical = 40;

// 把期望尺寸限制在窗口将要出现的那台显示器的工作区内。
//
// ⚠ 为什么必须夹取(实测教训): Win32Window::Create 会把逻辑尺寸按显示器 DPI 放大,
//   不夹取的话小屏或高缩放比下窗口会超出屏幕。本机实测 —— 主屏 1280x720、
//   工作区仅 672px 高, 960 逻辑像素的窗口被系统截断成 737 物理像素,
//   底部内容直接看不见, 而且没有任何报错。
//
// ⚠ 夹取而不是把常量直接调小: 调小会让真正的 1080p+ 屏白白浪费高度。
//   期望值保持"理想尺寸", 由运行时按实际屏幕退让。
Win32Window::Size ClampToWorkArea(const Win32Window::Point& origin,
                                  int width_logical,
                                  int height_logical) {
  // Win32Window::Point 用 unsigned int, 而 POINT 用 LONG —— 花括号初始化会把
  // 这种收缩转换判为 C4838 警告, 而 runner 的 /WX 把警告升成错误。显式转换。
  POINT pt{static_cast<LONG>(origin.x), static_cast<LONG>(origin.y)};
  HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

  MONITORINFO info{};
  info.cbSize = sizeof(MONITORINFO);
  if (!::GetMonitorInfo(monitor, &info)) {
    return Win32Window::Size(width_logical, height_logical);
  }

  // FlutterDesktopGetDpiForMonitor 与 win32_window.cpp 里 Create() 用的是同一个
  // DPI 来源 —— 必须一致, 否则夹取用的比例和实际放大用的比例不同, 白算。
  const UINT dpi = ::FlutterDesktopGetDpiForMonitor(monitor);
  const double scale = (dpi == 0) ? 1.0 : dpi / 96.0;

  const int avail_w_logical =
      static_cast<int>((info.rcWork.right - info.rcWork.left) / scale);
  const int avail_h_logical =
      static_cast<int>((info.rcWork.bottom - info.rcWork.top) / scale) -
      kNonClientHeightLogical;

  return Win32Window::Size(std::min(width_logical, avail_w_logical),
                           std::min(height_logical, avail_h_logical));
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);

  // 竖窗(portrait): 本工具的信息是纵向堆叠的 —— 设备列表、传感器三通道读数、
  // 标定步骤、记录列表都是纵向长列表, 横向并不需要宽度。
  //
  // 尺寸是逻辑像素, Win32Window::Create 内部会按显示器 DPI 放大(见
  // win32_window.cpp 的 Scale())。期望值定义在文件顶部, 这里按实际工作区夹取 ——
  // 详见 ClampToWorkArea 上方那段实测教训。
  Win32Window::Point origin(10, 10);
  Win32Window::Size size =
      ClampToWorkArea(origin, kDesiredWidthLogical, kDesiredHeightLogical);

  // 标题保持 ASCII: 本文件由 MSVC 按当前代码页解析, 塞中文需要给 CMake 加
  // /utf-8 或存成带 BOM 的 UTF-8, 不值得为一个标题冒乱码的风险。
  // 要中文标题就用 Dart 侧的 window_manager 包在运行时设。
  if (!window.Create(L"sensor_beacon tool", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
