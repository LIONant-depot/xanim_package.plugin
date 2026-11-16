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
        if (!L.m_SharedMask.empty())        // contract v3: a skeleton mask group by name replaces the layer's own entries
        {
            std::vector<float> W(Bones.size(), 0.0f);
            const auto It = std::find_if(Manifest.m_Masks.begin(), Manifest.m_Masks.end(), [&](const auto& M) { return M.m_Name == L.m_SharedMask; });
            if (It == Manifest.m_Masks.end())
            {
                if (pWarnings) pWarnings->push_back(std::format("ERROR layer '{}': the skeleton has no mask group '{}' - the layer contributes nothing until it is fixed", L.m_Name, L.m_SharedMask));
                return W;
            }
            for (const auto& E : It->m_Bones)
                for (int s = 0; s < int(Bones.size()); ++s) if (Bones[s].m_Name == E.m_Bone) { W[s] = E.m_Weight; break; }
            return W;
        }
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

    // The skeleton's joint limits from its manifest (empty: its JointLimits are Off, or an old manifest without them)
    inline std::vector<xanimation::joint_limit> ManifestLimits(const xskeleton_desc::bone_manifest& M)
    {
        std::vector<xanimation::joint_limit> L;
        if (M.m_Joints.size() != M.m_Bones.size()) return L;
        L.resize(M.m_Joints.size());
        for (std::size_t i = 0; i < L.size(); ++i)
        {
            const auto& J = M.m_Joints[i]; const auto& B = M.m_Bones[i]; auto& O = L[i];
            O.m_Type   = static_cast<xanimation::joint_type>(J.m_Type);
            O.m_SwingY = static_cast<xanimation::axis_motion>(J.m_SwingY); O.m_SwingZ = static_cast<xanimation::axis_motion>(J.m_SwingZ); O.m_Twist = static_cast<xanimation::axis_motion>(J.m_Twist);
            O.m_Rest   = xmath::fquat(B.m_RestRotX, B.m_RestRotY, B.m_RestRotZ, B.m_RestRotW).NormalizeSafeCopy();
            O.m_Frame  = xmath::fquat(J.m_FrameX, J.m_FrameY, J.m_FrameZ, J.m_FrameW);
            O.m_HingeMin = J.m_HingeMin; O.m_HingeMax = J.m_HingeMax;
            O.m_SwingCy = J.m_SwingCy; O.m_SwingCz = J.m_SwingCz; O.m_SwingRy = J.m_SwingRy; O.m_SwingRz = J.m_SwingRz;
            O.m_TwistCentre = J.m_TwistCentre; O.m_TwistHalf = J.m_TwistHalf;
        }
        return L;
    }

    // Base (the clip's own source frames) + the clip's enabled layers -> Out (same size as Base). False with Error when a layer cannot run.
    // pBelowOf / pBelow: also the stack exactly as that layer sees it (the frames just before it is applied: inside a group, the group's own
    // sub-stack) - what an absolute pose key is anchored against. *pBelow stays empty when that layer does not run (disabled).
    inline bool Compose(const source_frames& Base, const xanim_package_desc::clip& Ov, const xskeleton_desc::bone_manifest& Manifest, const find_source& Find
                       , std::vector<xmath::transform3>& Out, report& Report, std::string& Error
                       , const xanim_package_desc::clip_layer* pBelowOf = nullptr, std::vector<xmath::transform3>* pBelow = nullptr)
    {
        if (pBelow) pBelow->clear();
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
            std::vector<xanimation::pose_track>     m_Tracks;               // Pose layers
            std::vector<xanimation::ik_track>       m_Ik;                   // Pose layers: IK keys, solved per frame after the layer's own keys
            std::uint8_t                            m_Channels = 0;
            std::size_t                             m_Written = 0, m_WrittenOutside = 0;
        };
        // contract v3: each layer's effective parent (its folder or group, or 0 = top level) and whether it is in at all (its own Enabled and
        // every ancestor's). A parent that names nothing, names a layer that is not a folder/group, or loops: the top level, with a warning.
        using kind = xanim_package_desc::layer_kind;
        const auto& LL = Ov.m_Layers;
        auto Container = [&](std::uint64_t Id) -> const xanim_package_desc::clip_layer* { if (!Id) return nullptr; for (const auto& L : LL) if (L.m_Id == Id && (L.m_Kind == kind::FOLDER || L.m_Kind == kind::GROUP)) return &L; return nullptr; };
        std::vector<std::uint64_t> Parent(LL.size(), 0);
        std::vector<char> In(LL.size(), 0);
        for (std::size_t i = 0; i < LL.size(); ++i)
        {
            const auto* pP = Container(LL[i].m_Parent);
            if (LL[i].m_Parent && !pP) Report.m_Warnings.push_back(std::format("layer '{}': its parent {} is not a folder or group of this clip (taken as top level)", LL[i].m_Name, LL[i].m_Parent));
            bool bLoop = false, bOn = LL[i].m_bEnabled;
            int Steps = 0;
            for (const auto* pA = pP; pA; pA = Container(pA->m_Parent)) { if (++Steps > int(LL.size()) || pA == &LL[i]) { bLoop = true; break; } bOn = bOn && pA->m_bEnabled; }
            if (bLoop) { Report.m_Warnings.push_back(std::format("layer '{}': its parents loop (taken as top level)", LL[i].m_Name)); pP = nullptr; bOn = LL[i].m_bEnabled; }
            Parent[i] = pP ? pP->m_Id : 0;
            In[i] = bOn;
        }

        if (auto E = xanim_package_desc::RowOverlapError(LL, ToTick(double(nFrames) / FPS)); !E.empty()) { Error = E; return false; }

        std::vector<prepared> Layers;
        Layers.reserve(Ov.m_Layers.size());
        std::vector<std::uint64_t> PreparedParent;
        std::vector<float> Union(nSkel, 0.0f);
        for (std::size_t li = 0; li < LL.size(); ++li)
        {
            const auto& L = LL[li];
            if (!In[li]) continue;
            if (L.m_Kind == kind::KIMODO && L.m_SourceKey.empty()) continue;      // a Kimodo layer with no accepted take yet: nothing
            auto& P = Layers.emplace_back();
            PreparedParent.push_back(Parent[li]);
            P.m_pDesc = &L;
            const xanimation::tick ClipEnd = ToTick(double(nFrames) / FPS);
            if (L.m_Kind == kind::FOLDER || L.m_Kind == kind::GROUP)
            {
                // a folder has nothing of its own; a group: its mask (empty = the whole body) and its weight (keys below)
                if (L.m_Kind == kind::GROUP) P.m_Mask = (L.m_Mask.empty() && L.m_SharedMask.empty()) ? std::vector<float>(nSkel, 1.0f) : LayerMask(L, Manifest, &Report.m_Warnings);
            }
            else if (L.m_Kind == xanim_package_desc::layer_kind::POSE)
            {
                // a Pose layer: tracks from its keys (bones by name); its mask, when empty, is the whole body (the keys decide)
                P.m_Time = { .m_Start = L.m_Start, .m_End = L.m_End >= 0 ? L.m_End : ClipEnd };
                if (P.m_Time.m_End <= P.m_Time.m_Start) { Error = std::format("layer '{}': it ends before it starts", L.m_Name); return false; }
                for (const auto& K : L.m_Keys)
                {
                    int b = -1;
                    for (int s = 0; s < nSkel; ++s) if (Bones[s].m_Name == K.m_Bone) { b = s; break; }
                    if (b < 0) { Report.m_Warnings.push_back(std::format("layer '{}': key bone '{}' is not in the skeleton (ignored)", L.m_Name, K.m_Bone)); continue; }
                    auto It = std::find_if(P.m_Tracks.begin(), P.m_Tracks.end(), [b](const auto& T) { return T.m_Bone == b; });
                    if (It == P.m_Tracks.end()) { P.m_Tracks.push_back({ .m_Bone = b }); It = std::prev(P.m_Tracks.end()); }
                    It->m_Keys.push_back({ .m_Tick = K.m_Tick, .m_Value = xmath::fquat(K.m_X, K.m_Y, K.m_Z, K.m_W).NormalizeCopy(), .m_bOffset = K.m_bOffset, .m_bStep = K.m_bStep, .m_bLinear = K.m_bLinear
                                         , .m_bPosition = K.m_bPosition, .m_Position = xmath::fvec3(K.m_PX, K.m_PY, K.m_PZ) });
                }
                for (auto& T : P.m_Tracks) xanimation::SortTrack(T);
                for (const auto& K : L.m_IkKeys)
                {
                    int b = -1;
                    for (int s = 0; s < nSkel; ++s) if (Bones[s].m_Name == K.m_Effector) { b = s; break; }
                    if (b < 0) { Report.m_Warnings.push_back(std::format("layer '{}': IK key effector '{}' is not in the skeleton (ignored)", L.m_Name, K.m_Effector)); continue; }
                    auto It = std::find_if(P.m_Ik.begin(), P.m_Ik.end(), [b](const auto& T) { return T.m_Bone == b; });
                    if (It == P.m_Ik.end()) { P.m_Ik.push_back({ .m_Bone = b, .m_Attach = xmath::fvec3(K.m_AX, K.m_AY, K.m_AZ) }); It = std::prev(P.m_Ik.end()); }
                    It->m_Keys.push_back({ .m_Tick = K.m_Tick, .m_Position = xmath::fvec3(K.m_PX, K.m_PY, K.m_PZ), .m_bRotation = K.m_bRotation
                                         , .m_Rotation = xmath::fquat(K.m_X, K.m_Y, K.m_Z, K.m_W).NormalizeSafeCopy(), .m_bPinned = K.m_bPinned, .m_bStep = K.m_bStep, .m_bLinear = K.m_bLinear });
                }
                for (auto& T : P.m_Ik) xanimation::SortIkTrack(T);
                P.m_Mask = L.m_Mask.empty() ? std::vector<float>(nSkel, 1.0f) : LayerMask(L, Manifest, &Report.m_Warnings);
                if (L.m_Mode == xanim_package_desc::layer_mode::ADDITIVE) P.m_Reference = xanimation::IdentityReference(nSkel);
                P.m_Channels = xanimation::CHANNEL_ROTATION;
                std::vector<float> Written(nSkel, 0.0f);         // the write set: keyed bones in the mask
                for (const auto& T : P.m_Tracks) Written[T.m_Bone] = P.m_Mask[T.m_Bone];
                for (const auto& T : P.m_Ik)          // the IK's chains: the effector and the two bones above it (the root: itself)
                    for (int a = T.m_Bone, k = 0; a >= 0 && k < 3; a = Parents[a], ++k) Written[a] = 1.0f;
                for (int s = 0; s < nSkel; ++s) Union[s] = std::max(Union[s], Written[s]);
            }
            else
            {
                const source_frames* pSrc = Find(L.m_SourceKey, L.m_SourceClip);
                if (!pSrc || int(pSrc->m_Frames.size()) != pSrc->m_nFrames * nSkel) { Error = std::format("layer '{}': its source clip '{}' was not found", L.m_Name, L.m_SourceClip); return false; }
                P.m_pSrc = pSrc;
                P.m_View = xanimation::clip_view{ pSrc->m_Frames, nSkel, pSrc->m_nFrames, std::max(1, pSrc->m_FPS), false };
                const xanimation::tick SourceEnd = ToTick(double(std::max(0, pSrc->m_nFrames - 1)) / P.m_View.m_FPS);
                P.m_Time = { .m_Start = L.m_Start, .m_End = L.m_End >= 0 ? L.m_End : ClipEnd
                           , .m_TrimIn = L.m_TrimIn, .m_TrimOut = L.m_TrimOut >= 0 ? L.m_TrimOut : SourceEnd, .m_Rate = L.m_Rate, .m_bReverse = L.m_bReverse };
                if (std::string Why; !xanimation::ValidateSection(P.m_Time, &Why)) { Error = std::format("layer '{}': {}", L.m_Name, Why); return false; }
                // a Kimodo layer's mask, when empty, is the whole body (its take is a whole-body motion); a Clip layer's empty mask is nothing
                P.m_Mask = (L.m_Kind == kind::KIMODO && L.m_Mask.empty() && L.m_SharedMask.empty()) ? std::vector<float>(nSkel, 1.0f) : LayerMask(L, Manifest, &Report.m_Warnings);
                for (int s = 0; s < nSkel; ++s) Union[s] = std::max(Union[s], P.m_Mask[s]);
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
            for (const auto& K : L.m_WeightKeys)
                P.m_Keys.push_back({ .m_Tick = K.m_Tick, .m_Value = K.m_Value, .m_Interp = K.m_bStep ? xanimation::weight_key::interp::STEP : xanimation::weight_key::interp::LINEAR });
            std::sort(P.m_Keys.begin(), P.m_Keys.end(), [](const auto& A, const auto& B) { return A.m_Tick < B.m_Tick; });
        }

        // Rows (T4b): Clip/Kimodo layers with the same Row and parent are one timeline row; where two overlap they crossfade. The upper one
        // eases in (it starts later) or out (it ends earlier) over the overlap with a smoothstep; over an override that is the crossfade
        // itself; an additive lower one eases the other way so the sum does not double. One covering the other entirely: no fade (it wins).
        auto RowFade = [&](std::size_t i, xanimation::tick T) -> float
        {
            const auto& Pi = *Layers[i].m_pDesc;
            if (!Pi.m_Row || (Pi.m_Kind != kind::CLIP && Pi.m_Kind != kind::KIMODO)) return 1.0f;
            // the upper layer U's weight over the lower D at T; -1 when no crossfade applies (outside the overlap, or U covers D)
            auto Fade = [&](const prepared& U, const prepared& D) -> float
            {
                const auto a = std::max(U.m_Time.m_Start, D.m_Time.m_Start), b = std::min(U.m_Time.m_End, D.m_Time.m_End);
                if (b <= a || T < a || T >= b) return -1.0f;
                const float u = float(double(T - a) / double(b - a)), s = u * u * (3.0f - 2.0f * u);
                if (U.m_Time.m_Start > D.m_Time.m_Start) return s;
                if (U.m_Time.m_End < D.m_Time.m_End)     return 1.0f - s;
                return -1.0f;
            };
            float W = 1.0f;
            for (std::size_t j = 0; j < Layers.size(); ++j)
            {
                const auto& Pj = *Layers[j].m_pDesc;
                if (j == i || Pj.m_Row != Pi.m_Row || PreparedParent[j] != PreparedParent[i] || (Pj.m_Kind != kind::CLIP && Pj.m_Kind != kind::KIMODO)) continue;
                if (j < i) { if (const float f = Fade(Layers[i], Layers[j]); f >= 0) W *= f; }                                             // i is the upper one
                else if (Pi.m_Mode == xanim_package_desc::layer_mode::ADDITIVE) { if (const float f = Fade(Layers[j], Layers[i]); f >= 0) W *= 1.0f - f; }   // i additive, under j
            }
            return W;
        };

        // layer by layer over the whole clip (each layer sees the finished stack below it: an additive Pose layer's anchors read it)
        xanimation::pose Pose; Pose.resize(nSkel);
        xanimation::layer_sample Sample; Sample.resize(nSkel);
        std::vector<std::uint8_t> Written;
        // contract v3: the top level in list order; a folder's children at its place onto the same stack (pass-through); a group's children over
        // a copy of the stack below it, the result applied with the group's weight and mask (isolated)
        auto ApplyOne = [&](prepared& P, std::vector<xmath::transform3>& Out)
        {
            const bool bPose = P.m_pDesc->m_Kind == xanim_package_desc::layer_kind::POSE;
            const bool bAdd  = P.m_pDesc->m_Mode == xanim_package_desc::layer_mode::ADDITIVE;
            if (bPose && bAdd)
            {
                xanimation::DeriveAnchors(std::span<xanimation::pose_track>(P.m_Tracks), [&](int b, xanimation::tick T)
                {   // the stack below at the key's tick: its frames blended like a clip sample
                    const double FT = std::clamp(xanimation::TicksToSeconds(T) * FPS, 0.0, double(nFrames - 1));
                    const int A = int(FT), B = std::min(A + 1, nFrames - 1);
                    return xmath::fquat::Slerp(Out[std::size_t(A) * nSkel + b].m_Rotation, Out[std::size_t(B) * nSkel + b].m_Rotation, float(FT - A));
                });
                xanimation::DerivePositionAnchors(std::span<xanimation::pose_track>(P.m_Tracks), [&](int b, xanimation::tick T)
                {
                    const double FT = std::clamp(xanimation::TicksToSeconds(T) * FPS, 0.0, double(nFrames - 1));
                    const int A = int(FT), B = std::min(A + 1, nFrames - 1);
                    const auto& PA = Out[std::size_t(A) * nSkel + b].m_Position; const auto& PB = Out[std::size_t(B) * nSkel + b].m_Position;
                    return PA + (PB - PA) * float(FT - A);
                });
            }
            const xanimation::pose_sampling Sampling{ .m_bSmooth = P.m_pDesc->m_bSmooth, .m_Falloff = ToTick(std::max(0.0, double(P.m_pDesc->m_Falloff))) };
            for (int f = 0; f < nFrames; ++f)
            {
                const xanimation::tick T = ToTick(double(f) / FPS);
                const bool bIn = bPose ? xanimation::SamplePoseTracks(P.m_Tracks, P.m_Time, T, bAdd ? xanimation::layer_mode::ADDITIVE : xanimation::layer_mode::OVERRIDE, Sample, Sampling)
                                       : xanimation::SampleSection(P.m_View, P.m_Time, T, P.m_Channels, Sample);
                if (!bIn) continue;
                for (int s = 0; s < nSkel; ++s) Pose.set(s, Out[static_cast<std::size_t>(f) * nSkel + s]);
                xanimation::layer_apply A;
                A.m_Mode       = bAdd ? xanimation::layer_mode::ADDITIVE : xanimation::layer_mode::OVERRIDE;
                A.m_Weight     = xanimation::EvaluateWeight(P.m_Keys, T, P.m_pDesc->m_Weight) * RowFade(std::size_t(&P - Layers.data()), T);
                A.m_Mask       = P.m_Mask;
                A.m_pReference = bAdd ? &P.m_Reference : nullptr;
                A.m_Root       = (bPose || P.m_pDesc->m_Root == xanim_package_desc::layer_root::FROM_LAYER) ? xanimation::root_policy::FROM_LAYER : xanimation::root_policy::INHERIT_BELOW;   // a pose layer's positioned key is the user's intent, the root's too
                A.m_RootBone   = iRoot;
                A.m_Space      = P.m_pDesc->m_Space == xanim_package_desc::layer_space::SKELETON_MODEL ? xanimation::rotation_space::SKELETON_MODEL : xanimation::rotation_space::LOCAL;
                A.m_Parents    = Parents;
                xanimation::ApplyLayer(Pose, Sample, A, &Written);
                for (int s = 0; s < nSkel; ++s)
                    if (Written[s]) { ++P.m_Written; if (P.m_Mask[s] <= 0.0f) ++P.m_WrittenOutside; Out[static_cast<std::size_t>(f) * nSkel + s] = Pose.get(s); }
            }
            // IK keys: the layer's effector targets solved on every frame of the finished layer (baked: no runtime IK)
            if (bPose && !P.m_Ik.empty())
            {
                thread_local xanimation::body_ik Solver;
                std::vector<xanimation::two_bone_chain> Limbs;
                for (const auto& T : P.m_Ik) { const int p = Parents[T.m_Bone], g = p >= 0 ? Parents[p] : -1; if (g >= 0) Limbs.push_back({ g, p, T.m_Bone }); }
                const auto Limits = P.m_pDesc->m_bBoneLimits ? ManifestLimits(Manifest) : std::vector<xanimation::joint_limit>{};
                Solver.Setup(Parents, Limits, {}, Limbs);     // the skeleton's limits (C59): none when its JointLimits are Off or the layer's BoneLimits is off
                std::vector<std::uint8_t> W; std::vector<xanimation::ik_task> Tasks; std::vector<xanimation::ik_task_report> Rep;
                xanimation::pose Solved;
                for (int f = 0; f < nFrames; ++f)
                {
                    for (int s = 0; s < nSkel; ++s) Pose.set(s, Out[static_cast<std::size_t>(f) * nSkel + s]);
                    if (!xanimation::SolveIkFrame(Solver, Parents, P.m_Ik, ToTick(double(f) / FPS), P.m_pDesc->m_bSmooth, Sampling.m_Falloff, Pose, W, Tasks, Rep, Solved)) continue;
                    for (int s = 0; s < nSkel; ++s) if (W[s]) { ++P.m_Written; Out[static_cast<std::size_t>(f) * nSkel + s] = Solved.get(s); }
                }
            }
        };
        std::function<void(std::uint64_t, std::vector<xmath::transform3>&)> Eval = [&](std::uint64_t ParentId, std::vector<xmath::transform3>& Out)
        {
            for (std::size_t i = 0; i < Layers.size(); ++i)
            {
                if (PreparedParent[i] != ParentId) continue;
                auto& P = Layers[i];
                if (pBelow && P.m_pDesc == pBelowOf) *pBelow = Out;
                const auto K = P.m_pDesc->m_Kind;
                if (K == kind::FOLDER) { Eval(P.m_pDesc->m_Id, Out); continue; }
                if (K != kind::GROUP) { ApplyOne(P, Out); continue; }
                std::vector<xmath::transform3> R = Out;           // the group's base: the stack below it
                Eval(P.m_pDesc->m_Id, R);
                xanimation::pose GR; GR.resize(nSkel);
                for (int f = 0; f < nFrames; ++f)
                {
                    const xanimation::tick T = ToTick(double(f) / FPS);
                    for (int s = 0; s < nSkel; ++s) { Pose.set(s, Out[static_cast<std::size_t>(f) * nSkel + s]); GR.set(s, R[static_cast<std::size_t>(f) * nSkel + s]); }
                    xanimation::ApplyGroupResult(Pose, GR, xanimation::EvaluateWeight(P.m_Keys, T, P.m_pDesc->m_Weight), P.m_Mask, &Written);
                    for (int s = 0; s < nSkel; ++s)
                        if (Written[s]) { ++P.m_Written; if (P.m_Mask[s] <= 0.0f) ++P.m_WrittenOutside; Out[static_cast<std::size_t>(f) * nSkel + s] = Pose.get(s); }
                }
            }
        };
        Eval(0, Out);

        for (const auto& P : Layers)
        {
            auto& R = Report.m_Layers.emplace_back();
            R.m_Name = P.m_pDesc->m_Name;
            const auto K = P.m_pDesc->m_Kind;
            R.m_Source = K == kind::POSE ? std::format("pose: {} keys on {} bones", P.m_pDesc->m_Keys.size(), P.m_Tracks.size())
                       : K == kind::FOLDER ? std::string("folder (pass-through)")
                       : K == kind::GROUP  ? std::string("group (isolated)")
                       : xstrtool::To(P.m_pDesc->m_SourceKey) + ":" + P.m_pDesc->m_SourceClip;
            R.m_bAdditive = P.m_pDesc->m_Mode == xanim_package_desc::layer_mode::ADDITIVE;
            if (P.m_pDesc->m_Kind == xanim_package_desc::layer_kind::POSE) for (const auto& T : P.m_Tracks) R.m_nMaskBones += P.m_Mask[T.m_Bone] > 0.0f;   // its write set
            else for (float M : P.m_Mask) R.m_nMaskBones += M > 0.0f;
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
