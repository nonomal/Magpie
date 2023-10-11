#pragma once
#include "MagOptions.h"
#include <SmallVector.h>

namespace Magpie::Core {

class DeviceResources;
class Renderer;
class FrameSourceBase;
class CursorManager;
class ExclModeHack;

class MagApp {
public:
	~MagApp();

	static MagApp& Get() noexcept {
		static MagApp instance;
		return instance;
	}

	bool Start(HWND hwndSrc, MagOptions&& options);

	void Stop(bool isSrcMovingOrSizing = false);

	void ToggleOverlay();

	HINSTANCE GetHInstance() const noexcept {
		return _hInst;
	}

	HWND GetHwndSrc() const noexcept {
		return _hwndSrc;
	}

	HWND GetHwndHost() const noexcept {
		return _hwndHost;
	}

	const RECT& GetHostWndRect() const noexcept {
		return _hostWndRect;
	}

	DeviceResources& GetDeviceResources() noexcept {
		return *_deviceResources;
	}

	Renderer& GetRenderer() noexcept {
		return *_renderer;
	}

	FrameSourceBase& GetFrameSource() noexcept {
		return *_frameSource;
	}

	CursorManager& GetCursorManager() noexcept {
		return *_cursorManager;
	}

	MagOptions& GetOptions() noexcept {
		return _options;
	}

	winrt::DispatcherQueue Dispatcher() const noexcept {
		return _dispatcher;
	}

	// 注册消息回调，回调函数如果不阻断消息应返回空
	// 返回 ID，不会为 0
	uint32_t RegisterWndProcHandler(std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)> handler) noexcept;
	bool UnregisterWndProcHandler(uint32_t id) noexcept;

	bool MessageLoop();

private:
	MagApp();

	void _RegisterWndClasses() const;

	// 创建主窗口
	bool _CreateHostWnd();

	bool _InitFrameSource();

	bool _DisableDirectFlip();

	static LRESULT CALLBACK _HostWndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		return Get()._HostWndProc(hWnd, msg, wParam, lParam);
	}

	LRESULT _HostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void _OnQuit();

	winrt::fire_and_forget _WaitForSrcMovingOrSizing();

	const winrt::DispatcherQueue _dispatcher{ nullptr };

	HINSTANCE _hInst = NULL;
	HWND _hwndSrc = NULL;
	HWND _hwndHost = NULL;

	// 关闭 DirectFlip 时的背景全屏窗口
	HWND _hwndDDF = NULL;

	RECT _hostWndRect{};

	std::unique_ptr<DeviceResources> _deviceResources;
	std::unique_ptr<Renderer> _renderer;
	std::unique_ptr<FrameSourceBase> _frameSource;
	std::unique_ptr<CursorManager> _cursorManager;
	std::unique_ptr<ExclModeHack> _exclModeHack;
	MagOptions _options;

	HHOOK _hKeyboardHook = NULL;

	SmallVector<std::pair<std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)>, uint32_t>, 2> _wndProcHandlers;
	uint32_t _nextWndProcHandlerID = 1;

	bool _windowResizingDisabled = false;
	bool _roundCornerDisabled = false;

	bool _isWaitingForSrcMovingOrSizing = false;
};

}
