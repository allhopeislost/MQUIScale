// MQUIScale.cpp : Feasibility prototype that draws the RoF2 interface larger
// while leaving the 3D world untouched.
//
// How it works:
//
//  * The RoF2 client draws its whole UI (windows, icons, text) from a single
//    function, C2DPrimitiveManager::Render, in EQGraphicsDX9.dll. We detour it
//    only to raise a flag while it runs ("we are drawing UI now").
//  * Inside that function the client draws pre-transformed vertices
//    (FVF 0x44, 0x144 and 0x1C4, all containing D3DFVF_XYZRHW) from its own
//    vertex buffers with DrawPrimitive / DrawIndexedPrimitive.
//  * We detour those two IDirect3DDevice9 methods. When the flag is up and the
//    active FVF is XYZRHW (and no vertex shader is bound), we read the vertex
//    range the draw uses, write a scaled copy (x * f, y * f, around 0,0) into a
//    vertex buffer we own, and draw from that instead. The client's buffer is
//    never modified.
//  * MacroQuest's ImGui is drawn later, from the EndScene hook, and uses a
//    non-RHW FVF, so it never passes either check.
//
// Because the UI is drawn f times larger, it has to live in a smaller
// "logical" screen of (width / f) x (height / f). See "UI space" below:
//  * CXWndManager::ScreenExtentX/Y is set to the logical size.
//  * Windows outside the logical screen are moved inside (and moved back at
//    1.0 or on unload).
//  * Mouse coordinates are divided by f before the UI sees them. The 3D world
//    still receives the raw coordinates.
//
// Deliberately NOT handled in this prototype (notes on where to do it later):
//  * Name tags above heads: drawn outside C2DPrimitiveManager (DrawNameTagText).
//  * Inventory doll: CRender::DrawObjectPreview / ResizeObjectPreview.
//
// Everything is undone by unloading the plugin: detours are removed, the
// screen extent and window positions are restored and our vertex buffer is
// released. The default factor is 1.0, which changes nothing.

#include <mq/Plugin.h>

#include <d3d9.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <map>

PreSetup("MQUIScale");
PLUGIN_VERSION(2.6);

// mq::AddRenderCallbacks (MacroQuest's render callback API, available through
// mq/Plugin.h) is used to release our D3DPOOL_DEFAULT buffer when the device
// is reset.

//============================================================================
// State

static constexpr float MinScale = 1.0f;
static constexpr float MaxScale = 3.0f;

static float s_scale = 1.0f;              // current UI scale factor
static int s_iniDepth = 0;                 // > 0 while EQ loads/stores a window's ini info
static bool s_inLoginFrontend = false;     // login / server select (eqmain.dll) is running
static bool s_scalePending = false;        // scale from MQUIScale.ini not applied yet
static float s_pendingScale = 1.0f;
static bool s_showWindow = true;           // ImGui slider window
static bool s_inUIPass = false;            // true while C2DPrimitiveManager::Render runs
static bool s_hooksInstalled = false;
static bool s_uiHookInstalled = false;
static int s_renderCallbacksId = -1;

static uintptr_t s_addrDrawPrimitive = 0;
static uintptr_t s_addrDrawIndexedPrimitive = 0;
static uintptr_t s_addrSetScissorRect = 0;

// Our own vertex buffer that receives the scaled copies.
static IDirect3DVertexBuffer9* s_scaledVB = nullptr;
static UINT s_scaledVBSize = 0;

// Per-frame counters for the ImGui window (useful for the test report).
struct DrawStats
{
	int scaled = 0;        // UI draws we scaled
	int notRHW = 0;        // draws during the UI pass that were not XYZRHW
	int failed = 0;        // UI draws we could not scale (lock/read failed), drawn unscaled
	int uiPasses = 0;      // number of C2DPrimitiveManager::Render calls
	int keptVertices = 0;  // vertices left unscaled by the name tag filter
	int sharpText = 0;     // text draws that used a twin texture
	int widenedGlyphs = 0; // glyph quads widened for their overhang
	int condensedLines = 0;    // Even: lines short of room (glyphs drawn narrower)
	int shrunkSpaceLines = 0;  // Even: of those, lines whose spaces gave room
	int grownLines = 0;         // Even, option C: lines that took free room in their label
	float minDrawScale = 1.0f; // Even: narrowest glyph scale this frame
	// Bounding box of the UI vertices before scaling (diagnostics: shows the
	// coordinate space the UI is really drawn in).
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
};
static DrawStats s_stats;      // being collected this frame
static DrawStats s_lastStats;  // last completed frame, shown in the window

// Name tag filter.
//
// Test result: name tags above heads are also drawn inside the UI pass, as
// XYZRHW text, so they were scaled away from their heads. They are drawn with
// CCachedFont::DrawNameTagText, which takes a depth (z) per tag, while window
// text (CCachedFont::DrawWrappedText) has no z parameter.
//
// Test results (/uiscale dump and screenshots): the depth (z) cannot tell
// name tags apart. Window borders, icons and window text are all drawn at
// many different z values, so a z filter left most of the UI unscaled.
//
// What does work: windows are placed in the logical screen (see "UI space"),
// so all window content - including its text - lies inside the logical rect
// of some visible top-level window. Name tags are placed by the 3D camera in
// real screen coordinates and normally are not inside a window. So for text
// (stride 32, FONT_D3DTLVERTEX) each glyph (4 vertices) is scaled only when
// its center lies inside a visible window. Quads (stride 20 and 28) are
// always scaled. A name tag that happens to overlap a window gets scaled;
// window-less text (if any) does not.
static constexpr UINT TextVertexStride = sizeof(FONT_D3DTLVERTEX);   // 32
static bool s_nameTagFilter = true;

// Test result: with the window rect test, random parts of name tags vanished.
// Window rects are logical coordinates, name tags are real screen
// coordinates, so a name tag glyph was scaled away wherever it crossed the
// logical rect of a window - which on screen is somewhere else entirely.
//
// Better: the glyph writer at 0x100B4540 in EQGraphicsDX9.dll is only called
// by CCachedFont::DrawNameTagText (via 0x100B4890). It writes 4 vertices with
// specular 0xFF000000, like all text. We detour it and change the specular of
// those vertices to a marker; the draw hook recognizes the marker, leaves the
// glyph unscaled and writes the normal value back. With specular lighting off
// (the 2D UI) the value is not used; even if it were, the marker differs by
// at most 3/255 in color and has the same alpha (fog).
static constexpr uint32_t NameTagSpecular = 0xFF000000;
static constexpr uint32_t NameTagSpecularMarker = 0xFF010203;

// Even mode, "Use label room" (option C). The general glyph writer
// (0x100B4340) gets the label's clip rect with every glyph. Its hook writes
// an index into s_clipRects in the specular of the 4 vertices (top byte
// ClipMarkerTag), so the draw hook knows how much room each label has. Text
// objects keep their vertices, so the index must stay valid: the table only
// grows (cleared, with a relayout, when it gets very large). The draw hook
// puts GlyphSpecular back before drawing.
static constexpr uint32_t GlyphSpecular = 0xFF000000;
static constexpr uint32_t ClipMarkerTag = 0xFE000000;
static bool s_useLabelRoom = true;
static bool s_clipMarksWanted = false;   // marks are written (C on, Even, scaled)
static std::vector<RECT> s_clipRects = { RECT{} };   // index 0 = none
static std::map<std::tuple<LONG, LONG, LONG, LONG>, uint32_t> s_clipRectIds;

static uint32_t ClipRectId(const RECT& r)
{
	const auto key = std::make_tuple(r.left, r.top, r.right, r.bottom);
	auto it = s_clipRectIds.find(key);
	if (it != s_clipRectIds.end())
		return it->second;
	if (s_clipRects.size() >= 0x100000)
		return 0;   // full; ResetClipRects runs on the next pulse
	const uint32_t id = static_cast<uint32_t>(s_clipRects.size());
	s_clipRects.push_back(r);
	s_clipRectIds.emplace(key, id);
	return id;
}

static const RECT* ClipRectOf(uint32_t specular)
{
	if ((specular & 0xFF000000u) != ClipMarkerTag)
		return nullptr;
	const uint32_t id = specular & 0x00FFFFFFu;
	return id > 0 && id < s_clipRects.size() ? &s_clipRects[id] : nullptr;
}
static bool s_nameTagMarkerInstalled = false;

// Texture filter for the enlarged UI. Enlarging stretches EQ's small UI
// bitmaps (glyphs, icons). "Sharp" (point sampling) turns every texel into a
// block - crisp and even at whole factors (2.0, 3.0), uneven at fractional
// ones. "Smooth" (linear) blends texels - no jagged edges, a little softer.
// Only applied around our scaled draws and restored right after, so the
// world and the game's own state cache are unaffected.
enum class TextureFilter { Game = 0, Sharp = 1, Smooth = 2 };
static TextureFilter s_textureFilter = TextureFilter::Game;

struct SamplerOverride
{
	DWORD oldMin = 0, oldMag = 0;
	bool active = false;

	// Test result: linear filtering on text made it darker and warmer in tone
	// (it blends each glyph with the transparent/shadow texels around it in
	// the font atlas). So "Smooth" keeps text point-sampled and only smooths
	// icons, borders and bars.
	void Apply(IDirect3DDevice9* device, bool isText)
	{
		if (s_textureFilter == TextureFilter::Game)
			return;

		const DWORD filter = (s_textureFilter == TextureFilter::Sharp || isText) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
		if (FAILED(device->GetSamplerState(0, D3DSAMP_MINFILTER, &oldMin))
			|| FAILED(device->GetSamplerState(0, D3DSAMP_MAGFILTER, &oldMag)))
			return;

		device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
		active = true;
	}

	void Restore(IDirect3DDevice9* device)
	{
		if (!active)
			return;
		device->SetSamplerState(0, D3DSAMP_MINFILTER, oldMin);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, oldMag);
		active = false;
	}
};

// Visible top-level window rects in logical coordinates, taken at the start
// of each UI pass.
static std::vector<CXRect> s_windowRects;

// Test result: tooltips (e.g. a spell gem's name) showed only part of their
// text. Tooltips are not windows: CXWnd::DrawTooltipAtPoint (0x8679B0) draws a
// box and text at a rect from GetTooltipRect (0x863150, or 0x8631C0 when the
// position is given). The box was scaled, but glyphs outside every window
// were taken for name tags and left small. So the tooltip rects count as
// window rects too. They are recorded while the UI is built and kept for one
// extra frame, in case a tooltip is built after the UI pass.
static std::vector<CXRect> s_tooltipRects;
static std::vector<CXRect> s_prevTooltipRects;

static void RecordTooltipRect(const CXRect* rect)
{
	if (rect && rect->right > rect->left && rect->bottom > rect->top && s_tooltipRects.size() < 16)
		s_tooltipRects.push_back(*rect);
}

static void SnapshotWindowRects()
{
	s_windowRects.clear();
	s_windowRects.insert(s_windowRects.end(), s_tooltipRects.begin(), s_tooltipRects.end());
	s_windowRects.insert(s_windowRects.end(), s_prevTooltipRects.begin(), s_prevTooltipRects.end());
	if (!pWndMgr)
		return;

	for (int i = 0; i < pWndMgr->pWindows.GetCount(); ++i)
	{
		CXWnd* wnd = pWndMgr->pWindows[i];
		if (wnd && wnd->GetParentWindow() == nullptr && wnd->IsVisible() && wnd != pWndMgr->MainWindow)
			s_windowRects.push_back(wnd->GetLocation());
	}
}

static bool IsInsideWindow(float x, float y)
{
	for (const CXRect& rc : s_windowRects)
	{
		if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom)
			return true;
	}
	return false;
}

//============================================================================
// Helpers

static IDirect3DDevice9* GetGameDevice()
{
	if (pGraphicsEngine && pGraphicsEngine->pRender)
		return pGraphicsEngine->pRender->pD3DDevice;
	return nullptr;
}

static void ReleaseScaledVB()
{
	if (s_scaledVB)
	{
		s_scaledVB->Release();
		s_scaledVB = nullptr;
	}
	s_scaledVBSize = 0;
}

static bool EnsureScaledVB(IDirect3DDevice9* device, UINT size)
{
	if (s_scaledVB && s_scaledVBSize >= size)
		return true;

	ReleaseScaledVB();

	HRESULT hr = device->CreateVertexBuffer(size, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		0, D3DPOOL_DEFAULT, &s_scaledVB, nullptr);
	if (FAILED(hr))
	{
		s_scaledVB = nullptr;
		return false;
	}

	s_scaledVBSize = size;
	return true;
}

static UINT VertexCountForPrimitives(D3DPRIMITIVETYPE type, UINT primitiveCount)
{
	switch (type)
	{
	case D3DPT_POINTLIST:     return primitiveCount;
	case D3DPT_LINELIST:      return primitiveCount * 2;
	case D3DPT_LINESTRIP:     return primitiveCount + 1;
	case D3DPT_TRIANGLELIST:  return primitiveCount * 3;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_TRIANGLEFAN:   return primitiveCount + 2;
	default:                  return 0;
	}
}

// True when the current draw is a UI draw we should scale.
// Test result: the loading screen between zones was drawn off center (to the
// bottom right). It is drawn in the UI pass, centered on the real screen, so
// scaling it moved it. While zoning (MacroQuest's gbInZone is false from
// BeginZone until EndZone, and outside the world/character select) nothing
// is scaled; there are no windows to use then anyway. The login frontend is
// scaled too (see "Login and server select").
// Entering the world from character select does not go through BeginZone,
// and gbInZone/gGameState are only refreshed by MacroQuest's pulse, which does
// not run while the world loads (gbInZone stays true from character select).
// So the live game state is checked too: the load runs outside
// CHARSELECT/CHARCREATE/INGAME (assumed POSTCHARSELECT; changes are logged).
static void LogLiveGameState(int state)
{
	static int lastState = INT_MIN;
	if (state == lastState)
		return;
	lastState = state;
	char path[MAX_PATH];
	sprintf_s(path, "%s\\MQUIScale.log", gPathLogs);
	FILE* file = nullptr;
	if (fopen_s(&file, path, "a") == 0 && file)
	{
		fprintf(file, "live game state %d (gGameState %d, gbInZone %d, tick %llu)\n",
			state, gGameState, gbInZone ? 1 : 0, MQGetTickCount64());
		fclose(file);
	}
}

static bool IsScalingActive()
{
	if (!s_inUIPass || s_scale == 1.0f)
		return false;
	if (s_inLoginFrontend)
		return true;
	const int state = GetGameState();
	LogLiveGameState(state);
	return gbInZone && (state == GAMESTATE_INGAME || state == GAMESTATE_CHARSELECT || state == GAMESTATE_CHARCREATE);
}

static bool IsScalableUIDraw(IDirect3DDevice9* device)
{
	if (!IsScalingActive())
		return false;

	// With a vertex shader bound, XYZRHW is not used; leave it alone.
	IDirect3DVertexShader9* shader = nullptr;
	if (SUCCEEDED(device->GetVertexShader(&shader)) && shader != nullptr)
	{
		shader->Release();
		++s_stats.notRHW;
		return false;
	}

	DWORD fvf = 0;
	if (FAILED(device->GetFVF(&fvf)) || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
	{
		++s_stats.notRHW;
		return false;
	}

	return true;
}

//============================================================================
// Sharp text
//
// Research (EQGraphicsDX9.dll):
//  * CCachedFont rasterizes every glyph on its own with GDI (ExtTextOutW,
//    white on black) into a 32 bpp DIB of nMaxWidth x nHeight (0x100B40B0,
//    and 0x100B41F0 when a cached glyph lost its texture slot), then copies
//    that cell into its font texture at gti.rectInTexture.left/top
//    (0x100B0450). Font textures are D3DFMT_A4R4G4B4, D3DPOOL_MANAGED.
//  * Text vertices use u = texel x / fTextureSize, v likewise: normalized.
//
// So a texture k times as large, with every cell at k times its position,
// has exactly the same UVs. We keep such a "twin" per font texture. Whenever
// the game uploads a glyph we render the same character again with a k times
// larger GDI font into the twin, and when window text is drawn we bind the
// twin instead of the game's texture. Layout and character widths stay the
// game's own (GDI at 1x); only the pixels get finer. The twin also has 8-bit
// alpha instead of the game's 4-bit.

static bool s_sharpText = true;
static bool s_uploadHookInstalled = false;   // without it, twins would miss new glyphs

static constexpr uintptr_t CCachedFont__UploadGlyph_x = 0x100B0450;   // thiscall (const gti&), ret 4
static constexpr uint8_t UploadGlyphPrologue[4] = { 0x83, 0xEC, 0x20, 0x53 };
static constexpr uintptr_t ExtTextOutW_IAT_x = 0x10131034;           // EQGraphicsDX9's import of ExtTextOutW

// Resolution of the twins for the current scale: 2 or 3, 0 = no twins.
static int SharpTextFactor()
{
	if (!s_sharpText || !s_uploadHookInstalled || s_scale <= 1.0f)
		return 0;
	return std::clamp(static_cast<int>(std::ceil(s_scale - 0.001f)), 2, 3);
}

// A k times larger copy of one CCachedFont: own DC, font and DIB.
struct SharpFont
{
	HFONT gameFont = nullptr;   // the CCachedFont's hFont this was made for
	int k = 0;
	HDC dc = nullptr;
	HFONT font = nullptr;
	HGDIOBJ oldFont = nullptr;
	HBITMAP bitmap = nullptr;
	HGDIOBJ oldBitmap = nullptr;
	uint32_t* bits = nullptr;
	int width = 0;              // k * nMaxWidth
	int height = 0;             // k * nHeight
};

// How glyphs are drawn whose k x ink is wider than k times the game's advance
// (the part of the twin the game's quad shows), e.g. W and M in small fonts.
enum class WideGlyphs
{
	Squeeze,    // condensed with a world transform until it fits (v1.3)
	Overhang,   // drawn at full width; the draw hook widens the glyph's quad
	Smaller,    // drawn with a smaller (still hinted) font that fits
	Spacing,    // like Overhang to the right, and the game's advance grows to make room
	Even,       // every glyph at full width, spread evenly over its word (words stay put)
};
static WideGlyphs s_wideGlyphs = WideGlyphs::Overhang;
// Overhang mode: share of the overhang moved to the left of the glyph (into
// the space before it, so it runs less into the next letter), and how much a
// wide glyph is condensed first (0.1 = at most 10 %) before it overhangs.
static float s_overhangLeft = 0.5f;
static float s_overhangSqueeze = 0.0f;
// Spacing mode: only glyphs that overhang at least this much (in game pixels)
// get extra advance, and then all of it (rounded up). Smaller overhangs stay
// overhangs, so the game's layout changes only for the really wide letters.
static float s_spacingMinOverhang = 1.5f;

static const char* WideGlyphsName(WideGlyphs mode)
{
	return mode == WideGlyphs::Squeeze ? "squeeze" : mode == WideGlyphs::Smaller ? "smaller"
		: mode == WideGlyphs::Spacing ? "spacing" : mode == WideGlyphs::Even ? "even" : "overhang";
}

// Spacing mode. The game lays text out with CCachedFontGlyphInfo::nCharWidth
// (GetTextExtentPoint32W at 1x, 0x100B40F7); the glyph's texture rect was
// already allocated with it (0x100B3CD4), so changing it later only moves the
// following characters. Every change is remembered and undone when the twins
// are released (mode change, sharp text off, scale 1.0, unload).
struct WidenedChar
{
	CCachedFont* font = nullptr;
	wchar_t ch = 0;
	int original = 0;
	int widened = 0;
};
static std::vector<WidenedChar> s_widenedChars;
// Text objects keep their laid-out vertices until they are marked dirty, so
// after a width change they are all marked once (next pulse).
static bool s_relayoutText = false;
static int s_lastRelayoutCount = 0;   // text objects marked the last time

// A glyph cell whose ink runs past the game's quad (Overhang/Spacing), or any
// glyph cell (Even mode, which needs every glyph's k x metrics).
struct WideCell
{
	int right = 0;      // rectInTexture.right, game texels
	int overhang = 0;   // twin texels of ink right of k * right (<= 0: none)
	int advance = 0;    // Even: the k x font's advance (A + B + C), twin texels
	int ink = 0;        // Even: right edge of the ink (A + B), twin texels
	wchar_t ch = 0;
};

struct TwinTexture
{
	IDirect3DTexture9* texture = nullptr;
	UINT sourceSize = 0;        // width of the game's texture it belongs to
	UINT sourceHeight = 0;
	std::map<uint32_t, WideCell> wideCells;   // key: top << 16 | left, game texels
};

static uint32_t WideCellKey(int top, int left)
{
	return (static_cast<uint32_t>(top) << 16) | static_cast<uint32_t>(left & 0xFFFF);
}

static std::map<CCachedFont*, SharpFont> s_sharpFonts;
static std::map<IDirect3DBaseTexture9*, TwinTexture> s_twins;   // game's font texture -> twin
static int s_twinFactor = 0;      // k the twins were made with, 0 = none
static bool s_twinsDirty = true;  // rebuild on the next pulse

// "Condense letters": in Even mode a line that is short of room draws its
// glyphs narrower along with their spacing (v2.2) instead of overlapping.
static bool s_evenCondense = true;

// The character the game last rasterized, taken from its ExtTextOutW call.
// The upload function only receives the texture slot, not the character.
using ExtTextOutW_t = BOOL(WINAPI*)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
static ExtTextOutW_t s_origExtTextOutW = nullptr;
static uintptr_t s_extTextOutIAT = 0;
static HDC s_lastGlyphDC = nullptr;
static wchar_t s_lastGlyphChar = 0;

static BOOL WINAPI ExtTextOutW_Hook(HDC dc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count, const INT* dx)
{
	if (count == 1 && text)
	{
		s_lastGlyphDC = dc;
		s_lastGlyphChar = text[0];
	}
	return s_origExtTextOutW(dc, x, y, options, rect, text, count, dx);
}

static void DestroySharpFont(SharpFont& sf)
{
	if (sf.dc)
	{
		if (sf.oldFont) SelectObject(sf.dc, sf.oldFont);
		if (sf.oldBitmap) SelectObject(sf.dc, sf.oldBitmap);
		DeleteDC(sf.dc);
	}
	if (sf.font) DeleteObject(sf.font);
	if (sf.bitmap) DeleteObject(sf.bitmap);
	sf = SharpFont();
}

static bool CreateSharpFont(CCachedFont* font, int k, SharpFont& sf)
{
	LOGFONTA lf;
	if (!font->hFont || GetObjectA(font->hFont, sizeof(lf), &lf) != sizeof(lf))
		return false;

	sf.gameFont = font->hFont;
	sf.k = k;
	sf.width = font->nMaxWidth * k;
	sf.height = font->nHeight * k;
	if (sf.width <= 0 || sf.height <= 0)
		return false;

	sf.dc = CreateCompatibleDC(nullptr);
	if (!sf.dc)
		return false;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = sf.width;
	bmi.bmiHeader.biHeight = -sf.height;   // top-down, like the game's DIB
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	sf.bitmap = CreateDIBSection(sf.dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!sf.bitmap || !bits)
		return false;
	sf.bits = static_cast<uint32_t*>(bits);
	sf.oldBitmap = SelectObject(sf.dc, sf.bitmap);

	// k times the game's font. GDI hinting does not scale exactly, so take the
	// largest size whose glyphs still fit in the k times larger cell.
	const LONG baseHeight = lf.lfHeight != 0 ? lf.lfHeight : font->nHeight;
	lf.lfWidth *= k;
	lf.lfQuality = ANTIALIASED_QUALITY;   // grayscale; ClearType would color the edges
	const HGDIOBJ original = GetCurrentObject(sf.dc, OBJ_FONT);
	for (LONG h = std::abs(baseHeight) * k; h > 0 && !sf.font; --h)
	{
		lf.lfHeight = baseHeight < 0 ? -h : h;
		HFONT candidate = CreateFontIndirectA(&lf);
		if (!candidate)
			break;

		SelectObject(sf.dc, candidate);
		TEXTMETRICA tm = {};
		if (GetTextMetricsA(sf.dc, &tm) && (tm.tmHeight <= sf.height || h == 1))
		{
			sf.font = candidate;
			sf.oldFont = original;
		}
		else
		{
			SelectObject(sf.dc, original);
			DeleteObject(candidate);
		}
	}
	if (!sf.font)
		return false;

	// The same DC setup as the game's (0x100B3ED0), plus GM_ADVANCED so wide
	// glyphs can be condensed with a world transform (WriteSharpGlyph).
	SetGraphicsMode(sf.dc, GM_ADVANCED);
	SetMapMode(sf.dc, MM_TEXT);
	SetBkColor(sf.dc, 0x000000);     // COLORREF values: eqlib's RGB type hides the macro
	SetTextColor(sf.dc, 0xFFFFFF);
	SetTextAlign(sf.dc, TA_LEFT | TA_TOP);
	return true;
}

static SharpFont* GetSharpFont(CCachedFont* font, int k)
{
	SharpFont& sf = s_sharpFonts[font];
	if (sf.dc && sf.k == k && sf.gameFont == font->hFont
		&& sf.width == font->nMaxWidth * k && sf.height == font->nHeight * k)
	{
		return &sf;
	}

	// New font, or a font object that was replaced at the same address.
	DestroySharpFont(sf);
	if (CreateSharpFont(font, k, sf))
		return &sf;

	DestroySharpFont(sf);
	return nullptr;
}

static IDirect3DTexture9* GetFontTexture(CCachedFont* font, int index)
{
	if (index < 0 || index >= font->arTextures.GetLength())
		return nullptr;
	CD3DTexturePointerNode* node = font->arTextures[index];
	return node ? node->pTexture : nullptr;
}

static void ReleaseTwin(TwinTexture& twin)
{
	if (twin.texture)
		twin.texture->Release();
	twin = TwinTexture();
}

static TwinTexture* GetOrCreateTwin(IDirect3DTexture9* source, int k)
{
	D3DSURFACE_DESC desc;
	if (!source || FAILED(source->GetLevelDesc(0, &desc)))
		return nullptr;

	TwinTexture& twin = s_twins[source];
	if (twin.texture && twin.sourceSize == desc.Width)
		return &twin;

	// New texture, or a different texture at the address of a released one.
	ReleaseTwin(twin);

	IDirect3DDevice9* device = GetGameDevice();
	if (!device || FAILED(device->CreateTexture(desc.Width * k, desc.Height * k, 1, 0,
		D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &twin.texture, nullptr)))
	{
		twin.texture = nullptr;
		return nullptr;
	}
	twin.sourceSize = desc.Width;
	twin.sourceHeight = desc.Height;

	// Transparent white, so filtering at glyph edges does not pull in a color.
	D3DLOCKED_RECT locked;
	if (SUCCEEDED(twin.texture->LockRect(0, &locked, nullptr, 0)))
	{
		for (UINT y = 0; y < desc.Height * k; ++y)
		{
			uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
			std::fill(row, row + desc.Width * k, 0x00FFFFFFu);
		}
		twin.texture->UnlockRect(0);
	}
	return &twin;
}

// Smaller mode: the largest smaller version of the k x font in which `ch`
// fits in `fitWidth`, or nullptr. `yOffset` keeps the baselines level.
static HFONT CreateFittingFont(const SharpFont& sf, wchar_t ch, int fitWidth, int& yOffset)
{
	LOGFONTA lf;
	TEXTMETRICA tm = {};
	if (GetObjectA(sf.font, sizeof(lf), &lf) != sizeof(lf) || !GetTextMetricsA(sf.dc, &tm))
		return nullptr;

	const LONG size = std::abs(lf.lfHeight);
	const HGDIOBJ current = GetCurrentObject(sf.dc, OBJ_FONT);
	for (LONG h = size - 1; h >= size * 2 / 3; --h)   // never more than a third smaller
	{
		lf.lfHeight = lf.lfHeight < 0 ? -h : h;
		HFONT candidate = CreateFontIndirectA(&lf);
		if (!candidate)
			break;

		SelectObject(sf.dc, candidate);
		ABC abc = {};
		TEXTMETRICA fitted = {};
		const bool fits = GetCharABCWidthsW(sf.dc, ch, ch, &abc) && GetTextMetricsA(sf.dc, &fitted)
			&& abc.abcA + static_cast<int>(abc.abcB) <= fitWidth;
		SelectObject(sf.dc, current);
		if (fits)
		{
			yOffset = tm.tmAscent - fitted.tmAscent;
			return candidate;
		}
		DeleteObject(candidate);
	}
	return nullptr;
}

static CCachedFontGlyphNode* FindGlyphNode(CCachedFont* font, wchar_t ch)
{
	if (static_cast<int>(ch) >= font->arGlyphs.GetLength())
		return nullptr;
	return font->arGlyphs[ch];
}

// Spacing mode: give `ch` `extra` more pixels of advance (once).
static void WidenCharWidth(CCachedFont* font, wchar_t ch, int extra)
{
	for (const WidenedChar& w : s_widenedChars)
	{
		if (w.font == font && w.ch == ch)
			return;
	}
	CCachedFontGlyphNode* node = FindGlyphNode(font, ch);
	if (!node || extra <= 0)
		return;
	const int original = node->gi.nCharWidth;
	node->gi.nCharWidth = original + extra;
	s_widenedChars.push_back({ font, ch, original, original + extra });
	s_relayoutText = true;
}

// Marks every text object dirty so it is laid out again with the current
// character widths. They are kept in one linked list (m_pPrev/m_pNext); we
// find it through any window's text object.
static void RelayoutTextObjects()
{
	s_relayoutText = false;
	if (!pWndMgr)
		return;

	CTextObjectBase* any = nullptr;
	for (int i = 0; i < pWndMgr->pWindows.GetCount() && !any; ++i)
	{
		CXWnd* wnd = pWndMgr->pWindows[i];
		if (wnd && wnd->pTextObject)
			any = static_cast<CTextObjectBase*>(wnd->pTextObject);
	}
	if (!any)
		return;

	constexpr int MaxObjects = 200000;   // guards against a broken list
	CTextObjectBase* head = any;
	for (int n = 0; head->m_pPrev && n < MaxObjects; ++n)
		head = head->m_pPrev;
	int count = 0;
	for (CTextObjectBase* obj = head; obj && count < MaxObjects; obj = obj->m_pNext, ++count)
		obj->m_dirty = true;
	s_lastRelayoutCount = count;
}

// Renders one character at k x into the twin of the texture the game put it in.
static void WriteSharpGlyph(CCachedFont* font, int k, wchar_t ch, const CCachedFontGlyphTextureInfo& gti)
{
	SharpFont* sf = GetSharpFont(font, k);
	TwinTexture* twinInfo = sf ? GetOrCreateTwin(GetFontTexture(font, gti.nTextureIdx), k) : nullptr;
	if (!twinInfo)
		return;
	IDirect3DTexture9* twin = twinInfo->texture;

	D3DSURFACE_DESC desc;
	if (FAILED(twin->GetLevelDesc(0, &desc)))
		return;

	const int x0 = gti.rectInTexture.left * k;
	const int y0 = gti.rectInTexture.top * k;
	const int w = std::min(sf->width, static_cast<int>(desc.Width) - x0);
	const int h = std::min(sf->height, static_cast<int>(desc.Height) - y0);
	if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0)
		return;

	// The quad only shows k times the game's advance (GetTextExtentPoint32W
	// at 1x, rect right - left). Small fonts are hinted narrow, so the k x
	// glyph can be up to a quarter wider (Arial 14, k = 2: W needs 25 of 20
	// texels) and its right side would be cut off. What happens to such
	// glyphs is s_wideGlyphs; narrower ones stay as they are.
	const int fitWidth = (gti.rectInTexture.right - gti.rectInTexture.left) * k;
	const uint32_t cellKey = WideCellKey(gti.rectInTexture.top, gti.rectInTexture.left);
	twinInfo->wideCells.erase(cellKey);   // the slot may have held another glyph

	float squeeze = 1.0f;
	HFONT smaller = nullptr;
	int yOffset = 0;
	ABC abc = {};
	if (fitWidth > 0 && GetCharABCWidthsW(sf->dc, ch, ch, &abc))
	{
		const int ink = abc.abcA + static_cast<int>(abc.abcB);
		if (s_wideGlyphs == WideGlyphs::Even)
		{
			// Every glyph at full width; the draw hook places them.
			const int advance = abc.abcA + static_cast<int>(abc.abcB) + abc.abcC;
			twinInfo->wideCells[cellKey] = { gti.rectInTexture.right, std::min(ink, w) - fitWidth,
				advance, std::min(ink, w), ch };
		}
		else if (ink > fitWidth)
		{
			switch (s_wideGlyphs)
			{
			case WideGlyphs::Squeeze:
				squeeze = static_cast<float>(fitWidth) / static_cast<float>(ink);
				break;
			case WideGlyphs::Overhang:
			{
				// Optionally condense a little first. The cell is nMaxWidth
				// wide, so the rest of the ink fits in the twin; the draw hook
				// shows the extra texels by widening the quad.
				squeeze = std::max(static_cast<float>(fitWidth) / static_cast<float>(ink), 1.0f - s_overhangSqueeze);
				const int drawnInk = static_cast<int>(std::ceil(ink * squeeze));
				if (const int overhang = std::min(drawnInk, w) - fitWidth; overhang > 0)
					twinInfo->wideCells[cellKey] = { gti.rectInTexture.right, overhang };
				break;
			}
			case WideGlyphs::Smaller:
				smaller = CreateFittingFont(*sf, ch, fitWidth, yOffset);
				break;
			case WideGlyphs::Spacing:
				// Ink overhangs to the right, into the room the wider advance makes.
				if (const int overhang = std::min(ink, w) - fitWidth; overhang > 0)
				{
					twinInfo->wideCells[cellKey] = { gti.rectInTexture.right, overhang };
					if (static_cast<float>(overhang) / k >= s_spacingMinOverhang - 0.01f)
						WidenCharWidth(font, ch, (overhang + k - 1) / k);
				}
				break;
			}
		}
	}

	// Clear the whole cell (the game's ETO_OPAQUE without a rect only clears
	// the character box) and draw the character at 0,0 like the game does.
	GdiFlush();
	PatBlt(sf->dc, 0, 0, sf->width, sf->height, BLACKNESS);
	if (squeeze < 1.0f)
	{
		XFORM xf = { squeeze, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		SetWorldTransform(sf->dc, &xf);
	}
	const HGDIOBJ previousFont = smaller ? SelectObject(sf->dc, smaller) : nullptr;
	ExtTextOutW(sf->dc, 0, yOffset, 0, nullptr, &ch, 1, nullptr);
	if (smaller)
	{
		SelectObject(sf->dc, previousFont);
		DeleteObject(smaller);
	}
	if (squeeze < 1.0f)
		ModifyWorldTransform(sf->dc, nullptr, MWT_IDENTITY);
	GdiFlush();

	RECT target = { x0, y0, x0 + w, y0 + h };
	D3DLOCKED_RECT locked;
	if (FAILED(twin->LockRect(0, &locked, &target, 0)))
		return;

	// White text, coverage in each channel (blue is what the game reads).
	// The game adds a black drop shadow at (nDropShadowOffsetX/Y) into the
	// empty pixels; we do the same at k times the offset, glyph over shadow.
	auto coverage = [sf](int x, int y) -> uint32_t {
		if (x < 0 || y < 0 || x >= sf->width || y >= sf->height)
			return 0;
		return sf->bits[y * sf->width + x] & 0xFF;
	};
	const int sx = font->nDropShadowOffsetX * k;
	const int sy = font->nDropShadowOffsetY * k;
	const bool shadow = sx != 0 || sy != 0;

	for (int y = 0; y < h; ++y)
	{
		uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
		for (int x = 0; x < w; ++x)
		{
			const uint32_t g = coverage(x, y);
			const uint32_t s = shadow ? coverage(x - sx, y - sy) : 0;
			const uint32_t a = g + s * (255 - g) / 255;
			if (a == 0)
			{
				row[x] = 0x00FFFFFFu;
				continue;
			}
			const uint32_t c = g * 255 / a;
			row[x] = (a << 24) | (c << 16) | (c << 8) | c;
		}
	}
	twin->UnlockRect(0);
}

static CFontManager* GetFontManager()
{
	for (int style = 0; style < 16; ++style)
	{
		if (CCachedFont* font = CCachedFont::Get(style))
			return font->pFontManager;
	}
	return nullptr;
}

// Fills the twins with every glyph the game already has in a texture.
static void SeedSharpText(int k)
{
	CFontManager* manager = GetFontManager();
	if (!manager)
		return;

	for (CCachedFontNode* node = manager->listFonts.head; node; node = node->next)
	{
		CCachedFont* font = node->pFont;
		if (!font)
			continue;

		for (int i = 0; i < font->arGlyphs.GetLength(); ++i)
		{
			const CCachedFontGlyphNode* glyph = font->arGlyphs[i];
			if (glyph && glyph->gi.bIsValidTexture)
				WriteSharpGlyph(font, k, static_cast<wchar_t>(glyph->gi.uUnicodeValue), glyph->gi.gti);
		}
	}
}

// Undoes Spacing mode. Fonts can be destroyed and recreated with the UI, so a
// width is only put back when the font still exists and still has our value.
static void RestoreCharWidths()
{
	if (CFontManager* manager = GetFontManager())
	{
		for (const WidenedChar& w : s_widenedChars)
		{
			bool alive = false;
			for (CCachedFontNode* node = manager->listFonts.head; node && !alive; node = node->next)
				alive = node->pFont == w.font;
			CCachedFontGlyphNode* glyph = alive ? FindGlyphNode(w.font, w.ch) : nullptr;
			if (glyph && glyph->gi.nCharWidth == w.widened)
				glyph->gi.nCharWidth = w.original;
		}
	}
	if (!s_widenedChars.empty())
		s_relayoutText = true;
	s_widenedChars.clear();
}

static void ReleaseSharpText()
{
	RestoreCharWidths();

	for (auto& [source, twin] : s_twins)
		ReleaseTwin(twin);
	s_twins.clear();

	for (auto& [font, sf] : s_sharpFonts)
		DestroySharpFont(sf);
	s_sharpFonts.clear();

	s_twinFactor = 0;
}

// The twins hold the glyphs as drawn in the old mode: rebuild them.
static void SetWideGlyphs(WideGlyphs mode)
{
	if (mode == s_wideGlyphs)
		return;
	s_wideGlyphs = mode;
	s_twinsDirty = true;
}

// Called every pulse: (re)builds the twins when the factor changed.
static void UpdateSharpText()
{
	const int k = SharpTextFactor();
	if (k == s_twinFactor && !s_twinsDirty)
		return;

	ReleaseSharpText();
	s_twinsDirty = false;
	if (k > 0 && GetGameDevice())
	{
		s_twinFactor = k;
		SeedSharpText(k);
	}
}

// The twin for the texture bound for this draw, or nullptr.
static const TwinTexture* FindBoundTwin(IDirect3DDevice9* device)
{
	if (s_twinFactor == 0 || s_twinsDirty || s_twins.empty())
		return nullptr;

	IDirect3DBaseTexture9* bound = nullptr;
	if (FAILED(device->GetTexture(0, &bound)) || !bound)
		return nullptr;
	bound->Release();   // only compared, the device keeps it alive

	auto it = s_twins.find(bound);
	return it != s_twins.end() && it->second.texture ? &it->second : nullptr;
}

// One glyph quad of a text draw (4 vertices, logical coordinates) and the
// twin cell it shows, if it shows a whole cell (not clipped by the game).
struct GlyphQuad
{
	uint8_t* vertices = nullptr;
	float minX = 0, maxX = 0, minY = 0, maxY = 0, minU = 0, maxU = 0;
	const WideCell* cell = nullptr;
	int cellLeft = 0;
	const RECT* clip = nullptr;   // the label's rect (option C), if marked
};

static GlyphQuad ReadGlyphQuad(uint8_t* quad, UINT stride, const TwinTexture& twin)
{
	GlyphQuad q;
	q.vertices = quad;
	q.minX = q.minY = q.minU = FLT_MAX;
	q.maxX = q.maxY = q.maxU = -FLT_MAX;
	float minV = FLT_MAX;
	for (UINT i = 0; i < 4; ++i)
	{
		const auto* v = reinterpret_cast<const FONT_D3DTLVERTEX*>(quad + i * stride);
		q.minX = std::min(q.minX, v->x);
		q.maxX = std::max(q.maxX, v->x);
		q.minY = std::min(q.minY, v->y);
		q.maxY = std::max(q.maxY, v->y);
		q.minU = std::min(q.minU, v->u);
		q.maxU = std::max(q.maxU, v->u);
		minV = std::min(minV, v->v);
	}
	q.clip = ClipRectOf(reinterpret_cast<const FONT_D3DTLVERTEX*>(quad)->specular);

	const float left = q.minU * twin.sourceSize;
	const float top = minV * twin.sourceHeight;
	const int topTexel = static_cast<int>(std::lround(top));
	const int leftTexel = static_cast<int>(std::lround(left));
	if (std::abs(top - topTexel) > 0.05f || std::abs(left - leftTexel) > 0.05f)
		return q;
	auto it = twin.wideCells.find(WideCellKey(topTexel, leftTexel));
	if (it != twin.wideCells.end()
		&& std::abs(q.maxU * twin.sourceSize - static_cast<float>(it->second.right)) < 0.05f)
	{
		q.cell = &it->second;
		q.cellLeft = leftTexel;
	}
	return q;
}

// Even mode, per line of text (glyphs on one row that touch or are separated
// by a space-sized gap): the line keeps its start and end, so alignment and
// the window layout stay the game's. Inside it, glyphs are placed by the k x
// font's own advances and drawn at full ink width. If the large font needs
// more room than the line has, the gaps between words give up to
// s_evenSpaceShrink of their width first; the rest is spread evenly over all
// glyphs. So what the small font loses to hinting no longer piles up at one
// letter. A clipped glyph (cell not whole) ends the line and is left alone.
static float s_evenSpaceShrink = 0.3f;
// Set by SpreadLines when a glyph in this draw was drawn narrower: with exact
// pixels that draw then samples linearly (point would drop texel columns).
static bool s_drawCondensed = false;

static void SpreadLines(uint8_t* base, UINT numVertices, UINT stride, const TwinTexture& twin)
{
	const float k = static_cast<float>(s_twinFactor);

	// A line: glyphs, each with the gap (in game pixels) that precedes it.
	struct Item { GlyphQuad q; float gapBefore; };
	std::vector<Item> line;
	float pendingGap = 0.0f;

	auto flush = [&]() {
		if (!line.empty())
		{
			float start = line.front().q.minX;
			float available = line.back().q.maxX - start;
			float glyphs = 0.0f, gaps = 0.0f;
			for (const Item& item : line)
			{
				glyphs += item.q.cell->advance / k;
				gaps += item.gapBefore;
			}
			// Option C: short of room, the line first takes free room in its
			// label (the rect the game clips it to). Which side follows from
			// where the text sits: at the left edge it grows to the right, at
			// the right edge to the left, centered on both sides.
			const RECT* clip = line.front().q.clip;
			if (glyphs > 0.0f && s_useLabelRoom && clip)
			{
				const float need = glyphs + gaps - available;
				const float freeLeft = std::max(0.0f, start - static_cast<float>(clip->left));
				const float freeRight = std::max(0.0f, static_cast<float>(clip->right) - (start + available));
				float growLeft = 0.0f, growRight = 0.0f;
				if (need > 0.0f)
				{
					if (std::abs(freeLeft - freeRight) <= 1.0f)
						growLeft = growRight = std::min(need / 2.0f, std::min(freeLeft, freeRight));
					else if (freeRight > freeLeft)
						growRight = std::min(need, freeRight);
					else
						growLeft = std::min(need, freeLeft);
				}
				if (growLeft + growRight > 0.0f)
				{
					start -= growLeft;
					available += growLeft + growRight;
					++s_stats.grownLines;
				}
			}
			if (glyphs > 0.0f)
			{
				const float deficit = glyphs + gaps - available;
				const float taken = deficit > 0.0f ? std::min(deficit, gaps * s_evenSpaceShrink) : 0.0f;
				const float gapScale = gaps > 0.0f ? (gaps - taken) / gaps : 1.0f;
				const float glyphScale = (available - (gaps - taken)) / glyphs;
				// Short of room: the glyphs get narrower along with their
				// spacing, so the whole line is condensed evenly and no glyph
				// runs into the next. Never stretched.
				const float condense = std::min(1.0f, glyphScale);
				const float drawScale = s_evenCondense ? condense : 1.0f;
				if (drawScale < 0.999f)
					s_drawCondensed = true;
				if (condense < 0.999f)
				{
					++s_stats.condensedLines;
					s_stats.minDrawScale = std::min(s_stats.minDrawScale, condense);
				}
				if (taken > 0.0f)
					++s_stats.shrunkSpaceLines;

				float x = start;
				for (const Item& item : line)
				{
					x += item.gapBefore * gapScale;
					const GlyphQuad& q = item.q;
					const float width = std::max(q.maxX - q.minX, q.cell->ink / k);
					const float u1 = (q.cellLeft + width) / twin.sourceSize;
					for (UINT i = 0; i < 4; ++i)
					{
						auto* v = reinterpret_cast<FONT_D3DTLVERTEX*>(q.vertices + i * stride);
						const bool right = v->u == q.maxU;
						v->x = right ? x + width * drawScale : x;
						if (right)
							v->u = u1;
					}
					x += q.cell->advance / k * glyphScale;
				}
				s_stats.widenedGlyphs += static_cast<int>(line.size());
			}
		}
		line.clear();
		pendingGap = 0.0f;
	};

	float lineEnd = 0.0f, lineTop = 0.0f;   // right edge and top of the last quad seen in the line
	const RECT* lineClip = nullptr;
	for (UINT first = 0; first + 4 <= numVertices; first += 4)
	{
		GlyphQuad q = ReadGlyphQuad(base + first * stride, stride, twin);
		const float height = q.maxY - q.minY;
		const bool space = q.cell && iswspace(q.cell->ch);
		const bool started = !line.empty() || pendingGap > 0.0f;
		if (started)
		{
			const float gap = q.minX - lineEnd;
			const bool sameRow = std::abs(q.minY - lineTop) < 0.05f;
			// More than about two spaces apart, or another label (option C
			// knows the labels): a separate label or column.
			if (!sameRow || gap < -0.05f || gap > 0.6f * height || q.clip != lineClip)
				flush();
			else
				pendingGap += std::max(0.0f, gap);
		}
		if (!q.cell)
		{
			flush();   // clipped or unknown: keep the game's placement
			continue;
		}
		if (space)
		{
			if (!line.empty())
				pendingGap += q.maxX - q.minX;
		}
		else
		{
			line.push_back({ q, line.empty() ? 0.0f : pendingGap });
			pendingGap = 0.0f;
		}
		if (!line.empty())
		{
			lineEnd = q.maxX;
			lineTop = q.minY;
			lineClip = q.clip;
		}
	}
	flush();
}
// Overhang mode: if this glyph quad (4 vertices, logical coordinates) shows a
// cell whose ink runs past the game's advance, move its right edge outward by
// the overhang, on screen and in the twin. Quads the game clipped on the right
// (or at the top) do not match a cell exactly and are left alone.
static void WidenGlyphQuad(uint8_t* quad, UINT stride, const TwinTexture& twin)
{
	float minU = FLT_MAX, maxU = -FLT_MAX, minV = FLT_MAX;
	for (UINT i = 0; i < 4; ++i)
	{
		const auto* v = reinterpret_cast<const FONT_D3DTLVERTEX*>(quad + i * stride);
		minU = std::min(minU, v->u);
		maxU = std::max(maxU, v->u);
		minV = std::min(minV, v->v);
	}

	const float left = minU * twin.sourceSize;
	const float top = minV * twin.sourceHeight;
	const int topTexel = static_cast<int>(std::lround(top));
	if (std::abs(top - topTexel) > 0.05f || left < 0.0f)
		return;

	auto it = twin.wideCells.upper_bound(WideCellKey(topTexel, static_cast<int>(left + 0.05f)));
	if (it == twin.wideCells.begin())
		return;
	--it;
	if (static_cast<int>(it->first >> 16) != topTexel
		|| std::abs(maxU * twin.sourceSize - static_cast<float>(it->second.right)) > 0.05f)
	{
		return;
	}

	const float k = static_cast<float>(s_twinFactor);

	// Spacing mode, text laid out after the advance grew: the game made the
	// quad nCharWidth wide but its UVs still span the old texture rect, which
	// would stretch the glyph. Let the UVs span the same width instead (the
	// extra twin texels hold the overhanging ink).
	if (s_wideGlyphs == WideGlyphs::Spacing)
	{
		float minX = FLT_MAX, maxX = -FLT_MAX;
		for (UINT i = 0; i < 4; ++i)
		{
			const auto* v = reinterpret_cast<const FONT_D3DTLVERTEX*>(quad + i * stride);
			minX = std::min(minX, v->x);
			maxX = std::max(maxX, v->x);
		}
		// Pixels the game already added (0 for text laid out before the
		// change); the rest of the overhang still sticks out to the right.
		const float extra = std::max(0.0f, std::round((maxX - minX) - (maxU - minU) * twin.sourceSize));
		const float overhangPx = static_cast<float>(it->second.overhang) / k;
		const float dx = std::max(0.0f, overhangPx - extra);
		const float du = (extra + dx) / twin.sourceSize;
		for (UINT i = 0; i < 4; ++i)
		{
			auto* v = reinterpret_cast<FONT_D3DTLVERTEX*>(quad + i * stride);
			if (v->u == maxU)
			{
				v->x += dx;
				v->u += du;
			}
		}
		++s_stats.widenedGlyphs;
		return;
	}

	// The whole quad shifts left by part of the overhang (only when the game
	// did not clip its left side); the right edge also gets the rest.
	const bool fullLeft = std::abs(left - static_cast<float>(it->first & 0xFFFF)) < 0.05f;
	const float leftShare = s_wideGlyphs == WideGlyphs::Spacing ? 0.0f : s_overhangLeft;
	const float shift = fullLeft ? std::round(it->second.overhang * leftShare) / k : 0.0f;
	const float dx = static_cast<float>(it->second.overhang) / k - shift;
	const float du = static_cast<float>(it->second.overhang) / (twin.sourceSize * k);
	for (UINT i = 0; i < 4; ++i)
	{
		auto* v = reinterpret_cast<FONT_D3DTLVERTEX*>(quad + i * stride);
		if (v->u == maxU)
		{
			v->x += dx;
			v->u += du;
		}
		else
		{
			v->x -= shift;
		}
	}
	++s_stats.widenedGlyphs;
}

// Binds a twin around one draw and restores the game's texture after.
struct TwinBinding
{
	IDirect3DBaseTexture9* original = nullptr;
	DWORD oldMin = 0, oldMag = 0, oldMip = 0;
	bool active = false;

	void Apply(IDirect3DDevice9* device, IDirect3DTexture9* twin)
	{
		if (FAILED(device->GetTexture(0, &original)))
			return;
		device->GetSamplerState(0, D3DSAMP_MINFILTER, &oldMin);
		device->GetSamplerState(0, D3DSAMP_MAGFILTER, &oldMag);
		device->GetSamplerState(0, D3DSAMP_MIPFILTER, &oldMip);

		// At a whole factor (f == k) every twin texel lands on one pixel, so
		// point sampling is exact. In between, linear blends the extra detail.
		// A condensed glyph is resampled anyway: then linear.
		const bool exact = std::abs(s_scale - static_cast<float>(s_twinFactor)) < 0.01f && !s_drawCondensed;
		const DWORD filter = exact ? D3DTEXF_POINT : D3DTEXF_LINEAR;
		device->SetTexture(0, twin);
		device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
		device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		active = true;
	}

	void Restore(IDirect3DDevice9* device)
	{
		if (!active)
			return;
		device->SetTexture(0, original);
		device->SetSamplerState(0, D3DSAMP_MINFILTER, oldMin);
		device->SetSamplerState(0, D3DSAMP_MAGFILTER, oldMag);
		device->SetSamplerState(0, D3DSAMP_MIPFILTER, oldMip);
		if (original)
			original->Release();
		original = nullptr;
		active = false;
	}
};

// Copies vertices [firstVertex, firstVertex + numVertices) of stream 0 into our
// own buffer at the same byte offset, with x and y multiplied by the factor.
// On success, binds our buffer as stream 0 and returns the original stream so
// the caller can restore it after drawing.
struct BoundStream
{
	IDirect3DVertexBuffer9* vb = nullptr;
	UINT offset = 0;
	UINT stride = 0;
};

// `sharpText`: the text is drawn from a twin texture. `scaledGlyphs` returns
// how many glyphs were scaled (0 = only name tags, keep the game's texture).
static bool BindScaledCopy(IDirect3DDevice9* device, UINT firstVertex, UINT numVertices, BoundStream& original,
	const TwinTexture* twin, int& scaledGlyphs)
{
	const bool sharpText = twin != nullptr;
	scaledGlyphs = 0;
	s_drawCondensed = false;
	if (FAILED(device->GetStreamSource(0, &original.vb, &original.offset, &original.stride))
		|| original.vb == nullptr || original.stride < 8)
	{
		return false;
	}

	D3DVERTEXBUFFER_DESC desc;
	if (FAILED(original.vb->GetDesc(&desc)))
		return false;

	const UINT start = original.offset + firstVertex * original.stride;
	const UINT bytes = numVertices * original.stride;
	if (bytes == 0 || start + bytes > desc.Size)
		return false;

	if (!EnsureScaledVB(device, desc.Size))
		return false;

	// Read the client's vertices. We only lock for reading; the client's data
	// stays exactly as it was.
	void* src = nullptr;
	if (FAILED(original.vb->Lock(start, bytes, &src, D3DLOCK_READONLY)) || src == nullptr)
		return false;

	void* dst = nullptr;
	if (FAILED(s_scaledVB->Lock(start, bytes, &dst, D3DLOCK_DISCARD)) || dst == nullptr)
	{
		original.vb->Unlock();
		return false;
	}

	memcpy(dst, src, bytes);
	original.vb->Unlock();

	// XYZRHW: the first three floats of every vertex are screen x, y and z.
	// Text is handled per glyph (4 vertices), everything else per vertex.
	const bool isText = original.stride == TextVertexStride;
	const UINT group = (isText && s_nameTagFilter) ? 4 : 1;

	// The game places glyphs at x - 0.5 so D3D9 pixel centers hit texel
	// centers. Scaling around 0,0 turns that into -0.5 * f, which lands
	// between texels of a twin at even factors. For twin text we scale around
	// the pixel corner instead: (x + 0.5) * f - 0.5.
	const bool pixelCorner = isText && sharpText;

	if (pixelCorner && group == 4 && s_wideGlyphs == WideGlyphs::Even && !twin->wideCells.empty())
		SpreadLines(static_cast<uint8_t*>(dst), numVertices, original.stride, *twin);

	// The label rect marks (see ClipRectGlyphHook) go back to the game's value.
	if (isText)
	{
		for (UINT i = 0; i < numVertices; ++i)
		{
			uint32_t* specular = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dst) + i * original.stride
				+ offsetof(FONT_D3DTLVERTEX, specular));
			if ((*specular & 0xFF000000u) == ClipMarkerTag)
				*specular = GlyphSpecular;
		}
	}

	uint8_t* base = static_cast<uint8_t*>(dst);
	for (UINT first = 0; first < numVertices; first += group)
	{
		const UINT count = std::min(group, numVertices - first);

		bool scale = true;
		if (group > 1)
		{
			// Glyph center, in logical (unscaled) coordinates.
			float cx = 0.0f, cy = 0.0f;
			for (UINT k = 0; k < count; ++k)
			{
				const float* pos = reinterpret_cast<const float*>(base + (first + k) * original.stride);
				cx += pos[0];
				cy += pos[1];
			}
			if (s_nameTagMarkerInstalled)
			{
				// Marked by the name tag glyph writer: real screen coordinates.
				scale = true;
				for (UINT k = 0; k < count; ++k)
				{
					uint32_t* specular = reinterpret_cast<uint32_t*>(base + (first + k) * original.stride + offsetof(FONT_D3DTLVERTEX, specular));
					if (*specular == NameTagSpecularMarker)
					{
						*specular = NameTagSpecular;
						scale = false;
					}
				}
			}
			else
			{
				scale = IsInsideWindow(cx / count, cy / count);
			}
			if (!scale)
				s_stats.keptVertices += count;   // a name tag: leave it where it is
		}
		if (scale && isText)
			++scaledGlyphs;
		if (scale && pixelCorner && count == 4 && !twin->wideCells.empty()
			&& (s_wideGlyphs == WideGlyphs::Overhang || s_wideGlyphs == WideGlyphs::Spacing))
			WidenGlyphQuad(base + first * original.stride, original.stride, *twin);

		for (UINT k = 0; k < count; ++k)
		{
			float* pos = reinterpret_cast<float*>(base + (first + k) * original.stride);
			s_stats.minX = std::min(s_stats.minX, pos[0]);
			s_stats.maxX = std::max(s_stats.maxX, pos[0]);
			s_stats.minY = std::min(s_stats.minY, pos[1]);
			s_stats.maxY = std::max(s_stats.maxY, pos[1]);

			if (scale && pixelCorner)
			{
				pos[0] = (pos[0] + 0.5f) * s_scale - 0.5f;
				pos[1] = (pos[1] + 0.5f) * s_scale - 0.5f;
			}
			else if (scale)
			{
				pos[0] *= s_scale;
				pos[1] *= s_scale;
			}
		}
	}

	s_scaledVB->Unlock();

	device->SetStreamSource(0, s_scaledVB, original.offset, original.stride);
	return true;
}

static void RestoreStream(IDirect3DDevice9* device, BoundStream& original)
{
	if (original.vb)
	{
		device->SetStreamSource(0, original.vb, original.offset, original.stride);
		original.vb->Release();
		original.vb = nullptr;
	}
}

// Texture and filter for a scaled draw: window text from a twin, everything
// else (and name-tag-only text) with the chosen texture filter.
static void BindTextures(IDirect3DDevice9* device, const BoundStream& original, IDirect3DTexture9* twin,
	int scaledGlyphs, SamplerOverride& sampler, TwinBinding& binding)
{
	const bool isText = original.stride == TextVertexStride;
	if (isText && twin && scaledGlyphs > 0)
	{
		binding.Apply(device, twin);
		++s_stats.sharpText;
		return;
	}
	sampler.Apply(device, isText);
}

//============================================================================
// Detours

// IDirect3DDevice9 methods. As in MacroQuest's own GraphicsEngineDX9.cpp, the
// detours are member functions so that `this` is the device.
class DeviceHooks
{
public:
	IDirect3DDevice9* Device() { return reinterpret_cast<IDirect3DDevice9*>(this); }

	DETOUR_TRAMPOLINE_DEF(HRESULT WINAPI, DrawPrimitive_Trampoline, (D3DPRIMITIVETYPE, UINT, UINT))
	HRESULT WINAPI DrawPrimitive_Detour(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
	{
		IDirect3DDevice9* device = Device();
		if (!IsScalableUIDraw(device))
			return DrawPrimitive_Trampoline(type, startVertex, primitiveCount);

		const TwinTexture* twin = FindBoundTwin(device);
		BoundStream original;
		int scaledGlyphs = 0;
		if (!BindScaledCopy(device, startVertex, VertexCountForPrimitives(type, primitiveCount), original,
			twin, scaledGlyphs))
		{
			if (original.vb) original.vb->Release();
			++s_stats.failed;
			return DrawPrimitive_Trampoline(type, startVertex, primitiveCount);
		}

		++s_stats.scaled;
		SamplerOverride sampler;
		TwinBinding binding;
		BindTextures(device, original, twin ? twin->texture : nullptr, scaledGlyphs, sampler, binding);
		HRESULT hr = DrawPrimitive_Trampoline(type, startVertex, primitiveCount);
		binding.Restore(device);
		sampler.Restore(device);
		RestoreStream(device, original);
		return hr;
	}

	DETOUR_TRAMPOLINE_DEF(HRESULT WINAPI, DrawIndexedPrimitive_Trampoline, (D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT))
	HRESULT WINAPI DrawIndexedPrimitive_Detour(D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex,
		UINT numVertices, UINT startIndex, UINT primitiveCount)
	{
		IDirect3DDevice9* device = Device();
		if (!IsScalableUIDraw(device) || baseVertexIndex + static_cast<INT>(minVertexIndex) < 0)
			return DrawIndexedPrimitive_Trampoline(type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount);

		// The indices refer to vertices [base + min, base + min + numVertices).
		// Our copy keeps the same layout, so the client's index buffer and the
		// draw parameters can be used unchanged.
		const TwinTexture* twin = FindBoundTwin(device);
		BoundStream original;
		int scaledGlyphs = 0;
		if (!BindScaledCopy(device, baseVertexIndex + minVertexIndex, numVertices, original,
			twin, scaledGlyphs))
		{
			if (original.vb) original.vb->Release();
			++s_stats.failed;
			return DrawIndexedPrimitive_Trampoline(type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount);
		}

		++s_stats.scaled;
		SamplerOverride sampler;
		TwinBinding binding;
		BindTextures(device, original, twin ? twin->texture : nullptr, scaledGlyphs, sampler, binding);
		HRESULT hr = DrawIndexedPrimitive_Trampoline(type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount);
		binding.Restore(device);
		sampler.Restore(device);
		RestoreStream(device, original);
		return hr;
	}

	// If the UI clips with scissor rects, the rects must grow with the UI.
	DETOUR_TRAMPOLINE_DEF(HRESULT WINAPI, SetScissorRect_Trampoline, (const RECT*))
	HRESULT WINAPI SetScissorRect_Detour(const RECT* rect)
	{
		if (!IsScalingActive() || rect == nullptr)
			return SetScissorRect_Trampoline(rect);

		RECT scaled = {
			static_cast<LONG>(rect->left * s_scale),
			static_cast<LONG>(rect->top * s_scale),
			static_cast<LONG>(rect->right * s_scale),
			static_cast<LONG>(rect->bottom * s_scale),
		};
		return SetScissorRect_Trampoline(&scaled);
	}
};

// C2DPrimitiveManager::Render(bool, bool) in EQGraphicsDX9.dll. Verified in the
// RoF2 disassembly: thiscall, ends with `ret 8`. We only raise a flag around it.
class C2DPrimitiveManagerHook
{
public:
	DETOUR_TRAMPOLINE_DEF(void, Render_Trampoline, (bool, bool))
	void Render_Detour(bool a, bool b)
	{
		++s_stats.uiPasses;
		SnapshotWindowRects();
		s_inUIPass = true;
		Render_Trampoline(a, b);
		s_inUIPass = false;

		s_prevTooltipRects.swap(s_tooltipRects);
		s_tooltipRects.clear();
	}
};

// Tooltip rects, see RecordTooltipRect. Both are cdecl and return `out`.
DETOUR_TRAMPOLINE_DEF(CXRect*, GetTooltipRect_Trampoline, (CXRect*, const CXSize*))
CXRect* GetTooltipRect_Detour(CXRect* out, const CXSize* size)
{
	CXRect* result = GetTooltipRect_Trampoline(out, size);
	RecordTooltipRect(result);
	return result;
}

DETOUR_TRAMPOLINE_DEF(CXRect*, GetTooltipRectAt_Trampoline, (CXRect*, const CXPoint*, const CXSize*))
CXRect* GetTooltipRectAt_Detour(CXRect* out, const CXPoint* pos, const CXSize* size)
{
	CXRect* result = GetTooltipRectAt_Trampoline(out, pos, size);
	RecordTooltipRect(result);
	return result;
}

struct TooltipHook
{
	uintptr_t offset;
	uint8_t prologue[5];
	bool installed;
	uintptr_t address;
};
static TooltipHook s_tooltipHooks[] = {
	{ 0x863150, { 0x8B, 0x44, 0x24, 0x04, 0x33 }, false, 0 },   // GetTooltipRect
	{ 0x8631C0, { 0xA1, 0xFC, 0x48, 0xB6, 0x00 }, false, 0 },   // GetTooltipRect at a position
};

static void InstallTooltipHooks()
{
	for (int i = 0; i < static_cast<int>(std::size(s_tooltipHooks)); ++i)
	{
		TooltipHook& hook = s_tooltipHooks[i];
		hook.address = FixEQGameOffset(hook.offset);
		if (memcmp(reinterpret_cast<const void*>(hook.address), hook.prologue, sizeof(hook.prologue)) != 0)
		{
			WriteChatf("\ar[MQUIScale]\ax unexpected code at 0x%X, tooltip hook skipped.", hook.offset);
			continue;
		}
		hook.installed = i == 0
			? mq::AddDetour(hook.address, &GetTooltipRect_Detour, &GetTooltipRect_Trampoline_Ptr, "MQUIScale_GetTooltipRect")
			: mq::AddDetour(hook.address, &GetTooltipRectAt_Detour, &GetTooltipRectAt_Trampoline_Ptr, "MQUIScale_GetTooltipRectAt");
	}
}

static void RemoveTooltipHooks()
{
	for (TooltipHook& hook : s_tooltipHooks)
	{
		if (hook.installed)
			mq::RemoveDetour(hook.address);
		hook.installed = false;
	}
	s_tooltipRects.clear();
	s_prevTooltipRects.clear();
}

// CCachedFont glyph upload (0x100B0450): copies the glyph the game just drew
// with ExtTextOutW into its font texture. After it, we draw the same character
// into the twin.
class CachedFontHook
{
public:
	DETOUR_TRAMPOLINE_DEF(void, UploadGlyph_Trampoline, (const CCachedFontGlyphTextureInfo&))
	void UploadGlyph_Detour(const CCachedFontGlyphTextureInfo& gti)
	{
		UploadGlyph_Trampoline(gti);

		CCachedFont* font = reinterpret_cast<CCachedFont*>(this);
		if (s_twinFactor > 0 && !s_twinsDirty && s_lastGlyphDC != nullptr && s_lastGlyphDC == font->hDC)
			WriteSharpGlyph(font, s_twinFactor, s_lastGlyphChar, gti);
		s_lastGlyphDC = nullptr;
	}
};

static uintptr_t s_addrUploadGlyph = 0;

// Name tag glyph writer (0x100B4540): thiscall, 8 arguments (ret 20h), the
// last one the 4 vertices to write. See NameTagSpecularMarker.
static constexpr uintptr_t CCachedFont__WriteNameTagGlyph_x = 0x100B4540;
static constexpr uint8_t WriteNameTagGlyphPrologue[6] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
static uintptr_t s_addrWriteNameTagGlyph = 0;

class NameTagGlyphHook
{
public:
	DETOUR_TRAMPOLINE_DEF(bool, Trampoline, (int, float, float, int, float, float, float, FONT_D3DTLVERTEX*))
	bool Detour(int ch, float x, float y, int color, float a, float b, float c, FONT_D3DTLVERTEX* vertices)
	{
		const bool result = Trampoline(ch, x, y, color, a, b, c, vertices);
		if (vertices && s_scale != 1.0f)
		{
			for (int i = 0; i < 4; ++i)
			{
				if (vertices[i].specular == NameTagSpecular)
					vertices[i].specular = NameTagSpecularMarker;
			}
		}
		return result;
	}
};

// General glyph writer (0x100B4340, only caller 0x100B4838): thiscall, 9
// arguments (ret 24h): character, x, y (ints), the clip RECT by value, color,
// and the 4 vertices to write. It clips the quad to the rect on the CPU and
// always writes specular 0xFF000000 (0x100B4452).
static constexpr uintptr_t CCachedFont__WriteGlyph_x = 0x100B4340;
static constexpr uint8_t WriteGlyphPrologue[9] = { 0x83, 0xEC, 0x10, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x1C };
static uintptr_t s_addrWriteGlyph = 0;
static bool s_clipHookInstalled = false;

class ClipRectGlyphHook
{
public:
	DETOUR_TRAMPOLINE_DEF(bool, Trampoline, (int, int, int, RECT, uint32_t, FONT_D3DTLVERTEX*))
	bool Detour(int ch, int x, int y, RECT clip, uint32_t color, FONT_D3DTLVERTEX* vertices)
	{
		const bool result = Trampoline(ch, x, y, clip, color, vertices);
		if (vertices && s_clipMarksWanted)
		{
			if (const uint32_t id = ClipRectId(clip))
			{
				for (int i = 0; i < 4; ++i)
				{
					if (vertices[i].specular == GlyphSpecular)
						vertices[i].specular = ClipMarkerTag | id;
				}
			}
		}
		return result;
	}
};

static void InstallClipRectHook()
{
	s_addrWriteGlyph = FixEQGraphicsOffset(CCachedFont__WriteGlyph_x);
	if (memcmp(reinterpret_cast<const void*>(s_addrWriteGlyph), WriteGlyphPrologue, sizeof(WriteGlyphPrologue)) != 0)
	{
		WriteChatf("\ar[MQUIScale]\ax unexpected code at glyph writer, 'Use label room' not available.");
		return;
	}
	s_clipHookInstalled = mq::AddDetour(s_addrWriteGlyph, &ClipRectGlyphHook::Detour,
		&ClipRectGlyphHook::Trampoline_Ptr, "MQUIScale_CCachedFont_WriteGlyph");
}

static void RemoveClipRectHook()
{
	if (s_clipHookInstalled)
		mq::RemoveDetour(s_addrWriteGlyph);
	s_clipHookInstalled = false;
}

// Called every pulse: marks are only written while they are used; switching
// relays out all text, so cached vertices get (or lose) their marks.
static void UpdateClipMarks()
{
	const bool wanted = s_clipHookInstalled && s_useLabelRoom && s_wideGlyphs == WideGlyphs::Even
		&& s_twinFactor > 0;
	if (wanted != s_clipMarksWanted || s_clipRects.size() >= 0x100000)
	{
		s_clipMarksWanted = wanted;
		s_clipRects.assign(1, RECT{});
		s_clipRectIds.clear();
		s_relayoutText = true;
	}
}

static void InstallNameTagHook()
{
	s_addrWriteNameTagGlyph = FixEQGraphicsOffset(CCachedFont__WriteNameTagGlyph_x);
	if (memcmp(reinterpret_cast<const void*>(s_addrWriteNameTagGlyph), WriteNameTagGlyphPrologue, sizeof(WriteNameTagGlyphPrologue)) != 0)
	{
		WriteChatf("\ar[MQUIScale]\ax unexpected code at name tag glyph writer, using the window rect filter.");
		return;
	}
	s_nameTagMarkerInstalled = mq::AddDetour(s_addrWriteNameTagGlyph, &NameTagGlyphHook::Detour,
		&NameTagGlyphHook::Trampoline_Ptr, "MQUIScale_CCachedFont_WriteNameTagGlyph");
}

static void RemoveNameTagHook()
{
	if (s_nameTagMarkerInstalled)
		mq::RemoveDetour(s_addrWriteNameTagGlyph);
	s_nameTagMarkerInstalled = false;
}

static void InstallSharpTextHooks()
{
	// ExtTextOutW import of EQGraphicsDX9.dll: only patched when it still
	// points at GDI's own function.
	s_extTextOutIAT = FixEQGraphicsOffset(ExtTextOutW_IAT_x);
	auto* slot = reinterpret_cast<ExtTextOutW_t*>(s_extTextOutIAT);
	const auto gdiExtTextOutW = reinterpret_cast<ExtTextOutW_t>(GetProcAddress(GetModuleHandleA("gdi32.dll"), "ExtTextOutW"));
	if (*slot != gdiExtTextOutW || gdiExtTextOutW == nullptr)
	{
		WriteChatf("\ar[MQUIScale]\ax ExtTextOutW import not as expected, sharp text disabled.");
		s_extTextOutIAT = 0;
		return;
	}

	s_addrUploadGlyph = FixEQGraphicsOffset(CCachedFont__UploadGlyph_x);
	if (memcmp(reinterpret_cast<const void*>(s_addrUploadGlyph), UploadGlyphPrologue, sizeof(UploadGlyphPrologue)) != 0)
	{
		WriteChatf("\ar[MQUIScale]\ax unexpected code at glyph upload, sharp text disabled.");
		s_extTextOutIAT = 0;
		return;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect))
	{
		s_extTextOutIAT = 0;
		return;
	}
	s_origExtTextOutW = *slot;
	*slot = &ExtTextOutW_Hook;
	VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);

	s_uploadHookInstalled = mq::AddDetour(s_addrUploadGlyph, &CachedFontHook::UploadGlyph_Detour,
		&CachedFontHook::UploadGlyph_Trampoline_Ptr, "MQUIScale_CCachedFont_UploadGlyph");
}

static void RemoveSharpTextHooks()
{
	if (s_uploadHookInstalled)
		mq::RemoveDetour(s_addrUploadGlyph);
	s_uploadHookInstalled = false;

	if (s_extTextOutIAT)
	{
		auto* slot = reinterpret_cast<ExtTextOutW_t*>(s_extTextOutIAT);
		DWORD oldProtect = 0;
		if (*slot == &ExtTextOutW_Hook && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect))
		{
			*slot = s_origExtTextOutW;
			VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
		}
		s_extTextOutIAT = 0;
	}
}

//============================================================================
// UI space: screen size, window positions and mouse

// --- Screen size ---------------------------------------------------------
// The client sets CXWndManager::ScreenExtentX/Y when the UI is created and
// when the resolution changes. We overwrite it with the logical size and
// re-apply it every pulse, in case the client sets it again.
//
// Test result: the "real" UI size is NOT always EverQuestinfo::ScreenXRes.
// With a window resized by dragging, ScreenXRes kept the configured
// resolution while the UI and the render target were larger, so windows were
// mapped into too small a screen and bunched up to the top left. The UI is
// drawn in render-target pixels, so we take the viewport the client uses for
// the UI pass (CRender::D3DViewPort) as the real size, and fall back to the
// client's own extent from before we touched it.

static bool s_extentOverridden = false;
static uint32_t s_origExtentX = 0;    // client's extent before we changed it
static uint32_t s_origExtentY = 0;

static bool GetRealUISize(int& width, int& height)
{
	if (pWndMgr && !s_extentOverridden)
	{
		s_origExtentX = pWndMgr->ScreenExtentX;
		s_origExtentY = pWndMgr->ScreenExtentY;
	}

	if (pGraphicsEngine && pGraphicsEngine->pRender
		&& pGraphicsEngine->pRender->D3DViewPort.Width > 0 && pGraphicsEngine->pRender->D3DViewPort.Height > 0)
	{
		width = static_cast<int>(pGraphicsEngine->pRender->D3DViewPort.Width);
		height = static_cast<int>(pGraphicsEngine->pRender->D3DViewPort.Height);
		return true;
	}

	width = static_cast<int>(s_origExtentX);
	height = static_cast<int>(s_origExtentY);
	return width > 0 && height > 0;
}

static void ApplyScreenExtent()
{
	if (!pWndMgr)
		return;

	int realX = 0, realY = 0;
	if (!GetRealUISize(realX, realY))
		return;

	if (s_scale == 1.0f)
	{
		// Give the client back exactly what it had.
		if (s_extentOverridden)
		{
			pWndMgr->ScreenExtentX = s_origExtentX;
			pWndMgr->ScreenExtentY = s_origExtentY;
			s_extentOverridden = false;
		}
		return;
	}

	pWndMgr->ScreenExtentX = static_cast<uint32_t>(realX / s_scale);
	pWndMgr->ScreenExtentY = static_cast<uint32_t>(realY / s_scale);
	s_extentOverridden = true;
}

// --- Window positions ----------------------------------------------------
// Every top-level window is mapped from the real screen into the logical
// screen per axis, within its free space (screen - size).
//
// Test result: keeping only the relative place (pos / free space) put a
// hotbar on top of the chat window below it. The free space depends on the
// window's size, so a small window just above a large one at the bottom
// moved down further than the large one. Now windows near an edge keep their
// distance to that edge, which keeps stacks along the edges intact, and only
// the middle part of the free space is compressed:
//
//     a = min(free real, free logical) / 3        (edge zone, same both ways)
//     pos <= a              -> pos                (distance to left/top kept)
//     pos >= free - a       -> distance to right/bottom kept
//     in between            -> linear from a to (free - a)
//
// The window is then drawn f times larger from (0,0). The mapping is
// continuous and its inverse is the same mapping the other way, so windows
// you drag while scaled keep the place you gave them. A window that was not
// touched in between gets its exact original position back (no rounding).

struct PlacedWindow
{
	CXPoint original;   // position at 1.0
	CXPoint placed;     // logical position we gave it
};
static std::map<CXWnd*, PlacedWindow> s_movedWindows;

// Maps one coordinate from a screen of size `from` to one of size `to`
// (see above: edge distances kept, middle compressed).
static int MapRelative(int pos, int size, int from, int to)
{
	const int freeFrom = from - size;
	const int freeTo = to - size;
	if (freeFrom <= 0 || freeTo <= 0)
		return std::max(0, std::min(pos, freeTo));   // window as large as the screen

	const float p = std::clamp(static_cast<float>(pos), 0.0f, static_cast<float>(freeFrom));
	const float edge = std::min(freeFrom, freeTo) / 3.0f;

	float q;
	if (p <= edge)
		q = p;
	else if (p >= freeFrom - edge)
		q = freeTo - (freeFrom - p);
	else
		q = edge + (p - edge) * (freeTo - 2.0f * edge) / (freeFrom - 2.0f * edge);

	return std::clamp(static_cast<int>(std::lround(q)), 0, freeTo);
}

static bool IsLiveWindow(CXWnd* wnd)
{
	if (!pWndMgr || !wnd)
		return false;

	for (int i = 0; i < pWndMgr->pWindows.GetCount(); ++i)
	{
		if (pWndMgr->pWindows[i] == wnd)
			return true;
	}
	return false;
}

// Size of the logical screen the windows currently live in.
static int s_placedScreenX = 0;
static int s_placedScreenY = 0;

static void RestoreWindows()
{
	int realX = 0, realY = 0;
	GetRealUISize(realX, realY);

	for (const auto& [wnd, placed] : s_movedWindows)
	{
		if (!IsLiveWindow(wnd))
			continue;

		const CXRect rc = wnd->GetLocation();
		if (rc.left == placed.placed.x && rc.top == placed.placed.y)
		{
			wnd->Move(placed.original);   // untouched: exact original position
		}
		else if (realX > 0 && realY > 0)
		{
			// Moved by the player while scaled: map its new place back.
			const CXPoint pos(
				MapRelative(rc.left, rc.right - rc.left, s_placedScreenX, realX),
				MapRelative(rc.top, rc.bottom - rc.top, s_placedScreenY, realY));
			wnd->Move(pos);
		}
	}
	s_movedWindows.clear();
}

static void FitWindowsToScreen()
{
	// Always start from the 1.0 positions so changing the factor several
	// times doesn't drift windows around.
	// Test result: the EQ button ended up in the middle after dragging the
	// slider. The extent is already the new (smaller) one here, and moving
	// EQMainWnd back to its 1.0 place clamps it into that extent (original
	// 2247,1907 became 1795,1267 = 1848x1320 - 53, the extent at 1.5); that
	// clamped place was then taken as its original. So restore on the real
	// screen size.
	if (pWndMgr && !s_movedWindows.empty())
	{
		int realX = 0, realY = 0;
		if (GetRealUISize(realX, realY) && realX > 0 && realY > 0)
		{
			const uint32_t extentX = pWndMgr->ScreenExtentX;
			const uint32_t extentY = pWndMgr->ScreenExtentY;
			pWndMgr->ScreenExtentX = realX;
			pWndMgr->ScreenExtentY = realY;
			RestoreWindows();
			pWndMgr->ScreenExtentX = extentX;
			pWndMgr->ScreenExtentY = extentY;
		}
		else
		{
			RestoreWindows();
		}
	}

	if (!pWndMgr || s_scale == 1.0f)
		return;

	int realX = 0, realY = 0;
	if (!GetRealUISize(realX, realY))
		return;

	const int logicalX = static_cast<int>(pWndMgr->ScreenExtentX);
	const int logicalY = static_cast<int>(pWndMgr->ScreenExtentY);
	if (realX <= 0 || realY <= 0 || logicalX <= 0 || logicalY <= 0)
		return;

	s_placedScreenX = logicalX;
	s_placedScreenY = logicalY;

	for (int i = 0; i < pWndMgr->pWindows.GetCount(); ++i)
	{
		CXWnd* wnd = pWndMgr->pWindows[i];
		if (!wnd || wnd->GetParentWindow() != nullptr || wnd == pWndMgr->MainWindow)
			continue;

		const CXRect rc = wnd->GetLocation();
		const CXPoint pos(
			MapRelative(rc.left, rc.right - rc.left, realX, logicalX),
			MapRelative(rc.top, rc.bottom - rc.top, realY, logicalY));

		s_movedWindows.emplace(wnd, PlacedWindow{ CXPoint(rc.left, rc.top), pos });
		if (pos.x != rc.left || pos.y != rc.top)
			wnd->Move(pos);
	}
}

// --- Window positions in EQ's UI ini ---------------------------------------
// Test result: windows came back slightly off after a relog. EQ stores a
// window as its distance to the nearest screen edge, using the screen size
// from 0x761E30 - which returns the logical size while scaled (menu fix
// below). Loading at 1.0 then measured that distance from the real edge
// (EQ button: logical 1336 = 488 from the right of 1824, loaded as
// 2773 - 488 = 2285). Also, EQ stores at more moments than CleanUI.
//
// So both CSidlScreenWnd::StoreIniInfo (0x85CA70) and LoadIniInfo (0x85DDA0)
// run in the 1.0 world: the real screen size and extent, and while storing,
// the window at its 1.0 position. A window loaded while scaled is fitted
// into the logical screen right after.

// Set by OnCleanUI: the windows in s_movedWindows belong to the old UI.
// Cleared when the new UI starts loading, before any new window is recorded
// (a new window can have the address of an old one).
static bool s_forgetMovedWindows = false;

static void ForgetOldWindowsIfCleaned()
{
	if (s_forgetMovedWindows)
	{
		s_movedWindows.clear();
		s_forgetMovedWindows = false;
	}
}

// The window's position at 1.0, from where it is now in the logical screen.
static CXPoint UnscaledPosition(CXWnd* wnd, int realX, int realY)
{
	const CXRect rc = wnd->GetLocation();
	auto it = s_movedWindows.find(wnd);
	if (it != s_movedWindows.end() && rc.left == it->second.placed.x && rc.top == it->second.placed.y)
		return it->second.original;   // untouched: exact original position

	const int fromX = it != s_movedWindows.end() ? s_placedScreenX : static_cast<int>(pWndMgr->ScreenExtentX);
	const int fromY = it != s_movedWindows.end() ? s_placedScreenY : static_cast<int>(pWndMgr->ScreenExtentY);
	return CXPoint(
		MapRelative(rc.left, rc.right - rc.left, fromX, realX),
		MapRelative(rc.top, rc.bottom - rc.top, fromY, realY));
}

// Shows EQ the 1.0 screen extent for the duration of an ini call.
struct IniScope
{
	uint32_t extentX = 0, extentY = 0;
	bool active = false;

	IniScope()
	{
		++s_iniDepth;
		if (pWndMgr && s_extentOverridden && s_origExtentX > 0 && s_origExtentY > 0)
		{
			extentX = pWndMgr->ScreenExtentX;
			extentY = pWndMgr->ScreenExtentY;
			pWndMgr->ScreenExtentX = s_origExtentX;
			pWndMgr->ScreenExtentY = s_origExtentY;
			active = true;
		}
	}

	~IniScope()
	{
		if (active && pWndMgr)
		{
			pWndMgr->ScreenExtentX = extentX;
			pWndMgr->ScreenExtentY = extentY;
		}
		--s_iniDepth;
	}
};

static bool IsScaledForIni()
{
	return s_scale != 1.0f && s_extentOverridden && s_iniDepth == 0 && pWndMgr != nullptr;
}

class SidlIniHook
{
public:
	DETOUR_TRAMPOLINE_DEF(void, StoreIniInfo_Trampoline, ())
	void StoreIniInfo_Detour()
	{
		CXWnd* wnd = reinterpret_cast<CXWnd*>(this);
		int realX = 0, realY = 0;
		if (!IsScaledForIni() || !GetRealUISize(realX, realY))
		{
			StoreIniInfo_Trampoline();
			return;
		}

		const CXRect rc = wnd->GetLocation();
		const CXPoint unscaled = UnscaledPosition(wnd, realX, realY);

		IniScope scope;
		wnd->Move(unscaled);
		StoreIniInfo_Trampoline();
		wnd->Move(CXPoint(rc.left, rc.top));
	}

	DETOUR_TRAMPOLINE_DEF(void, LoadIniInfo_Trampoline, ())
	void LoadIniInfo_Detour()
	{
		CXWnd* wnd = reinterpret_cast<CXWnd*>(this);
		int realX = 0, realY = 0;
		if (!IsScaledForIni() || !GetRealUISize(realX, realY))
		{
			LoadIniInfo_Trampoline();
			return;
		}

		ForgetOldWindowsIfCleaned();
		{
			IniScope scope;
			LoadIniInfo_Trampoline();
		}

		// Loaded at its 1.0 position: fit it into the logical screen, like
		// FitWindowsToScreen does (top-level windows only).
		if (wnd->GetParentWindow() != nullptr || wnd == pWndMgr->MainWindow)
			return;

		const int logicalX = static_cast<int>(pWndMgr->ScreenExtentX);
		const int logicalY = static_cast<int>(pWndMgr->ScreenExtentY);
		const CXRect rc = wnd->GetLocation();
		const CXPoint pos(
			MapRelative(rc.left, rc.right - rc.left, realX, logicalX),
			MapRelative(rc.top, rc.bottom - rc.top, realY, logicalY));

		s_movedWindows[wnd] = PlacedWindow{ CXPoint(rc.left, rc.top), pos };
		if (s_placedScreenX == 0)
		{
			s_placedScreenX = logicalX;
			s_placedScreenY = logicalY;
		}
		if (pos.x != rc.left || pos.y != rc.top)
			wnd->Move(pos);
	}
};

static constexpr uintptr_t CSidlScreenWnd__StoreIniInfo_Addr = 0x85CA70;
static constexpr uintptr_t CSidlScreenWnd__LoadIniInfo_Addr = 0x85DDA0;
static constexpr uint8_t StoreIniInfoPrologue[6] = { 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00 };
static constexpr uint8_t LoadIniInfoPrologue[7] = { 0x6A, 0xFF, 0x68, 0xE1, 0x1E, 0x9A, 0x00 };
static uintptr_t s_addrStoreIniInfo = 0;
static uintptr_t s_addrLoadIniInfo = 0;
static bool s_storeIniHookInstalled = false;
static bool s_loadIniHookInstalled = false;

static void InstallIniHooks()
{
	s_addrStoreIniInfo = FixEQGameOffset(CSidlScreenWnd__StoreIniInfo_Addr);
	s_addrLoadIniInfo = FixEQGameOffset(CSidlScreenWnd__LoadIniInfo_Addr);
	if (memcmp(reinterpret_cast<const void*>(s_addrStoreIniInfo), StoreIniInfoPrologue, sizeof(StoreIniInfoPrologue)) != 0
		|| memcmp(reinterpret_cast<const void*>(s_addrLoadIniInfo), LoadIniInfoPrologue, sizeof(LoadIniInfoPrologue)) != 0)
	{
		WriteChatf("\ar[MQUIScale]\ax unexpected code at StoreIniInfo/LoadIniInfo, window position hooks skipped.");
		return;
	}

	// Both or neither: storing in the 1.0 world only makes sense when
	// loading is too.
	s_storeIniHookInstalled = mq::AddDetour(s_addrStoreIniInfo, &SidlIniHook::StoreIniInfo_Detour,
		&SidlIniHook::StoreIniInfo_Trampoline_Ptr, "MQUIScale_StoreIniInfo");
	s_loadIniHookInstalled = mq::AddDetour(s_addrLoadIniInfo, &SidlIniHook::LoadIniInfo_Detour,
		&SidlIniHook::LoadIniInfo_Trampoline_Ptr, "MQUIScale_LoadIniInfo");
}

static void RemoveIniHooks()
{
	if (s_storeIniHookInstalled)
		mq::RemoveDetour(s_addrStoreIniInfo);
	if (s_loadIniHookInstalled)
		mq::RemoveDetour(s_addrLoadIniInfo);
	s_storeIniHookInstalled = false;
	s_loadIniHookInstalled = false;
}

// --- Mouse ---------------------------------------------------------------
// __ProcessMouseEvents (0x539E60) passes the raw mouse position to these
// CXWndManager handlers; only when the UI does not consume a click does it go
// to the 3D world (CEverQuest::LMouseUp etc.) with the raw position. The main
// loop does the same for mouse movement (0x53AA19). So dividing the point in
// these handlers moves the UI into logical space and leaves the world alone.
//
// Addresses are for this exact RoF2 eqgame.exe, found by disassembly. All are
// int __thiscall (const CXPoint&), ending in `ret 4`. The first four bytes are
// checked before hooking, so a different eqgame.exe build is left alone.

static CXPoint ToUIPoint(const CXPoint& pt)
{
	if (s_scale == 1.0f)
		return pt;
	return CXPoint(static_cast<int>(pt.x / s_scale), static_cast<int>(pt.y / s_scale));
}

// The handlers also call each other with a point that is already logical:
// CXWndManager::HandleLButtonDown starts by calling HandleMouseMove with its
// own argument (0x879796). Converting again there divided the point twice,
// so the click hit the wrong spot (test: dragging/resizing windows turned the
// camera instead). Only the outermost call converts.
static int s_mouseHandlerDepth = 0;

struct MouseHandlerScope
{
	MouseHandlerScope() { ++s_mouseHandlerDepth; }
	~MouseHandlerScope() { --s_mouseHandlerDepth; }
};

#define MQUISCALE_MOUSE_HOOK(Name)                                              \
	class Name##Hook                                                            \
	{                                                                           \
	public:                                                                     \
		DETOUR_TRAMPOLINE_DEF(int, Trampoline, (const CXPoint&))                \
		int Detour(const CXPoint& pt)                                           \
		{                                                                       \
			if (s_mouseHandlerDepth > 0)                                        \
				return Trampoline(pt);                                          \
			MouseHandlerScope scope;                                            \
			return Trampoline(ToUIPoint(pt));                                   \
		}                                                                       \
	};

MQUISCALE_MOUSE_HOOK(LButtonDown)
MQUISCALE_MOUSE_HOOK(LButtonHeld)
MQUISCALE_MOUSE_HOOK(LButtonUp)
MQUISCALE_MOUSE_HOOK(LButtonUpAfterHeld)
MQUISCALE_MOUSE_HOOK(RButtonDown)
MQUISCALE_MOUSE_HOOK(RButtonHeld)
MQUISCALE_MOUSE_HOOK(RButtonUp)
MQUISCALE_MOUSE_HOOK(RButtonUpAfterHeld)

// The main loop only calls HandleMouseMove when the raw position differs from
// CXWndManager::MousePoint. Once MousePoint is logical that is true every
// frame, which would keep resetting hover and tooltip timers. So we skip the
// call when the logical position did not change.
class MouseMoveHook
{
public:
	DETOUR_TRAMPOLINE_DEF(int, Trampoline, (const CXPoint&))
	int Detour(const CXPoint& pt)
	{
		if (s_mouseHandlerDepth > 0)
			return Trampoline(pt);   // called from another handler: already logical

		const CXPoint ui = ToUIPoint(pt);
		if (s_scale != 1.0f && pWndMgr && pWndMgr->MousePoint.x == ui.x && pWndMgr->MousePoint.y == ui.y)
			return 0;

		MouseHandlerScope scope;
		return Trampoline(ui);
	}
};

struct MouseHandlerHook
{
	const char* name;
	uintptr_t offset;          // eqgame.exe address at preferred base 0x400000
	uint8_t prologue[4];       // expected first bytes
	bool (*install)(uintptr_t address);
	bool installed;
	uintptr_t address;
};

#define MQUISCALE_MOUSE_ENTRY(Name, Offset, b0, b1, b2, b3)                     \
	{ #Name, Offset, { b0, b1, b2, b3 }, [](uintptr_t address) {               \
		return mq::AddDetour(address, &Name##Hook::Detour,                     \
			&Name##Hook::Trampoline_Ptr, "MQUIScale_" #Name); }, false, 0 }

static MouseHandlerHook s_mouseHooks[] = {
	MQUISCALE_MOUSE_ENTRY(MouseMove,          0x877C90, 0x83, 0xEC, 0x48, 0x8B),
	MQUISCALE_MOUSE_ENTRY(LButtonDown,        0x879780, 0x83, 0xEC, 0x20, 0x8B),
	MQUISCALE_MOUSE_ENTRY(LButtonHeld,        0x878940, 0x51, 0x8B, 0x44, 0x24),
	MQUISCALE_MOUSE_ENTRY(LButtonUp,          0x878B60, 0x51, 0x8B, 0x44, 0x24),
	MQUISCALE_MOUSE_ENTRY(LButtonUpAfterHeld, 0x879F00, 0x51, 0x53, 0x55, 0x8B),
	MQUISCALE_MOUSE_ENTRY(RButtonDown,        0x878DA0, 0x83, 0xEC, 0x08, 0x8B),
	MQUISCALE_MOUSE_ENTRY(RButtonHeld,        0x878A50, 0x51, 0x8B, 0x44, 0x24),
	MQUISCALE_MOUSE_ENTRY(RButtonUp,          0x878F30, 0x51, 0x8B, 0x44, 0x24),
	MQUISCALE_MOUSE_ENTRY(RButtonUpAfterHeld, 0x87A020, 0x51, 0x53, 0x55, 0x8B),
};

static int s_mouseHooksInstalled = 0;

// --- Screen size for menus -------------------------------------------------
// Test result: the EQ button menu always opened down/right, also at the
// bottom of the screen. Its placement code (0x645090, called from
// EQMainWnd's WndNotification) gets the screen size from a small helper at
// 0x761E30 that returns EverQuestinfo::ScreenXRes/YRes, the real resolution,
// not CXWndManager::ScreenExtentX/Y. In the logical screen that leaves room
// below and to the right, so the menu never flips. The helper has 7 callers,
// all UI code (that menu, context submenus, CXWnd), so while scaled it
// returns the logical size. The renderer reads ScreenXRes/YRes directly.

static constexpr uintptr_t GetScreenSize_x = 0x761E30;   // void __cdecl (int* width, int* height)
static constexpr uint8_t GetScreenSizePrologue[6] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D };
static uintptr_t s_addrGetScreenSize = 0;
static bool s_screenSizeHookInstalled = false;

DETOUR_TRAMPOLINE_DEF(void, GetScreenSize_Trampoline, (int*, int*))
void GetScreenSize_Detour(int* width, int* height)
{
	GetScreenSize_Trampoline(width, height);
	if (s_scale != 1.0f && s_extentOverridden && s_iniDepth == 0 && pWndMgr && width && height)
	{
		*width = static_cast<int>(pWndMgr->ScreenExtentX);
		*height = static_cast<int>(pWndMgr->ScreenExtentY);
	}
}

static void InstallScreenSizeHook()
{
	s_addrGetScreenSize = FixEQGameOffset(GetScreenSize_x);
	if (memcmp(reinterpret_cast<const void*>(s_addrGetScreenSize), GetScreenSizePrologue, sizeof(GetScreenSizePrologue)) != 0)
	{
		WriteChatf("\ar[MQUIScale]\ax unexpected code at 0x%X, menu screen size hook skipped.", GetScreenSize_x);
		return;
	}
	s_screenSizeHookInstalled = mq::AddDetour(s_addrGetScreenSize, &GetScreenSize_Detour,
		&GetScreenSize_Trampoline_Ptr, "MQUIScale_GetScreenSize");
}

static void RemoveScreenSizeHook()
{
	if (s_screenSizeHookInstalled)
		mq::RemoveDetour(s_addrGetScreenSize);
	s_screenSizeHookInstalled = false;
}

static void InstallMouseHooks()
{
	for (MouseHandlerHook& hook : s_mouseHooks)
	{
		hook.address = FixEQGameOffset(hook.offset);
		if (memcmp(reinterpret_cast<const void*>(hook.address), hook.prologue, sizeof(hook.prologue)) != 0)
		{
			WriteChatf("\ar[MQUIScale]\ax %s: unexpected code at 0x%X, mouse hook skipped.", hook.name, hook.offset);
			continue;
		}

		hook.installed = hook.install(hook.address);
		if (hook.installed)
			++s_mouseHooksInstalled;
	}
}

static void RemoveMouseHooks()
{
	for (MouseHandlerHook& hook : s_mouseHooks)
	{
		if (hook.installed)
			mq::RemoveDetour(hook.address);
		hook.installed = false;
	}
	s_mouseHooksInstalled = 0;
}

// --- Login and server select (eqmain.dll) ----------------------------------
// Before character select the client runs eqmain.dll, with its own
// CXWndManager (MacroQuest points pWndMgr at it, same layout) drawn by the
// same EQGraphicsDX9 renderer, so drawing, screen extent and window fitting
// work unchanged. Its mouse input goes through CLoginViewManager instead:
// LoginController::ProcessMouseEvents (0x10011FD0, reached from GiveTime)
// calls CLoginViewManager methods, all int __thiscall (const CXPoint&), ret 4.
//
// Test result: hovering worked on server select but clicks did nothing.
// Only MouseMove (0x10015CC0) gets the raw client-area cursor position. The
// button handlers (0x10015C00, 0x10017D60, 0x10015C60, 0x10017E60) get a
// copy of CXWndManager::MousePoint, which MouseMove already made logical, so
// dividing them as well put the click somewhere else. So only MouseMove is
// hooked. Like in eqgame, the move is skipped when the logical position did
// not change (the client compares the raw position with MousePoint).
// eqmain.dll is loaded and unloaded by the client, so the hook lives from
// OnLoginFrontendEntered to ...Exited.

class LoginMouseMoveHook
{
public:
	DETOUR_TRAMPOLINE_DEF(int, Trampoline, (const CXPoint&))
	int Detour(const CXPoint& pt)
	{
		const CXPoint ui = ToUIPoint(pt);
		if (s_scale != 1.0f && pWndMgr && pWndMgr->MousePoint.x == ui.x && pWndMgr->MousePoint.y == ui.y)
			return 0;
		return Trampoline(ui);
	}
};

static MouseHandlerHook s_loginMouseHooks[] = {
	MQUISCALE_MOUSE_ENTRY(LoginMouseMove, 0x10015CC0, 0x55, 0x8B, 0xEC, 0x83),
};

static int s_loginMouseHooksInstalled = 0;

static void InstallLoginMouseHooks()
{
	if (s_loginMouseHooksInstalled > 0 || EQMainBaseAddress == 0)
		return;

	for (MouseHandlerHook& hook : s_loginMouseHooks)
	{
		hook.address = FixEQMainOffset(hook.offset);
		if (memcmp(reinterpret_cast<const void*>(hook.address), hook.prologue, sizeof(hook.prologue)) != 0)
		{
			WriteChatf("\ar[MQUIScale]\ax %s: unexpected code at 0x%X, login mouse hook skipped.", hook.name, hook.offset);
			continue;
		}

		hook.installed = hook.install(hook.address);
		if (hook.installed)
			++s_loginMouseHooksInstalled;
	}
}

static void RemoveLoginMouseHooks()
{
	for (MouseHandlerHook& hook : s_loginMouseHooks)
	{
		if (hook.installed)
			mq::RemoveDetour(hook.address);
		hook.installed = false;
	}
	s_loginMouseHooksInstalled = 0;
}

//============================================================================
// Hook management

static void InstallHooks()
{
	if (s_hooksInstalled)
		return;

	IDirect3DDevice9* device = GetGameDevice();
	if (!device || !C2DPrimitiveManager__Render)
		return;

	// Detour the code behind the device's vtable entries, like MacroQuest does
	// for BeginScene/EndScene. Indices are from the IDirect3DDevice9 interface.
	uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(device);
	s_addrSetScissorRect = vtable[75];
	s_addrDrawPrimitive = vtable[81];
	s_addrDrawIndexedPrimitive = vtable[82];

	mq::AddDetour(s_addrDrawPrimitive, &DeviceHooks::DrawPrimitive_Detour,
		&DeviceHooks::DrawPrimitive_Trampoline_Ptr, "MQUIScale_DrawPrimitive");
	mq::AddDetour(s_addrDrawIndexedPrimitive, &DeviceHooks::DrawIndexedPrimitive_Detour,
		&DeviceHooks::DrawIndexedPrimitive_Trampoline_Ptr, "MQUIScale_DrawIndexedPrimitive");
	mq::AddDetour(s_addrSetScissorRect, &DeviceHooks::SetScissorRect_Detour,
		&DeviceHooks::SetScissorRect_Trampoline_Ptr, "MQUIScale_SetScissorRect");

	s_uiHookInstalled = mq::AddDetour(C2DPrimitiveManager__Render, &C2DPrimitiveManagerHook::Render_Detour,
		&C2DPrimitiveManagerHook::Render_Trampoline_Ptr, "MQUIScale_C2DPrimitiveManager_Render");

	InstallMouseHooks();
	InstallScreenSizeHook();
	InstallTooltipHooks();
	InstallIniHooks();
	InstallNameTagHook();
	InstallClipRectHook();
	InstallSharpTextHooks();

	s_hooksInstalled = true;

	WriteChatf("\ag[MQUIScale]\ax hooks installed (UI pass hook: %s, mouse hooks: %d/%d, sharp text hook: %s).",
		s_uiHookInstalled ? "\agok\ax" : "\arFAILED\ax", s_mouseHooksInstalled, static_cast<int>(std::size(s_mouseHooks)),
		s_uploadHookInstalled ? "\agok\ax" : "\arFAILED\ax");
}

static void RemoveHooks()
{
	if (!s_hooksInstalled)
		return;

	RemoveSharpTextHooks();
	RemoveScreenSizeHook();
	RemoveTooltipHooks();
	RemoveIniHooks();
	RemoveNameTagHook();
	RemoveClipRectHook();
	if (s_clipMarksWanted)
	{
		// Text keeps its marked vertices: lay it out again, now unmarked.
		s_clipMarksWanted = false;
		RelayoutTextObjects();
	}
	RemoveMouseHooks();
	if (s_uiHookInstalled)
		mq::RemoveDetour(C2DPrimitiveManager__Render);
	mq::RemoveDetour(s_addrSetScissorRect);
	mq::RemoveDetour(s_addrDrawIndexedPrimitive);
	mq::RemoveDetour(s_addrDrawPrimitive);

	s_inUIPass = false;
	s_uiHookInstalled = false;
	s_hooksInstalled = false;
}

//============================================================================
// Command and UI

// Sets the factor and moves the UI into the matching logical screen.
static void SetScale(float factor)
{
	s_scale = std::clamp(factor, MinScale, MaxScale);
	s_scalePending = false;   // a choice made now wins over the saved one

	ApplyScreenExtent();
	FitWindowsToScreen();
}

//============================================================================
// Diagnostics: /uiscale dump
//
// Writes the screen sizes and every top-level window (where we put it, where
// it is now) to Logs\MQUIScale.log, once right away and once again a moment
// later, so we can see whether the game moves windows back.

static int s_pendingDumpPulses = 0;

static void DumpState(const char* when)
{
	char path[MAX_PATH];
	sprintf_s(path, "%s\\MQUIScale.log", gPathLogs);

	FILE* file = nullptr;
	if (fopen_s(&file, path, "a") != 0 || !file)
	{
		WriteChatf("\ar[MQUIScale]\ax could not write %s", path);
		return;
	}

	int realX = 0, realY = 0;
	GetRealUISize(realX, realY);

	fprintf(file, "==== %s, scale %.3f ====\n", when, s_scale);
	if (pEverQuestInfo)
		fprintf(file, "ScreenRes %d x %d\n", pEverQuestInfo->ScreenXRes, pEverQuestInfo->ScreenYRes);
	if (pWndMgr)
		fprintf(file, "Extent now %u x %u, client extent %u x %u, overridden %d\n",
			pWndMgr->ScreenExtentX, pWndMgr->ScreenExtentY, s_origExtentX, s_origExtentY, s_extentOverridden ? 1 : 0);
	fprintf(file, "Real UI size (viewport) %d x %d, placed screen %d x %d\n", realX, realY, s_placedScreenX, s_placedScreenY);
	if (pGraphicsEngine && pGraphicsEngine->pRender)
	{
		const CRender* render = pGraphicsEngine->pRender;
		fprintf(file, "CRender display %d x %d, backbuffer %u x %u, viewport %lu,%lu %lu x %lu\n",
			render->nDisplayWidth, render->nDisplayHeight,
			render->d3dpp.BackBufferWidth, render->d3dpp.BackBufferHeight,
			render->D3DViewPort.X, render->D3DViewPort.Y, render->D3DViewPort.Width, render->D3DViewPort.Height);
	}
	fprintf(file, "UI vertices last frame (unscaled coords): x %.0f..%.0f, y %.0f..%.0f\n",
		s_lastStats.minX, s_lastStats.maxX, s_lastStats.minY, s_lastStats.maxY);
	fprintf(file, "Text outside windows (left unscaled) %d vertices, visible windows %d\n",
		s_lastStats.keptVertices, static_cast<int>(s_windowRects.size()));

	// Fonts, for checking the CCachedFont layout and the twins.
	fprintf(file, "Sharp text %s, upload hook %d, twin factor %d, twins %d, sharp text draws last frame %d\n",
		s_sharpText ? "on" : "off", s_uploadHookInstalled ? 1 : 0, s_twinFactor,
		static_cast<int>(s_twins.size()), s_lastStats.sharpText);
	if (CFontManager* manager = GetFontManager())
	{
		fprintf(file, "%-24s %4s %4s %5s %7s %7s %6s %4s %6s  %s\n", "font", "id", "h", "maxw", "texsize",
			"ftex", "shadow", "tex", "glyphs", "LOGFONT height/width/weight/quality, textures");
		for (CCachedFontNode* node = manager->listFonts.head; node; node = node->next)
		{
			CCachedFont* font = node->pFont;
			if (!font)
				continue;

			int glyphs = 0;
			for (int i = 0; i < font->arGlyphs.GetLength(); ++i)
			{
				if (font->arGlyphs[i] && font->arGlyphs[i]->gi.bIsValidTexture)
					++glyphs;
			}

			LOGFONTA lf = {};
			GetObjectA(font->hFont, sizeof(lf), &lf);
			fprintf(file, "%-24.24s %4d %4d %5d %7d %7.1f %3d,%-2d %4d %6d  %ld/%ld/%ld/%d,", font->strFontName.c_str(),
				font->eFontId, font->nHeight, font->nMaxWidth, font->nTextureSize, font->fTextureSize,
				font->nDropShadowOffsetX, font->nDropShadowOffsetY, font->arTextures.GetLength(), glyphs,
				lf.lfHeight, lf.lfWidth, lf.lfWeight, lf.lfQuality);
			for (int i = 0; i < font->arTextures.GetLength(); ++i)
			{
				IDirect3DTexture9* texture = GetFontTexture(font, i);
				D3DSURFACE_DESC desc = {};
				if (texture)
					texture->GetLevelDesc(0, &desc);
				fprintf(file, " %p %ux%u fmt %d%s", texture, desc.Width, desc.Height, static_cast<int>(desc.Format),
					texture && s_twins.count(texture) ? " (twin)" : "");
			}
			fprintf(file, "\n");
		}
	}
	else
	{
		fprintf(file, "No font manager found\n");
	}

	if (pWndMgr)
	{
		fprintf(file, "%-28s %3s %3s  %-22s %-12s %-12s\n", "window", "vis", "kos", "location now", "original", "placed");
		for (int i = 0; i < pWndMgr->pWindows.GetCount(); ++i)
		{
			CXWnd* wnd = pWndMgr->pWindows[i];
			if (!wnd || wnd->GetParentWindow() != nullptr)
				continue;

			const CXRect rc = wnd->GetLocation();
			char original[32] = "-", placed[32] = "-";
			auto it = s_movedWindows.find(wnd);
			if (it != s_movedWindows.end())
			{
				sprintf_s(original, "%d,%d", it->second.original.x, it->second.original.y);
				sprintf_s(placed, "%d,%d", it->second.placed.x, it->second.placed.y);
			}

			char location[48];
			sprintf_s(location, "%d,%d %dx%d", rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);

			fprintf(file, "%-28.28s %3d %3d  %-22s %-12s %-12s%s\n", wnd->GetXMLName().c_str(),
				wnd->IsVisible() ? 1 : 0, wnd->bKeepOnScreen ? 1 : 0, location, original, placed,
				wnd == pWndMgr->MainWindow ? "  (MainWindow)" : "");
		}
	}

	fprintf(file, "\n");
	fclose(file);
}

static void UIScaleCommand(PlayerClient*, const char* line)
{
	char arg[MAX_STRING] = { 0 };
	GetArg(arg, line, 1);

	if (arg[0] == 0)
	{
		WriteChatf("\ag[MQUIScale]\ax scale %.2f, hooks %s. Usage: /uiscale <1.0-3.0> | off | ui | filter game|sharp|smooth | sharptext on|off | dump",
			s_scale, s_hooksInstalled ? "on" : "not installed");
		return;
	}

	if (ci_equals(arg, "off"))
	{
		SetScale(1.0f);
		WriteChatf("\ag[MQUIScale]\ax scale reset to 1.0");
		return;
	}

	if (ci_equals(arg, "filter"))
	{
		char mode[MAX_STRING] = { 0 };
		GetArg(mode, line, 2);
		if (ci_equals(mode, "sharp"))
			s_textureFilter = TextureFilter::Sharp;
		else if (ci_equals(mode, "smooth"))
			s_textureFilter = TextureFilter::Smooth;
		else
			s_textureFilter = TextureFilter::Game;
		WriteChatf("\ag[MQUIScale]\ax texture filter: %s", s_textureFilter == TextureFilter::Sharp ? "sharp"
			: s_textureFilter == TextureFilter::Smooth ? "smooth" : "game");
		return;
	}

	if (ci_equals(arg, "labelroom") || ci_equals(arg, "condense"))
	{
		char mode[MAX_STRING] = { 0 };
		GetArg(mode, line, 2);
		bool& option = ci_equals(arg, "labelroom") ? s_useLabelRoom : s_evenCondense;
		if (ci_equals(mode, "on"))
			option = true;
		else if (ci_equals(mode, "off"))
			option = false;
		else
			option = !option;
		WriteChatf("\ag[MQUIScale]\ax %s: %s (Even mode)", arg, option ? "on" : "off");
		return;
	}

	if (ci_equals(arg, "sharptext"))
	{
		char mode[MAX_STRING] = { 0 };
		GetArg(mode, line, 2);
		if (ci_equals(mode, "on"))
			s_sharpText = true;
		else if (ci_equals(mode, "off"))
			s_sharpText = false;
		else
			s_sharpText = !s_sharpText;
		WriteChatf("\ag[MQUIScale]\ax sharp text: %s%s", s_sharpText ? "on" : "off",
			s_uploadHookInstalled ? "" : " (\arhook not installed\ax)");
		return;
	}

	if (ci_equals(arg, "wide"))
	{
		char mode[MAX_STRING] = { 0 };
		GetArg(mode, line, 2);
		if (ci_equals(mode, "squeeze"))
			SetWideGlyphs(WideGlyphs::Squeeze);
		else if (ci_equals(mode, "overhang"))
			SetWideGlyphs(WideGlyphs::Overhang);
		else if (ci_equals(mode, "smaller"))
			SetWideGlyphs(WideGlyphs::Smaller);
		else if (ci_equals(mode, "spacing"))
			SetWideGlyphs(WideGlyphs::Spacing);
		else if (ci_equals(mode, "even"))
			SetWideGlyphs(WideGlyphs::Even);
		WriteChatf("\ag[MQUIScale]\ax wide letters (sharp text): %s", WideGlyphsName(s_wideGlyphs));
		return;
	}

	if (ci_equals(arg, "dump"))
	{
		DumpState("dump");
		s_pendingDumpPulses = 60;   // and again a moment later
		WriteChatf("\ag[MQUIScale]\ax state written to Logs\\MQUIScale.log (second dump follows shortly)");
		return;
	}

	if (ci_equals(arg, "ui"))
	{
		s_showWindow = !s_showWindow;
		return;
	}

	float factor = GetFloatFromString(arg, -1.0f);
	if (factor <= 0.0f)
	{
		WriteChatf("\ar[MQUIScale]\ax '%s' is not a number. Usage: /uiscale <1.0-3.0> | off | ui | filter game|sharp|smooth | sharptext on|off | wide squeeze|overhang|smaller|spacing|even | dump", arg);
		return;
	}

	SetScale(factor);
	WriteChatf("\ag[MQUIScale]\ax scale set to %.2f", s_scale);
}

//============================================================================
// Settings: config\MQUIScale.ini
//
// Saved whenever something changed (checked every pulse, so dragging the
// slider does not write the file every frame), loaded when the plugin loads.
// The scale is applied once the player is in game, so the windows of the
// world UI are the ones that get fitted.

struct Settings
{
	float scale = 1.0f;
	TextureFilter filter = TextureFilter::Game;
	bool sharpText = true;
	bool nameTagFilter = true;
	bool showWindow = true;
	WideGlyphs wideGlyphs = WideGlyphs::Overhang;
	float overhangLeft = 0.5f;
	float overhangSqueeze = 0.0f;
	float spacingMinOverhang = 1.5f;
	float evenSpaceShrink = 0.3f;
	bool useLabelRoom = true;
	bool evenCondense = true;

	bool operator==(const Settings& other) const
	{
		return scale == other.scale && filter == other.filter && sharpText == other.sharpText
			&& nameTagFilter == other.nameTagFilter && showWindow == other.showWindow
			&& wideGlyphs == other.wideGlyphs && overhangLeft == other.overhangLeft
			&& overhangSqueeze == other.overhangSqueeze && spacingMinOverhang == other.spacingMinOverhang
			&& evenSpaceShrink == other.evenSpaceShrink
			&& useLabelRoom == other.useLabelRoom && evenCondense == other.evenCondense;
	}
};

static char s_iniPath[MAX_PATH] = { 0 };
static Settings s_savedSettings;

static Settings CurrentSettings()
{
	Settings settings;
	settings.scale = s_scalePending ? s_pendingScale : s_scale;
	settings.filter = s_textureFilter;
	settings.sharpText = s_sharpText;
	settings.nameTagFilter = s_nameTagFilter;
	settings.showWindow = s_showWindow;
	settings.wideGlyphs = s_wideGlyphs;
	settings.overhangLeft = s_overhangLeft;
	settings.overhangSqueeze = s_overhangSqueeze;
	settings.spacingMinOverhang = s_spacingMinOverhang;
	settings.evenSpaceShrink = s_evenSpaceShrink;
	settings.useLabelRoom = s_useLabelRoom;
	settings.evenCondense = s_evenCondense;
	return settings;
}

static const char* FilterName(TextureFilter filter)
{
	return filter == TextureFilter::Sharp ? "sharp" : filter == TextureFilter::Smooth ? "smooth" : "game";
}

static void LoadSettings()
{
	sprintf_s(s_iniPath, "%s\\MQUIScale.ini", gPathConfig);

	char value[64] = { 0 };
	GetPrivateProfileStringA("Settings", "Scale", "1.0", value, sizeof(value), s_iniPath);
	s_pendingScale = std::clamp(GetFloatFromString(value, 1.0f), MinScale, MaxScale);
	s_scalePending = s_pendingScale != 1.0f;

	GetPrivateProfileStringA("Settings", "TextureFilter", "game", value, sizeof(value), s_iniPath);
	s_textureFilter = ci_equals(value, "sharp") ? TextureFilter::Sharp
		: ci_equals(value, "smooth") ? TextureFilter::Smooth : TextureFilter::Game;

	s_sharpText = GetPrivateProfileIntA("Settings", "SharpText", 1, s_iniPath) != 0;
	s_nameTagFilter = GetPrivateProfileIntA("Settings", "NameTagFilter", 1, s_iniPath) != 0;
	s_showWindow = GetPrivateProfileIntA("Settings", "ShowWindow", 1, s_iniPath) != 0;

	GetPrivateProfileStringA("Settings", "WideLetters", "overhang", value, sizeof(value), s_iniPath);
	s_wideGlyphs = ci_equals(value, "squeeze") ? WideGlyphs::Squeeze
		: ci_equals(value, "smaller") ? WideGlyphs::Smaller
		: ci_equals(value, "spacing") ? WideGlyphs::Spacing
		: ci_equals(value, "even") ? WideGlyphs::Even : WideGlyphs::Overhang;
	GetPrivateProfileStringA("Settings", "OverhangLeft", "0.5", value, sizeof(value), s_iniPath);
	s_overhangLeft = std::clamp(GetFloatFromString(value, 0.5f), 0.0f, 1.0f);
	GetPrivateProfileStringA("Settings", "OverhangSqueeze", "0", value, sizeof(value), s_iniPath);
	s_overhangSqueeze = std::clamp(GetFloatFromString(value, 0.0f), 0.0f, 0.2f);
	GetPrivateProfileStringA("Settings", "SpacingMinOverhang", "1.5", value, sizeof(value), s_iniPath);
	s_spacingMinOverhang = std::clamp(GetFloatFromString(value, 1.5f), 0.5f, 4.0f);
	GetPrivateProfileStringA("Settings", "EvenSpaceShrink", "0.3", value, sizeof(value), s_iniPath);
	s_evenSpaceShrink = std::clamp(GetFloatFromString(value, 0.3f), 0.0f, 0.6f);
	s_useLabelRoom = GetPrivateProfileIntA("Settings", "UseLabelRoom", 1, s_iniPath) != 0;
	s_evenCondense = GetPrivateProfileIntA("Settings", "CondenseLetters", 1, s_iniPath) != 0;

	s_savedSettings = CurrentSettings();
}

static void SaveSettingsIfChanged()
{
	const Settings settings = CurrentSettings();
	if (s_iniPath[0] == 0 || settings == s_savedSettings)
		return;

	char value[32];
	sprintf_s(value, "%.2f", settings.scale);
	WritePrivateProfileStringA("Settings", "Scale", value, s_iniPath);
	WritePrivateProfileStringA("Settings", "TextureFilter", FilterName(settings.filter), s_iniPath);
	WritePrivateProfileStringA("Settings", "SharpText", settings.sharpText ? "1" : "0", s_iniPath);
	WritePrivateProfileStringA("Settings", "NameTagFilter", settings.nameTagFilter ? "1" : "0", s_iniPath);
	WritePrivateProfileStringA("Settings", "ShowWindow", settings.showWindow ? "1" : "0", s_iniPath);
	WritePrivateProfileStringA("Settings", "WideLetters", WideGlyphsName(settings.wideGlyphs), s_iniPath);
	sprintf_s(value, "%.2f", settings.overhangLeft);
	WritePrivateProfileStringA("Settings", "OverhangLeft", value, s_iniPath);
	sprintf_s(value, "%.2f", settings.overhangSqueeze);
	WritePrivateProfileStringA("Settings", "OverhangSqueeze", value, s_iniPath);
	sprintf_s(value, "%.1f", settings.spacingMinOverhang);
	WritePrivateProfileStringA("Settings", "SpacingMinOverhang", value, s_iniPath);
	sprintf_s(value, "%.2f", settings.evenSpaceShrink);
	WritePrivateProfileStringA("Settings", "EvenSpaceShrink", value, s_iniPath);
	WritePrivateProfileStringA("Settings", "UseLabelRoom", settings.useLabelRoom ? "1" : "0", s_iniPath);
	WritePrivateProfileStringA("Settings", "CondenseLetters", settings.evenCondense ? "1" : "0", s_iniPath);
	s_savedSettings = settings;
}

static void ApplyPendingScale()
{
	const bool uiReady = s_inLoginFrontend || gGameState == GAMESTATE_CHARSELECT || gGameState == GAMESTATE_INGAME;
	if (!s_scalePending || !s_hooksInstalled || !uiReady || !pWndMgr)
		return;

	s_scalePending = false;
	DumpState("before applying saved scale");
	SetScale(s_pendingScale);
	DumpState("after applying saved scale");
	WriteChatf("\ag[MQUIScale]\ax scale %.2f loaded from MQUIScale.ini", s_scale);
}

// pWndMgr switches between eqgame's and eqmain's window manager. Our state
// about the screen extent and window positions belongs to one of them.
static void ForgetWindowManagerState()
{
	s_movedWindows.clear();
	s_forgetMovedWindows = false;
	s_extentOverridden = false;
	s_placedScreenX = 0;
	s_placedScreenY = 0;
}

PLUGIN_API void OnLoginFrontendEntered()
{
	// Coming back from the game (camp to server select): give eqgame's window
	// manager its own extent back before pWndMgr is eqmain's.
	if (s_extentOverridden && pinstCXWndManager)
	{
		if (CXWndManager* gameWndMgr = *reinterpret_cast<CXWndManager**>(pinstCXWndManager))
		{
			gameWndMgr->ScreenExtentX = s_origExtentX;
			gameWndMgr->ScreenExtentY = s_origExtentY;
		}
	}
	ForgetWindowManagerState();

	s_inLoginFrontend = true;
	InstallLoginMouseHooks();

	// Fit the login windows (scale set earlier, or the saved one).
	if (s_scale != 1.0f)
	{
		ApplyScreenExtent();
		FitWindowsToScreen();
	}
}

PLUGIN_API void OnLoginFrontendExited()
{
	// eqmain.dll is about to go away: its windows and window manager too.
	RemoveLoginMouseHooks();
	s_inLoginFrontend = false;
	ForgetWindowManagerState();
}

//============================================================================
// Plugin callbacks

PLUGIN_API void InitializePlugin()
{
	LoadSettings();
	AddCommand("/uiscale", UIScaleCommand);

	mq::MQRenderCallbacks callbacks;
	callbacks.InvalidateDeviceObjects = []() { ReleaseScaledVB(); };
	s_renderCallbacksId = mq::AddRenderCallbacks(callbacks);

	InstallHooks();

	// Loaded while the login frontend is already running.
	if (EQMainBaseAddress != 0 && gGameState == GAMESTATE_PRECHARSELECT)
		OnLoginFrontendEntered();
}

PLUGIN_API void ShutdownPlugin()
{
	RemoveCommand("/uiscale");
	SaveSettingsIfChanged();   // before the reset below, so the scale is kept

	// Back to 1.0 first: restores the screen extent and window positions.
	SetScale(1.0f);
	RemoveLoginMouseHooks();
	RemoveHooks();
	ReleaseScaledVB();
	ReleaseSharpText();

	if (s_renderCallbacksId != -1)
	{
		mq::RemoveRenderCallbacks(s_renderCallbacksId);
		s_renderCallbacksId = -1;
	}
}

PLUGIN_API void OnPulse()
{
	// The device may not exist yet when the plugin loads very early.
	if (!s_hooksInstalled)
		InstallHooks();

	if (s_pendingDumpPulses > 0 && --s_pendingDumpPulses == 0)
	{
		DumpState("dump, ~60 pulses later");
		WriteChatf("\ag[MQUIScale]\ax second dump written.");
	}

	// The client resets the screen extent when the resolution changes.
	ApplyScreenExtent();

	ApplyPendingScale();
	UpdateSharpText();
	UpdateClipMarks();
	if (s_relayoutText)
		RelayoutTextObjects();
	SaveSettingsIfChanged();
}

PLUGIN_API void OnCleanUI()
{
	// The windows are about to be destroyed. EQ may still store their
	// positions while doing so (MacroQuest calls this before the client's
	// CleanUI), and the StoreIniInfo hook needs their entries for that, so
	// the list is only forgotten when the new UI loads.
	s_forgetMovedWindows = true;

	// Fonts and their textures may be recreated: rebuild the twins.
	s_twinsDirty = true;
}

PLUGIN_API void OnReloadUI()
{
	s_twinsDirty = true;
	ForgetOldWindowsIfCleaned();

	// New windows (after /loadskin or entering the world): fit them again.
	ApplyScreenExtent();
	FitWindowsToScreen();
	if (s_scale != 1.0f)
		DumpState("ReloadUI, after fitting windows");
}

PLUGIN_API void OnUpdateImGui()
{
	// OnUpdateImGui runs once per frame: publish the counters of the last frame.
	s_lastStats = s_stats;
	s_stats = DrawStats();

	if (!s_showWindow)
		return;

	ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MQUIScale", &s_showWindow))
	{
		float scale = s_scale;
		if (ImGui::SliderFloat("UI scale", &scale, MinScale, MaxScale, "%.2f"))
			SetScale(scale);

		if (ImGui::Button("Reset (1.0)"))
			SetScale(1.0f);

		// Whole factors give the cleanest result: every texel becomes an even block.
		ImGui::SameLine();
		if (ImGui::Button("2x"))
			SetScale(2.0f);
		ImGui::SameLine();
		if (ImGui::Button("3x"))
			SetScale(3.0f);

		int filter = static_cast<int>(s_textureFilter);
		ImGui::Text("Texture filter:");
		ImGui::SameLine();
		ImGui::RadioButton("Game", &filter, static_cast<int>(TextureFilter::Game));
		ImGui::SameLine();
		ImGui::RadioButton("Sharp", &filter, static_cast<int>(TextureFilter::Sharp));
		ImGui::SameLine();
		ImGui::RadioButton("Smooth", &filter, static_cast<int>(TextureFilter::Smooth));
		s_textureFilter = static_cast<TextureFilter>(filter);

		ImGui::Checkbox("Sharp text", &s_sharpText);
		ImGui::SameLine();
		ImGui::Text("(hook %s, %dx, %d textures, %d draws/frame)", s_uploadHookInstalled ? "ok" : "missing",
			s_twinFactor, static_cast<int>(s_twins.size()), s_lastStats.sharpText);

		// Live comparison of the ways to fit W, M and other wide letters.
		int wide = static_cast<int>(s_wideGlyphs);
		ImGui::Text("Wide letters:");
		ImGui::SameLine();
		ImGui::RadioButton("Squeeze", &wide, static_cast<int>(WideGlyphs::Squeeze));
		ImGui::SameLine();
		ImGui::RadioButton("Overhang", &wide, static_cast<int>(WideGlyphs::Overhang));
		ImGui::SameLine();
		ImGui::RadioButton("Smaller", &wide, static_cast<int>(WideGlyphs::Smaller));
		ImGui::SameLine();
		ImGui::RadioButton("Spacing", &wide, static_cast<int>(WideGlyphs::Spacing));
		ImGui::SameLine();
		ImGui::RadioButton("Even", &wide, static_cast<int>(WideGlyphs::Even));
		if (s_wideGlyphs == WideGlyphs::Even)
		{
			ImGui::Text("(%d glyphs placed/frame)", s_lastStats.widenedGlyphs);
			ImGui::Text("(%d lines used label room, %d still condensed, narrowest %.0f %%, %d with shrunk spaces)",
				s_lastStats.grownLines, s_lastStats.condensedLines, s_lastStats.minDrawScale * 100.0f,
				s_lastStats.shrunkSpaceLines);
			ImGui::Checkbox("Use label room (C)", &s_useLabelRoom);
			if (!s_clipHookInstalled)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(hook not installed)");
			}
			ImGui::SameLine();
			ImGui::Checkbox("Condense letters (A)", &s_evenCondense);
			float shrinkPct = s_evenSpaceShrink * 100.0f;
			if (ImGui::SliderFloat("Spaces may shrink", &shrinkPct, 0.0f, 60.0f, "%.0f %%"))
				s_evenSpaceShrink = std::clamp(shrinkPct / 100.0f, 0.0f, 0.6f);
		}
		SetWideGlyphs(static_cast<WideGlyphs>(wide));
		if (s_wideGlyphs == WideGlyphs::Spacing)
			ImGui::Text("(%d glyphs widened/frame, %d letters given more room, %d texts relaid out)",
				s_lastStats.widenedGlyphs, static_cast<int>(s_widenedChars.size()), s_lastRelayoutCount);
		if (s_wideGlyphs == WideGlyphs::Spacing)
		{
			// Applied when the slider is let go: the widths and twins are redone.
			static float minOverhang = 1.5f;
			if (!ImGui::IsAnyItemActive())
				minOverhang = s_spacingMinOverhang;
			ImGui::SliderFloat("Room only above (px)", &minOverhang, 0.5f, 4.0f, "%.1f");
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				s_spacingMinOverhang = std::clamp(std::round(minOverhang * 2.0f) / 2.0f, 0.5f, 4.0f);
				s_twinsDirty = true;
			}
		}
		if (s_wideGlyphs == WideGlyphs::Overhang)
		{
			ImGui::Text("(%d glyphs widened/frame)", s_lastStats.widenedGlyphs);
			float leftPct = s_overhangLeft * 100.0f;
			if (ImGui::SliderFloat("Overhang to the left", &leftPct, 0.0f, 100.0f, "%.0f %%"))
				s_overhangLeft = std::clamp(leftPct / 100.0f, 0.0f, 1.0f);
			// The twins are redrawn when the slider is let go, not every frame.
			static float squeezePct = 0.0f;
			if (!ImGui::IsAnyItemActive())
				squeezePct = s_overhangSqueeze * 100.0f;
			ImGui::SliderFloat("Condense first, up to", &squeezePct, 0.0f, 20.0f, "%.0f %%");
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				s_overhangSqueeze = std::clamp(squeezePct / 100.0f, 0.0f, 0.2f);
				s_twinsDirty = true;
			}
		}

		ImGui::Separator();
		ImGui::Text("Hooks: %s, UI pass hook: %s, mouse hooks: %d, menu size hook: %s, window ini hooks: %s", s_hooksInstalled ? "on" : "off",
			s_uiHookInstalled ? "ok" : "missing", s_mouseHooksInstalled, s_screenSizeHookInstalled ? "ok" : "missing",
			s_storeIniHookInstalled && s_loadIniHookInstalled ? "ok" : "missing");
		if (pWndMgr)
		{
			int realX = 0, realY = 0;
			GetRealUISize(realX, realY);
			ImGui::Text("UI screen: %u x %u, windows moved: %d", pWndMgr->ScreenExtentX, pWndMgr->ScreenExtentY,
				static_cast<int>(s_movedWindows.size()));
			ImGui::Text("Real UI size: %d x %d (client extent %u x %u, ScreenRes %d x %d)", realX, realY,
				s_origExtentX, s_origExtentY,
				pEverQuestInfo ? pEverQuestInfo->ScreenXRes : 0, pEverQuestInfo ? pEverQuestInfo->ScreenYRes : 0);
		}
		ImGui::Text("UI passes/frame: %d", s_lastStats.uiPasses);
		ImGui::Text("Scaled draws/frame: %d", s_lastStats.scaled);
		ImGui::Text("Skipped (not RHW): %d", s_lastStats.notRHW);
		ImGui::Text("Failed (drawn unscaled): %d", s_lastStats.failed);

		ImGui::Separator();
		ImGui::Checkbox("Name tag filter", &s_nameTagFilter);
		ImGui::SameLine();
		ImGui::Text(s_nameTagMarkerInstalled ? "(marker)" : "(window rects)");
		ImGui::Text("Text outside windows (unscaled): %d vertices/frame", s_lastStats.keptVertices);
		ImGui::Text("Visible windows: %d", static_cast<int>(s_windowRects.size()));
	}
	ImGui::End();
}
