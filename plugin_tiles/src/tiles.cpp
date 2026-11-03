// Sharp pictures over the map's 8K sheet for the Large and Full sizes. The sheet can't hold them (16384 is a texture's limit, and they'd all sit in video memory),
// so only the tiles in view are read, as WebP, on worker threads, and shown on copies of the sheet's own shape placed over their ground.
//
// Tile files: Data/textures/terrain/gtsmap/t/<edid>/<tiles|tiles_n>/<256|1024>/tI_J.webp, I and J counted from the land's top left corner, 4 cells a tile at 256, 1 at 1024.
// Places: Data/SKSE/Plugins/GTSLandMapTiles.txt, one line a land: edid x0 y1 sheetx0 sheetx1 sheety0 sheety1 (cells, then world units). A land with no line just keeps its sheet.
// A shape can only show one texture, and the game's own small slot files give each copy a texture object of its own, whose view is swapped for the tile's.

#include "tiles.h"

#include <webp/decode.h>

#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>

namespace Tiles
{
	namespace
	{
		constexpr int   kSlots = 64;
		constexpr int   kKeep = 96;           // tiles kept in video memory, about 500 MB at most with their small copies
		constexpr int   kLevels = 3;          // the tile and two smaller copies: Tom's tests, 2 Nov: the whole chain down to one pixel draws as smears, three look sharp
		constexpr float kUnits = 4096.0f;
		constexpr float kFine = 24000.0f;     // a view narrower than this asks for one cell a tile
		constexpr float kWide = 90000.0f;     // a view wider than this is the sheet alone
		constexpr float kLift256 = 16.0f;     // above the sheet, in world units: more than the depth buffer's step at this distance
		constexpr float kLift1024 = 32.0f;

		struct Place
		{
			int   x0 = 0, y1 = 0;
			float sheet[4]{};
		};

		struct Slot
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiSourceTexture*          texture = nullptr;
			REX::W32::ID3D11ShaderResourceView* own = nullptr;
			std::uint64_t                 showing = 0;
		};

		struct Tile
		{
			REX::W32::ID3D11ShaderResourceView* view = nullptr;
			std::uint64_t                       used = 0;
			float                               shareX = 1.0f, shareY = 1.0f;  // a tile at the land's edge is cut short
		};

		struct Done
		{
			std::uint64_t                       key;
			REX::W32::ID3D11ShaderResourceView* view;
			float                               shareX, shareY;
		};

		std::unordered_map<std::string, Place> g_places;
		bool                                   g_placesRead = false;

		std::vector<Slot>                      g_slots;
		RE::NiAVObject*                        g_sheet = nullptr;
		RE::NiNode*                            g_parent = nullptr;
		Place                                  g_place;
		std::string                            g_folder;
		bool                                   g_on = false;
		std::uint64_t                          g_clock = 0;

		// worker side
		std::mutex                             g_lock;
		std::condition_variable                g_wake;
		std::deque<std::uint64_t>              g_wanted;
		std::vector<Done>                      g_ready;
		std::atomic<std::uint64_t>             g_session{ 0 };
		std::atomic<int>                       g_decoded{ 0 }, g_missing{ 0 }, g_failed{ 0 };
		std::vector<std::thread>               g_workers;

		// main thread side
		std::unordered_map<std::uint64_t, Tile> g_tiles;
		std::unordered_map<std::uint64_t, int>  g_asked;   // 1 asked, 2 no such file

		std::uint64_t Key(int a_level, int a_i, int a_j)
		{
			return (static_cast<std::uint64_t>(a_level) << 40) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_i) & 0xFFFFF) << 20) | (static_cast<std::uint32_t>(a_j) & 0xFFFFF);
		}

		void ReadPlaces()
		{
			g_placesRead = true;
			std::ifstream in("Data/SKSE/Plugins/GTSLandMapTiles.txt");
			std::string   line;
			while (std::getline(in, line)) {
				if (line.empty() || line[0] == '#') {
					continue;
				}
				std::istringstream s(line);
				std::string        name;
				Place              p;
				s >> name >> p.x0 >> p.y1 >> p.sheet[0] >> p.sheet[1] >> p.sheet[2] >> p.sheet[3];
				if (s.fail()) {
					logs::error("Bad tile line: {}", line);
					continue;
				}
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				g_places[name] = p;
			}
			logs::info("Tile places: {}", g_places.size());
		}

		// The tile and its smaller copies: without them a far view of a sharp picture shimmers.
		REX::W32::ID3D11ShaderResourceView* Build(std::vector<std::uint8_t>& a_px, int a_w, int a_h, int a_levels)
		{
			std::vector<std::vector<std::uint8_t>>      levels;
			std::vector<REX::W32::D3D11_SUBRESOURCE_DATA> init;
			int w = a_w, h = a_h;
			const std::uint8_t* src = a_px.data();
			init.push_back({ src, static_cast<std::uint32_t>(w * 4), 0 });
			levels.reserve(12);
			while ((w > 1 || h > 1) && static_cast<int>(init.size()) < a_levels) {
				const int nw = (std::max)(w / 2, 1), nh = (std::max)(h / 2, 1);
				std::vector<std::uint8_t> next(static_cast<std::size_t>(nw) * nh * 4);
				for (int y = 0; y < nh; ++y) {
					const auto* a = src + static_cast<std::size_t>(y * 2 % h) * w * 4;
					const auto* b = src + static_cast<std::size_t>((y * 2 + 1) % h) * w * 4;
					auto*       o = next.data() + static_cast<std::size_t>(y) * nw * 4;
					for (int x = 0; x < nw; ++x) {
						const int x0 = (x * 2 % w) * 4, x1 = ((x * 2 + 1) % w) * 4;
						for (int c = 0; c < 3; ++c) {
							o[x * 4 + c] = static_cast<std::uint8_t>((a[x0 + c] + a[x1 + c] + b[x0 + c] + b[x1 + c] + 2) / 4);
						}
						o[x * 4 + 3] = 255;
					}
				}
				levels.push_back(std::move(next));
				w = nw;
				h = nh;
				src = levels.back().data();
				init.push_back({ src, static_cast<std::uint32_t>(w * 4), 0 });
			}
			REX::W32::D3D11_TEXTURE2D_DESC d{};
			d.width = a_w;
			d.height = a_h;
			d.mipLevels = static_cast<std::uint32_t>(init.size());
			d.arraySize = 1;
			d.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
			d.sampleDesc.count = 1;
			d.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
			d.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
			auto device = RE::BSGraphics::Renderer::GetDevice();
			REX::W32::ID3D11Texture2D* tex = nullptr;
			if (!device || device->CreateTexture2D(&d, init.data(), &tex) < 0 || !tex) {
				return nullptr;
			}
			REX::W32::ID3D11ShaderResourceView* view = nullptr;
			device->CreateShaderResourceView(tex, nullptr, &view);
			tex->Release();  // the view holds it
			return view;
		}

		void Worker()
		{
			for (;;) {
				std::uint64_t key = 0, session = 0;
				std::string   folder;
				{
					std::unique_lock l(g_lock);
					g_wake.wait(l, [] { return !g_wanted.empty(); });
					key = g_wanted.front();
					g_wanted.pop_front();
					session = g_session.load();
					folder = g_folder;
				}
				const int level = static_cast<int>(key >> 40);
				const int i = static_cast<int>((key >> 20) & 0xFFFFF), j = static_cast<int>(key & 0xFFFFF);
				const auto path = std::format("{}/{}/t{}_{}.webp", folder, level, i, j);
				REX::W32::ID3D11ShaderResourceView* view = nullptr;
				float shareX = 1.0f, shareY = 1.0f;
				std::ifstream f(path, std::ios::binary);
				if (f.is_open()) {
					std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
					int w = 0, h = 0;
					if (WebPGetInfo(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(), &w, &h) && w > 0 && h > 0) {
						std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
						if (WebPDecodeRGBAInto(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(), px.data(), px.size(), w * 4)) {
							view = Build(px, w, h, kLevels);
							shareX = w / 1024.0f;
							shareY = h / 1024.0f;
						}
					}
					view ? ++g_decoded : ++g_failed;
				} else {
					++g_missing;
				}
				std::lock_guard l(g_lock);
				if (session == g_session.load()) {
					g_ready.push_back({ key, view, shareX, shareY });
				} else if (view) {
					view->Release();
				}
			}
		}

		void Hide(Slot& a_slot)
		{
			if (a_slot.showing) {
				a_slot.showing = 0;
				a_slot.node->SetAppCulled(true);
			}
		}

		// World place of the copy, from the sheet's own: the same vertices squeezed onto the tile. Lifted towards the camera and drawn in by as much,
		// so from the camera it sits exactly where it belongs on the sheet.
		void PlaceSlot(Slot& a_slot, float a_x0, float a_x1, float a_y0, float a_y1, float a_lift, const RE::NiPoint3& a_cam, float a_d)
		{
			const auto& S = g_sheet->world;
			const float r = (a_d - a_lift) / a_d;
			const float x0 = a_cam.x + (a_x0 - a_cam.x) * r, x1 = a_cam.x + (a_x1 - a_cam.x) * r;
			const float y0 = a_cam.y + (a_y0 - a_cam.y) * r, y1 = a_cam.y + (a_y1 - a_cam.y) * r;
			const auto& P = g_place.sheet;
			const float ax = (x1 - x0) / (P[1] - P[0]), ay = (y1 - y0) / (P[3] - P[2]);
			RE::NiTransform want;
			want.scale = S.scale;
			want.rotate = S.rotate;
			for (int c = 0; c < 3; ++c) {
				want.rotate.entry[0][c] *= ax;
				want.rotate.entry[1][c] *= ay;
			}
			want.translate = { x0 + ax * (S.translate.x - P[0]), y0 + ay * (S.translate.y - P[2]), S.translate.z + a_lift };
			a_slot.node->local = g_parent->world.Invert() * want;
			a_slot.node->SetAppCulled(false);
			RE::NiUpdateData update{};
			a_slot.node->Update(update);
		}

		bool MakeSlots()
		{
			auto geometry = g_sheet->AsGeometry();
			auto parent = g_sheet->parent;
			if (!geometry || !parent) {
				logs::error("Tiles: sheet has no parent");
				return false;
			}
			g_parent = parent;
			const auto& R = g_sheet->world.rotate;
			if (std::fabs(R.entry[0][1]) > 1e-4f || std::fabs(R.entry[1][0]) > 1e-4f) {
				logs::error("Tiles: sheet is turned, leaving it alone");
				return false;
			}
			for (int n = 0; n < kSlots; ++n) {
				const auto path = std::format("textures\\terrain\\gtsmap\\slot\\s{:02}.dds", n);
				RE::NiPointer<RE::NiTexture> texture;
				RE::BSShaderManager::GetTexture(path.c_str(), true, texture, false);
				auto source = netimmerse_cast<RE::NiSourceTexture*>(texture.get());
				if (!source || !source->rendererTexture) {
					logs::error("Tiles: no slot file {}", path);
					return false;
				}
				auto copy = netimmerse_cast<RE::BSGeometry*>(g_sheet->Clone());
				if (!copy) {
					logs::error("Tiles: the sheet won't copy");
					return false;
				}
				auto effect = netimmerse_cast<RE::BSEffectShaderProperty*>(copy->GetGeometryRuntimeData().shaderProperty.get());
				auto theirs = netimmerse_cast<RE::BSEffectShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
				if (!effect || effect == theirs || !effect->GetMaterial()) {
					logs::error("Tiles: copy {} shares the sheet's shader", n);
					return false;
				}
				auto material = static_cast<RE::BSEffectShaderMaterial*>(effect->GetMaterial()->Create());
				material->CopyMembers(effect->GetMaterial());
				material->sourceTexturePath = path.c_str();  // the game shares materials that look the same
				material->effectClampMode = RE::BSGraphics::TextureAddressMode::kClampSClampT;  // the sheet wraps; a tile would blend each edge with its opposite, a line at every join
				material->sourceTexture = RE::NiPointer<RE::NiSourceTexture>(source);
				effect->SetMaterial(material, true);
				if (auto used = effect->GetMaterial()) {
					used->sourceTexture = RE::NiPointer<RE::NiSourceTexture>(source);
					used->effectClampMode = RE::BSGraphics::TextureAddressMode::kClampSClampT;
				}
				Slot s;
				s.node = RE::NiPointer<RE::NiAVObject>(copy);
				s.texture = source;
				s.own = source->rendererTexture->resourceView;
				copy->SetAppCulled(true);
				parent->AttachChild(copy, true);
				g_slots.push_back(std::move(s));
			}
			return true;
		}

		void Clear()
		{
			for (auto& s : g_slots) {
				if (s.texture && s.texture->rendererTexture) {
					s.texture->rendererTexture->resourceView = s.own;
				}
				if (g_parent && s.node) {
					g_parent->DetachChild(s.node.get());
				}
			}
			g_slots.clear();
			{
				std::lock_guard l(g_lock);
				++g_session;
				g_wanted.clear();
				for (auto& d : g_ready) {
					if (d.view) {
						d.view->Release();
					}
				}
				g_ready.clear();
			}
			for (auto& [k, t] : g_tiles) {
				if (t.view) {
					t.view->Release();
				}
			}
			g_tiles.clear();
			g_asked.clear();
			g_sheet = nullptr;
			g_parent = nullptr;
			g_on = false;
		}
	}

	void Open(const std::string& a_edid, RE::NiAVObject* a_sheet, bool a_night)
	{
		if (g_on) {
			Clear();
		}
		if (!g_placesRead) {
			ReadPlaces();
		}
		auto it = g_places.find(a_edid);
		if (it == g_places.end() || !a_sheet) {
			return;
		}
		g_sheet = a_sheet;
		g_place = it->second;
		{
			std::lock_guard l(g_lock);
			g_folder = std::format("Data/textures/terrain/gtsmap/t/{}/{}", a_edid, a_night ? "tiles_n" : "tiles");
		}
		if (!MakeSlots()) {
			Clear();
			return;
		}
		if (g_workers.empty()) {
			for (int n = 0; n < 2; ++n) {
				g_workers.emplace_back(Worker).detach();
			}
		}
		g_decoded = g_missing = g_failed = 0;
		g_on = true;
		logs::info("Tiles on for {}: {} copies, folder {}", a_edid, g_slots.size(), g_folder);
	}

	void Frame(const RE::NiPoint3& a_cam, float a_width, float a_aspect, float a_d)
	{
		if (!g_on) {
			return;
		}
		++g_clock;
		// what the workers have finished
		{
			std::lock_guard l(g_lock);
			for (auto& d : g_ready) {
				if (d.view) {
					g_tiles[d.key] = { d.view, g_clock, d.shareX, d.shareY };
				} else {
					g_asked[d.key] = 2;
				}
			}
			g_ready.clear();
		}
		struct Want
		{
			std::uint64_t key;
			float         x0, x1, y0, y1, lift;
			float         dist;
		};
		std::vector<Want> wants;
		const float h = a_width * a_aspect;
		const auto  add = [&](int a_level, int a_cells, float a_lift) {
			const float size = a_cells * kUnits, top = g_place.y1 * kUnits, left = g_place.x0 * kUnits, m = size * 0.25f;
			const int   i0 = static_cast<int>(std::floor((a_cam.x - a_width / 2 - m - left) / size)), i1 = static_cast<int>(std::floor((a_cam.x + a_width / 2 + m - left) / size));
			const int   j0 = static_cast<int>(std::floor((top - (a_cam.y + h / 2 + m)) / size)), j1 = static_cast<int>(std::floor((top - (a_cam.y - h / 2 - m)) / size));
			for (int j = (std::max)(j0, 0); j <= j1; ++j) {
				for (int i = (std::max)(i0, 0); i <= i1; ++i) {
					const float x0 = left + i * size, y1 = top - j * size;
					const float cx = x0 + size / 2 - a_cam.x, cy = y1 - size / 2 - a_cam.y;
					wants.push_back({ Key(a_level, i, j), x0, x0 + size, y1 - size, y1, a_lift, cx * cx + cy * cy });
				}
			}
		};
		if (a_width < kWide) {
			add(256, 4, kLift256);
		}
		if (a_width < kFine) {
			add(1024, 1, kLift1024);
		}
		// nearest the middle first; broad tiles always have a place under the sharp ones
		std::stable_sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
			if (a.lift != b.lift) {
				return a.lift < b.lift;
			}
			return a.dist < b.dist;
		});
		std::vector<std::uint64_t> ask;
		std::size_t next = 0;
		for (auto& w : wants) {
			auto found = g_tiles.find(w.key);
			if (found == g_tiles.end()) {
				if (!g_asked.contains(w.key)) {
					g_asked[w.key] = 1;
					ask.push_back(w.key);
				}
				continue;
			}
			found->second.used = g_clock;
			if (next >= g_slots.size()) {
				continue;
			}
			auto& s = g_slots[next++];
			if (s.showing != w.key) {
				s.texture->rendererTexture->resourceView = found->second.view;
				s.showing = w.key;
			}
			PlaceSlot(s, w.x0, w.x0 + (w.x1 - w.x0) * found->second.shareX, w.y1 - (w.y1 - w.y0) * found->second.shareY, w.y1, w.lift, a_cam, a_d);
		}
		for (; next < g_slots.size(); ++next) {
			Hide(g_slots[next]);
		}
		if (!ask.empty()) {
			std::lock_guard l(g_lock);
			// the view moves on: what it asked for before and still hasn't got waits behind this
			for (auto it = ask.rbegin(); it != ask.rend(); ++it) {
				g_wanted.push_front(*it);
			}
			g_wake.notify_all();
		}
		// oldest out, never one on show
		if (g_tiles.size() > kKeep) {
			std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
			for (auto& [k, t] : g_tiles) {
				if (t.used != g_clock) {
					order.emplace_back(t.used, k);
				}
			}
			std::sort(order.begin(), order.end());
			for (std::size_t n = 0; n < order.size() && g_tiles.size() > kKeep; ++n) {
				auto it = g_tiles.find(order[n].second);
				it->second.view->Release();
				g_tiles.erase(it);
				g_asked.erase(order[n].second);
			}
		}
		if (g_clock % 600 == 1) {
			logs::info("Tiles: {} wanted, {} held, read {} missing {} failed {}", wants.size(), g_tiles.size(), g_decoded.load(), g_missing.load(), g_failed.load());
		}
	}

	void Close()
	{
		if (g_on) {
			logs::info("Tiles off: read {} missing {} failed {}", g_decoded.load(), g_missing.load(), g_failed.load());
			Clear();
		}
	}
}
