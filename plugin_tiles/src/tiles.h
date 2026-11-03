#pragma once

namespace Tiles
{
	// a_edid in lower case. Call once the sheet is found and the hour is known.
	void Open(const std::string& a_edid, RE::NiAVObject* a_sheet, bool a_night);
	// Each frame while the map is open: the camera, the view's width in world units, height over width, and the camera's distance to the sheet.
	void Frame(const RE::NiPoint3& a_cam, float a_width, float a_aspect, float a_d);
	void Close();
}
