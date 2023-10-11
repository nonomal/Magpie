#include "pch.h"
#include "CursorManager.h"
#include "MagApp.h"
#include "FrameSourceBase.h"
#include "Renderer.h"
#include "Logger.h"
#include "Win32Utils.h"
#include "DeviceResources.h"
#include "GraphicsCaptureFrameSource.h"
#include "WindowHelper.h"
#include "Utils.h"
#include <magnification.h>

#pragma comment(lib, "Magnification.lib")


namespace Magpie::Core {

// 将源窗口的光标位置映射到缩放后的光标位置
// 当光标位于源窗口之外，与源窗口的距离不会缩放
static POINT SrcToHost(POINT pt, bool screenCoord) {
	const RECT& srcFrameRect = MagApp::Get().GetFrameSource().GetSrcFrameRect();
	const RECT& virtualOutputRect = MagApp::Get().GetRenderer().GetVirtualOutputRect();
	const RECT& hostRect = MagApp::Get().GetHostWndRect();

	POINT result;
	if (screenCoord) {
		result = { hostRect.left, hostRect.top };
	} else {
		result = {};
	}

	if (pt.x >= srcFrameRect.right) {
		result.x += hostRect.right - hostRect.left + pt.x - srcFrameRect.right;
	} else if (pt.x < srcFrameRect.left) {
		result.x += pt.x - srcFrameRect.left;
	} else {
		double pos = double(pt.x - srcFrameRect.left) / (srcFrameRect.right - srcFrameRect.left - 1);
		result.x += std::lround(pos * (virtualOutputRect.right - virtualOutputRect.left - 1)) + virtualOutputRect.left;
	}

	if (pt.y >= srcFrameRect.bottom) {
		result.y += hostRect.bottom - hostRect.top + pt.y - srcFrameRect.bottom;
	} else if (pt.y < srcFrameRect.top) {
		result.y += pt.y - srcFrameRect.top;
	} else {
		double pos = double(pt.y - srcFrameRect.top) / (srcFrameRect.bottom - srcFrameRect.top - 1);
		result.y += std::lround(pos * (virtualOutputRect.bottom - virtualOutputRect.top - 1)) + virtualOutputRect.top;
	}

	return result;
}

// 将缩放后的光标位置映射到源窗口
static POINT HostToSrc(POINT pt) {
	const RECT& srcFrameRect = MagApp::Get().GetFrameSource().GetSrcFrameRect();
	const RECT& hostRect = MagApp::Get().GetHostWndRect();
	const RECT& virtualOutputRect = MagApp::Get().GetRenderer().GetVirtualOutputRect();
	RECT outputRect = MagApp::Get().GetRenderer().GetOutputRect();

	const SIZE srcFrameSize = Win32Utils::GetSizeOfRect(srcFrameRect);
	const SIZE virtualOutputSize = Win32Utils::GetSizeOfRect(virtualOutputRect);
	const SIZE outputSize = Win32Utils::GetSizeOfRect(outputRect);

	pt.x -= hostRect.left;
	pt.y -= hostRect.top;

	POINT result = { srcFrameRect.left, srcFrameRect.top };

	if (pt.x >= outputRect.right) {
		result.x += srcFrameSize.cx + pt.x - outputRect.right;
	} else if (pt.x < outputRect.left) {
		result.x += pt.x - outputRect.left;
	} else {
		double pos = double(pt.x - virtualOutputRect.left) / (virtualOutputSize.cx - 1);
		result.x += std::lround(pos * (srcFrameSize.cx - 1));
	}

	if (pt.y >= outputRect.bottom) {
		result.y += srcFrameSize.cx + pt.y - outputRect.bottom;
	} else if (pt.y < outputRect.top) {
		result.y += pt.y - outputRect.top;
	} else {
		double pos = double(pt.y - virtualOutputRect.top) / (virtualOutputSize.cy - 1);
		result.y += std::lround(pos * (srcFrameSize.cy - 1));
	}

	return result;
}

CursorManager::~CursorManager() {
	if (_curClips != RECT{}) {
		ClipCursor(nullptr);
	}

	if (_isUnderCapture) {
		POINT pt{};
		if (!::GetCursorPos(&pt)) {
			Logger::Get().Win32Error("GetCursorPos 失败");
		}
		_StopCapture(pt, true);
	}

	MagApp::Get().UnregisterWndProcHandler(_handlerId);
}

static std::optional<LRESULT> HostWndProc(HWND /*hWnd*/, UINT message, WPARAM /*wParam*/, LPARAM /*lParam*/) {
	if (MagApp::Get().GetOptions().Is3DGameMode() && MagApp::Get().GetRenderer().IsUIVisiable()) {
		return std::nullopt;
	}

	if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
		// 主窗口会在非常特定的情况下收到光标消息：
		// 1. 未处于捕获状态
		// 2. 缩放后的位置未被遮挡而缩放前的位置被遮挡
		// 或用户操作 UI 时
		HWND hwndSrc = MagApp::Get().GetHwndSrc();
		HWND hwndForground = GetForegroundWindow();
		if (hwndForground != hwndSrc) {
			if (!Win32Utils::SetForegroundWindow(hwndSrc)) {
				// 设置前台窗口失败，可能是因为前台窗口是开始菜单
				if (WindowHelper::IsStartMenu(hwndForground)) {
					using namespace std::chrono;

					// 限制触发频率
					static steady_clock::time_point prevTimePoint{};
					auto now = steady_clock::now();
					if (duration_cast<milliseconds>(now - prevTimePoint).count() >= 1000) {
						prevTimePoint = now;

						// 模拟按键关闭开始菜单
						INPUT inputs[4]{};
						inputs[0].type = INPUT_KEYBOARD;
						inputs[0].ki.wVk = VK_LWIN;
						inputs[1].type = INPUT_KEYBOARD;
						inputs[1].ki.wVk = VK_LWIN;
						inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
						SendInput((UINT)std::size(inputs), inputs, sizeof(INPUT));

						// 等待系统处理
						Sleep(1);
					}

					SetForegroundWindow(hwndSrc);
				}
			}

			return 0;
		}

		if (!MagApp::Get().GetOptions().IsDebugMode()) {
			SetWindowPos(MagApp::Get().GetHwndHost(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOREDRAW);
		}
	}

	return std::nullopt;
}

bool CursorManager::Initialize() {
	_handlerId = MagApp::Get().RegisterWndProcHandler(HostWndProc);

	if (MagApp::Get().GetOptions().Is3DGameMode()) {
		POINT cursorPos;
		::GetCursorPos(&cursorPos);
		_StartCapture(cursorPos);
	}

	Logger::Get().Info("CursorManager 初始化完成");
	return true;
}

// 检测光标位于哪个窗口上，是否检测缩放窗口由 clickThroughHost 指定
static HWND WindowFromPoint(POINT pt, bool clickThroughHost) {
	struct EnumData {
		HWND result;
		POINT pt;
		bool clickThroughHost;
	} data{ NULL, pt, clickThroughHost };

	EnumWindows([](HWND hWnd, LPARAM lParam) {
		EnumData& data = *(EnumData*)lParam;
		if (hWnd == MagApp::Get().GetHwndHost()) {
			if (PtInRect(&MagApp::Get().GetHostWndRect(), data.pt) && !data.clickThroughHost) {
				data.result = hWnd;
				return FALSE;
			} else {
				return TRUE;
			}
		}

		// 跳过不可见的窗口
		if (!(GetWindowLongPtr(hWnd, GWL_STYLE) & WS_VISIBLE)) {
			return TRUE;
		}

		// 跳过透明窗口
		if (GetWindowLongPtr(hWnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) {
			return TRUE;
		}

		// 跳过被冻结的窗口
		UINT isCloaked{};
		DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
		if (isCloaked != 0) {
			return TRUE;
		}

		// 对于分层窗口（Layered Window），没有公开的 API 可以检测某个像素是否透明。
		// ChildWindowFromPointEx 是一个替代方案，当命中透明像素时它将返回 NULL。
		// Windows 内部有 LayerHitTest (https://github.com/tongzx/nt5src/blob/daad8a087a4e75422ec96b7911f1df4669989611/Source/XPSP1/NT/windows/core/ntuser/kernel/winwhere.c#L21) 方法用于对分层窗口执行命中测试，虽然它没有被公开，但 ChildWindowFromPointEx 使用了它
		// 在比 Magpie 权限更高的窗口上使用会失败，失败则假设不是分层窗口
		POINT clientPt = data.pt;
		ScreenToClient(hWnd, &clientPt);
		SetLastError(0);
		if (!ChildWindowFromPointEx(hWnd, clientPt, CWP_SKIPDISABLED | CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT)) {
			if (GetLastError() == 0) {
				// 命中了透明像素
				return TRUE;
			}

			// 源窗口的权限比 Magpie 更高，回落到 GetWindowRect
			RECT windowRect{};
			if (!GetWindowRect(hWnd, &windowRect) || !PtInRect(&windowRect, data.pt)) {
				return TRUE;
			}
		}

		data.result = hWnd;
		return FALSE;
	}, (LPARAM)&data);

	return data.result;
}

void CursorManager::OnBeginFrame() {
	_UpdateCursorClip();

	if (!MagApp::Get().GetOptions().IsDrawCursor() || !_isShowCursor || !_isUnderCapture) {
		// 不绘制光标
		_curCursor = NULL;
		return;
	}

	if (MagApp::Get().GetOptions().Is3DGameMode()) {
		HWND hwndFore = GetForegroundWindow();
		if (hwndFore != MagApp::Get().GetHwndHost() && hwndFore != MagApp::Get().GetHwndSrc()) {
			_curCursor = NULL;
			return;
		}
	}

	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (!::GetCursorInfo(&ci)) {
		Logger::Get().Win32Error("GetCursorInfo 失败");
		return;
	}

	if (!ci.hCursor || ci.flags != CURSOR_SHOWING) {
		_curCursor = NULL;
		return;
	}

	if (!_ResolveCursor(ci.hCursor, false)) {
		Logger::Get().Error("解析光标失败");
		_curCursor = NULL;
		return;
	}

	_curCursorPos = SrcToHost(ci.ptScreenPos, false);
	_curCursor = ci.hCursor;
}

bool CursorManager::GetCursorTexture(ID3D11Texture2D** texture, CursorManager::CursorType& cursorType) {
	if (_curCursorInfo->texture) {
		*texture = _curCursorInfo->texture.get();
		cursorType = _curCursorInfo->type;
		return true;
	}

	if (!_ResolveCursor(_curCursor, true)) {
		return false;
	} else {
		const char* cursorTypes[] = { "Color", "Masked Color", "Monochrome" };
		Logger::Get().Info(fmt::format("已解析光标：{}\n\t类型：{}",
			(void*)_curCursor, cursorTypes[(int)_curCursorInfo->type]));
	}

	*texture = _curCursorInfo->texture.get();
	cursorType = _curCursorInfo->type;
	return true;
}

void CursorManager::OnCursorCapturedOnOverlay() {
	_isCapturedOnOverlay = true;

	// 用户拖动 UI 时将光标限制在输出区域内
	const RECT& outputRect = MagApp::Get().GetRenderer().GetOutputRect();
	const RECT& hostRect = MagApp::Get().GetHostWndRect();
	_curClips = {
		outputRect.left + hostRect.left,
		outputRect.top + hostRect.top,
		outputRect.right + hostRect.left,
		outputRect.bottom + hostRect.top
	};
	ClipCursor(&_curClips);
}

void CursorManager::OnCursorReleasedOnOverlay() {
	_isCapturedOnOverlay = false;
	_UpdateCursorClip();
}

void CursorManager::OnCursorHoverOverlay() {
	_isOnOverlay = true;
	_UpdateCursorClip();
}

void CursorManager::OnCursorLeaveOverlay() {
	_isOnOverlay = false;
	_UpdateCursorClip();
}

static void ShowSystemCursor(bool show) {
	static void (WINAPI* const showSystemCursor)(BOOL bShow) = []()->void(WINAPI*)(BOOL) {
		HMODULE lib = LoadLibrary(L"user32.dll");
		if (!lib) {
			return nullptr;
		}

		return (void(WINAPI*)(BOOL))GetProcAddress(lib, "ShowSystemCursor");
	}();

	if (showSystemCursor) {
		showSystemCursor((BOOL)show);
	} else {
		// 获取 ShowSystemCursor 失败则回落到 Magnification API
		static bool initialized = []() {
			if (!MagInitialize()) {
				Logger::Get().Win32Error("MagInitialize 失败");
				return false;
			}

			return true;
		}();

		if (initialized) {
			MagShowSystemCursor(show);
		}
	}

	if (show) {
		MagApp::Get().Dispatcher().TryEnqueue([]() {
			if (!MagApp::Get().GetHwndHost()) {
				return;
			}

			// 修复有时不会立即显示光标的问题
			FrameSourceBase& frameSource = MagApp::Get().GetFrameSource();
			if (frameSource.GetName() == GraphicsCaptureFrameSource::NAME) {
				GraphicsCaptureFrameSource& wgc = (GraphicsCaptureFrameSource&)frameSource;
				// WGC 需要重启捕获
				// 没有用户报告这个问题，只在我的电脑上出现，可能和驱动有关
				wgc.StopCapture();
				wgc.StartCapture();
			} else {
				SystemParametersInfo(SPI_SETCURSORS, 0, 0, 0);
			}
		});
	}
}

void CursorManager::_StartCapture(POINT cursorPt) {
	if (_isUnderCapture) {
		return;
	}

	// 在以下情况下进入捕获状态：
	// 1. 当前未捕获
	// 2. 光标进入全屏区域
	// 
	// 进入捕获状态时：
	// 1. 调整光标速度，全局隐藏光标
	// 2. 将光标移到源窗口的对应位置
	//
	// 在有黑边的情况下自动将光标调整到画面内

	// 全局隐藏光标
	ShowSystemCursor(false);

	const RECT& srcFrameRect = MagApp::Get().GetFrameSource().GetSrcFrameRect();
	const RECT& hostRect = MagApp::Get().GetHostWndRect();
	const RECT& outputRect = MagApp::Get().GetRenderer().GetOutputRect();

	SIZE srcFrameSize = Win32Utils::GetSizeOfRect(srcFrameRect);
	SIZE outputSize = Win32Utils::GetSizeOfRect(outputRect);

	if (MagApp::Get().GetOptions().IsAdjustCursorSpeed()) {
		_AdjustCursorSpeed();
	}

	// 移动光标位置

	// 跳过黑边
	cursorPt.x = std::clamp(cursorPt.x, hostRect.left + outputRect.left, hostRect.left + outputRect.right - 1);
	cursorPt.y = std::clamp(cursorPt.y, hostRect.top + outputRect.top, hostRect.top + outputRect.bottom - 1);

	POINT newCursorPos = HostToSrc(cursorPt);
	SetCursorPos(newCursorPos.x, newCursorPos.y);

	_isUnderCapture = true;
}

void CursorManager::_StopCapture(POINT cursorPos, bool onDestroy) {
	if (!_isUnderCapture) {
		return;
	}

	if (_curClips != RECT{}) {
		_curClips = {};
		ClipCursor(nullptr);
	}

	// 在以下情况下离开捕获状态：
	// 1. 当前处于捕获状态
	// 2. 光标离开源窗口客户区
	// 3. 目标位置存在屏幕
	//
	// 离开捕获状态时
	// 1. 还原光标速度，全局显示光标
	// 2. 将光标移到全屏窗口外的对应位置
	//
	// 在有黑边的情况下自动将光标调整到全屏窗口外

	POINT newCursorPos = SrcToHost(cursorPos, true);

	if (onDestroy || MonitorFromPoint(newCursorPos, MONITOR_DEFAULTTONULL)) {
		SetCursorPos(newCursorPos.x, newCursorPos.y);

		if (MagApp::Get().GetOptions().IsAdjustCursorSpeed()) {
			SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)_cursorSpeed, 0);
		}

		ShowSystemCursor(true);
		_isUnderCapture = false;
	} else {
		// 目标位置不存在屏幕，则将光标限制在源窗口内
		const RECT& srcFrameRect = MagApp::Get().GetFrameSource().GetSrcFrameRect();
		SetCursorPos(
			std::clamp(cursorPos.x, srcFrameRect.left, srcFrameRect.right - 1),
			std::clamp(cursorPos.y, srcFrameRect.top, srcFrameRect.bottom - 1)
		);
	}
}

bool CursorManager::_ResolveCursor(HCURSOR hCursor, bool resolveTexture) {
	auto it = _cursorInfos.find(hCursor);
	if (it != _cursorInfos.end() && (!resolveTexture || (resolveTexture && _curCursorInfo->texture))) {
		_curCursorInfo = &it->second;
		return true;
	}

	ICONINFO ii{};
	if (!GetIconInfo(hCursor, &ii)) {
		Logger::Get().Win32Error("GetIconInfo 失败");
		return false;
	}

	Utils::ScopeExit se([&ii]() {
		if (ii.hbmColor) {
			DeleteBitmap(ii.hbmColor);
		}
		DeleteBitmap(ii.hbmMask);
	});

	BITMAP bmp{};
	if (!GetObject(ii.hbmMask, sizeof(bmp), &bmp)) {
		Logger::Get().Win32Error("GetObject 失败");
		return false;
	}

	_curCursorInfo = it == _cursorInfos.end() ? &_cursorInfos[hCursor] : &it->second;

	_curCursorInfo->hotSpot = { (LONG)ii.xHotspot, (LONG)ii.yHotspot };
	// 单色光标的 hbmMask 高度为实际高度的两倍
	_curCursorInfo->size = { bmp.bmWidth, ii.hbmColor ? bmp.bmHeight : bmp.bmHeight / 2 };

	if (!resolveTexture) {
		return true;
	}

	auto& dr = MagApp::Get().GetDeviceResources();

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = bmp.bmWidth;
	bi.bmiHeader.biHeight = -bmp.bmHeight;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biCompression = BI_RGB;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biSizeImage = bmp.bmWidth * bmp.bmHeight * 4;

	if (ii.hbmColor == NULL) {
		// 单色光标
		_curCursorInfo->type = CursorType::Monochrome;

		std::unique_ptr<BYTE[]> pixels(new BYTE[bi.bmiHeader.biSizeImage]);
		HDC hdc = GetDC(NULL);
		if (GetDIBits(hdc, ii.hbmMask, 0, bmp.bmHeight, pixels.get(), &bi, DIB_RGB_COLORS) != bmp.bmHeight) {
			Logger::Get().Win32Error("GetDIBits 失败");
			ReleaseDC(NULL, hdc);
			return false;
		}
		ReleaseDC(NULL, hdc);

		// 红色通道是 AND 掩码，绿色通道是 XOR 掩码
		// 这里将下半部分的 XOR 掩码复制到上半部分的绿色通道中
		const int halfSize = bi.bmiHeader.biSizeImage / 8;
		BYTE* upPtr = &pixels[1];
		BYTE* downPtr = &pixels[static_cast<size_t>(halfSize) * 4];
		for (int i = 0; i < halfSize; ++i) {
			*upPtr = *downPtr;

			upPtr += 4;
			downPtr += 4;
		}

		D3D11_SUBRESOURCE_DATA initData{};
		initData.pSysMem = pixels.get();
		initData.SysMemPitch = bmp.bmWidth * 4;

		_curCursorInfo->texture = dr.CreateTexture2D(
			DXGI_FORMAT_R8G8B8A8_UNORM,
			bmp.bmWidth,
			bmp.bmHeight / 2,
			D3D11_BIND_SHADER_RESOURCE,
			D3D11_USAGE_IMMUTABLE,
			0,
			&initData
		);
		if (!_curCursorInfo->texture) {
			Logger::Get().Error("创建纹理失败");
			return false;
		}

		return true;
	}

	std::unique_ptr<BYTE[]> pixels(new BYTE[bi.bmiHeader.biSizeImage]);
	HDC hdc = GetDC(NULL);
	if (GetDIBits(hdc, ii.hbmColor, 0, bmp.bmHeight, pixels.get(), &bi, DIB_RGB_COLORS) != bmp.bmHeight) {
		Logger::Get().Win32Error("GetDIBits 失败");
		ReleaseDC(NULL, hdc);
		return false;
	}
	ReleaseDC(NULL, hdc);

	// 若颜色掩码有 A 通道，则是彩色光标，否则是彩色掩码光标
	bool hasAlpha = false;
	for (UINT i = 3; i < bi.bmiHeader.biSizeImage; i += 4) {
		if (pixels[i] != 0) {
			hasAlpha = true;
			break;
		}
	}

	if (hasAlpha) {
		// 彩色光标
		_curCursorInfo->type = CursorType::Color;

		for (size_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
			// 预乘 Alpha 通道
			double alpha = pixels[i + 3] / 255.0f;

			BYTE b = (BYTE)std::lround(pixels[i] * alpha);
			pixels[i] = (BYTE)std::lround(pixels[i + 2] * alpha);
			pixels[i + 1] = (BYTE)std::lround(pixels[i + 1] * alpha);
			pixels[i + 2] = b;

			pixels[i + 3] = 255 - pixels[i + 3];
		}
	} else {
		// 彩色掩码光标
		_curCursorInfo->type = CursorType::MaskedColor;

		std::unique_ptr<BYTE[]> maskPixels(new BYTE[bi.bmiHeader.biSizeImage]);
		hdc = GetDC(NULL);
		if (GetDIBits(hdc, ii.hbmMask, 0, bmp.bmHeight, maskPixels.get(), &bi, DIB_RGB_COLORS) != bmp.bmHeight) {
			Logger::Get().Win32Error("GetDIBits 失败");
			ReleaseDC(NULL, hdc);
			return false;
		}
		ReleaseDC(NULL, hdc);

		// 将 XOR 掩码复制到透明通道中
		for (size_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
			std::swap(pixels[i], pixels[i + 2]);
			pixels[i + 3] = maskPixels[i];
		}
	}

	D3D11_SUBRESOURCE_DATA initData{};
	initData.pSysMem = &pixels[0];
	initData.SysMemPitch = bmp.bmWidth * 4;

	_curCursorInfo->texture = dr.CreateTexture2D(
		DXGI_FORMAT_R8G8B8A8_UNORM,
		bmp.bmWidth,
		bmp.bmHeight,
		D3D11_BIND_SHADER_RESOURCE,
		D3D11_USAGE_IMMUTABLE,
		0,
		&initData
	);
	if (!_curCursorInfo->texture) {
		Logger::Get().Error("创建纹理失败");
		return false;
	}

	return true;
}

void CursorManager::_AdjustCursorSpeed() {
	if (!SystemParametersInfo(SPI_GETMOUSESPEED, 0, &_cursorSpeed, 0)) {
		Logger::Get().Win32Error("获取光标移速失败");
		return;
	}

	// 鼠标加速默认打开
	bool isMouseAccelerationOn = true;
	{
		std::array<INT, 3> values{};
		if (SystemParametersInfo(SPI_GETMOUSE, 0, values.data(), 0)) {
			isMouseAccelerationOn = values[2];
		} else {
			Logger::Get().Win32Error("检索鼠标加速失败");
		}
	}

	SIZE srcFrameSize = Win32Utils::GetSizeOfRect(MagApp::Get().GetFrameSource().GetSrcFrameRect());
	SIZE virtualOutputSize = Win32Utils::GetSizeOfRect(MagApp::Get().GetRenderer().GetVirtualOutputRect());
	double scale = ((double)virtualOutputSize.cx / srcFrameSize.cx + (double)virtualOutputSize.cy / srcFrameSize.cy) / 2;

	INT newSpeed = 0;

	// “提高指针精确度”（鼠标加速）打开时光标移速的调整为线性，否则为非线性
	// 参见 https://liquipedia.net/counterstrike/Mouse_Settings#Windows_Sensitivity
	if (isMouseAccelerationOn) {
		newSpeed = std::clamp((INT)lround(_cursorSpeed / scale), 1, 20);
	} else {
		static constexpr std::array<double, 20> SENSITIVITIES = {
			0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875,
			1.0, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 2.75, 3, 3.25, 3.5
		};

		_cursorSpeed = std::clamp(_cursorSpeed, 1, 20);
		double newSensitivity = SENSITIVITIES[static_cast<size_t>(_cursorSpeed) - 1] / scale;

		auto it = std::lower_bound(SENSITIVITIES.begin(), SENSITIVITIES.end(), newSensitivity - 1e-6);
		newSpeed = INT(it - SENSITIVITIES.begin()) + 1;

		if (it != SENSITIVITIES.begin() && it != SENSITIVITIES.end()) {
			// 找到两侧最接近的数值
			if (std::abs(*it - newSensitivity) > std::abs(*(it - 1) - newSensitivity)) {
				--newSpeed;
			}
		}
	}

	if (!SystemParametersInfo(SPI_SETMOUSESPEED, 0, (PVOID)(intptr_t)newSpeed, 0)) {
		Logger::Get().Win32Error("设置光标移速失败");
	}
}

void CursorManager::_UpdateCursorClip() {
	// 优先级：
	// 1. 断点模式：不限制，捕获/取消捕获，支持 UI
	// 2. 在 3D 游戏中限制光标：每帧都限制一次，不退出捕获，因此无法使用 UI，不支持多屏幕
	// 3. 常规：根据多屏幕限制光标，捕获/取消捕获，支持 UI 和多屏幕

	const RECT& srcFrameRect = MagApp::Get().GetFrameSource().GetSrcFrameRect();

	if (!MagApp::Get().GetOptions().IsDebugMode() && MagApp::Get().GetOptions().Is3DGameMode()) {
		// 开启“在 3D 游戏中限制光标”则每帧都限制一次光标
		_curClips = srcFrameRect;
		ClipCursor(&srcFrameRect);
		return;
	}

	if (_isCapturedOnOverlay) {
		// 已在 OnCursorCapturedOnOverlay 中限制光标
		return;
	}

	const HWND hwndHost = MagApp::Get().GetHwndHost();
	const HWND hwndSrc = MagApp::Get().GetHwndSrc();
	const RECT& hostRect = MagApp::Get().GetHostWndRect();

	const RECT& outputRect = MagApp::Get().GetRenderer().GetOutputRect();
	const RECT& virtualOutputRect = MagApp::Get().GetRenderer().GetVirtualOutputRect();

	const SIZE outputSize = Win32Utils::GetSizeOfRect(outputRect);
	const SIZE srcFrameSize = Win32Utils::GetSizeOfRect(srcFrameRect);
	const SIZE virtualOutputSize = Win32Utils::GetSizeOfRect(virtualOutputRect);

	INT_PTR style = GetWindowLongPtr(hwndHost, GWL_EXSTYLE);

	POINT cursorPos;
	if (!::GetCursorPos(&cursorPos)) {
		Logger::Get().Win32Error("GetCursorPos 失败");
		return;
	}

	if (_isUnderCapture) {
		///////////////////////////////////////////////////////////
		// 
		// 处于捕获状态
		// --------------------------------------------------------
		//                  |  虚拟位置被遮挡  |    虚拟位置未被遮挡
		// --------------------------------------------------------
		// 实际位置被遮挡    |    退出捕获     | 退出捕获，主窗口不透明
		// --------------------------------------------------------
		// 实际位置未被遮挡  |    退出捕获      |        无操作
		// --------------------------------------------------------
		// 
		///////////////////////////////////////////////////////////

		HWND hwndCur = WindowFromPoint(SrcToHost(cursorPos, true), false);

		if (hwndCur != hwndHost) {
			// 主窗口被遮挡
			if (style | WS_EX_TRANSPARENT) {
				SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
			}

			_StopCapture(cursorPos);
		} else {
			// 主窗口未被遮挡
			bool stopCapture = _isOnOverlay;

			if (!stopCapture) {
				// 判断源窗口是否被遮挡
				hwndCur = WindowFromPoint(cursorPos, true);
				stopCapture = hwndCur != hwndSrc && (!IsChild(hwndSrc, hwndCur) || !((GetWindowStyle(hwndCur) & WS_CHILD)));
			}

			if (stopCapture) {
				if (style | WS_EX_TRANSPARENT) {
					SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
				}

				_StopCapture(cursorPos);
			} else {
				if (!(style & WS_EX_TRANSPARENT)) {
					SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
				}
			}
		}
	} else {
		/////////////////////////////////////////////////////////
		// 
		// 未处于捕获状态
		// -----------------------------------------------------
		//					|  虚拟位置被遮挡	|  虚拟位置未被遮挡
		// ------------------------------------------------------
		// 实际位置被遮挡		|    无操作		|    主窗口不透明
		// ------------------------------------------------------
		// 实际位置未被遮挡	|    无操作		| 开始捕获，主窗口透明
		// ------------------------------------------------------
		// 
		/////////////////////////////////////////////////////////

		HWND hwndCur = WindowFromPoint(cursorPos, false);

		if (hwndCur == hwndHost) {
			// 主窗口未被遮挡
			POINT newCursorPos = HostToSrc(cursorPos);

			if (!PtInRect(&srcFrameRect, newCursorPos)) {
				// 跳过黑边
				if (_isOnOverlay) {
					// 从内部移到外部
					// 此时有 UI 贴边
					if (newCursorPos.x >= srcFrameRect.right) {
						cursorPos.x += hostRect.right - hostRect.left - outputRect.right;
					} else if (newCursorPos.x < srcFrameRect.left) {
						cursorPos.x -= outputRect.left;
					}

					if (newCursorPos.y >= srcFrameRect.bottom) {
						cursorPos.y += hostRect.bottom - hostRect.top - outputRect.bottom;
					} else if (newCursorPos.y < srcFrameRect.top) {
						cursorPos.y -= outputRect.top;
					}

					if (MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONULL)) {
						SetCursorPos(cursorPos.x, cursorPos.y);
					} else {
						// 目标位置不存在屏幕，则将光标限制在输出区域内
						SetCursorPos(
							std::clamp(cursorPos.x, hostRect.left + outputRect.left, hostRect.left + outputRect.right - 1),
							std::clamp(cursorPos.y, hostRect.top + outputRect.top, hostRect.top + outputRect.bottom - 1)
						);
					}
				} else {
					// 从外部移到内部

					POINT clampedPos = {
						std::clamp(cursorPos.x, hostRect.left + outputRect.left, hostRect.left + outputRect.right - 1),
						std::clamp(cursorPos.y, hostRect.top + outputRect.top, hostRect.top + outputRect.bottom - 1)
					};

					if (WindowFromPoint(clampedPos, false) == hwndHost) {
						if (!(style & WS_EX_TRANSPARENT)) {
							SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
						}

						_StartCapture(cursorPos);
					} else {
						// 要跳跃的位置被遮挡
						if (style | WS_EX_TRANSPARENT) {
							SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
						}
					}
				}
			} else {
				bool startCapture = !_isOnOverlay;

				if (startCapture) {
					// 判断源窗口是否被遮挡
					hwndCur = WindowFromPoint(newCursorPos, true);
					startCapture = hwndCur == hwndSrc || ((IsChild(hwndSrc, hwndCur) && (GetWindowStyle(hwndCur) & WS_CHILD)));
				}

				if (startCapture) {
					if (!(style & WS_EX_TRANSPARENT)) {
						SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style | WS_EX_TRANSPARENT);
					}

					_StartCapture(cursorPos);
				} else {
					if (style | WS_EX_TRANSPARENT) {
						SetWindowLongPtr(hwndHost, GWL_EXSTYLE, style & ~WS_EX_TRANSPARENT);
					}
				}
			}
		}
	}

	if (MagApp::Get().GetOptions().IsDebugMode()) {
		return;
	}

	if (!_isOnOverlay && !_isUnderCapture) {
		return;
	}

	// 根据当前光标位置的四个方向有无屏幕来确定应该在哪些方向限制光标，但这无法
	// 处理屏幕之间存在间隙的情况。解决办法是 _StopCapture 只在目标位置存在屏幕时才取消捕获，
	// 当光标试图移动到间隙中时将被挡住。如果光标的速度足以跨越间隙，则它依然可以在屏幕间移动。
	::GetCursorPos(&cursorPos);
	POINT hostPos = _isOnOverlay ? cursorPos : SrcToHost(cursorPos, true);

	RECT clips{ LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX };

	// left
	RECT rect{ LONG_MIN, hostPos.y, hostRect.left, hostPos.y + 1 };
	if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
		clips.left = _isOnOverlay ? outputRect.left + hostRect.left : srcFrameRect.left;
	}

	// top
	rect = { hostPos.x, LONG_MIN, hostPos.x + 1,hostRect.top };
	if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
		clips.top = _isOnOverlay ? outputRect.top + hostRect.top : srcFrameRect.top;
	}

	// right
	rect = { hostRect.right, hostPos.y, LONG_MAX, hostPos.y + 1 };
	if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
		clips.right = _isOnOverlay ? outputRect.right + hostRect.left : srcFrameRect.right;
	}

	// bottom
	rect = { hostPos.x, hostRect.bottom, hostPos.x + 1, LONG_MAX };
	if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
		clips.bottom = _isOnOverlay ? outputRect.bottom + hostRect.top : srcFrameRect.bottom;
	}

	if (clips != _curClips) {
		_curClips = clips;
		ClipCursor(&clips);
	}
}

}
