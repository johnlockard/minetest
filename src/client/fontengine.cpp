/*
Minetest
Copyright (C) 2010-2014 sapier <sapier at gmx dot net>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "fontengine.h"
#include <cmath>
#include "client/renderingengine.h"
#include "config.h"
#include "porting.h"
#include "filesys.h"
#include "gettext.h"

#if USE_FREETYPE
#include "irrlicht_changes/CGUITTFont.h"
#endif

/** maximum size distance for getting a "similar" font size */
#define MAX_FONT_SIZE_OFFSET 10

/** reference to access font engine, has to be initialized by main */
FontEngine* g_fontengine = NULL;

/** callback to be used on change of font size setting */
static void font_setting_changed(const std::string &name, void *userdata)
{
	g_fontengine->readSettings();
}

/******************************************************************************/
FontEngine::FontEngine(Settings* main_settings, gui::IGUIEnvironment* env) :
	m_settings(main_settings),
	m_env(env)
{

	for (u32 &i : m_default_size) {
		i = (FontMode) FONT_SIZE_UNSPECIFIED;
	}

	assert(m_settings != NULL); // pre-condition
	assert(m_env != NULL); // pre-condition
	assert(m_env->getSkin() != NULL); // pre-condition

	readSettings();

	if (m_currentMode == FM_Standard) {
		m_settings->registerChangedCallback("font_size", font_setting_changed, NULL);
		m_settings->registerChangedCallback("font_path", font_setting_changed, NULL);
		m_settings->registerChangedCallback("font_shadow", font_setting_changed, NULL);
		m_settings->registerChangedCallback("font_shadow_alpha", font_setting_changed, NULL);
	}
	else if (m_currentMode == FM_Fallback) {
		m_settings->registerChangedCallback("fallback_font_size", font_setting_changed, NULL);
		m_settings->registerChangedCallback("fallback_font_path", font_setting_changed, NULL);
		m_settings->registerChangedCallback("fallback_font_shadow", font_setting_changed, NULL);
		m_settings->registerChangedCallback("fallback_font_shadow_alpha", font_setting_changed, NULL);
	}

	m_settings->registerChangedCallback("mono_font_path", font_setting_changed, NULL);
	m_settings->registerChangedCallback("mono_font_size", font_setting_changed, NULL);
	m_settings->registerChangedCallback("screen_dpi", font_setting_changed, NULL);
	m_settings->registerChangedCallback("gui_scaling", font_setting_changed, NULL);
}

/******************************************************************************/
FontEngine::~FontEngine()
{
	cleanCache();
}

/******************************************************************************/
void FontEngine::cleanCache()
{
	for (auto &font_cache_it : m_font_cache) {

		for (auto &font_it : font_cache_it) {
			font_it.second->drop();
			font_it.second = NULL;
		}
		font_cache_it.clear();
	}
}

/******************************************************************************/
irr::gui::IGUIFont* FontEngine::getFont(unsigned int font_size, FontMode mode)
{
	if (mode == FM_Unspecified) {
		mode = m_currentMode;
	} else if (m_currentMode == FM_Simple) {
		// Freetype disabled -> Force simple mode
		mode = (mode == FM_Mono || mode == FM_SimpleMono) ?
				 FM_SimpleMono : FM_Simple;
	}

	// Fallback to default size
	if (font_size == FONT_SIZE_UNSPECIFIED)
		font_size = m_default_size[mode];

	const auto &cache = m_font_cache[mode];
	if (cache.find(font_size) == cache.end()) {
		if (mode == FM_Simple || mode == FM_SimpleMono)
			initSimpleFont(font_size, mode);
		else
			initFont(font_size, mode);
	}

	const auto &font = cache.find(font_size);
	return font != cache.end() ? font->second : nullptr;
}

/******************************************************************************/
unsigned int FontEngine::getTextHeight(unsigned int font_size, FontMode mode)
{
	irr::gui::IGUIFont* font = getFont(font_size, mode);

	// use current skin font as fallback
	if (font == NULL) {
		font = m_env->getSkin()->getFont();
	}
	FATAL_ERROR_IF(font == NULL, "Could not get skin font");

	return font->getDimension(L"Some unimportant example String").Height;
}

/******************************************************************************/
unsigned int FontEngine::getTextWidth(const std::wstring& text,
		unsigned int font_size, FontMode mode)
{
	irr::gui::IGUIFont* font = getFont(font_size, mode);

	// use current skin font as fallback
	if (font == NULL) {
		font = m_env->getSkin()->getFont();
	}
	FATAL_ERROR_IF(font == NULL, "Could not get font");

	return font->getDimension(text.c_str()).Width;
}


/** get line height for a specific font (including empty room between lines) */
unsigned int FontEngine::getLineHeight(unsigned int font_size, FontMode mode)
{
	irr::gui::IGUIFont* font = getFont(font_size, mode);

	// use current skin font as fallback
	if (font == NULL) {
		font = m_env->getSkin()->getFont();
	}
	FATAL_ERROR_IF(font == NULL, "Could not get font");

	return font->getDimension(L"Some unimportant example String").Height
			+ font->getKerningHeight();
}

/******************************************************************************/
unsigned int FontEngine::getDefaultFontSize()
{
	return m_default_size[m_currentMode];
}

/******************************************************************************/
void FontEngine::readSettings()
{
	if (USE_FREETYPE && g_settings->getBool("freetype")) {
		m_default_size[FM_Standard] = m_settings->getU16("font_size");
		m_default_size[FM_Fallback] = m_settings->getU16("fallback_font_size");
		m_default_size[FM_Mono]     = m_settings->getU16("mono_font_size");

		m_currentMode = is_yes(gettext("needs_fallback_font")) ?
				FM_Fallback : FM_Standard;
	} else {
		m_currentMode = FM_Simple;
	}

	m_default_size[FM_Simple]       = m_settings->getU16("font_size");
	m_default_size[FM_SimpleMono]   = m_settings->getU16("mono_font_size");

	cleanCache();
	updateFontCache();
	updateSkin();
}

/******************************************************************************/
void FontEngine::updateSkin()
{
	gui::IGUIFont *font = getFont();

	if (font)
		m_env->getSkin()->setFont(font);
	else
		errorstream << "FontEngine: Default font file: " <<
				"\n\t\"" << m_settings->get("font_path") << "\"" <<
				"\n\trequired for current screen configuration was not found" <<
				" or was invalid file format." <<
				"\n\tUsing irrlicht default font." << std::endl;

	// If we did fail to create a font our own make irrlicht find a default one
	font = m_env->getSkin()->getFont();
	FATAL_ERROR_IF(font == NULL, "Could not create/get font");

	u32 text_height = font->getDimension(L"Hello, world!").Height;
	infostream << "text_height=" << text_height << std::endl;
}

/******************************************************************************/
void FontEngine::updateFontCache()
{
	/* the only font to be initialized is default one,
	 * all others are re-initialized on demand */
	getFont(FONT_SIZE_UNSPECIFIED, FM_Unspecified);
}

/******************************************************************************/
void FontEngine::initFont(unsigned int basesize, FontMode mode)
{
	assert(mode != FM_Unspecified);
	assert(basesize != FONT_SIZE_UNSPECIFIED);

	if (m_font_cache[mode].find(basesize) != m_font_cache[mode].end())
		return;


	std::string setting_prefix = "";

	switch (mode) {
		case FM_Fallback:
			setting_prefix = "fallback_";
			break;
		case FM_Mono:
		case FM_SimpleMono:
			setting_prefix = "mono_";
			break;
		default:
			break;
	}

	u32 size = std::floor(RenderingEngine::getDisplayDensity() *
			m_settings->getFloat("gui_scaling") * basesize);
	if (size == 0) {
		errorstream << "FontEngine: attempt to use font size 0" << std::endl;
		errorstream << "  display density: " << RenderingEngine::getDisplayDensity() << std::endl;
		abort();
	}

	u16 font_shadow       = 0;
	u16 font_shadow_alpha = 0;
	g_settings->getU16NoEx(setting_prefix + "font_shadow", font_shadow);
	g_settings->getU16NoEx(setting_prefix + "font_shadow_alpha", font_shadow_alpha);

	std::string fallback_settings[] = {
		m_settings->get(setting_prefix + "font_path"),
		m_settings->get("fallback_font_path"),
		m_settings->getDefault(setting_prefix + "font_path")
	};

#if USE_FREETYPE
	for (const std::string &font_path : fallback_settings) {
		irr::gui::IGUIFont *font = gui::CGUITTFont::createTTFont(m_env,
				font_path.c_str(), size, true, true, font_shadow,
				font_shadow_alpha);

		if (font) {
			m_font_cache[mode][basesize] = font;
			return;
		}

		errorstream << "FontEngine: Cannot load '" << font_path <<
				"'. Trying to fall back to another path." << std::endl;
	}


	// give up
	errorstream << "minetest can not continue without a valid font. "
			"Please correct the 'font_path' setting or install the font "
			"file in the proper location" << std::endl;
#else
	errorstream << "FontEngine: Tried to load freetype fonts but Minetest was"
			" not compiled with that library." << std::endl;
#endif
	abort();
}

/** initialize a font without freetype */
void FontEngine::initSimpleFont(unsigned int basesize, FontMode mode)
{
	assert(mode == FM_Simple || mode == FM_SimpleMono);

	const std::string &font_path = m_settings->get(
			(mode == FM_SimpleMono) ? "mono_font_path" : "font_path");

	size_t pos_dot = font_path.find_last_of('.');
	std::string basename = font_path;
	std::string ending = lowercase(font_path.substr(pos_dot));

	if (ending == ".ttf") {
		errorstream << "FontEngine: Found font \"" << font_path
				<< "\" but freetype is not available." << std::endl;
		return;
	}

	if (ending == ".xml" || ending == ".png")
		basename = font_path.substr(0, pos_dot);

	if (basesize == FONT_SIZE_UNSPECIFIED)
		basesize = DEFAULT_FONT_SIZE;

	u32 size = std::floor(
			RenderingEngine::getDisplayDensity() *
			m_settings->getFloat("gui_scaling") *
			basesize);

	irr::gui::IGUIFont *font = nullptr;
	std::string font_extensions[] = { ".png", ".xml" };

	// Find nearest matching font scale
	// Does a "zig-zag motion" (positibe/negative), from 0 to MAX_FONT_SIZE_OFFSET
	for (s32 zoffset = 0; zoffset < MAX_FONT_SIZE_OFFSET * 2; zoffset++) {
		std::stringstream path;

		// LSB to sign
		s32 sign = (zoffset & 1) ? -1 : 1;
		s32 offset = zoffset >> 1;

		for (const std::string &ext : font_extensions) {
			path.str(""); // Clear
			path << basename << "_" << (size + offset * sign) << ext;

			if (!fs::PathExists(path.str())) 
				continue;

			font = m_env->getFont(path.str().c_str());

			if (font) {
				verbosestream << "FontEngine: found font: " << path.str() << std::endl;
				break;
			}
		}

		if (font)
			break;
	}

	// try name direct
	if (font == NULL) {
		if (fs::PathExists(font_path)) {
			font = m_env->getFont(font_path.c_str());
			if (font)
				verbosestream << "FontEngine: found font: " << font_path << std::endl;
		}
	}

	if (font)
		m_font_cache[mode][basesize] = font;
}
