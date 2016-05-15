/*
    yadifmod2 : yadif + yadifmod for avysynth2.6/Avisynth+
        Copyright (C) 2016 OKA Motofumi

    yadifmod : Modification of Fizick's yadif avisynth filter.
        Copyright (C) 2007 Kevin Stone aka tritical

    Yadif C-plugin for Avisynth 2.5 : Port from MPlayer filter
        Copyright (C) 2007 Alexander G. Balakhnin aka Fizick

    YADIF : Yet Another DeInterlacing Filter
        Copyright (C) 2006 Michael Niedermayer <michaelni@gmx.at>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#include <cstdint>
#include <algorithm>
#include "yadifmod2.h"


YadifMod2::YadifMod2(PClip c, PClip e, int o, int f, int m, arch_t arch) :
    GenericVideoFilter(c), edeint(e), order(o), field(f), mode(m)
{
    numPlanes = vi.IsY8() ? 1 : 3;

    nfSrc = vi.num_frames;

    if (mode == 1 || mode == 3) {
        vi.num_frames *= 2;
        vi.SetFPS(vi.fps_numerator * 2, vi.fps_denominator);
    }

    if (order == -1) {
        order = child->GetParity(0) ? 1 : 0;
    }
    if (field == -1) {
        field = order;
    }

    prev_first = nfSrc == 1 ? 0 : edeint ? 0 : 1; // forth original yadif and yadifmod

    mainProc = get_main_proc(mode < 2, edeint != nullptr, arch);
}


static inline void
interp(uint8_t* dstp, const uint8_t* srcp0, int pitch, int width)
{
    const uint8_t* srcp1 = srcp0 + pitch * 2;
    for (int x = 0; x < width; ++x) {
        dstp[x] = (srcp0[x] + srcp1[x] + 1) / 2;
    }
}


PVideoFrame __stdcall YadifMod2::GetFrame(int n, ise_t* env)
{
    const int planes[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };

    PVideoFrame edeint;
    if (this->edeint) {
        edeint = this->edeint->GetFrame(n, env);
    }

    int ft = field;
    if (mode == 1 || mode == 3) {
        ft = (n & 1) ? 1 - order : order;
        n /= 2;
    }

    auto curr = child->GetFrame(n, env);
    auto prev = child->GetFrame(n == 0 ? prev_first : n - 1, env);
    auto next = child->GetFrame(std::min(n + 1, nfSrc - 1), env);
    auto dst = env->NewVideoFrame(vi);

    for (int p = 0; p < numPlanes; ++p) {

        const int plane = planes[p];

        const uint8_t* currp = curr->GetReadPtr(plane);
        const uint8_t* prevp = prev->GetReadPtr(plane);
        const uint8_t* nextp = next->GetReadPtr(plane);

        const int width = curr->GetRowSize(plane);
        const int height = curr->GetHeight(plane);
        const int cpitch = curr->GetPitch(plane);
        const int ppitch = prev->GetPitch(plane);
        const int npitch = next->GetPitch(plane);

        const int begin = 2 + ft;
        const int count = (height - 4 + ft - begin) / 2 + 1;

        const uint8_t *fm_prev, *fm_next;
        int fm_ppitch, fm_npitch;
        if (ft != order) {
            fm_ppitch = cpitch * 2;
            fm_npitch = npitch * 2;
            fm_prev = currp + begin * cpitch;
            fm_next = nextp + begin * npitch;
        } else {
            fm_ppitch = ppitch * 2;
            fm_npitch = cpitch * 2;
            fm_prev = prevp + begin * ppitch;
            fm_next = currp + begin * cpitch;
        }

        const uint8_t* edeintp = nullptr;
        int epitch = 0;
        if (this->edeint) {
            edeintp = edeint->GetReadPtr(plane);
            epitch = edeint->GetPitch(plane);
        }

        uint8_t* dstp = dst->GetWritePtr(plane);
        const int dpitch = dst->GetPitch(plane);

        if (ft == 0) {
            memcpy(dstp, currp + cpitch, width);
            if (edeintp) {
                memcpy(dstp + dpitch * (height - 2),
                       edeintp + epitch * (height - 2), width);
            } else {
                interp(dstp + dpitch * (height - 2),
                       currp + cpitch * (height - 3), cpitch, width);
            }
        } else {
            if (edeintp) {
                memcpy(dstp + dpitch, edeintp + epitch, width);
            } else {
                interp(dstp + dpitch, currp, cpitch, width);
            }
            memcpy(dstp + dpitch * (height - 1),
                   currp + cpitch * (height - 2), width);
        }
        env->BitBlt(dstp + (1 - ft) * dpitch, 2 * dpitch,
                    currp + (1 - ft) * cpitch, 2 * cpitch, width, height / 2);

        mainProc(currp + begin * cpitch, prevp + begin * ppitch,
                 nextp + begin * npitch, fm_prev, fm_next,
                 edeintp + begin * epitch, dstp + begin * dpitch, width,
                 cpitch, ppitch, npitch, fm_ppitch, fm_npitch, 2 * epitch,
                 2 * dpitch, count);
    }

    return dst;
}


static arch_t get_arch(int opt)
{
    if (opt == 0 || !has_sse2()) {
        return NO_SIMD;
    }
    if (opt == 1 || !has_ssse3()) {
        return USE_SSE2;
    }
    if (opt == 2 || !has_avx2()) {
        return USE_SSSE3;
    }
    return USE_AVX2;
}

static void validate(bool cond, const char* msg, ise_t* env)
{
    if (!cond) {
        env->ThrowError("yadifmod2: %s", msg);
    }
}

static AVSValue __cdecl
create_yadifmod2(AVSValue args, void* user_data, ise_t* env)
{
    PClip child = args[0].AsClip();
    const VideoInfo& vi = child->GetVideoInfo();
    validate(vi.IsPlanar(), "input clip must be a planar format.", env);

    int order = args[1].AsInt(-1);
    validate(order >= -1 && order <= 1,
             "order must be set to -1, 0 or 1.", env);

    int field = args[2].AsInt(-1);
    validate(field >= -1 && field <= 1,
             "field must be set to -1, 0 or 1.", env);

    int mode = args[3].AsInt(0);
    validate(mode >= 0 && mode <= 3,
             "mode must be set to 0, 1, 2 or 3", env);

    PClip edeint = nullptr;
    if (args[4].Defined()) {
        edeint = args[4].AsClip();
        const VideoInfo& vi_ed = edeint->GetVideoInfo();
        validate(vi.IsSameColorspace(vi_ed),
                 "edeint clip's colorspace doesn't match.", env);
        validate(vi.width == vi_ed.width && vi.height == vi_ed.height,
                "input and edeint must be the same resolution.", env);
        validate(vi.num_frames * ((mode & 1) ? 2 : 1) == vi_ed.num_frames,
                 "edeint clip's number of frames doesn't match.", env);
    }

    int opt = args[5].AsInt(-1);
    validate(opt >= -1 && opt <= 3,
             "opt must be set to -1(auto), 0(C), 1(SSE2), 2(SSSE3) or 3(AVX2).", env);
    arch_t arch = get_arch(opt);

    return new YadifMod2(child, edeint, order, field, mode, arch);
}


static const AVS_Linkage* AVS_linkage = nullptr;


extern "C" __declspec(dllexport) const char* __stdcall
AvisynthPluginInit3(ise_t* env, const AVS_Linkage* vectors)
{
    AVS_linkage = vectors;
    env->AddFunction("yadifmod2",
                     "c"
                     "[order]i"
                     "[field]i"
                     "[mode]i"
                     "[edeint]c"
                     "[opt]i",
                     create_yadifmod2, nullptr);
    return "yadifmod2 = yadif + yadifmod ... ver. " YADIF_MOD_2_VERSION;
}