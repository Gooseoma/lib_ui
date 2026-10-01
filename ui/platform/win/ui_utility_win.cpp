// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/platform/win/ui_utility_win.h"

#include "ui/widgets/popup_menu.h"

#include <QtWidgets/QApplication>
#include <QtGui/QWindow>
#include <QtCore/QAbstractNativeEventFilter>

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include "base/event_filter.h"
#include <QTimer>
#include <cmath>
#include <QScreen>

namespace Ui::Platform {
namespace {

constexpr auto kDWMWA_USE_IMMERSIVE_DARK_MODE = DWORD(20);
constexpr auto kDWMWA_WINDOW_CORNER_PREFERENCE = DWORD(33);
constexpr auto kDWMWA_SYSTEMBACKDROP_TYPE = DWORD(38);
constexpr auto kDWMWCP_ROUND = DWORD(2);
constexpr auto kDWMSBT_NONE = DWORD(1);
constexpr auto kDWMSBT_TRANSIENTWINDOW = DWORD(3);

constexpr auto kBuildAcrylicAccent = 17134; // Windows 10 1803.
constexpr auto kBuildSystemBackdrop = 22621; // Windows 11 22H2.

constexpr auto kWcaAccentPolicy = 19;
constexpr auto kAccentDisabled = 0;
constexpr auto kAccentAcrylicBlurBehind = 4;

struct AccentPolicy {
	int state = 0;
	int flags = 0;
	uint32 gradientColor = 0;
	int animationId = 0;
};

struct WindowCompositionAttributeData {
	int attribute = 0;
	void *data = nullptr;
	ULONG size = 0;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(
	HWND,
	WindowCompositionAttributeData*);

[[nodiscard]] int WindowsBuild() {
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
	static const auto result = [] {
		const auto ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto function = ntdll
			? reinterpret_cast<RtlGetVersionFn>(
				GetProcAddress(ntdll, "RtlGetVersion"))
			: nullptr;
		auto info = OSVERSIONINFOW();
		info.dwOSVersionInfoSize = sizeof(info);
		return (function && (function(&info) == 0))
			? int(info.dwBuildNumber)
			: 0;
	}();
	return result;
}

[[nodiscard]] SetWindowCompositionAttributeFn FindSetWindowCompositionAttribute() {
	static const auto result = [] {
		const auto user32 = GetModuleHandleW(L"user32.dll");
		return user32
			? reinterpret_cast<SetWindowCompositionAttributeFn>(
				GetProcAddress(user32, "SetWindowCompositionAttribute"))
			: nullptr;
	}();
	return result;
}

} // namespace

bool GlassBackdropSupported() {
	const auto build = WindowsBuild();
	return (build >= kBuildSystemBackdrop)
		|| ((build >= kBuildAcrylicAccent)
			&& (FindSetWindowCompositionAttribute() != nullptr));
}

bool SetGlassBackdrop(
		not_null<QWidget*> widget,
		bool enabled,
		bool dark,
		int radius) {
	widget->createWinId();
	const auto handle = reinterpret_cast<HWND>(widget->winId());
	if (!handle) {
		return false;
	}
	const auto build = WindowsBuild();
	if (build >= kBuildSystemBackdrop) {
		const auto darkMode = BOOL(dark ? TRUE : FALSE);
		const auto corners = kDWMWCP_ROUND;
		const auto backdrop = enabled
			? kDWMSBT_TRANSIENTWINDOW
			: kDWMSBT_NONE;
		const auto margins = enabled
			? MARGINS{ -1, -1, -1, -1 }
			: MARGINS{ 0, 0, 0, 0 };
		DwmSetWindowAttribute(
			handle,
			kDWMWA_USE_IMMERSIVE_DARK_MODE,
			&darkMode,
			sizeof(darkMode));
		DwmSetWindowAttribute(
			handle,
			kDWMWA_WINDOW_CORNER_PREFERENCE,
			&corners,
			sizeof(corners));
		DwmExtendFrameIntoClientArea(handle, &margins);
		return SUCCEEDED(DwmSetWindowAttribute(
			handle,
			kDWMWA_SYSTEMBACKDROP_TYPE,
			&backdrop,
			sizeof(backdrop)));
	}
	const auto set = (build >= kBuildAcrylicAccent)
		? FindSetWindowCompositionAttribute()
		: nullptr;
	if (!set) {
		return false;
	}
	auto accent = AccentPolicy{
		.state = enabled ? kAccentAcrylicBlurBehind : kAccentDisabled,
		.gradientColor = dark ? 0x40202020U : 0x40F0F0F0U,
	};
	auto data = WindowCompositionAttributeData{
		.attribute = kWcaAccentPolicy,
		.data = &accent,
		.size = sizeof(accent),
	};
	const auto ok = (set(handle, &data) != FALSE);
	if (enabled) {
		const auto ratio = widget->devicePixelRatioF();
		const auto width = int(std::ceil(widget->width() * ratio));
		const auto height = int(std::ceil(widget->height() * ratio));
		const auto diameter = int(std::ceil(radius * 2 * ratio));
		SetWindowRgn(
			handle,
			CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter),
			TRUE);
	} else {
		SetWindowRgn(handle, nullptr, TRUE);
	}
	return ok;
}

bool IsApplicationActive() {
	return QApplication::activeWindow() != nullptr;
}

void IgnoreAllActivation(not_null<QWidget*> widget) {
	widget->createWinId();

	const auto handle = reinterpret_cast<HWND>(widget->winId());
	Assert(handle != nullptr);

	ShowWindow(handle, SW_HIDE);
	const auto style = GetWindowLongPtr(handle, GWL_EXSTYLE);
	SetWindowLongPtr(
		handle,
		GWL_EXSTYLE,
		style | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
	ShowWindow(handle, SW_SHOW);
}

void ShowWindowMenu(not_null<QWidget*> widget, const QPoint &point) {
	const auto handle = HWND(widget->winId());
	const auto mapped = point * widget->windowHandle()->devicePixelRatio();
	POINT p{ mapped.x(), mapped.y() };
	ClientToScreen(handle, &p);
	SendMessage(
		handle,
		0x313 /* WM_POPUPSYSTEMMENU */,
		0,
		MAKELPARAM(p.x, p.y));
}

void FixPopupMenuNativeEmojiPopup(not_null<PopupMenu*> menu) {
	// Windows native emoji selector, that can be called by Win+. shortcut,
	// is behaving strangely within an input field in a popup menu.
	//
	// When the selector is shown and a mouse button is pressed the system
	// sends two events "MousePress + MouseRelease" to the popup menu, even
	// before the button is physically released. That way we hide the menu
	// on this MousePress, that we shouldn't have received (in case of
	// input field in the main window no such events are sent at all).
	//
	// To workaround this we detect a WM_MOUSELEAVE event that is sent to
	// the popup menu when the selector is shown and skip all mouse press
	// events while we don't receive mouse move events. If we receive mouse
	// move events that means the selector was hidden and the mouse is
	// captured by the popup menu again.
	class Filter final : public QAbstractNativeEventFilter {
	public:
		explicit Filter(not_null<PopupMenu*> menu) : _menu(menu) {
		}

		bool nativeEventFilter(
				const QByteArray &eventType,
				void *message,
				native_event_filter_result *result) override {
			const auto msg = static_cast<MSG*>(message);
			switch (msg->message) {
			case WM_MOUSELEAVE: if (msg->hwnd == hwnd()) {
				_skipMouseDown = true;
			} break;
			case WM_MOUSEMOVE: if (msg->hwnd == hwnd()) {
				_skipMouseDown = false;
			} break;
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK: if (msg->hwnd == hwnd()) {
				return _skipMouseDown;
			}
			}
			return false;
		}

	private:
		[[nodiscard]] HWND hwnd() const {
			const auto top = _menu->window()->windowHandle();
			return top ? reinterpret_cast<HWND>(top->winId()) : nullptr;
		}

		not_null<PopupMenu*> _menu;
		bool _skipMouseDown = false;

	};

	QGuiApplication::instance()->installNativeEventFilter(
		menu->lifetime().make_state<Filter>(menu));
}

} // namespace Ui::Platform
