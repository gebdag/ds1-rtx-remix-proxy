#include "std_include.hpp"
#include "env_reflection.hpp"
#include "map_lights.hpp"

#include <unordered_map>

namespace comp::game::env_reflection
{
	namespace
	{
		constexpr DWORD kMarker = 0xD7000000u;
		// The game's EnvDif probe and EnvSpc cube samplers.
		constexpr DWORD kProbeSampler = 11;
		constexpr DWORD kEnvSampler = 12;
		// The z_pbr mod's specular calibration (ptde_specular.py SPECULAR_CALIBRATION).
		constexpr float kModCalibration = 0.5f;

		constexpr DWORD kNoTag = 0xFFFFFFFFu;
		DWORD g_tag_set = 0;

		// Mean luminance of a cube's faces as the game's shaders decode them, rgb / a for A8R8G8B8, by texture.
		std::unordered_map<IDirect3DBaseTexture9*, float> g_cube_means;

		float luminance(float r, float g, float b)
		{
			return 0.2126f * r + 0.7152f * g + 0.0722f * b;
		}

		// Mean decoded luminance of one locked face, or a negative value for a format the shaders do not
		// read as rgb / a.
		float face_mean(const D3DSURFACE_DESC& desc, const D3DLOCKED_RECT& lr)
		{
			if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
				return -1.0f;
			const bool divide = desc.Format == D3DFMT_A8R8G8B8;
			double sum = 0.0;
			const auto* p = static_cast<const unsigned char*>(lr.pBits);
			for (UINT y = 0; y < desc.Height; y++)
			{
				for (UINT x = 0; x < desc.Width; x++)
				{
					const unsigned char* t = p + static_cast<size_t>(y) * lr.Pitch + x * 4;
					const float a = divide ? t[3] / 255.0f : 1.0f;
					if (a > 0.0f)
						sum += luminance(t[2] / 255.0f, t[1] / 255.0f, t[0] / 255.0f) / a;
				}
			}
			return static_cast<float>(sum / (static_cast<double>(desc.Width) * desc.Height));
		}

		// Mean decoded luminance of a cube texture, or a negative value when it cannot be read.
		float cube_mean(IDirect3DBaseTexture9* texture)
		{
			// A map's cubes are freed with it, and the next map's can take the same addresses.
			static unsigned cached_map = 0;
			if (map_lights::current_map_id() != cached_map)
			{
				g_cube_means.clear();
				cached_map = map_lights::current_map_id();
			}
			if (const auto it = g_cube_means.find(texture); it != g_cube_means.end())
				return it->second;
			float mean = -1.0f;
			if (texture && texture->GetType() == D3DRTYPE_CUBETEXTURE)
			{
				auto* cube = static_cast<IDirect3DCubeTexture9*>(texture);
				D3DSURFACE_DESC desc {};
				if (SUCCEEDED(cube->GetLevelDesc(0, &desc)))
				{
					double sum = 0.0;
					int faces = 0;
					for (int f = 0; f < 6; f++)
					{
						const auto face = static_cast<D3DCUBEMAP_FACES>(f);
						D3DLOCKED_RECT lr {};
						if (FAILED(cube->LockRect(face, 0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
							break;
						const float m = face_mean(desc, lr);
						cube->UnlockRect(face, 0);
						if (m < 0.0f)
							break;
						sum += m;
						++faces;
					}
					if (faces == 6)
						mean = static_cast<float>(sum / 6.0);
				}
			}
			g_cube_means[texture] = mean;
			return mean;
		}

		float mean3(const float* c)
		{
			return (c[0] + c[1] + c[2]) / 3.0f;
		}

		// The draw's weight, or a negative value when it has none.
		float weight(IDirect3DDevice9* dev, const float* ps)
		{
			IDirect3DBaseTexture9* probe = nullptr;
			IDirect3DBaseTexture9* env = nullptr;
			dev->GetTexture(kProbeSampler, &probe);
			dev->GetTexture(kEnvSampler, &env);
			const float probe_mean = cube_mean(probe);
			const float env_mean = cube_mean(env);
			if (probe) probe->Release();
			if (env) env->Release();

			const float c86 = mean3(&ps[86 * 4]);
			const float c87 = mean3(&ps[87 * 4]);
			const float c100 = mean3(&ps[100 * 4]);
			const float c101 = mean3(&ps[101 * 4]);
			if (probe_mean <= 0.0f || env_mean < 0.0f || c86 <= 0.0f || c100 <= 0.0f)
				return -1.0f;
			return kModCalibration * (c101 / c100) * (c87 * env_mean) / (c86 * probe_mean);
		}
	}

	void tag(IDirect3DDevice9* dev, bool applies, const float* ps_const)
	{
		DWORD tag = 0;
		if (applies && dev && ps_const)
		{
			const float w = weight(dev, ps_const);
			if (w >= 0.0f)
			{
				D3DXFLOAT16 half;
				D3DXFloat32To16Array(&half, &w, 1);
				WORD bits;
				std::memcpy(&bits, &half, sizeof(bits));
				tag = kMarker | bits;
			}
		}
		if (tag != g_tag_set && dev)
		{
			dev->SetTextureStageState(1, D3DTSS_CONSTANT, tag);
			g_tag_set = tag;
		}
	}

	void forget()
	{
		g_tag_set = kNoTag;
	}
}
