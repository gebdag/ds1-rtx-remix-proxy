#include <unordered_map>
#include <unordered_set>
#include "std_include.hpp"
#include "skinning.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ffp_prof.hpp"
#include "../game/texture_scroll.hpp"
#include "../game/texture_names.hpp"
#include "../game/mesh_tables.hpp"
#include "../game/effect_lights.hpp"
#include "../game/atmosphere.hpp"
#include "../game/map_lights.hpp"

namespace comp
{
	skinning::skinning()
	{
		p_this = this;
		initialized_ = true;
		s_shell_fur = shared::common::config::get().get_int("Fur", "ShellGeometry", 0) != 0;
		shared::common::log("Skinning", "Module initialized.");
	}

	skinning::~skinning()
	{
		on_reset();
		p_this = nullptr;
	}

	HRESULT skinning::draw_skinned_dip(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE pt, INT base_vtx, UINT min_vtx, UINT num_verts,
		UINT start_idx, UINT prim_count)
	{
		auto& ffp = shared::common::ffp_state::get();

		struct stats_scope
		{
			LARGE_INTEGER start {};
			stats_scope() { if (shared::common::ffp_prof::profiling()) QueryPerformanceCounter(&start); }
			~stats_scope()
			{
				if (!start.QuadPart)
					return;
				LARGE_INTEGER end;
				QueryPerformanceCounter(&end);
				s_stats.ticks += end.QuadPart - start.QuadPart;
				++s_stats.draws;
			}
		} stats;

		// Create expanded declaration on first use
		if (!skin_exp_decl_)
			create_expanded_decl(dev);

		if (!skin_exp_decl_)
		{
			// Fallback: passthrough with shaders
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		// Get expanded vertex buffer from cache or create new
		auto* src_vb = ffp.stream_vb(0);
		UINT stride = ffp.stream_stride(0);
		UINT total_verts = min_vtx + num_verts;
		// A fur shell carries its vertex colour (ffp_state::cur_draw_is_fur_shell).
		const std::string diffuse_name = game::texture_names::name_of(ffp.cur_texture(0));
		const bool priscilla_fur = diffuse_name == "c2730_2" && ffp.cur_decl_has_color();
		// Shell fur whose diffuse is neither named for fur nor punch-through: Sif (c5210_2) and the Darkroot beast (c5360_2), seen in
		// frame_20261111_115114 / 115217 / 115518. Listed by name, like Priscilla's; any other shell mesh is only reported, or taken
		// with [Fur] ShellGeometry on (is_shell_fur). The covenant cat Alvina (c5369_2, frame_20261112_015533) is the beast's model
		// with her own textures; hers is punch-through, so she had her vertex colour but not the fur marker below. Her texture
		// gets its name from its content (texture_names.cpp name_by_content).
		// Blended draws only (every draw of the two in the captures is): also while the creature fades out (c139.w), as Priscilla's.
		DWORD listed_blend = FALSE;
		const bool listed_fur = (diffuse_name == "c5210_2" || diffuse_name == "c5360_2" || diffuse_name == "c5369_2") && ffp.cur_decl_has_color() &&
			SUCCEEDED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &listed_blend)) && listed_blend;
		const bool shell_fur = skin_overlay_decl_ && !priscilla_fur &&
			(listed_fur || is_shell_fur(dev, diffuse_name, src_vb, base_vtx, total_verts, stride));
		const bool fur = skin_overlay_decl_ && (priscilla_fur || shell_fur || ffp.cur_draw_is_fur_shell(dev));
		// A body part's vertices carry their place on the body, for the runtime's wet player. Hair reads the grid too,
		// but its cards are not body-tagged in lightmap_sun, so only the body wets the grid.
		// Not on what the game blends otherwise (ghosts, a fading body), which needs its alpha as it is.
		DWORD body_blend = FALSE, body_dest = D3DBLEND_ZERO;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &body_blend);
		dev->GetRenderState(D3DRS_DESTBLEND, &body_dest);
		const bool body = skin_overlay_decl_ && !fur && (!body_blend || body_dest == D3DBLEND_INVSRCALPHA) &&
			game::texture_names::is_body_part(ffp.cur_texture(0));
		const bool coloured = fur || body;
		auto* exp_vb = get_expanded_vb(dev, src_vb, base_vtx, total_verts, stride,
			fur ? skin_layout::vertex_colour : body ? skin_layout::body : skin_layout::base);

		if (!exp_vb)
		{
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		// Engage FFP with skinning
		ffp.engage(dev);

		// Set albedo texture — skip the global-texture filter for skinned draws
		// (characters/enemies should never be filtered out).
		ffp.setup_albedo_texture(dev);

		upload_bones(dev);
		log_transition(dev, src_vb, base_vtx, total_verts, stride, start_idx, prim_count);

		// Swap in expanded VB and declaration
		IDirect3DVertexDeclaration9* orig_decl = nullptr;
		dev->GetVertexDeclaration(&orig_decl);
		dev->SetVertexDeclaration(coloured ? skin_overlay_decl_ : skin_exp_decl_);
		dev->SetStreamSource(0, exp_vb, 0, coloured ? SKIN_OVERLAY_VTX_SIZE : SKIN_VTX_SIZE);
		if (fur)
			ffp.apply_vertex_colour_stage(dev);
		else if (body)
			ffp.apply_texture_only_stage(dev);

		if (game::mesh_tables::is_emitter_draw(ffp.cur_ib(), pt, start_idx, prim_count))
			game::effect_lights::observe_emitter(ffp.vs_const_data());
		const bool tinted = ffp.apply_draw_tint(dev);
		const bool effect = ffp.apply_sp_effect(dev);
		const bool toned = ffp.apply_skin_tone(dev, false);
		// Priscilla's fur (c2730_2): many overlapping blended cards at a quarter coverage. Remix's alpha-blended layers lose most of her against the dark behind
		// (half the game's brightness). The game's velocity pass draws the same cards as a cutout, alpha test 128: so do these.
		const bool fur_cards = priscilla_fur && ffp.ps_const_data()[139 * 4 + 3] >= 0.99f; // not while she fades (invisibility goes through c139.w)
		DWORD fur_blend = FALSE, fur_test = FALSE, fur_ref = 0, fur_func = 0;
		if (fur_cards)
		{
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &fur_blend);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &fur_test);
			dev->GetRenderState(D3DRS_ALPHAREF, &fur_ref);
			dev->GetRenderState(D3DRS_ALPHAFUNC, &fur_func);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
			// Tested against texture alpha times the shell's vertex alpha, which falls layer by layer: lower layers keep most of the hair, the outer ones only its tips.
			static const int fur_alpha_ref = shared::common::config::get().get_int("Fur", "AlphaRef", 64);
			dev->SetRenderState(D3DRS_ALPHAREF, fur_alpha_ref);
			dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		}
		const bool scrolled = game::texture_scroll::apply(dev, false, false, src_vb, ffp.cur_ib(),
			base_vtx, start_idx, prim_count, ffp.frame_count());
		// Seath's wings, tentacles and band (c5290_2 at c100 3): alpha-blended layers the game brightens past white, so they read as solid
		// light. As lit see-through layers Remix showed them dim and see-through. They carry the bright-character tag already (atmosphere
		// tag_self_lit), so Remix gives them texture * c100 * light as their own light once it draws them as a surface:
		// - mode 2 (default): a cutout (alpha test, no blend), as Priscilla's fur: an ordinary surface of the frame, upscaled, denoised and
		//   hazed at its own depth like the rest;
		// - mode 1: added (destination One), as the game's additive effects; flickers, as additive layers come back at render resolution.
		// proxy.Character.SeathGlow (0 off), proxy.Character.SeathAlphaRef.
		const int seath_mode = game::atmosphere::seath_glow_mode();
		DWORD seath_dest = 0, seath_blend = FALSE, seath_test = FALSE, seath_ref = 0, seath_func = 0;
		const float* glow_ps = ffp.ps_const_data();
		// FFP conversion on only: the F8 vanilla view turns it off and must show the game's draws as they are.
		const bool seath_glow = seath_mode != 0 && ffp.is_enabled() && glow_ps && glow_ps[100 * 4] >= 2.9f &&
			game::texture_names::name_of(ffp.cur_texture(0)) == "c5290_2" &&
			SUCCEEDED(dev->GetRenderState(D3DRS_DESTBLEND, &seath_dest)) && seath_dest == D3DBLEND_INVSRCALPHA;
		if (seath_glow && seath_mode == 1)
			dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		else if (seath_glow)
		{
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &seath_blend);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &seath_test);
			dev->GetRenderState(D3DRS_ALPHAREF, &seath_ref);
			dev->GetRenderState(D3DRS_ALPHAFUNC, &seath_func);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
			dev->SetRenderState(D3DRS_ALPHAREF, game::atmosphere::seath_alpha_ref());
			dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		}
		// Blended fur (a diffuse named for fur: the Taurus Demon's c2250_fur, Priscilla's, the DLC's; or shell fur by its geometry: Sif,
		// the Darkroot beast) carries the fur marker: Remix blends its denser texels in order rather than stochastically
		// (rtx.stochasticAlphaBlendOpacityThresholdFur, Surface::isDs1Fur). Not by fur_shell: that test (punch-through DXT1 with vertex
		// colour) also takes bushes. The Taurus is no shell geometry: each of its layers is a draw of its own. Stage 4 put back after.
		constexpr DWORD kFurMarker = 0xD400000F;
		DWORD fur_stage4 = 0, fur_alpha_blend = FALSE;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &fur_alpha_blend);
		const bool fur_marked = fur_alpha_blend && (diffuse_name.find("_fur") != std::string::npos || priscilla_fur || shell_fur);
		if (fur_marked)
		{
			dev->GetTextureStageState(4, D3DTSS_CONSTANT, &fur_stage4);
			dev->SetTextureStageState(4, D3DTSS_CONSTANT, kFurMarker);
		}
		auto hr = dev->DrawIndexedPrimitive(pt, 0, min_vtx, num_verts, start_idx, prim_count);
		if (seath_glow && seath_mode == 1)
			dev->SetRenderState(D3DRS_DESTBLEND, seath_dest);
		else if (seath_glow)
		{
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, seath_blend);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, seath_test);
			dev->SetRenderState(D3DRS_ALPHAREF, seath_ref);
			dev->SetRenderState(D3DRS_ALPHAFUNC, seath_func);
		}
		// More shells than the mesh has: the same triangles again between its layers (expand_skin_vertex).
		// Her cutout only: blended shells (rats) gained nothing measurable and would only stack up coverage.
		if (fur && fur_cards && SUCCEEDED(hr))
		{
			static const int extra = std::clamp(shared::common::config::get().get_int("Fur", "ExtraLayers", 1), 0, 3);
			fur_layer_count_ = extra;
			static constexpr skin_layout layers[3] = { skin_layout::fur_layer_1, skin_layout::fur_layer_2, skin_layout::fur_layer_3 };
			for (int k = 0; k < extra; k++)
				if (auto* layer_vb = get_expanded_vb(dev, src_vb, base_vtx, total_verts, stride, layers[k]))
				{
					dev->SetStreamSource(0, layer_vb, 0, SKIN_OVERLAY_VTX_SIZE);
					dev->DrawIndexedPrimitive(pt, 0, min_vtx, num_verts, start_idx, prim_count);
				}
		}
		if (fur_marked)
			dev->SetTextureStageState(4, D3DTSS_CONSTANT, fur_stage4);
		if (scrolled)
			game::texture_scroll::clear(dev);
		if (toned)
			ffp.clear_skin_tone(dev);
		if (fur_cards)
		{
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, fur_blend);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, fur_test);
			dev->SetRenderState(D3DRS_ALPHAREF, fur_ref);
			dev->SetRenderState(D3DRS_ALPHAFUNC, fur_func);
		}
		// Two-layer material (the FaceGen face: lerp(s0, s3 + c156, vertex alpha)): the second
		// diffuse as a coplanar decal with vertex alpha as its opacity, as on the rigid path.
		if (SUCCEEDED(hr) && ffp.overlay_texture())
		{
			const HRESULT overlay_hr = draw_overlay(dev, pt, src_vb, base_vtx, min_vtx, num_verts,
				start_idx, prim_count, stride);
			if (FAILED(overlay_hr))
				hr = overlay_hr;
		}
		if (tinted)
			ffp.clear_draw_tint(dev);
		if (effect)
			ffp.clear_sp_effect(dev);

		// Restore original state
		dev->SetVertexDeclaration(orig_decl);
		if (orig_decl) orig_decl->Release();
		dev->SetStreamSource(0, src_vb, ffp.stream_offset(0), stride);

		disable_skinning(dev);
		ffp.restore_textures(dev);
		ffp.disengage(dev);

		return hr;
	}

	void skinning::on_reset()
	{
		release_cache();
		for (auto** decl : { &skin_exp_decl_, &skin_overlay_decl_ })
		{
			if (*decl)
			{
				(*decl)->Release();
				*decl = nullptr;
			}
		}
	}

	void skinning::create_expanded_decl(IDirect3DDevice9* dev)
	{
		// Expanded skinned vertex layout: FLOAT3 pos + FLOAT3 weights + UBYTE4 idx + FLOAT3 normal + FLOAT2 uv
		const D3DVERTEXELEMENT9 base[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
			{ 0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
			{ 0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
			{ 0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};
		// The overlay pass: the same, the uv being the second set, plus the vertex colour.
		const D3DVERTEXELEMENT9 overlay[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
			{ 0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
			{ 0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
			{ 0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 0, 48, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
			D3DDECL_END()
		};

		if (!skin_exp_decl_ && FAILED(dev->CreateVertexDeclaration(base, &skin_exp_decl_)))
			skin_exp_decl_ = nullptr;
		if (!skin_overlay_decl_ && FAILED(dev->CreateVertexDeclaration(overlay, &skin_overlay_decl_)))
			skin_overlay_decl_ = nullptr;
		if (!skin_exp_decl_ || !skin_overlay_decl_)
			shared::common::log("Skinning", "Failed to create expanded vertex declaration!",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
	}

	void skinning::release_cache()
	{
		for (auto& [key, entry] : skin_cache_)
		{
			if (entry.vb)
				entry.vb->Release();
		}
		skin_cache_.clear();
	}

	void skinning::prune_cache(UINT frame)
	{
		for (auto it = skin_cache_.begin(); it != skin_cache_.end(); )
		{
			if (frame - it->second.last_frame > SKIN_CACHE_MAX_AGE)
			{
				if (it->second.vb)
					it->second.vb->Release();
				it = skin_cache_.erase(it);
			}
			else
			{
				++it;
			}
		}
		last_prune_frame_ = frame;
	}

	uint64_t skinning::fingerprint(const unsigned char* src, UINT num_verts, UINT stride)
	{
		// FNV-1a over the full bytes of up to 64 vertices spread across the range,
		// always including the first and the last.
		uint64_t h = 0xCBF29CE484222325ull;
		auto mix = [&h](const unsigned char* p, UINT n)
		{
			for (UINT i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 0x100000001B3ull;
			}
		};
		mix(reinterpret_cast<const unsigned char*>(&num_verts), sizeof(num_verts));

		constexpr UINT kSamples = 64;
		const UINT step = num_verts > kSamples ? num_verts / kSamples : 1;
		for (UINT v = 0; v < num_verts; v += step)
			mix(&src[v * stride], stride);
		mix(&src[(num_verts - 1) * stride], stride);
		return h;
	}

	IDirect3DVertexBuffer9* skinning::get_expanded_vb(IDirect3DDevice9* dev,
		IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride, skin_layout layout)
	{
		if (!src_vb || stride == 0 || num_verts == 0) return nullptr;
		const UINT vtx_size = layout == skin_layout::base ? SKIN_VTX_SIZE : SKIN_OVERLAY_VTX_SIZE;

		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (frame - last_prune_frame_ >= SKIN_CACHE_MAX_AGE)
			prune_cache(frame);

		auto& entry = skin_cache_[skin_key{ src_vb, base_vtx, num_verts, stride, layout }];
		transition_event_ = !entry.vb ? 1 : frame - entry.last_frame >= 30 ? 2 : 0;
		entry.last_frame = frame;

		// Lock source VB
		unsigned char* src_data = nullptr;
		UINT read_off = static_cast<UINT>(base_vtx) * stride;
		if (FAILED(src_vb->Lock(read_off, num_verts * stride, reinterpret_cast<void**>(&src_data), D3DLOCK_READONLY)))
			return nullptr;

		const uint64_t fp = fingerprint(src_data, num_verts, stride);

		// Cache hit: still the same vertices
		if (entry.vb && entry.fp == fp)
		{
			src_vb->Unlock();
			cur_max_idx_ = entry.max_idx;
			cur_raw_max_idx_ = entry.raw_max_idx;
			return entry.vb;
		}

		// Other vertices in the range re-expand into the entry's buffer, which the key sizes exactly:
		// a new buffer would cost Remix the draw's instance pairing. A new range creates one.
		IDirect3DVertexBuffer9* exp_vb = entry.vb;
		if (entry.vb)
			transition_event_ = 3;
		if (!exp_vb && FAILED(dev->CreateVertexBuffer(num_verts * vtx_size, 0,
			0, D3DPOOL_MANAGED, &exp_vb, nullptr)))
		{
			src_vb->Unlock();
			return nullptr;
		}
		entry.vb = exp_vb;

		// Lock and expand
		unsigned char* dst_data = nullptr;
		if (FAILED(exp_vb->Lock(0, num_verts * vtx_size, reinterpret_cast<void**>(&dst_data), 0)))
		{
			src_vb->Unlock();
			exp_vb->Release();
			entry.vb = nullptr;
			return nullptr;
		}

		cur_max_idx_ = 0;
		cur_raw_max_idx_ = 0;
		if (layout == skin_layout::fur_layer_1 || layout == skin_layout::fur_layer_2 || layout == skin_layout::fur_layer_3)
			measure_fur_shells(src_data, num_verts, stride);
		for (UINT v = 0; v < num_verts; v++)
			expand_skin_vertex(&dst_data[v * vtx_size], &src_data[v * stride], layout);

		exp_vb->Unlock();
		src_vb->Unlock();

		entry.fp = fp;
		entry.max_idx = cur_max_idx_;
		entry.raw_max_idx = cur_raw_max_idx_;

		return exp_vb;
	}

	bool skinning::is_shell_fur(IDirect3DDevice9* dev, const std::string& diffuse_name, IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride)
	{
		auto& ffp = shared::common::ffp_state::get();
		if (!src_vb || !stride || num_verts < 12 || !ffp.cur_decl_has_color() || !ffp.cur_decl_has_texcoord())
			return false;
		// The uv is compared as its four bytes: two shorts.
		if (const int tc_type = ffp.cur_decl_texcoord_type(); tc_type != D3DDECLTYPE_SHORT2 && tc_type != D3DDECLTYPE_SHORT4)
			return false;
		// A character's own texture (cNNNN_...), never hair: hair cards repeat uvs the same way and have their own path.
		if (diffuse_name.size() < 6 || diffuse_name[0] != 'c' || diffuse_name[5] != '_' || diffuse_name.find("hair") != std::string::npos)
			return false;
		for (int i = 1; i <= 4; i++)
			if (diffuse_name[i] < '0' || diffuse_name[i] > '9')
				return false;
		// The game's straight-alpha blend with the alpha test, at full draw alpha (a fading character is not fur). From the device: the
		// game applies blend state through state blocks.
		DWORD blend = FALSE, test = FALSE, dest = D3DBLEND_ZERO;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
		dev->GetRenderState(D3DRS_DESTBLEND, &dest);
		if (!blend || !test || dest != D3DBLEND_INVSRCALPHA || ffp.ps_const_data()[139 * 4 + 3] < 0.999f)
			return false;
		if (shell_fur_names_.contains(diffuse_name))
			return true;
		const shell_range range{ src_vb, base_vtx, num_verts };
		if (shell_measured_.contains(range))
			return false;
		static const float min_share = std::clamp(shared::common::config::get().get_float("Fur", "ShellShare", 0.4f), 0.05f, 1.0f);
		static const int min_layers = std::clamp(shared::common::config::get().get_int("Fur", "ShellLayers", 4), 3, 32);
		unsigned char* src = nullptr;
		if (FAILED(src_vb->Lock(static_cast<UINT>(base_vtx) * stride, num_verts * stride, reinterpret_cast<void**>(&src), D3DLOCK_READONLY)))
			return false;
		float share = 0.0f;
		int layers = 0;
		const bool shell = shell_geometry(src, num_verts, stride, ffp.cur_decl_pos_off(), ffp.cur_decl_texcoord_off(), min_share, min_layers, share, layers);
		src_vb->Unlock();
		// Once per range (the ranges are remembered): few lines, and they name a creature whose fur is not listed yet.
		shared::common::log("FFP", std::format("shell geometry of {}: {} vertices, {:.0f}% in shells of {} layers: {}", diffuse_name, num_verts,
			share * 100.0f, layers, !shell ? "not fur" : s_shell_fur ? "taken as fur" : "shell fur, not listed (only reported: [Fur] ShellGeometry is off)"));
		if (shell && s_shell_fur)
		{
			shell_fur_names_.insert(diffuse_name);
			return true;
		}
		// Buffers come and go: an address met again with other vertices is measured anew once the set is cleared.
		if (shell_measured_.size() > 4096)
			shell_measured_.clear();
		shell_measured_.insert(range);
		return false;
	}

	// Whether at least min_share of the vertices sit in groups of one uv at three or more places 2 mm to 0.5 m apart, the groups' median
	// size being min_layers or more.
	bool skinning::shell_geometry(const unsigned char* src, UINT num_verts, UINT stride, int pos_off, int uv_off, float min_share, int min_layers,
		float& share, int& median_layers)
	{
		struct vert { uint32_t uv; float p[3]; };
		std::vector<vert> verts(num_verts);
		for (UINT v = 0; v < num_verts; v++)
		{
			const unsigned char* at = src + static_cast<size_t>(v) * stride;
			std::memcpy(&verts[v].uv, at + uv_off, sizeof(uint32_t));
			std::memcpy(verts[v].p, at + pos_off, sizeof(float) * 3);
		}
		std::sort(verts.begin(), verts.end(), [](const vert& a, const vert& b)
		{
			return a.uv != b.uv ? a.uv < b.uv : std::lexicographical_compare(a.p, a.p + 3, b.p, b.p + 3);
		});
		UINT in_shell = 0;
		std::vector<int> layers;
		for (size_t a = 0; a < verts.size();)
		{
			size_t b = a + 1;
			while (b < verts.size() && verts[b].uv == verts[a].uv)
				b++;
			if (b - a >= 3)
			{
				int places = 1;
				float lo[3] = { verts[a].p[0], verts[a].p[1], verts[a].p[2] }, hi[3] = { lo[0], lo[1], lo[2] };
				for (size_t i = a + 1; i < b; i++)
				{
					// Sorted by position within the group: a new place differs from the one before by a tenth of a millimetre or more.
					const float* p = verts[i].p; const float* q = verts[i - 1].p;
					if (std::abs(p[0] - q[0]) > 1e-4f || std::abs(p[1] - q[1]) > 1e-4f || std::abs(p[2] - q[2]) > 1e-4f)
						places++;
					for (int k = 0; k < 3; k++)
					{
						lo[k] = std::min(lo[k], p[k]);
						hi[k] = std::max(hi[k], p[k]);
					}
				}
				const float spread = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
				if (places >= 3 && spread > 0.002f && spread < 0.5f)
				{
					in_shell += static_cast<UINT>(b - a);
					layers.push_back(places);
				}
			}
			a = b;
		}
		share = static_cast<float>(in_shell) / static_cast<float>(num_verts);
		median_layers = 0;
		if (layers.empty())
			return false;
		std::nth_element(layers.begin(), layers.begin() + layers.size() / 2, layers.end());
		median_layers = layers[layers.size() / 2];
		return share >= min_share && median_layers >= min_layers;
	}

	// Shell fur: the same uv at several distances along the normal. The median gap between neighbours is the mesh's layer spacing, the median alpha lost per gap its fade.
	void skinning::measure_fur_shells(const unsigned char* src, UINT num_verts, UINT stride)
	{
		auto& ffp = shared::common::ffp_state::get();
		fur_step_ = 0.015f;
		fur_alpha_step_ = 0.0f;
		if (!ffp.cur_decl_has_texcoord() || !ffp.cur_decl_has_normal())
			return;
		struct entry { float along, alpha; };
		std::unordered_map<uint32_t, std::vector<entry>> groups;
		const int tc_off = ffp.cur_decl_texcoord_off();
		for (UINT v = 0; v < num_verts; v++)
		{
			const unsigned char* at = src + static_cast<size_t>(v) * stride;
			float n[3] = {};
			decode_normal(&at[ffp.cur_decl_normal_off()], ffp.cur_decl_normal_type(), n);
			const auto* pos = reinterpret_cast<const float*>(&at[ffp.cur_decl_pos_off()]);
			const auto* uv = reinterpret_cast<const uint16_t*>(&at[tc_off]);
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (len < 1e-3f)
				continue;
			groups[static_cast<uint32_t>(uv[0]) | static_cast<uint32_t>(uv[1]) << 16].push_back({ (pos[0] * n[0] + pos[1] * n[1] + pos[2] * n[2]) / len,
				static_cast<float>(ffp.cur_vertex_color(at) >> 24) });
		}
		std::vector<float> gaps, fades;
		for (auto& [uv, list] : groups)
		{
			if (list.size() < 3 || list.size() > 32)
				continue;
			std::sort(list.begin(), list.end(), [](const entry& a, const entry& b) { return a.along < b.along; });
			for (size_t i = 1; i < list.size(); i++)
			{
				const float gap = list[i].along - list[i - 1].along;
				if (gap > 0.001f && gap < 0.1f)
				{
					gaps.push_back(gap);
					fades.push_back(list[i - 1].alpha - list[i].alpha);
				}
			}
		}
		if (gaps.size() < 8)
			return;
		std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
		std::nth_element(fades.begin(), fades.begin() + fades.size() / 2, fades.end());
		fur_step_ = gaps[gaps.size() / 2];
		fur_alpha_step_ = std::max(fades[fades.size() / 2], 0.0f);
	}

	void skinning::expand_skin_vertex(unsigned char* dst, const unsigned char* src, skin_layout layout)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* out = reinterpret_cast<float*>(dst);

		// Position (FLOAT3, always at pos_off)
		auto* pos = reinterpret_cast<const float*>(&src[ffp.cur_decl_pos_off()]);
		out[0] = pos[0]; out[1] = pos[1]; out[2] = pos[2];

		// Blend weights (FLOAT3 at offset 12). DS1's skinned vertex shaders
		// (FRPG_*_PIWN*) read all four SHORT4N weights and divide by their sum
		// before blending; FFP 3WEIGHTS derives the fourth as 1 - (w0 + w1 + w2),
		// so the stored three must already be normalised for the two to agree.
		auto bw_off = ffp.cur_decl_blend_weight_off();
		auto bw_type = ffp.cur_decl_blend_weight_type();

		float w[4] = {};
		int w_count = 0;
		switch (bw_type)
		{
		case D3DDECLTYPE_FLOAT1: w_count = 1; break;
		case D3DDECLTYPE_FLOAT2: w_count = 2; break;
		case D3DDECLTYPE_FLOAT3: w_count = 3; break;
		case D3DDECLTYPE_FLOAT4: w_count = 4; break;
		default: break;
		}
		if (w_count > 0)
		{
			auto* f = reinterpret_cast<const float*>(&src[bw_off]);
			for (int i = 0; i < w_count; i++) w[i] = f[i];
		}
		else if (bw_type == D3DDECLTYPE_UBYTE4N)
		{
			for (int i = 0; i < 4; i++) w[i] = src[bw_off + i] / 255.0f;
		}
		else if (bw_type == D3DDECLTYPE_SHORT4N || bw_type == D3DDECLTYPE_SHORT2N)
		{
			auto* bw = reinterpret_cast<const short*>(&src[bw_off]);
			const int n = bw_type == D3DDECLTYPE_SHORT4N ? 4 : 2;
			for (int i = 0; i < n; i++) w[i] = std::max(-1.0f, bw[i] / 32767.0f);
		}

		const float w_sum = w[0] + w[1] + w[2] + w[3];
		if (std::fabs(w_sum) > 1e-6f)
		{
			for (int i = 0; i < 4; i++) w[i] /= w_sum;
		}
		else
		{
			// No usable weights: bind rigidly to the first index.
			w[0] = 1.0f; w[1] = w[2] = w[3] = 0.0f;
		}
		out[3] = w[0];
		out[4] = w[1];
		out[5] = w[2];

		// Blend indices (UBYTE4 at offset 24)
		const unsigned char* bi = &src[ffp.cur_decl_blend_indices_off()];
		std::memcpy(&dst[24], bi, 4);
		for (int i = 0; i < 4; i++)
		{
			// Indices whose weight is zero never reach the output.
			if (w[i] != 0.0f && bi[i] > cur_max_idx_)
				cur_max_idx_ = bi[i];
		}
		// Remix sizes and hashes its palette by the raw indices of every bone the vertex blend
		// mode reads (upload_bones: 2, 3 or 4), zero weights included.
		const int nw = ffp.cur_decl_num_weights();
		const int raw_count = nw <= 1 ? 2 : (nw == 2 ? 3 : 4);
		for (int i = 0; i < raw_count; i++)
		{
			if (bi[i] > cur_raw_max_idx_)
				cur_raw_max_idx_ = bi[i];
		}

		// Normal (FLOAT3 at offset 28)
		if (ffp.cur_decl_has_normal())
		{
			decode_normal(&src[ffp.cur_decl_normal_off()], ffp.cur_decl_normal_type(), &out[7]);
		}
		else
		{
			out[7] = 0.0f; out[8] = 1.0f; out[9] = 0.0f;
		}

		// Texcoord (FLOAT2 at offset 40). The overlay takes the second pair of a four-component
		// texcoord (TEXCOORD0.zw, the second layer's uv): only SHORT4/FLOAT4 draws have an overlay.
		const int first = layout == skin_layout::overlay ? 2 : 0;
		if (ffp.cur_decl_has_texcoord())
		{
			int tc_type = ffp.cur_decl_texcoord_type();
			int tc_off = ffp.cur_decl_texcoord_off();
			if (tc_type == D3DDECLTYPE_FLOAT2 || tc_type == D3DDECLTYPE_FLOAT3 || tc_type == D3DDECLTYPE_FLOAT4)
			{
				auto* tc = reinterpret_cast<const float*>(&src[tc_off]);
				out[10] = tc[first]; out[11] = tc[first + 1];
			}
			else if (tc_type == D3DDECLTYPE_FLOAT16_2)
			{
				auto* h = reinterpret_cast<const unsigned short*>(&src[tc_off]);
					out[10] = half_to_float(h[0]);
				out[11] = half_to_float(h[1]);
			}
			else if (tc_type == D3DDECLTYPE_SHORT2 || tc_type == D3DDECLTYPE_SHORT4)
			{
				// Dark Souls stores UVs as SHORT * 1024; convert to normalized FLOAT
				constexpr float kScale = 1.0f / 1024.0f;
				auto* tc = reinterpret_cast<const short*>(&src[tc_off]);
				out[10] = static_cast<float>(tc[first]) * kScale;
				out[11] = static_cast<float>(tc[first + 1]) * kScale;
			}
			else
			{
				out[10] = out[11] = 0.0f;
			}
		}
		else
		{
			out[10] = out[11] = 0.0f;
		}

		// Vertex colour (D3DCOLOR at offset 48). The overlay takes its alpha, the second layer's weight, its rgb
		// staying white as on the base layer, which carries no vertex colour; a fur shell takes all of it.
		if (layout == skin_layout::overlay)
		{
			const DWORD argb = (ffp.cur_vertex_color(src) & 0xFF000000u) | 0x00FFFFFFu;
			std::memcpy(&dst[48], &argb, sizeof(argb));
		}
		else if (layout == skin_layout::vertex_colour || layout == skin_layout::fur_layer_1 || layout == skin_layout::fur_layer_2 || layout == skin_layout::fur_layer_3)
		{
			const DWORD argb = ffp.cur_vertex_color(src);
			std::memcpy(&dst[48], &argb, sizeof(argb));
			if (layout != skin_layout::vertex_colour)
			{
				// Between this layer and the next: part of the way out along the normal, with part of the alpha it would lose.
				const float f = static_cast<float>(static_cast<int>(layout) - static_cast<int>(skin_layout::fur_layer_1) + 1) / static_cast<float>(fur_layer_count_ + 1);
				for (int k = 0; k < 3; k++)
					out[k] += out[7 + k] * fur_step_ * f;
				DWORD faded = 0;
				std::memcpy(&faded, &dst[48], sizeof(faded));
				const float alpha = std::max(static_cast<float>(faded >> 24) - fur_alpha_step_ * f, 0.0f);
				faded = (faded & 0x00FFFFFFu) | (static_cast<DWORD>(alpha + 0.5f) << 24);
				std::memcpy(&dst[48], &faded, sizeof(faded));
			}
		}
		else if (layout == skin_layout::body)
		{
			// The bind position, in the wet player's grid (fork_ds1_ripples.h in the runtime). Texture stage 0 leaves
			// the colour out on these draws (apply_texture_only_stage), so the draw looks the same.
			const auto encode = [](float v) { return static_cast<DWORD>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			const DWORD argb = 0xFF000000u | (encode((pos[0] + 1.0f) / 2.0f) << 16) | (encode((pos[1] + 0.2f) / 2.4f) << 8) | encode((pos[2] + 0.6f) / 1.2f);
			std::memcpy(&dst[48], &argb, sizeof(argb));
		}
	}

	HRESULT skinning::draw_overlay(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, IDirect3DVertexBuffer9* src_vb,
		INT base_vtx, UINT min_vtx, UINT num_verts, UINT start_idx, UINT prim_count, UINT stride)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* texture = ffp.overlay_texture();
		auto* overlay_vb = skin_overlay_decl_ ? get_expanded_vb(dev, src_vb, base_vtx, min_vtx + num_verts, stride, skin_layout::overlay) : nullptr;
		if (!texture || !overlay_vb)
			return S_OK;

		// The render states the overlay pass changes, restored after it.
		static constexpr D3DRENDERSTATETYPE kStates[] = {
			D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
			D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
		};
		DWORD saved[std::size(kStates)] = {};
		for (size_t i = 0; i < std::size(kStates); i++)
			dev->GetRenderState(kStates[i], &saved[i]);

		dev->SetVertexDeclaration(skin_overlay_decl_);
		dev->SetStreamSource(0, overlay_vb, 0, SKIN_OVERLAY_VTX_SIZE);
		ffp.apply_overlay_decal_states(dev, texture);
		const bool toned = ffp.apply_skin_tone(dev, true);
		const bool scrolled = game::texture_scroll::apply(dev, true, false, src_vb, ffp.cur_ib(),
			base_vtx, start_idx, prim_count, ffp.frame_count());
		const HRESULT hr = dev->DrawIndexedPrimitive(pt, 0, min_vtx, num_verts, start_idx, prim_count);
		if (scrolled)
			game::texture_scroll::clear(dev);
		if (toned)
			ffp.clear_skin_tone(dev);
		ffp.clear_decal_marker(dev);

		for (size_t i = 0; i < std::size(kStates); i++)
			dev->SetRenderState(kStates[i], saved[i]);
		return hr;
	}

	namespace
	{
		// Character pieces are seen for a change of model through the number of player body draws per frame. A change opens a window of 300 frames:
		// the frame it happened on (held back until it is known) and every piece that appears or changes in it, then one list of all pieces at its end.
		constexpr UINT kWatchFrames = 300;
		int g_watch_budget = 1200;
		UINT g_watch_frame = 0;
		UINT g_watch_until = 0;
		UINT g_watch_map_frame = 0;
		unsigned int g_watch_map = 0;
		int g_body_draws = 0;
		int g_body_draws_last = -1;
		int g_window_lines = 0;
		int g_list_lines = 0;
		std::vector<std::string> g_watch_held;

		std::string clock_text()
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			return std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		}

		void watch_write(const std::string& line)
		{
			if (g_watch_budget-- > 0)
				shared::common::log("Transition", line);
		}
	}

	void skinning::log_transition(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* src_vb, INT base_vtx, UINT num_verts, UINT stride, UINT start_idx, UINT prim_count)
	{
		const int event = transition_event_;
		transition_event_ = 0;
		if (g_watch_budget <= 0)
			return;
		auto& ffp = shared::common::ffp_state::get();
		const UINT frame = ffp.frame_count();
		const unsigned int map = game::map_lights::current_map_id();
		if (map != g_watch_map)
		{
			g_watch_map = map;
			g_watch_map_frame = frame;
		}
		if (frame != g_watch_frame)
		{
			// Not during a map's first seconds, when every character is new.
			if (g_body_draws_last >= 0 && g_body_draws != g_body_draws_last && frame - g_watch_map_frame > 240)
			{
				watch_write(std::format("{} frame {}: player body draws {} -> {}", clock_text(), g_watch_frame, g_body_draws_last, g_body_draws));
				g_window_lines = 0;
				g_list_lines = 0;
				g_watch_until = frame + kWatchFrames;
				for (const auto& line : g_watch_held)
					watch_write(line);
			}
			g_watch_held.clear();
			g_body_draws_last = g_body_draws;
			g_body_draws = 0;
			g_watch_frame = frame;
		}
		if (game::texture_names::is_body_part(ffp.cur_texture(0)))
			g_body_draws++;

		const bool in_window = frame < g_watch_until;
		const bool last_frame = in_window && frame + 1 == g_watch_until;
		if (!event && !last_frame)
			return;

		// The bones this piece reads, on this frame: what its acceleration structure is built from when it is new.
		const int used = std::max(cur_max_idx_, cur_raw_max_idx_) + 1;
		const int start = ffp.bone_start_reg(), regs = ffp.regs_per_bone();
		const float* c = ffp.vs_const_data();
		int bad = 0, zero = 0;
		float largest_scale = 0.0f, smallest_scale = 1e30f;
		if (c && regs > 0)
			for (int b = 0; b < used && start + b * regs + 3 <= 256; b++)
			{
				const float* m = &c[(start + b * regs) * 4];
				bool nan = false;
				for (int i = 0; i < 12; i++)
					nan = nan || !std::isfinite(m[i]);
				if (nan)
				{
					bad++;
					continue;
				}
				const float len = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
				if (len < 1e-4f)
					zero++;
				largest_scale = std::max(largest_scale, len);
				smallest_scale = std::min(smallest_scale, len);
			}
		DWORD test = 0, blend = 0;
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		static const char* const kEvents[] = { "drawn", "first seen", "back after a gap", "other vertices" };
		const std::string name(game::texture_names::name_of(ffp.cur_texture(0)));
		std::string line = std::format("{} frame {}: {} {} triangles {} vertices {} vb {:p}+{} stride {} ib {:p} start {} bones {} nan {} zero {} scale {:.3f}-{:.3f} atest {} blend {}",
			clock_text(), frame, kEvents[event], name.empty() ? "?" : name, prim_count, num_verts, static_cast<void*>(src_vb), base_vtx, stride,
			static_cast<void*>(ffp.cur_ib()), start_idx, used, bad, zero, smallest_scale > 1e29f ? 0.0f : smallest_scale, largest_scale, test, blend);
		if (bad || zero)
			line += " BAD BONES";
		if (in_window)
		{
			if (last_frame ? g_list_lines++ < 60 : g_window_lines++ < 80)
				watch_write(line);
		}
		else if (g_watch_held.size() < 60)
			g_watch_held.push_back(std::move(line));
	}

	void skinning::log_glow_draw(IDirect3DDevice9* dev, UINT base_vtx, UINT start_idx, UINT prim_count)
	{
		static const void* last_vb = nullptr;
		static const void* last_ib = nullptr;
		static UINT last_base = ~0u, last_start = ~0u;
		static int last_reused = -1;
		static int budget = 200;
		auto& ffp = shared::common::ffp_state::get();
		const void* vb = ffp.stream_vb(0);
		const void* ib = ffp.cur_ib();
		const int reused = ffp.last_rigid_expand_reused() ? 1 : 0;
		if (budget <= 0 || (vb == last_vb && ib == last_ib && base_vtx == last_base && start_idx == last_start && reused == last_reused))
			return;
		last_vb = vb, last_ib = ib, last_base = base_vtx, last_start = start_idx, last_reused = reused;
		budget--;
		DWORD test = 0, blend = 0;
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		shared::common::log("Transition", std::format("{} frame {}: lantern glow piece {} triangles {} vb {:p}+{} ib {:p} start {} vertices reused {} c100 {:.2f} atest {} blend {}",
			clock_text(), ffp.frame_count(), std::string(game::texture_names::name_of(ffp.cur_texture(0))), prim_count, vb, base_vtx, ib, start_idx, reused,
			ffp.ps_const_data() ? ffp.ps_const_data()[100 * 4] : 0.0f, test, blend));
	}

	void skinning::upload_bones(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();

		int bone_start = ffp.bone_start_reg();
		int num_bones = ffp.num_bones();
		if (num_bones <= 0) return;

		const float* vs_const = ffp.vs_const_data();
		int regs_per_bone = ffp.regs_per_bone();
		if (regs_per_bone != 3 && regs_per_bone != 4) return;

		// The vertex shader indexes the palette with the vertex's blend indices,
		// whatever the last constant write covered: upload every bone a vertex of
		// this draw can reach, from the same constants the GPU would read.
		const int palette_capacity = (256 - bone_start) / regs_per_bone;
		// num_bones is the palette's high-water mark over the whole session (every bone any draw's
		// constant writes ever reached, 83 in Blighttown); a mesh's vertices only index up to
		// cur_raw_max_idx_, the bones Remix reads and hashes (zero weights included).
		int max_bones = shared::common::config::get().ffp_steps.skin_upload_used_bones
			? std::max(cur_max_idx_, cur_raw_max_idx_) + 1 : std::max(num_bones, cur_max_idx_ + 1);
		max_bones = std::min({ max_bones, palette_capacity, 256 });
		if (shared::common::ffp_prof::profiling())
			s_stats.bones += static_cast<unsigned int>(std::max(max_bones, 0));

		// The main pass draws camera-relative; apply_transforms moves World into
		// absolute space by adding the camera position, and every bone needs the
		// same shift or the character renders offset by the camera position.
		float off_x = 0.0f, off_y = 0.0f, off_z = 0.0f;
		const bool offset = ffp.world_offset(off_x, off_y, off_z);

		for (int i = 0; i < max_bones; i++)
		{
			// Each bone is three rows of a row-major 3x4 matrix, out = M * [p, 1];
			// D3D wants the row-vector 4x4 form, i.e. its transpose.
			const float* src = &vs_const[(bone_start + i * regs_per_bone) * 4];
			float bone44[16];

			bone44[0]  = src[0]; bone44[1]  = src[4]; bone44[2]  = src[8];  bone44[3]  = 0.0f;
			bone44[4]  = src[1]; bone44[5]  = src[5]; bone44[6]  = src[9];  bone44[7]  = 0.0f;
			bone44[8]  = src[2]; bone44[9]  = src[6]; bone44[10] = src[10]; bone44[11] = 0.0f;
			bone44[12] = src[3]; bone44[13] = src[7]; bone44[14] = src[11]; bone44[15] = 1.0f;
			if (regs_per_bone == 4)
			{
				bone44[3] = src[12]; bone44[7] = src[13]; bone44[11] = src[14]; bone44[15] = src[15];
			}

			ffp.turn_menu_cell(bone44);
			if (offset)
			{
				bone44[12] += off_x;
				bone44[13] += off_y;
				bone44[14] += off_z;
			}

			dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLDMATRIX(i)),
				reinterpret_cast<const D3DMATRIX*>(bone44));
		}

		// Enable indexed vertex blending
		D3DVERTEXBLENDFLAGS blend_flag;
		int nw = ffp.cur_decl_num_weights();
		if (nw <= 1)      blend_flag = D3DVBF_1WEIGHTS;
		else if (nw == 2) blend_flag = D3DVBF_2WEIGHTS;
		else              blend_flag = D3DVBF_3WEIGHTS;

		dev->SetRenderState(D3DRS_VERTEXBLEND, blend_flag);
		dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
	}

	void skinning::disable_skinning(IDirect3DDevice9* dev)
	{
		dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
		dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	}

	float skinning::half_to_float(unsigned short h)
	{
		unsigned int sign = (h >> 15) & 0x1;
		unsigned int exp = (h >> 10) & 0x1F;
		unsigned int mant = h & 0x3FF;

		if (exp == 0)
		{
			if (mant == 0) {
				unsigned int f = sign << 31;
				float result;
				std::memcpy(&result, &f, sizeof(float));
				return result;
			}
			// Denormalized
			while (!(mant & 0x400)) { mant <<= 1; exp--; }
			exp++; mant &= ~0x400u;
		}
		else if (exp == 31)
		{
			unsigned int f = (sign << 31) | 0x7F800000u | (mant << 13);
			float result;
			std::memcpy(&result, &f, sizeof(float));
			return result;
		}

		exp += (127 - 15);
		unsigned int f = (sign << 31) | (exp << 23) | (mant << 13);
		float result;
		std::memcpy(&result, &f, sizeof(float));
		return result;
	}

	void skinning::decode_normal(const unsigned char* src, int type, float* out)
	{
		switch (type)
		{
		case D3DDECLTYPE_FLOAT3:
		{
			auto* fp = reinterpret_cast<const float*>(src);
			out[0] = fp[0]; out[1] = fp[1]; out[2] = fp[2];
			break;
		}
		case D3DDECLTYPE_FLOAT16_2:
		{
			auto* h = reinterpret_cast<const unsigned short*>(src);
			out[0] = half_to_float(h[0]);
			out[1] = half_to_float(h[1]);
			out[2] = 0.0f; // reconstruct Z if needed
			break;
		}
		case D3DDECLTYPE_DEC3N:
		{
			unsigned int packed = *reinterpret_cast<const unsigned int*>(src);
			int x = static_cast<int>((packed >>  0) & 0x3FF); if (x & 0x200) x |= ~0x3FF;
			int y = static_cast<int>((packed >> 10) & 0x3FF); if (y & 0x200) y |= ~0x3FF;
			int z = static_cast<int>((packed >> 20) & 0x3FF); if (z & 0x200) z |= ~0x3FF;
			out[0] = x / 511.0f;
			out[1] = y / 511.0f;
			out[2] = z / 511.0f;
			break;
		}
		case D3DDECLTYPE_UBYTE4N:
		{
			// FLVER normals are (b - 127) / 127. The skinned vertex shaders decode
			// the UBYTE4N input with (v - 0.49804) * 2.00787, which is exactly that.
			out[0] = (src[0] - 127.0f) / 127.0f;
			out[1] = (src[1] - 127.0f) / 127.0f;
			out[2] = (src[2] - 127.0f) / 127.0f;
			break;
		}
		default:
			out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
			break;
		}
	}
}
