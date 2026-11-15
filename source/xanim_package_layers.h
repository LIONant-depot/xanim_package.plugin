#ifndef XANIM_PACKAGE_LAYERS_H
#define XANIM_PACKAGE_LAYERS_H
#pragma once

// The layered edit mode's composition of one clip, shared by the package compiler (the bake) and the Animation Package editor (the live
// preview): same function, same inputs, so the preview IS what the compile stores (before the clip's trim / downsample / root motion).
// Inputs are a clip's source frames in the skeleton's bone order and space at the source's own rate (the compiler's SkeletonFrames; the
// editor reads them from the compiler's layer-sources cache, below), the clip's Layers (descriptor) and the skeleton's bone manifest.
// Evaluation: xAnimation blend/xanimation_layers.h.
#include "dependencies/xAnimation/source/xanimation.h"
#include "dependencies/xAnimation/source/blend/xanimation_layers.h"
#include "xanim_package_descriptor.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xanim_package_layers
{
    // A source clip's frames: skeleton order and space, every source frame, frame-major [frame * bones + bone]
    struct source_frames
    {
        std::wstring                    m_Key;          // the import source's key (path, or package:HEX)
        std::string                     m_Clip;         // the clip's original name in that source
        int                             m_FPS     = 30;
        int                             m_nFrames = 0;
        std::vector<xmath::transform3>  m_Frames;
    };
    using find_source = std::function<const source_frames*(const std::wstring& Key, const std::string& Clip)>;

    struct layer_report { std::string m_Name, m_Source; bool m_bAdditive = false; int m_nMaskBones = 0; std::size_t m_Written = 0, m_WrittenOutside = 0; };
    struct report
    {
        std::vector<layer_report>   m_Layers;
        std::size_t                 m_ChangedOutside = 0;
        std::string                 m_FirstOutside;
        float                       m_MaxRootMove = 0.0f;
        std::vector<std::string>    m_Warnings;
    };

    inline xanimation::tick ToTick(double Seconds) noexcept { return static_cast<xanimation::tick>(std::llround(Seconds * double(xanimation::ticks_per_second_v))); }

    inline xmath::transform3 RestOf(const xskeleton_desc::bone_manifest::bone_entry& B) noexcept
    {
        return xmath::transform3{ .m_Scale = B.m_RestScale, .m_Rotation = xmath::fquat(B.m_RestRotX, B.m_RestRotY, B.m_RestRotZ, B.m_RestRotW), .m_Position = B.m_RestPosition };
    }

    inline bool SameTransform(const xmath::transform3& A, const xmath::transform3& B) noexcept     // exact, component by component (never memcmp: padding)
    {
        return A.m_Position.m_X == B.m_Position.m_X && A.m_Position.m_Y == B.m_Position.m_Y && A.m_Position.m_Z == B.m_Position.m_Z && A.m_Rotation == B.m_Rotation
            && A.m_Scale.m_X == B.m_Scale.m_X && A.m_Scale.m_Y == B.m_Scale.m_Y && A.m_Scale.m_Z == B.m_Scale.m_Z;
    }

    // The flattened mask of a layer on this skeleton (bone entries by name; an unknown bone is a warning)
    inline std::vector<float> LayerMask(const xanim_package_desc::clip_layer& L, const xskeleton_desc::bone_manifest& Manifest, std::vector<std::string>* pWarnings = nullptr)
    {
        const auto& Bones = Manifest.m_Bones;
        std::vector<std::int32_t> Parents(Bones.size());
        for (std::size_t s = 0; s < Bones.size(); ++s) Parents[s] = Bones[s].m_iParent;
        xanimation::mask_rules Rules;
        for (const auto& E : L.m_Mask)
        {
            int b = -1;
            for (int s = 0; s < int(Bones.size()); ++s) if (Bones[s].m_Name == E.m_Bone) { b = s; break; }
            if (b < 0) { if (pWarnings) pWarnings->push_back(std::format("layer '{}': mask bone '{}' is not in the skeleton (ignored)", L.m_Name, E.m_Bone)); continue; }
            Rules.m_Entries.push_back({ .m_Bone = b, .m_Value = E.m_Value, .m_bInherit = E.m_bInherit, .m_BlendDepth = E.m_BlendDepth });
        }
        return xanimation::FlattenMask(xanimation::skeleton_view{ Parents }, Rules);
    }

    // Base (the clip's own source frames) + the clip's enabled layers -> Out (same size as Base). False with Error when a layer cannot run.
    inline bool Compose(const source_frames& Base, const xanim_package_desc::clip& Ov, const xskeleton_desc::bone_manifest& Manifest, const find_source& Find
                       , std::vector<xmath::transform3>& Out, report& Report, std::string& Error)
    {
        const auto& Bones = Manifest.m_Bones;
        const int nSkel = static_cast<int>(Bones.size()), nFrames = Base.m_nFrames, FPS = std::max(1, Base.m_FPS);
        Report = {};
        Out = Base.m_Frames;
        if (int(Base.m_Frames.size()) != nFrames * nSkel) { Error = "the source frames do not match the skeleton"; return false; }
        int iRoot = 0;
        for (int s = 0; s < nSkel; ++s) if (Bones[s].m_iParent < 0) { iRoot = s; break; }
        std::vector<std::int32_t> Parents(nSkel);
        for (int s = 0; s < nSkel; ++s) Parents[s] = Bones[s].m_iParent;

        struct prepared
        {
            const xanim_package_desc::clip_layer*   m_pDesc = nullptr;
            const source_frames*                    m_pSrc  = nullptr;
            xanimation::clip_view                   m_View;
            xanimation::section_time                m_Time;
            std::vector<float>                      m_Mask;
            std::vector<xanimation::weight_key>     m_Keys;
            xanimation::layer_sample                m_Reference;
            std::uint8_t                            m_Channels = 0;
            std::size_t                             m_Written = 0, m_WrittenOutside = 0;
        };
        std::vector<prepared> Layers;
        Layers.reserve(Ov.m_Layers.size());
        std::vector<float> Union(nSkel, 0.0f);
        for (const auto& L : Ov.m_Layers)
        {
            if (!L.m_bEnabled) continue;
            const source_frames* pSrc = Find(L.m_SourceKey, L.m_SourceClip);
            if (!pSrc || int(pSrc->m_Frames.size()) != pSrc->m_nFrames * nSkel) { Error = std::format("layer '{}': its source clip '{}' was not found", L.m_Name, L.m_SourceClip); return false; }
            auto& P = Layers.emplace_back();
            P.m_pDesc = &L; P.m_pSrc = pSrc;
            P.m_View  = xanimation::clip_view{ pSrc->m_Frames, nSkel, pSrc->m_nFrames, std::max(1, pSrc->m_FPS), false };
            const xanimation::tick SourceEnd = ToTick(double(std::max(0, pSrc->m_nFrames - 1)) / P.m_View.m_FPS);
            P.m_Time = { .m_Start = L.m_Start, .m_End = L.m_End >= 0 ? L.m_End : ToTick(double(nFrames) / FPS)
                       , .m_TrimIn = L.m_TrimIn, .m_TrimOut = L.m_TrimOut >= 0 ? L.m_TrimOut : SourceEnd, .m_Rate = L.m_Rate, .m_bReverse = L.m_bReverse };
            if (std::string Why; !xanimation::ValidateSection(P.m_Time, &Why)) { Error = std::format("layer '{}': {}", L.m_Name, Why); return false; }
            P.m_Mask = LayerMask(L, Manifest, &Report.m_Warnings);
            for (int s = 0; s < nSkel; ++s) Union[s] = std::max(Union[s], P.m_Mask[s]);
            for (const auto& K : L.m_WeightKeys)
                P.m_Keys.push_back({ .m_Tick = K.m_Tick, .m_Value = K.m_Value, .m_Interp = K.m_bStep ? xanimation::weight_key::interp::STEP : xanimation::weight_key::interp::LINEAR });
            std::sort(P.m_Keys.begin(), P.m_Keys.end(), [](const auto& A, const auto& B) { return A.m_Tick < B.m_Tick; });
            P.m_Channels = (L.m_bRotation ? xanimation::CHANNEL_ROTATION : 0) | (L.m_bPosition ? xanimation::CHANNEL_POSITION : 0);
            if (L.m_Mode == xanim_package_desc::layer_mode::ADDITIVE)
            {
                P.m_Reference.resize(nSkel, P.m_Channels);
                if (L.m_Reference == xanim_package_desc::additive_reference::SOURCE_FRAME)
                    xanimation::SampleLocal(P.m_View, float(xanimation::TicksToSeconds(L.m_ReferenceTick)), P.m_Reference.m_Pose);
                else
                    for (int s = 0; s < nSkel; ++s) P.m_Reference.m_Pose.set(s, RestOf(Bones[s]));
            }
        }

        xanimation::pose Pose; Pose.resize(nSkel);
        xanimation::layer_sample Sample; Sample.resize(nSkel);
        std::vector<std::uint8_t> Written;
        for (int f = 0; f < nFrames; ++f)
        {
            for (int s = 0; s < nSkel; ++s) Pose.set(s, Out[static_cast<std::size_t>(f) * nSkel + s]);
            const xanimation::tick T = ToTick(double(f) / FPS);
            for (auto& P : Layers)
            {
                if (!xanimation::SampleSection(P.m_View, P.m_Time, T, P.m_Channels, Sample)) continue;
                xanimation::layer_apply A;
                A.m_Mode       = P.m_pDesc->m_Mode == xanim_package_desc::layer_mode::ADDITIVE ? xanimation::layer_mode::ADDITIVE : xanimation::layer_mode::OVERRIDE;
                A.m_Weight     = xanimation::EvaluateWeight(P.m_Keys, T, P.m_pDesc->m_Weight);
                A.m_Mask       = P.m_Mask;
                A.m_pReference = A.m_Mode == xanimation::layer_mode::ADDITIVE ? &P.m_Reference : nullptr;
                A.m_Root       = P.m_pDesc->m_Root == xanim_package_desc::layer_root::FROM_LAYER ? xanimation::root_policy::FROM_LAYER : xanimation::root_policy::INHERIT_BELOW;
                A.m_RootBone   = iRoot;
                A.m_Space      = P.m_pDesc->m_Space == xanim_package_desc::layer_space::SKELETON_MODEL ? xanimation::rotation_space::SKELETON_MODEL : xanimation::rotation_space::LOCAL;
                A.m_Parents    = Parents;
                xanimation::ApplyLayer(Pose, Sample, A, &Written);
                for (int s = 0; s < nSkel; ++s) if (Written[s]) { ++P.m_Written; if (P.m_Mask[s] <= 0.0f) ++P.m_WrittenOutside; }
            }
            for (int s = 0; s < nSkel; ++s) Out[static_cast<std::size_t>(f) * nSkel + s] = Pose.get(s);
        }

        for (const auto& P : Layers)
        {
            auto& R = Report.m_Layers.emplace_back();
            R.m_Name = P.m_pDesc->m_Name; R.m_Source = xstrtool::To(P.m_pDesc->m_SourceKey) + ":" + P.m_pDesc->m_SourceClip;
            R.m_bAdditive = P.m_pDesc->m_Mode == xanim_package_desc::layer_mode::ADDITIVE;
            for (float M : P.m_Mask) R.m_nMaskBones += M > 0.0f;
            R.m_Written = P.m_Written; R.m_WrittenOutside = P.m_WrittenOutside;
        }
        for (int f = 0; f < nFrames; ++f)
            for (int s = 0; s < nSkel; ++s)
            {
                const auto& A = Out[static_cast<std::size_t>(f) * nSkel + s]; const auto& B = Base.m_Frames[static_cast<std::size_t>(f) * nSkel + s];
                if (s == iRoot) Report.m_MaxRootMove = std::max(Report.m_MaxRootMove, (A.m_Position - B.m_Position).Length());
                if (Union[s] <= 0.0f && !SameTransform(A, B)) { if (!Report.m_ChangedOutside) Report.m_FirstOutside = std::format("bone '{}' frame {}", Bones[s].m_Name, f); ++Report.m_ChangedOutside; }
            }
        return true;
    }

    //--------------------------------------------------------------------------------------------
    // The layer-sources cache: the compiler writes every source clip's skeleton frames into the package's log folder (LayerSources.bin,
    // editor only, never a resource) so the editor composes exactly what the compile composes. Little-endian, floats as stored.
    //   "XLS1" u32 count; per clip: u32 keylen + key (UTF-8), u32 cliplen + clip, i32 fps, i32 frames, i32 bones, frames*bones*10 floats
    //   (scale xyz, rotation xyzw, position xyz)
    //--------------------------------------------------------------------------------------------
    inline bool WriteSourcesCache(const std::wstring& Path, const std::vector<source_frames>& Clips, int nBones)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, Path.c_str(), L"wb") || !f) return false;
        auto U32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto Str = [&](const std::string& s) { U32(std::uint32_t(s.size())); std::fwrite(s.data(), 1, s.size(), f); };
        std::fwrite("XLS1", 1, 4, f);
        U32(std::uint32_t(Clips.size()));
        std::vector<float> Row(10);
        for (const auto& C : Clips)
        {
            Str(xstrtool::To(C.m_Key)); Str(C.m_Clip);
            const std::int32_t H[3] = { C.m_FPS, C.m_nFrames, nBones };
            std::fwrite(H, 4, 3, f);
            for (const auto& T : C.m_Frames)
            {
                const float V[10] = { T.m_Scale.m_X, T.m_Scale.m_Y, T.m_Scale.m_Z, T.m_Rotation.m_X, T.m_Rotation.m_Y, T.m_Rotation.m_Z, T.m_Rotation.m_W, T.m_Position.m_X, T.m_Position.m_Y, T.m_Position.m_Z };
                std::fwrite(V, 4, 10, f);
            }
        }
        std::fclose(f);
        return true;
    }

    inline bool ReadSourcesCache(const std::wstring& Path, std::vector<source_frames>& Clips, int& nBones)
    {
        Clips.clear();
        FILE* f = nullptr;
        if (_wfopen_s(&f, Path.c_str(), L"rb") || !f) return false;
        auto U32 = [&](std::uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
        auto Str = [&](std::string& s) { std::uint32_t n; if (!U32(n) || n > (1u << 20)) return false; s.resize(n); return std::fread(s.data(), 1, n, f) == n; };
        char Magic[4]; std::uint32_t Count = 0;
        bool Ok = std::fread(Magic, 1, 4, f) == 4 && !std::memcmp(Magic, "XLS1", 4) && U32(Count) && Count < 100000;
        for (std::uint32_t i = 0; Ok && i < Count; ++i)
        {
            auto& C = Clips.emplace_back();
            std::string Key; std::int32_t H[3];
            Ok = Str(Key) && Str(C.m_Clip) && std::fread(H, 4, 3, f) == 3 && H[1] >= 0 && H[2] > 0 && std::int64_t(H[1]) * H[2] < (1ll << 28);
            if (!Ok) break;
            C.m_Key = xstrtool::To(Key); C.m_FPS = H[0]; C.m_nFrames = H[1]; nBones = H[2];
            C.m_Frames.resize(std::size_t(H[1]) * H[2]);
            float V[10];
            for (auto& T : C.m_Frames)
            {
                if (std::fread(V, 4, 10, f) != 10) { Ok = false; break; }
                T.m_Scale = xmath::fvec3(V[0], V[1], V[2]); T.m_Rotation = xmath::fquat(V[3], V[4], V[5], V[6]); T.m_Position = xmath::fvec3(V[7], V[8], V[9]);
            }
        }
        std::fclose(f);
        if (!Ok) Clips.clear();
        return Ok;
    }
}
#endif
