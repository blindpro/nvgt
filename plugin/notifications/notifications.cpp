#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <mutex>
#include <sstream>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlguid.h>
#include "../../src/nvgt_plugin.h"

using namespace winrt;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::UI::Notifications;
using std::string;
using std::wstring;

#define WM_TRAYICON (WM_USER + 1)

class CToastNotifier {
public:
	CToastNotifier() {
		m_refCount = 1;
		m_appId = L"NVGT.Game";
		try {
			init_apartment(apartment_type::multi_threaded);
		} catch (...) {}
	}

	void AddRef() { asAtomicInc(m_refCount); }
	void Release() { if (asAtomicDec(m_refCount) == 0) delete this; }

	void set_app_id(const string& id) { m_appId = to_wstring(id); }

	bool show_text(const string& title, const string& body) {
		return show_generic(title, body, "", "", "");
	}

	bool show_with_actions_no_image(const string& title, const string& body, const string& buttons, const string& actions) {
		return show_generic(title, body, "", buttons, actions);
	}

	string get_action() {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_actionQueue.empty()) return "";
		string act = m_actionQueue.front();
		m_actionQueue.pop();
		return act;
	}

	bool install_shortcut(const string& app_id, const string& app_name) {
		try {
			wstring wAppId = to_wstring(app_id);
			wstring wAppName = to_wstring(app_name);
			wchar_t exePath[MAX_PATH];
			GetModuleFileNameW(NULL, exePath, MAX_PATH);
			wchar_t smPath[MAX_PATH];
			if (FAILED(SHGetFolderPathW(NULL, 0x0002, NULL, 0, smPath))) return false;
			wstring shortcutPath = smPath;
			shortcutPath += L"\\";
			shortcutPath += wAppName;
			shortcutPath += L".lnk";
			CoInitialize(NULL);
			IShellLinkW* pShellLink = NULL;
			if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&pShellLink))) return false;
			pShellLink->SetPath(exePath);
			pShellLink->SetArguments(L"");
			wstring workDir = extract_directory(exePath);
			pShellLink->SetWorkingDirectory(workDir.c_str());
			IPropertyStore* pPropStore = NULL;
			if (SUCCEEDED(pShellLink->QueryInterface(IID_IPropertyStore, (void**)&pPropStore))) {
				PROPVARIANT pv;
				if (SUCCEEDED(InitPropVariantFromString(wAppId.c_str(), &pv))) {
					pPropStore->SetValue(PKEY_AppUserModel_ID, pv);
					pPropStore->Commit();
					PropVariantClear(&pv);
				}
				pPropStore->Release();
			}
			IPersistFile* pPersistFile = NULL;
			bool success = false;
			if (SUCCEEDED(pShellLink->QueryInterface(IID_IPersistFile, (void**)&pPersistFile))) {
				if (SUCCEEDED(pPersistFile->Save(shortcutPath.c_str(), TRUE))) {
					success = true;
				}
				pPersistFile->Release();
			}
			pShellLink->Release();
			return success;
		} catch (...) { return false; }
	}

private:
	int m_refCount;
	wstring m_appId;
	struct ActiveToast {
		ToastNotification toast{ nullptr };
		event_token activatedToken;
		event_token dismissedToken;
		event_token failedToken;
	};
	std::vector<std::shared_ptr<ActiveToast>> m_activeToasts;
	std::mutex m_toastMutex;
	std::queue<string> m_actionQueue;
	std::mutex m_queueMutex;

	wstring to_wstring(const string& str) {
		if (str.empty()) return L"";
		int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
		wstring wstrTo(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
		return wstrTo;
	}

	string to_string(const wstring& wstr) {
		if (wstr.empty()) return "";
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
		string strTo(size_needed, 0);
		WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
		return strTo;
	}

	wstring extract_directory(const wstring& path) {
		size_t pos = path.find_last_of(L"\/");
		if (pos != wstring::npos) return path.substr(0, pos);
		return path;
	}

	std::vector<string> split(const string& s, char delimiter) {
		std::vector<string> tokens;
		string token;
		std::istringstream tokenStream(s);
		while (std::getline(tokenStream, token, delimiter)) {
			tokens.push_back(token);
		}
		return tokens;
	}

	wstring escape_xml(const wstring& data) {
		wstring buffer;
		buffer.reserve(data.size());
		for (size_t pos = 0; pos != data.size(); ++pos) {
			switch(data[pos]) {
				case L'&': buffer.append(L"&amp;"); break;
				case L'"': buffer.append(L"&quot;"); break;
				case L'\'': buffer.append(L"&apos;"); break;
				case L'<': buffer.append(L"&lt;"); break;
				case L'>': buffer.append(L"&gt;"); break;
				default: buffer.append(&data[pos], 1); break;
			}
		}
		return buffer;
	}

	bool show_generic(const string& title, const string& body, const string& image_path, const string& buttons, const string& action_ids) {
		try {
			XmlDocument doc = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastImageAndText02);
			auto textNodes = doc.GetElementsByTagName(L"text");
			if (textNodes.Length() > 0) textNodes.Item(0).AppendChild(doc.CreateTextNode(to_wstring(title)));
			if (textNodes.Length() > 1) textNodes.Item(1).AppendChild(doc.CreateTextNode(to_wstring(body)));
			if (!image_path.empty()) {
				auto imageNodes = doc.GetElementsByTagName(L"image");
				if (imageNodes.Length() > 0) {
					auto img = imageNodes.Item(0).as<XmlElement>();
					wchar_t fullPath[MAX_PATH];
					if (GetFullPathNameW(to_wstring(image_path).c_str(), MAX_PATH, fullPath, NULL)) {
						wstring uri = L"file:///";
						uri.append(fullPath);
						img.SetAttribute(L"src", uri);
					}
				}
			} else {
				auto imageNodes = doc.GetElementsByTagName(L"image");
				if (imageNodes.Length() > 0) {
					auto img = imageNodes.Item(0);
					img.ParentNode().RemoveChild(img);
				}
			}
			if (!buttons.empty() && !action_ids.empty()) {
				auto toastNode = doc.SelectSingleNode(L"/toast").as<XmlElement>();
				toastNode.SetAttribute(L"launch", L"default");
				auto actionsNode = doc.CreateElement(L"actions");
				toastNode.AppendChild(actionsNode);
				std::vector<string> btnLabels = split(buttons, '|');
				std::vector<string> btnIds = split(action_ids, '|');
				for (size_t i = 0; i < btnLabels.size(); i++) {
					auto actionNode = doc.CreateElement(L"action");
					actionNode.SetAttribute(L"content", to_wstring(btnLabels[i]));
					actionNode.SetAttribute(L"arguments", i < btnIds.size() ? to_wstring(btnIds[i]) : L"default");
					actionNode.SetAttribute(L"activationType", L"foreground");
					actionsNode.AppendChild(actionNode);
				}
			}
			ToastNotification toast(doc);
			auto activeToast = std::make_shared<ActiveToast>();
			activeToast->toast = toast;
			activeToast->activatedToken = toast.Activated([this, activeToast](ToastNotification const&, Windows::Foundation::IInspectable const& args) {
				try {
					auto result = args.as<ToastActivatedEventArgs>();
					string arg = to_string(result.Arguments().c_str());
					{
						std::lock_guard<std::mutex> lock(m_queueMutex);
						m_actionQueue.push(arg);
					}
				} catch (...) {}
				remove_toast(activeToast);
			});
			activeToast->dismissedToken = toast.Dismissed([this, activeToast](ToastNotification const&, ToastDismissedEventArgs const&) {
				remove_toast(activeToast);
			});
			activeToast->failedToken = toast.Failed([this, activeToast](ToastNotification const&, ToastFailedEventArgs const&) {
				remove_toast(activeToast);
			});
			{
				std::lock_guard<std::mutex> lock(m_toastMutex);
				m_activeToasts.push_back(activeToast);
			}
			ToastNotificationManager::CreateToastNotifier(m_appId).Show(toast);
			return true;
		} catch (...) { return false; }
	}

	void remove_toast(std::shared_ptr<ActiveToast> t) {
		std::lock_guard<std::mutex> lock(m_toastMutex);
		auto it = std::find(m_activeToasts.begin(), m_activeToasts.end(), t);
		if (it != m_activeToasts.end()) {
			m_activeToasts.erase(it);
		}
	}
};

class CTrayIcon {
public:
	CTrayIcon() : m_refCount(1), m_hWnd(NULL), m_visible(false) {
		WNDCLASSW wc = {0};
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = L"NVGTTrayHiddenWindow";
		RegisterClassW(&wc);
		m_hWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, this);
	}

	~CTrayIcon() {
		remove();
		if (m_hWnd) DestroyWindow(m_hWnd);
	}

	void AddRef() { asAtomicInc(m_refCount); }
	void Release() { if (asAtomicDec(m_refCount) == 0) delete this; }

	bool add(const string& tooltip) {
		if (m_visible) return false;
		NOTIFYICONDATAW nid = { sizeof(nid) };
		nid.hWnd = m_hWnd;
		nid.uID = 1;
		nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		nid.uCallbackMessage = WM_TRAYICON;
		nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
		wcsncpy(nid.szTip, to_wstring(tooltip).c_str(), 128);
		if (Shell_NotifyIconW(NIM_ADD, &nid)) {
			m_visible = true;
			return true;
		}
		return false;
	}

	void remove() {
		if (!m_visible) return;
		NOTIFYICONDATAW nid = { sizeof(nid) };
		nid.hWnd = m_hWnd;
		nid.uID = 1;
		Shell_NotifyIconW(NIM_DELETE, &nid);
		m_visible = false;
	}

	void set_tooltip(const string& tooltip) {
		if (!m_visible) return;
		NOTIFYICONDATAW nid = { sizeof(nid) };
		nid.hWnd = m_hWnd;
		nid.uID = 1;
		nid.uFlags = NIF_TIP;
		wcsncpy(nid.szTip, to_wstring(tooltip).c_str(), 128);
		Shell_NotifyIconW(NIM_MODIFY, &nid);
	}

	void set_menu_items(const string& items) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_menuItems = split(items, '|');
	}

	bool is_clicked() {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_clicked) {
			m_clicked = false;
			return true;
		}
		return false;
	}

	int get_menu_click() {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_menuClickQueue.empty()) return -1;
		int item = m_menuClickQueue.front();
		m_menuClickQueue.pop();
		return item;
	}

	void hide_window(uint64_t handle) {
		ShowWindow((HWND)handle, SW_HIDE);
	}

	void show_window(uint64_t handle) {
		ShowWindow((HWND)handle, SW_SHOW);
		SetForegroundWindow((HWND)handle);
	}

private:
	int m_refCount;
	HWND m_hWnd;
	bool m_visible;
	bool m_clicked = false;
	std::vector<string> m_menuItems;
	std::queue<int> m_menuClickQueue;
	std::mutex m_mutex;

	static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		CTrayIcon* pThis = (CTrayIcon*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
		if (msg == WM_TRAYICON && pThis) {
			if (LOWORD(lParam) == WM_LBUTTONUP) {
				std::lock_guard<std::mutex> lock(pThis->m_mutex);
				pThis->m_clicked = true;
			} else if (LOWORD(lParam) == WM_RBUTTONUP) {
				std::lock_guard<std::mutex> lock(pThis->m_mutex);
				if (!pThis->m_menuItems.empty()) {
					POINT pt;
					GetCursorPos(&pt);
					HMENU hMenu = CreatePopupMenu();
					for (size_t i = 0; i < pThis->m_menuItems.size(); i++) {
						wstring wItem = pThis->to_wstring(pThis->m_menuItems[i]);
						AppendMenuW(hMenu, MF_STRING, 1000 + i, wItem.c_str());
					}
					SetForegroundWindow(hWnd);
					TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
					DestroyMenu(hMenu);
				}
			}
		} else if (msg == WM_COMMAND && pThis) {
			int id = LOWORD(wParam);
			if (id >= 1000) {
				std::lock_guard<std::mutex> lock(pThis->m_mutex);
				pThis->m_menuClickQueue.push(id - 1000);
			}
		} else if (msg == WM_NCCREATE) {
			SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
		}
		return DefWindowProcW(hWnd, msg, wParam, lParam);
	}

	wstring to_wstring(const string& str) {
		int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
		wstring wstrTo(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
		return wstrTo;
	}

	std::vector<string> split(const string& s, char delimiter) {
		std::vector<string> tokens;
		string token;
		std::istringstream tokenStream(s);
		while (std::getline(tokenStream, token, delimiter)) {
			tokens.push_back(token);
		}
		return tokens;
	}
};

CToastNotifier* NotifierFactory() { return new CToastNotifier(); }
CTrayIcon* TrayIconFactory() { return new CTrayIcon(); }

plugin_main(nvgt_plugin_shared* shared) {
	asIScriptEngine* script_engine = shared->script_engine;
	if (!prepare_plugin(shared)) return false;
	script_engine->RegisterObjectType("toast_notifier", sizeof(CToastNotifier), asOBJ_REF);
	script_engine->RegisterObjectBehaviour("toast_notifier", asBEHAVE_FACTORY, "toast_notifier@ f()", asFUNCTION(NotifierFactory), asCALL_CDECL);
	script_engine->RegisterObjectBehaviour("toast_notifier", asBEHAVE_ADDREF, "void f()", asMETHOD(CToastNotifier, AddRef), asCALL_THISCALL);
	script_engine->RegisterObjectBehaviour("toast_notifier", asBEHAVE_RELEASE, "void f()", asMETHOD(CToastNotifier, Release), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("toast_notifier", "void set_app_id(const string &in id)", asMETHOD(CToastNotifier, set_app_id), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("toast_notifier", "bool show(const string &in title, const string &in body)", asMETHOD(CToastNotifier, show_text), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("toast_notifier", "bool show(const string &in title, const string &in body, const string &in buttons, const string &in actions)", asMETHOD(CToastNotifier, show_with_actions_no_image), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("toast_notifier", "bool install_shortcut(const string &in app_id, const string &in app_name)", asMETHOD(CToastNotifier, install_shortcut), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("toast_notifier", "string get_action()", asMETHOD(CToastNotifier, get_action), asCALL_THISCALL);
	script_engine->RegisterObjectType("tray_icon", sizeof(CTrayIcon), asOBJ_REF);
	script_engine->RegisterObjectBehaviour("tray_icon", asBEHAVE_FACTORY, "tray_icon@ f()", asFUNCTION(TrayIconFactory), asCALL_CDECL);
	script_engine->RegisterObjectBehaviour("tray_icon", asBEHAVE_ADDREF, "void f()", asMETHOD(CTrayIcon, AddRef), asCALL_THISCALL);
	script_engine->RegisterObjectBehaviour("tray_icon", asBEHAVE_RELEASE, "void f()", asMETHOD(CTrayIcon, Release), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "bool add(const string &in tooltip)", asMETHOD(CTrayIcon, add), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "void remove()", asMETHOD(CTrayIcon, remove), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "void set_tooltip(const string &in tooltip)", asMETHOD(CTrayIcon, set_tooltip), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "void set_menu_items(const string &in items)", asMETHOD(CTrayIcon, set_menu_items), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "bool is_clicked()", asMETHOD(CTrayIcon, is_clicked), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "int get_menu_click()", asMETHOD(CTrayIcon, get_menu_click), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "void hide_window(uint64 handle)", asMETHOD(CTrayIcon, hide_window), asCALL_THISCALL);
	script_engine->RegisterObjectMethod("tray_icon", "void show_window(uint64 handle)", asMETHOD(CTrayIcon, show_window), asCALL_THISCALL);
	return true;
}
