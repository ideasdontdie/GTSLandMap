// GTS Land Map: runs the flat maps' camera, view and panning while the world map is open, and swaps day and night sheets.
//
// Why the view narrows: Community Shaders' Screen Space GI fades out by 50000 units and has no map check, and it brightens
// everything nearer than that. So the camera stays past 50000 from the sheet, and the view narrows to show closer ground.
// The game's far clip for the map is 353840 (MapMenu sets it on opening), which is why hmax stays under that.

#include "tiles.h"

namespace
{
	struct World
	{
		float z, hmin, hmax, wfar, wnear;
		bool  night, record;
		int   cells[6];
		float onam[4];
		float land[4];   // where the land's picture has land: x0 x1 y0 y1, all 0 for no limit
		float sheet[4];  // the sheet's edges: x0 x1 y0 y1, all 0 for no limit
	};

	std::vector<std::pair<std::string, World>> g_worlds;

	std::string Lower(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	const World* Find(RE::TESWorldSpace* a_world)
	{
		if (!a_world) {
			return nullptr;
		}
		const auto id = Lower(a_world->editorID.c_str());
		for (auto& [name, w] : g_worlds) {
			if (name == id) {
				return &w;
			}
		}
		return nullptr;
	}

	// Speeds and zoom, in the table so they can change while the game runs.
	struct Tuning
	{
		float closest = 56000.0f;   // camera distance at the closest view: keep over 50000 for Screen Space GI
		float panNear = 20000.0f;   // pan speed at the closest view
		float panRange = 60000.0f;  // added by the widest view
		float keys = 0.00714f;      // keyboard speed, as a share of the view's width
	} g_tuning;

	// A copy in the log folder wins over the installed one, and is read every time the map opens: changes need no restart or Vortex.
	std::ifstream OpenTable()
	{
		if (auto folder = SKSE::log::log_directory()) {
			std::ifstream live(*folder / "GTSLandMap.txt");
			if (live.is_open()) {
				return live;
			}
		}
		return std::ifstream("Data/SKSE/Plugins/GTSLandMap.txt");
	}

	void LoadTable(bool a_log)
	{
		g_worlds.clear();
		g_tuning = Tuning{};
		auto        in = OpenTable();
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::istringstream s(line);
			std::string        name;
			if (line[0] == '@') {
				s >> name >> g_tuning.closest >> g_tuning.panNear >> g_tuning.panRange >> g_tuning.keys;
				continue;
			}
			World              w{};
			int                night = 0, record = 0;
			s >> name >> w.z >> w.hmin >> w.hmax >> w.wfar >> w.wnear >> night;
			for (auto& c : w.cells) {
				s >> c;
			}
			for (auto& o : w.onam) {
				s >> o;
			}
			s >> record;
			for (auto& l : w.land) {
				s >> l;
			}
			if (s.fail()) {
				logs::error("Bad line: {}", line);
				continue;
			}
			for (auto& e : w.sheet) {
				s >> e;
			}
			if (s.fail()) {
				std::fill(std::begin(w.sheet), std::end(w.sheet), 0.0f);
			}
			w.night = night != 0;
			w.record = record != 0;
			// A land left out at install keeps its paper map, untouched.
			const auto file = "textures\\terrain\\gtsmap\\" + Lower(name) + ".dds";
			if (!std::filesystem::exists("Data/" + file) && !RE::BSResourceNiBinaryStream(file).good()) {
				if (a_log) {
					logs::info("{} not installed, left alone", name);
				}
				continue;
			}
			g_worlds.emplace_back(Lower(name), w);
		}
		if (a_log) {
			logs::info("Worlds in table: {}", g_worlds.size());
		}
	}

	// In memory, so a later plugin carrying the old map record can't put the camera back under the sheet (the purple map).
	// The flat map's weather carries a 21% red tint, which turns our pictures' snow pink. Paper maps keep it.
	void SetMapTint(bool a_on)
	{
		static std::vector<std::pair<RE::TESImageSpace*, float>> saved;
		auto weather = RE::TESForm::LookupByID<RE::TESWeather>(0xA6858);
		if (!weather) {
			return;
		}
		for (auto space : weather->imageSpaces) {
			if (!space) {
				continue;
			}
			auto it = std::find_if(saved.begin(), saved.end(), [&](auto& a_entry) { return a_entry.first == space; });
			if (it == saved.end()) {
				saved.emplace_back(space, space->data.tint.amount);
				it = std::prev(saved.end());
				logs::info("Map weather tint {} on {:X}", it->second, space->GetFormID());
			}
			space->data.tint.amount = a_on ? it->second : 0.0f;
		}
	}

	void ApplyRecords(bool a_log)
	{
		auto data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		int done = 0;
		for (auto form : data->GetFormArray<RE::TESWorldSpace>()) {
			auto w = Find(form);
			if (!w) {
				continue;
			}
			auto& m = form->worldMapData;
			auto& o = form->worldMapOffsetData;
			if (a_log) {
				logs::info("{} was min {} max {} pitch {} onam {} {} {} {}", form->editorID.c_str(), m.cameraData.minHeight, m.cameraData.maxHeight, m.cameraData.initialPitch,
					o.mapScale, o.mapOffsetX, o.mapOffsetY, o.mapOffsetZ);
			}
			if (w->record) {
				m.usableWidth = static_cast<std::uint32_t>(w->cells[0]);
				m.usableHeight = static_cast<std::uint32_t>(w->cells[1]);
				m.nwCellX = static_cast<std::int16_t>(w->cells[2]);
				m.nwCellY = static_cast<std::int16_t>(w->cells[3]);
				m.seCellX = static_cast<std::int16_t>(w->cells[4]);
				m.seCellY = static_cast<std::int16_t>(w->cells[5]);
				o.mapScale = w->onam[0];
				o.mapOffsetX = w->onam[1];
				o.mapOffsetY = w->onam[2];
				o.mapOffsetZ = w->onam[3];
			}
			m.cameraData.minHeight = w->hmin;
			m.cameraData.maxHeight = w->hmax;
			m.cameraData.initialPitch = 90.0f;  // a later plugin's old record tilts the view off the sheet as well as lowering it
			++done;
		}
		if (a_log) {
			logs::info("Map records set: {}", done);
		}
	}

	void SetSetting(RE::Setting*& a_setting, std::string_view a_name, float a_value)
	{
		if (!a_setting) {
			if (auto ini = RE::INISettingCollection::GetSingleton()) {
				a_setting = ini->GetSetting(a_name);
			}
		}
		if (a_setting) {
			a_setting->data.f = a_value;
		}
	}

	float           g_sheetZ = 0.0f;    // where the sheet really is in the scene, 0 until found
	RE::NiAVObject* g_sheet = nullptr;  // only used while the map is open

	// Night and day: every sheet carrying one of our textures gets the picture for the hour.
	int SwapSheets(RE::NiAVObject* a_object, bool a_night)
	{
		if (!a_object) {
			return 0;
		}
		int found = 0;
		if (auto node = a_object->AsNode()) {
			for (auto& child : node->GetChildren()) {
				found += SwapSheets(child.get(), a_night);
			}
			return found;
		}
		auto geometry = a_object->AsGeometry();
		if (!geometry) {
			return 0;
		}
		auto effect = netimmerse_cast<RE::BSEffectShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (!effect) {
			return 0;
		}
		auto material = effect->GetMaterial();
		if (!material || !material->sourceTexturePath.c_str()) {
			return 0;
		}
		std::string path = material->sourceTexturePath.c_str();
		if (Lower(path).find("gtsmap") == std::string::npos) {
			return 0;
		}
		g_sheetZ = a_object->world.translate.z;
		g_sheet = a_object;
		if (a_night) {
			path.insert(path.size() - 4, "_n");
		}
		RE::NiPointer<RE::NiTexture> texture;
		RE::BSShaderManager::GetTexture(path.c_str(), true, texture, false);
		auto source = netimmerse_cast<RE::NiSourceTexture*>(texture.get());
		if (!source) {
			logs::error("Could not load {}", path);
			return 0;
		}
		material->sourceTexture = RE::NiPointer<RE::NiSourceTexture>(source);
		logs::info("Sheet {} now shows {}", a_object->name.c_str(), path);
		return 1;
	}

	struct State
	{
		bool  open = false;
		float savedWorld = 90.0f, savedFirst = 90.0f;
		int   frame = 0;
		bool  sheetsDone = false;
		bool  frustumFallback = false;
		int   mismatch = 0;
		float lastD = 0.0f;
		int   still = 0;
		float lastLoggedD = 0.0f;
		bool  fogSaved = false;
		bool  tintSet = false;
		bool  fogLogged = false;
		float fogNear = 0.0f, fogFar = 0.0f, fogClamp = 0.0f;
		bool  edgeLogged = false;
	} g;

	std::vector<RE::TESWorldSpace*> g_raised;  // lands whose limit has been lifted since the map opened: once each, so it can't creep

	// Worlds with no sky (Blackreach) don't get the map's fog-free weather, so their own fog can swallow a sheet this far away.
	void ClearFog()
	{
		auto tes = RE::TES::GetSingleton();
		auto sky = RE::Sky::GetSingleton();
		auto fog = tes ? netimmerse_cast<RE::BSFogProperty*>(tes->objFog) : nullptr;
		if (fog && !g.fogSaved) {
			g.fogSaved = true;
			g.fogNear = fog->nearDistance;
			g.fogFar = fog->farDistance;
			g.fogClamp = fog->clamp;
		}
		if (!g.fogLogged && g_sheet) {
			g.fogLogged = true;
			logs::info("No sky world: fog {} to {} clamp {}, sky fog {} to {} clamp {} mode {} weather {:X}", fog ? fog->nearDistance : -1.0f, fog ? fog->farDistance : -1.0f, fog ? fog->clamp : -1.0f,
				sky ? sky->fogNear : -1.0f, sky ? sky->fogFar : -1.0f, sky ? sky->fogClamp : -1.0f, sky ? static_cast<int>(sky->mode.get()) : -1, sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0);
			for (RE::NiAVObject* p = g_sheet; p; p = p->parent) {
				logs::info("  sheet chain: {} culled {}", p->name.c_str(), p->GetAppCulled());
			}
		}
		if (fog) {
			fog->nearDistance = 900000.0f;
			fog->farDistance = 1000000.0f;
			fog->clamp = 0.0f;
		}
		if (sky) {
			sky->fogNear = 900000.0f;
			sky->fogFar = 1000000.0f;
			sky->fogClamp = 0.0f;
		}
		for (RE::NiAVObject* p = g_sheet; p; p = p->parent) {
			if (p->GetAppCulled()) {
				p->SetAppCulled(false);
			}
		}
	}

	void RestoreFog()
	{
		auto tes = RE::TES::GetSingleton();
		auto fog = tes ? netimmerse_cast<RE::BSFogProperty*>(tes->objFog) : nullptr;
		if (fog && g.fogSaved) {
			fog->nearDistance = g.fogNear;
			fog->farDistance = g.fogFar;
			fog->clamp = g.fogClamp;
		}
	}

	void Frame(RE::MapMenu* a_menu)
	{
		auto& data = *a_menu->GetRuntimeData2();
		auto  world = Find(data.worldSpace);
		auto  root = data.camera.cameraRoot.get();
		auto  player = RE::PlayerCamera::GetSingleton();
		if (g.open && !g.tintSet) {
			g.tintSet = true;
			SetMapTint(world == nullptr);
		}
		if (!world || !root || !player || !g.open) {
			return;
		}
		++g.frame;

		// Real distance to the sheet once it's found. Record height and camera height may differ by the land's map offset.
		auto        camera = RE::Main::WorldRootCamera();
		const float camZ = camera ? camera->world.translate.z : root->world.translate.z;
		const float d0 = g_tuning.closest;
		const float d1 = d0 + (world->hmax - world->hmin);
		const float d = g_sheetZ != 0.0f ? (std::max)(1000.0f, camZ - g_sheetZ) : (std::max)(1000.0f, root->world.translate.z - world->z - 6000.0f);

		// The opening and closing flights pass close to the sheet, so only the settled view counts.
		RE::MapCameraStates::World* settled = nullptr;
		for (auto& state : data.camera.unk68) {
			if (state && static_cast<RE::TESCameraState*>(state.get()) == data.camera.currentState.get()) {
				settled = state.get();
			}
		}
		g.still = settled && std::fabs(d - g.lastD) < 1.0f ? g.still + 1 : 0;
		g.lastD = d;

		if (g_sheetZ != 0.0f && d < 52000.0f && g.still > 20 && std::find(g_raised.begin(), g_raised.end(), data.worldSpace) == g_raised.end()) {
			// Too close for Screen Space GI's fade: lift this land's limit by the shortfall, once.
			g_raised.push_back(data.worldSpace);
			auto& limits = data.worldSpace->worldMapData.cameraData;
			limits.minHeight += 54000.0f - d;
			limits.maxHeight = (std::max)(limits.maxHeight, limits.minHeight + 40000.0f);
			logs::warn("Settled {} from the sheet, minimum height now {}", d, limits.minHeight);
		}

		// Stop the view wandering off the land into the dark round it.
		if (settled && world->land[1] > world->land[0]) {
			auto&       offset = settled->currentPositionScrollOffset;
			const auto& base = settled->currentPosition;
			const float x = std::clamp(offset.x, world->land[0] - base.x, world->land[1] - base.x);
			const float y = std::clamp(offset.y, world->land[2] - base.y, world->land[3] - base.y);
			if (!g.edgeLogged && (x != offset.x || y != offset.y)) {
				g.edgeLogged = true;
				logs::info("Reached the land's edge at {} {}, base {} {}", offset.x + base.x, offset.y + base.y, base.x, base.y);
			}
			offset.x = x;
			offset.y = y;
		}

		if (data.worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoSky)) {
			ClearFog();
		}
		const float t = std::clamp(std::log(d / d0) / std::log(d1 / d0), 0.0f, 1.0f);
		const float width = world->wnear * std::pow(world->wfar / world->wnear, t);
		const float half = (std::min)(width / (2.0f * d), 1.0f);
		const float fov = 2.0f * std::atan(half) * 57.29578f;

		// Keep the whole screen on the sheet, so the backdrop under it never shows at the sides.
		if (settled && world->sheet[1] > world->sheet[0]) {
			const auto& fr = camera ? camera->GetRuntimeData2().viewFrustum : RE::NiFrustum{};
			const float aspect = fr.fRight != 0.0f ? fr.fTop / fr.fRight : 0.5625f;
			auto&       offset = settled->currentPositionScrollOffset;
			const auto& base = settled->currentPosition;
			const auto  inside = [](float a_at, float a_lo, float a_hi, float a_half) {
				return a_hi - a_lo <= a_half * 2.0f ? (a_lo + a_hi) / 2.0f : std::clamp(a_at, a_lo + a_half, a_hi - a_half);
			};
			offset.x = inside(base.x + offset.x, world->sheet[0], world->sheet[1], width / 2.0f) - base.x;
			offset.y = inside(base.y + offset.y, world->sheet[2], world->sheet[3], width / 2.0f * aspect) - base.y;
		}

		player->GetRuntimeData2().worldFOV = fov;
		player->GetRuntimeData2().firstPersonFOV = fov;

		// The game normally follows worldFOV by itself. If it doesn't, set the view directly.
		if (camera) {
			auto& fr = camera->GetRuntimeData2().viewFrustum;
			g.mismatch = std::fabs(fr.fRight - half) > half * 0.05f ? g.mismatch + 1 : 0;
			if (g.mismatch > 30) {
				if (!g.frustumFallback) {
					g.frustumFallback = true;
					logs::warn("View didn't follow worldFOV: right {} wanted {}, setting it directly", fr.fRight, half);
				}
				const float aspect = fr.fRight != 0.0f ? fr.fTop / fr.fRight : 0.5625f;
				fr.fRight = half;
				fr.fLeft = -half;
				fr.fTop = half * aspect;
				fr.fBottom = -half * aspect;
			}
		}

		// Mouse panning uses these directly; WASD is pan speed times the keyboard value. Set from what Tom found right:
		// 200 a step at the closest view, 3200 at the widest, in proportion to what's on screen.
		const float pan = (std::min)(width / 28000.0f, 1.0f) * g_tuning.panNear + g_tuning.panRange * std::clamp(std::log(width / 28000.0f) / std::log(498000.0f / 28000.0f), 0.0f, 1.0f);
		static RE::Setting* minPan = nullptr;
		static RE::Setting* maxPan = nullptr;
		static RE::Setting* keys = nullptr;
		SetSetting(minPan, "fMapWorldMinPanSpeed:MapMenu", pan);
		SetSetting(maxPan, "fMapWorldMaxPanSpeed:MapMenu", pan);
		SetSetting(keys, "fMapMoveKeyboardSpeed:MapMenu", g_tuning.keys * width / pan);

		if (camera && g_sheet && settled) {
			const auto& fr = camera->GetRuntimeData2().viewFrustum;
			Tiles::Frame(camera->world.translate, width, fr.fRight != 0.0f ? fr.fTop / fr.fRight : 0.5625f, d);
		}

		if (std::fabs(d - g.lastLoggedD) > d * 0.1f) {
			g.lastLoggedD = d;
			logs::info("Camera {} {} z {} distance {} width {} fov {} right {} pan {}", root->world.translate.x, root->world.translate.y, root->world.translate.z, d, width, fov, camera ? camera->GetRuntimeData2().viewFrustum.fRight : 0.0f, pan);
		}

		if (!g.sheetsDone && g.frame % 30 == 1) {
			auto       calendar = RE::Calendar::GetSingleton();
			const auto hour = calendar ? calendar->GetHour() : 12.0f;
			const bool night = world->night && (hour >= 20.0f || hour < 5.0f);
			auto       tes = RE::TES::GetSingleton();
			int        found = tes ? SwapSheets(tes->lodLandRoot, night) : 0;
			if (!found) {
				found = SwapSheets(RE::Main::WorldRootNode(), night);
			}
			if (found || g.frame > 600) {
				g.sheetsDone = true;
				if (found) {
					Tiles::Open(Lower(data.worldSpace->editorID.c_str()), g_sheet, night);
				}
				logs::info("Hour {} night {} sheets {} frame {} sheet z {} camera z {}", hour, night, found, g.frame, g_sheetZ, camZ);
			}
		}
	}

	struct AdvanceHook
	{
		static void Thunk(RE::MapMenu* a_menu, float a_interval, std::uint32_t a_time)
		{
			Frame(a_menu);
			Original(a_menu, a_interval, a_time);
		}
		static inline REL::Relocation<decltype(Thunk)> Original;
	};

	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!a_event || a_event->menuName != RE::MapMenu::MENU_NAME) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto player = RE::PlayerCamera::GetSingleton();
			if (!player) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto& fov = player->GetRuntimeData2();
			if (a_event->opening && !g.open) {
				g = State{};
				g.open = true;
				LoadTable(false);
				ApplyRecords(false);
				g_raised.clear();
				g_sheetZ = 0.0f;
				g_sheet = nullptr;
				g.savedWorld = fov.worldFOV;
				g.savedFirst = fov.firstPersonFOV;
				logs::info("Map opened, FOV was {} {}", g.savedWorld, g.savedFirst);
			} else if (!a_event->opening && g.open) {
				g.open = false;
				Tiles::Close();
				RestoreFog();
				g_sheet = nullptr;
				if (g.frustumFallback) {
					// The game only rebuilds the view when the value differs from its last one.
					if (auto scene = RE::Main::WorldRootNode()) {
						static_cast<RE::BSSceneGraph*>(scene)->GetRuntimeData().cameraFOV = 0.0f;
					}
				}
				fov.worldFOV = g.savedWorld;
				fov.firstPersonFOV = g.savedFirst;
				logs::info("Map closed, FOV back to {} {}", g.savedWorld, g.savedFirst);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			LoadTable(true);
			ApplyRecords(true);
			if (!g_worlds.empty()) {
				SetMapTint(false);
			}
			static MenuSink sink;
			if (auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
			}
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MapMenu[0] };
			AdvanceHook::Original = vtable.write_vfunc(0x5, AdvanceHook::Thunk);
			logs::info("Ready");
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
