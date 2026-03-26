#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <sstream>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include "../../src/nvgt_plugin.h"

using std::string;
using std::wstring;

const int CANVAS_W = 800;
const int CANVAS_H = 600;

class CGraphics {
public:
	CGraphics() : m_refCount(1), m_window(nullptr), m_renderer(nullptr), m_initialized(false), m_currentFontId(-1) {
	}

	~CGraphics() {
		unload_all_images();
		unload_all_fonts();
		if (m_renderer) SDL_DestroyRenderer(m_renderer);
		if (m_window) SDL_DestroyWindow(m_window);
	}

	void AddRef() { asAtomicInc(m_refCount); }
	void Release() { if (asAtomicDec(m_refCount) == 0) delete this; }

	bool setup(uint64_t window_handle_ptr) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (m_initialized) return true;
		if (SDL_Init(SDL_INIT_VIDEO) == false) return false;
		if (TTF_Init() == false) return false;
		
		SDL_PropertiesID props = SDL_CreateProperties();
		SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, (void*)window_handle_ptr);
		m_window = SDL_CreateWindowWithProperties(props);
		SDL_DestroyProperties(props);
		if (!m_window) return false;
		
		m_renderer = SDL_CreateRenderer(m_window, NULL);
		if (!m_renderer) return false;
		
		SDL_SetRenderLogicalPresentation(m_renderer, CANVAS_W, CANVAS_H, SDL_LOGICAL_PRESENTATION_STRETCH);
		m_initialized = true;
		return true;
	}

	int load_image(const string& filename) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return -1;
		
		SDL_Texture* tex = IMG_LoadTexture(m_renderer, filename.c_str());
		if (!tex) return -1;
		int id = m_nextImageId++;
		m_imageMap[id] = tex;
		return id;
	}

	void draw_image(int id, int x, int y, int w, int h) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		auto it = m_imageMap.find(id);
		if (it != m_imageMap.end()) {
			SDL_FRect dst = { (float)x, (float)y, (float)w, (float)h };
			SDL_RenderTexture(m_renderer, it->second, NULL, &dst);
		}
	}

	void draw_image_region(int id, int x, int y, int w, int h, int sx, int sy, int sw, int sh) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		auto it = m_imageMap.find(id);
		if (it != m_imageMap.end()) {
			SDL_FRect src = { (float)sx, (float)sy, (float)sw, (float)sh };
			SDL_FRect dst = { (float)x, (float)y, (float)w, (float)h };
			SDL_RenderTexture(m_renderer, it->second, &src, &dst);
		}
	}

	void unload_all_images() {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		for (auto& pair : m_imageMap) {
			SDL_DestroyTexture(pair.second);
		}
		m_imageMap.clear();
	}

	int load_font(const string& fontName, float size, bool bold, bool italic) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		string path = fontName;
		
		if (path.find("/") == string::npos && path.find("\\") == string::npos && path.find(":") == string::npos) {
#ifdef _WIN32
			path = "C:/Windows/Fonts/" + fontName + ".ttf";
#else
			path = "/system/fonts/" + fontName + ".ttf";
#endif
		}
		
		TTF_Font* font = TTF_OpenFont(path.c_str(), size);
		
#ifdef __ANDROID__
		if (!font) {
			font = TTF_OpenFont(fontName.c_str(), size);
		}
#endif

		if (!font) return -1;

		int style = TTF_STYLE_NORMAL;
		if (bold) style |= TTF_STYLE_BOLD;
		if (italic) style |= TTF_STYLE_ITALIC;
		TTF_SetFontStyle(font, style);

		int id = m_nextFontId++;
		m_fontMap[id] = font;
		if (m_currentFontId == -1) m_currentFontId = id;
		return id;
	}

	bool select_font(int id) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (m_fontMap.count(id)) {
			m_currentFontId = id;
			return true;
		}
		return false;
	}

	void unload_all_fonts() {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		for (auto& pair : m_fontMap) {
			TTF_CloseFont(pair.second);
		}
		m_fontMap.clear();
		m_currentFontId = -1;
	}

	void draw_text(const string& text, int x, int y, int r, int g, int b) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized || m_currentFontId == -1 || text.empty()) return;
		
		TTF_Font* font = m_fontMap[m_currentFontId];
		SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, 255 };
		SDL_Surface* surf = TTF_RenderText_Blended(font, text.c_str(), text.length(), color);
		if (surf) {
			SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surf);
			if (tex) {
				SDL_FRect dst = { (float)x, (float)y, (float)surf->w, (float)surf->h };
				SDL_RenderTexture(m_renderer, tex, NULL, &dst);
				SDL_DestroyTexture(tex);
			}
			SDL_DestroySurface(surf);
		}
	}

	uint64_t measure_text(const string& text) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized || m_currentFontId == -1) return 0;
		int w, h;
		TTF_GetStringSize(m_fontMap[m_currentFontId], text.c_str(), text.length(), &w, &h);
		return ((uint64_t)w << 32) | (uint32_t)h;
	}

	void draw_text_wrapped(const string& text, int x, int y, int w, int r, int g, int b) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized || m_currentFontId == -1 || text.empty()) return;

		TTF_Font* font = m_fontMap[m_currentFontId];
		SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, 255 };
		SDL_Surface* surf = TTF_RenderText_Blended_Wrapped(font, text.c_str(), text.length(), color, w);
		if (surf) {
			SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surf);
			if (tex) {
				SDL_FRect dst = { (float)x, (float)y, (float)surf->w, (float)surf->h };
				SDL_RenderTexture(m_renderer, tex, NULL, &dst);
				SDL_DestroyTexture(tex);
			}
			SDL_DestroySurface(surf);
		}
	}

	uint64_t measure_text_wrapped(const string& text, int w) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized || m_currentFontId == -1) return 0;
		int width, height;
		TTF_GetStringSizeWrapped(m_fontMap[m_currentFontId], text.c_str(), text.length(), w, &width, &height);
		return ((uint64_t)width << 32) | (uint32_t)height;
	}

	void clear_screen(int r, int g, int b) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		SDL_SetRenderDrawColor(m_renderer, r, g, b, 255);
		SDL_RenderClear(m_renderer);
	}

	void draw_rect(int x, int y, int w, int h, int r, int g, int b, bool filled) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		SDL_SetRenderDrawColor(m_renderer, r, g, b, 255);
		SDL_FRect rect = { (float)x, (float)y, (float)w, (float)h };
		if (filled) {
			SDL_RenderFillRect(m_renderer, &rect);
		} else {
			SDL_RenderRect(m_renderer, &rect);
		}
	}

	void draw_circle(int x, int y, int radius, int r, int g, int b, bool filled) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		SDL_SetRenderDrawColor(m_renderer, r, g, b, 255);
		int offsetx = 0;
		int offsety = radius;
		int d = radius - 1;
		int status = 0;
		
		while (offsety >= offsetx) {
			if (filled) {
				SDL_RenderLine(m_renderer, (float)(x - offsety), (float)(y + offsetx), (float)(x + offsety), (float)(y + offsetx));
				SDL_RenderLine(m_renderer, (float)(x - offsetx), (float)(y + offsety), (float)(x + offsetx), (float)(y + offsety));
				SDL_RenderLine(m_renderer, (float)(x - offsetx), (float)(y - offsety), (float)(x + offsetx), (float)(y - offsety));
				SDL_RenderLine(m_renderer, (float)(x - offsety), (float)(y - offsetx), (float)(x + offsety), (float)(y - offsetx));
			} else {
				SDL_RenderPoint(m_renderer, (float)(x + offsetx), (float)(y + offsety));
				SDL_RenderPoint(m_renderer, (float)(x + offsety), (float)(y + offsetx));
				SDL_RenderPoint(m_renderer, (float)(x - offsetx), (float)(y + offsety));
				SDL_RenderPoint(m_renderer, (float)(x - offsety), (float)(y + offsetx));
				SDL_RenderPoint(m_renderer, (float)(x + offsetx), (float)(y - offsety));
				SDL_RenderPoint(m_renderer, (float)(x + offsety), (float)(y - offsetx));
				SDL_RenderPoint(m_renderer, (float)(x - offsetx), (float)(y - offsety));
				SDL_RenderPoint(m_renderer, (float)(x - offsety), (float)(y - offsetx));
			}
			if (status >= 2 * offsetx) {
				status -= 2 * offsetx + 1;
				offsetx += 1;
			} else if (d < 2 * radius) {
				status += 2 * offsety - 1;
				offsety -= 1;
			} else {
				status -= 2 * (offsetx - offsety + 1);
				offsety -= 1;
				offsetx += 1;
			}
		}
	}

	void draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int thickness) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		SDL_SetRenderDrawColor(m_renderer, r, g, b, 255);
		SDL_RenderLine(m_renderer, (float)x1, (float)y1, (float)x2, (float)y2);
	}

	void draw_menu(const string& items_str, int selected_index, int x, int y) {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized || m_currentFontId == -1) return;
		
		TTF_Font* font = m_fontMap[m_currentFontId];
		std::vector<string> items;
		string token;
		std::istringstream tokenStream(items_str);
		while (std::getline(tokenStream, token, '|')) {
			items.push_back(token);
		}
		
		int currentY = y;
		int w, h;
		TTF_GetStringSize(font, "A", 1, &w, &h);
		int lineHeight = h + 10;
		
		for (size_t i = 0; i < items.size(); i++) {
			TTF_GetStringSize(font, items[i].c_str(), items[i].length(), &w, &h);
			int textX = x - (w / 2);
			if (i == (size_t)selected_index) {
				draw_rect(textX - 10, currentY, w + 20, lineHeight, 50, 50, 50, true);
				draw_text(items[i], textX, currentY + 5, 255, 255, 0);
			} else {
				draw_text(items[i], textX, currentY + 5, 200, 200, 200);
			}
			currentY += lineHeight;
		}
	}

	void present() {
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		if (!m_initialized) return;
		SDL_RenderPresent(m_renderer);
	}

private:
	int m_refCount;
	bool m_initialized;
	SDL_Window* m_window;
	SDL_Renderer* m_renderer;
	std::recursive_mutex m_mutex;
	std::map<int, SDL_Texture*> m_imageMap;
	std::map<int, TTF_Font*> m_fontMap;
	int m_nextImageId = 1;
	int m_nextFontId = 1;
	int m_currentFontId;
};

CGraphics* GfxFactory() { return new CGraphics(); }

plugin_main(nvgt_plugin_shared* shared) {
	asIScriptEngine* engine = shared->script_engine;
	if (!prepare_plugin(shared)) return false;
	
	engine->RegisterObjectType("imige_renderer", sizeof(CGraphics), asOBJ_REF);
	engine->RegisterObjectBehaviour("imige_renderer", asBEHAVE_FACTORY, "imige_renderer@ f()", asFUNCTION(GfxFactory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("imige_renderer", asBEHAVE_ADDREF, "void f()", asMETHOD(CGraphics, AddRef), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("imige_renderer", asBEHAVE_RELEASE, "void f()", asMETHOD(CGraphics, Release), asCALL_THISCALL);
	
	engine->RegisterObjectMethod("imige_renderer", "bool setup(uint64 window_handle)", asMETHOD(CGraphics, setup), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "int load_image(const string &in filename)", asMETHOD(CGraphics, load_image), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_image(int id, int x, int y, int w, int h)", asMETHOD(CGraphics, draw_image), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_image_region(int id, int x, int y, int w, int h, int sx, int sy, int sw, int sh)", asMETHOD(CGraphics, draw_image_region), asCALL_THISCALL);
	
	engine->RegisterObjectMethod("imige_renderer", "int load_font(const string &in font_name, float size, bool bold, bool italic)", asMETHOD(CGraphics, load_font), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "bool select_font(int id)", asMETHOD(CGraphics, select_font), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_text(const string &in text, int x, int y, int r, int g, int b)", asMETHOD(CGraphics, draw_text), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "uint64 measure_text(const string &in text)", asMETHOD(CGraphics, measure_text), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_text_wrapped(const string &in text, int x, int y, int w, int r, int g, int b)", asMETHOD(CGraphics, draw_text_wrapped), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "uint64 measure_text_wrapped(const string &in text, int w)", asMETHOD(CGraphics, measure_text_wrapped), asCALL_THISCALL);
	
	engine->RegisterObjectMethod("imige_renderer", "void clear_screen(int r, int g, int b)", asMETHOD(CGraphics, clear_screen), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_rect(int x, int y, int w, int h, int r, int g, int b, bool filled)", asMETHOD(CGraphics, draw_rect), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_circle(int x, int y, int radius, int r, int g, int b, bool filled)", asMETHOD(CGraphics, draw_circle), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int thickness)", asMETHOD(CGraphics, draw_line), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void draw_menu(const string &in items, int selected_index, int x, int y)", asMETHOD(CGraphics, draw_menu), asCALL_THISCALL);
	engine->RegisterObjectMethod("imige_renderer", "void present()", asMETHOD(CGraphics, present), asCALL_THISCALL);
	
	return true;
}
