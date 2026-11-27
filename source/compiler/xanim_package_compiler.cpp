#include "xanim_package_compiler.h"
#include "dependencies/xAnimation/source/xanimation_env.h"      // (xanimation::OpenW: _wfopen without MSVC's C4996)
#include "dependencies/xAnimation/source/semantic/xanimation_clip_traits.h"

#include "dependencies/xraw3d/source/xraw3d.h"
#include "dependencies/xraw3d/source/details/xraw3d_assimp_import_v3.h"

#include "dependencies/xstrtool/source/xstrtool.h"

#include "../xanim_package_descriptor.h"
#include "plugins/xretarget.plugin/source/xretarget_mapping.h"
#include "dependencies/xAnimation/source/xanimation.h"
#include "plugins/xskeleton.plugin/source/xskeleton_joint_evidence.h"
#include "../xanim_package_layers.h"
#include "../xanim_package.h"
#include "../xanim_package_details.h"

#include "dependencies/xproperty/source/xcore/my_properties.cpp"
#include "dependencies/xmath/source/bridge/xmath_to_xproperty.h"

#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace xanim_package_compiler
{
    struct implementation : xanim_package_compiler::instance
    {
        // One raw imported clip, tagged with which source file it came from - the "official list"
        // ComputeDetailStructure() builds m_Details from, before any descriptor override is applied.
        struct raw_clip
        {
            xraw3d::anim   m_Anim;
            std::wstring   m_SourceFile;
            bool           m_bInSkeletonSpace = false;     // the keys are already the package skeleton's own (retargeted: its Pre/PostTransform are not applied again)
            std::uint64_t  m_FromSkeleton = 0;             // a clip taken from another animation PACKAGE: that package's skeleton (its keys are in that skeleton's space already)
            bool           m_bPlanted = false;             // the Retarget's PlantFeet held its feet already (E58 does not plant again)
        };

        // The Retarget of an import source: its descriptor read (which reads both skeletons' bone manifests), the mapping resolved and the retargeter made.
        // Kept for the whole compile: the mapping holds views into the descriptor's manifests.
        struct retarget_context
        {
            xretarget_desc::descriptor      m_Descriptor;
            xretarget_desc::mapping         m_Mapping;
            xanimation::retargeter          m_Retargeter;
        };
        std::unordered_map<std::uint64_t, std::unique_ptr<retarget_context>> m_Retargets;

        //--------------------------------------------------------------------------------------

        implementation()
        {
            m_FinalPackage.Initialize();
        }

        //--------------------------------------------------------------------------------------
        // ImportAnimations() unconditionally uses m_pGeom as its own internal working area for bone
        // lookups (getBoneIDFromName) even when the caller has no interest in mesh/material data - a
        // throwaway scratch geom is required to avoid a null-pointer crash inside the importer.
        // Setting it also unavoidably triggers the importer's own ImportGeometry()/ImportMaterials()
        // passes (same flag gates both), which we simply discard - acceptable overhead for an
        // offline, animation-only compile.
        xerr LoadRaw(void)
        {
            for (auto& Source : m_Descriptor.m_ImportSources)
            {
                if (Source.bPackage())
                {
                    if (auto Err = LoadPackageSource(Source); Err) return Err;
                    continue;
                }
                if (Source.bNew())      // a clip made from nothing: the skeleton's rest pose held for its length, already in skeleton space
                {
                    const auto& Bones = m_Descriptor.m_ResolvedSkeletonBones.m_Bones;
                    if (Bones.empty()) return xerr::create_f<state, "A new animation needs the package's skeleton (compile the skeleton first)">();
                    xraw3d::anim A;
                    A.m_Name = xstrtool::To(Source.m_Path); A.m_FPS = Source.m_FPS;
                    A.m_nFrames = std::max(2, static_cast<int>(std::lround(double(Source.m_Seconds) * Source.m_FPS)) + 1);
                    A.m_Bone.resize(Bones.size());
                    for (std::size_t b = 0; b < Bones.size(); ++b) { A.m_Bone[b] = {}; A.m_Bone[b].m_Name = Bones[b].m_Name; A.m_Bone[b].m_iParent = Bones[b].m_iParent; }
                    A.m_KeyFrame.resize(static_cast<std::size_t>(A.m_nFrames) * Bones.size());
                    for (int f = 0; f < A.m_nFrames; ++f)
                        for (std::size_t b = 0; b < Bones.size(); ++b)
                        {
                            auto& K = A.m_KeyFrame[static_cast<std::size_t>(f) * Bones.size() + b];
                            K.m_Position = Bones[b].m_RestPosition; K.m_Scale = Bones[b].m_RestScale;
                            K.m_Rotation = xmath::fquat(Bones[b].m_RestRotX, Bones[b].m_RestRotY, Bones[b].m_RestRotZ, Bones[b].m_RestRotW).NormalizeSafeCopy();
                        }
                    m_RawClips.push_back(raw_clip{ .m_Anim = std::move(A), .m_SourceFile = Source.key(), .m_bInSkeletonSpace = true });
                    continue;
                }
                xraw3d::assimp_v3::importer             Importer;
                xraw3d::assimp_v3::importer::settings   Settings;
                xraw3d::geom                             GeomScratch;
                xraw3d::assimp_v3::importer::anim_list  AnimList;

                Settings.m_bStaticGeometry = false;
                Settings.m_pGeom           = &GeomScratch;
                Settings.m_pAnims          = &AnimList;

                const auto Path = std::format(L"{}/{}", m_ProjectPaths.m_Project, Source.m_Path);
                if (auto Err = Importer.Import(Path, Settings); Err)
                    return xerr::create_f<state, "Failed to import an animation source">(Err);

                for (auto& Anim : AnimList)
                    m_RawClips.push_back(raw_clip{ .m_Anim = std::move(Anim), .m_SourceFile = Source.key() });
            }
            return {};
        }

        //--------------------------------------------------------------------------------------

        static int FindRootBone(const xraw3d::anim& Anim) noexcept
        {
            for (int i = 0; i < static_cast<int>(Anim.m_Bone.size()); ++i)
                if (Anim.m_Bone[i].m_iParent == -1) return i;
            return -1;
        }

        //--------------------------------------------------------------------------------------
        // Editor-facing view of every clip actually found across every source file - unaffected by
        // descriptor overrides, same role/timing as xskeleton_compiler.cpp's ComputeDetailStructure().
        // Nested by source file (see xanim_package_details.h's own comment on why) - one `source`
        // entry per m_ImportSources entry, in the same order, so a source with zero clips (a failed
        // or animation-less import) still shows up as an empty group rather than disappearing.
        void ComputeDetailStructure()
        {
            m_Details.m_Sources.clear();
            for (auto& Source : m_Descriptor.m_ImportSources)
                m_Details.m_Sources.push_back({ .m_Path = Source.key() });

            for (auto& Raw : m_RawClips)
            {
                const int iSource = m_Details.findSource(Raw.m_SourceFile);
                if (iSource == -1) continue; // shouldn't happen - every raw clip is tagged from m_Descriptor.m_ImportSources itself
                auto& D = m_Details.m_Sources[iSource].m_ClipList.emplace_back();

                D.m_Name              = Raw.m_Anim.m_Name;
                D.m_OriginalFPS       = Raw.m_Anim.m_FPS;
                D.m_OriginalFrameCount = Raw.m_Anim.m_nFrames;
                D.m_DurationSeconds   = Raw.m_Anim.m_FPS > 0 ? static_cast<float>(Raw.m_Anim.m_nFrames) / Raw.m_Anim.m_FPS : 0.f;
                D.m_NumBonesAnimated  = static_cast<int>(Raw.m_Anim.m_Bone.size());

                D.m_bRootMotionCandidate = false;
                if (const int iRoot = FindRootBone(Raw.m_Anim); iRoot != -1 && Raw.m_Anim.m_nFrames > 1)
                {
                    const auto& First = Raw.m_Anim.m_KeyFrame[0 * Raw.m_Anim.m_Bone.size() + iRoot].m_Position;
                    const auto& Last  = Raw.m_Anim.m_KeyFrame[(Raw.m_Anim.m_nFrames - 1) * Raw.m_Anim.m_Bone.size() + iRoot].m_Position;
                    D.m_bRootMotionCandidate = (Last - First).Length() > 0.001f;
                }
            }

            m_Details.m_NumSourceFiles = static_cast<int>(m_Details.m_Sources.size());
            m_Details.m_NumClips = 0;
            for (auto& S : m_Details.m_Sources) m_Details.m_NumClips += static_cast<int>(S.m_ClipList.size());
        }

        //--------------------------------------------------------------------------------------
        // Keyed by [SourceFile][OriginalName] - a raw clip's override can only ever be looked up
        // scoped to the file it actually came from (see xanim_package_desc::clip's own comment on
        // why matching can't be a flat cross-file map).
        void CollectOverrides(std::unordered_map<std::wstring, std::unordered_map<std::string, const xanim_package_desc::clip*>>& Map)
        {
            for (auto& Source : m_Descriptor.m_ImportSources)
            {
                auto& SourceMap = Map[Source.key()];
                for (auto& C : Source.m_Clips)
                    SourceMap[C.m_OriginalName] = &C;
            }
        }

        //--------------------------------------------------------------------------------------
        // Resamples a clip already uniformly sampled at SourceFPS down to TargetFPS via nearest-frame
        // lookup (no interpolation - simple and sufficient for a first pass; TargetFPS must be <=
        // SourceFPS, enforced by Validate()). Returns the new frame count.
        static int ResampleFrameIndex(int OutFrame, int SourceFPS, int TargetFPS) noexcept
        {
            return static_cast<int>(std::lround(static_cast<double>(OutFrame) * SourceFPS / TargetFPS));
        }

        //--------------------------------------------------------------------------------------
        // Layers (the layered edit mode): a raw clip in the skeleton's bone order AND space, every source frame (no trim, no resample):
        // the same mapping and root corrections BuildCompiledClip applies, done once for the whole clip so layers can sample it.
        static std::vector<xmath::transform3> SkeletonFrames(const raw_clip& Raw, const xskeleton_desc::bone_manifest& Manifest)
        {
            const auto& Bones = Manifest.m_Bones;
            const int nSkel = static_cast<int>(Bones.size()), nRaw = static_cast<int>(Raw.m_Anim.m_Bone.size()), nFrames = Raw.m_Anim.m_nFrames;
            std::vector<int> SkelToRaw(nSkel, -1);
            for (int s = 0; s < nSkel; ++s)
                for (int r = 0; r < nRaw; ++r) if (Raw.m_Anim.m_Bone[r].m_Name == Bones[s].m_Name) { SkelToRaw[s] = r; break; }
            std::vector<xmath::transform3> Out(static_cast<std::size_t>(nFrames) * nSkel);
            for (int f = 0; f < nFrames; ++f)
                for (int s = 0; s < nSkel; ++s)
                    Out[static_cast<std::size_t>(f) * nSkel + s] = SkelToRaw[s] != -1 ? Raw.m_Anim.m_KeyFrame[static_cast<std::size_t>(f) * nRaw + SkelToRaw[s]]
                        : xmath::transform3{ .m_Scale = Bones[s].m_RestScale, .m_Rotation = xmath::fquat(Bones[s].m_RestRotX, Bones[s].m_RestRotY, Bones[s].m_RestRotZ, Bones[s].m_RestRotW), .m_Position = Bones[s].m_RestPosition };
            int iRoot = -1;
            for (int s = 0; s < nSkel; ++s) if (Bones[s].m_iParent < 0) { iRoot = s; break; }
            if (!Raw.m_bInSkeletonSpace && iRoot != -1 && SkelToRaw[iRoot] != -1)
            {
                auto Apply = [&](const xmath::fvec3& Scale, const xmath::fvec3& RotationDeg, const xmath::fvec3& Translation)
                {
                    if (Scale == xmath::fvec3::fromOne() && RotationDeg == xmath::fvec3::fromZero() && Translation == xmath::fvec3::fromZero()) return;
                    xmath::radian3 Rot;
                    Rot.m_Roll  = xmath::radian{ xmath::DegToRad(RotationDeg.m_Z) };
                    Rot.m_Pitch = xmath::radian{ xmath::DegToRad(RotationDeg.m_X) };
                    Rot.m_Yaw   = xmath::radian{ xmath::DegToRad(RotationDeg.m_Y) };
                    const xmath::fquat Quat(Rot);
                    const xmath::fmat4 M(Scale, Quat, Translation);
                    for (int f = 0; f < nFrames; ++f)
                    {
                        auto& K = Out[static_cast<std::size_t>(f) * nSkel + iRoot];
                        K.m_Position = M * K.m_Position; K.m_Rotation = Quat * K.m_Rotation; K.m_Scale = Scale * K.m_Scale;
                    }
                };
                Apply(Manifest.m_PostTransformScale, Manifest.m_PostTransformRotationDeg, Manifest.m_PostTransformTranslation);
                Apply(Manifest.m_PreTransformScale,  Manifest.m_PreTransformRotationDeg,  Manifest.m_PreTransformTranslation);
            }
            return Out;
        }

        // The clip's layers baked onto its source frames (xanim_package_layers.h: the same function the editor previews with), with the
        // isolation report in the log: per layer what it wrote, and over the clip what changed OUTSIDE every mask (must be 0) and the root's travel.
        xerr ComposeClip(const raw_clip& Base, const xanim_package_desc::clip& Ov, const xskeleton_desc::bone_manifest& Manifest, std::vector<xmath::transform3>& Out)
        {
            const auto* pBase = FindSourceFrames(Base.m_SourceFile, Base.m_Anim.m_Name);
            if (!pBase) return xerr::create_f<state, "A clip's own source frames are missing (internal)">();
            xanim_package_layers::report R; std::string Error;
            if (!xanim_package_layers::Compose(*pBase, Ov, Manifest, [this](const std::wstring& K, const std::string& C) { return FindSourceFrames(K, C); }, Out, R, Error))
            {
                LogMessage(xresource_pipeline::msg_type::ERROR, std::format("Clip '{}': {}", Ov.m_Name, Error));
                return xerr::create_f<state, "A clip's layers could not be composed (see the log)">();
            }
            for (const auto& W : R.m_Warnings) LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': {}", Ov.m_Name, W));
            for (const auto& L : R.m_Layers)
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Layers [{}] '{}' ({}, {}): {} bones in its mask; bone-frames written {}, outside its mask {}"
                    , Ov.m_Name, L.m_Name, L.m_bAdditive ? "additive" : "override", L.m_Source, L.m_nMaskBones, L.m_Written, L.m_WrittenOutside));
            LogMessage(R.m_ChangedOutside ? xresource_pipeline::msg_type::ERROR : xresource_pipeline::msg_type::INFO
                , std::format("Layers [{}] isolation: {} layer(s) over {} frames; bone-frames changed outside every mask {}{}; root travel change {:.6f} m"
                    , Ov.m_Name, R.m_Layers.size(), pBase->m_nFrames, R.m_ChangedOutside, R.m_ChangedOutside ? " (first: " + R.m_FirstOutside + ")" : std::string{}, R.m_MaxRootMove));
            return {};
        }

        // every raw clip's skeleton frames (the layers' sources and bases), made once per compile and written for the editor (LayerSources.bin)
        std::vector<xanim_package_layers::source_frames> m_SourceFrames;
        std::vector<float>                               m_RootTurn;
        std::string                                      m_LoopReport;      // C127: one row per Loop clip (LoopReport.txt next to the log)
        std::vector<xmath::transform3>                   m_SidecarKeys;  // K5 step 2: the raw keys, written next to the package (<package>.keys)     // parallel to the package's root motion: the root's turn about +Y per frame (radians)
        const xanim_package_layers::source_frames* FindSourceFrames(const std::wstring& Key, const std::string& Clip) const
        {
            for (const auto& S : m_SourceFrames) if (S.m_Key == Key && S.m_Clip == Clip) return &S;
            return nullptr;
        }

        //--------------------------------------------------------------------------------------
        // Builds one compiled clip from a raw import + its (possibly null) descriptor override.
        // Returns false (via Skip) if the clip is ignored.
        // pComposed: the clip's layers already baked (ComposeClip: skeleton order and space, every source frame); null = no layers, the
        // path below is the one every clip took before layers existed.
        //
        // Bone-order-synced by construction: the output curve is laid out in the REFERENCED
        // SKELETON's own compiled bone order/width (ResolvedBones), not the raw file's own bone
        // list/order - every skeleton bone gets a slot in every clip. A skeleton bone with a matching
        // raw bone (by name) gets that bone's animated curve; any skeleton bone the raw file doesn't
        // carry a channel for gets a constant curve equal to the skeleton's own compiled rest pose
        // (carried in the cross-plugin bone manifest - see xskeleton_bone_manifest.h). This is what
        // lets playback index a clip's frame directly by the skeleton's own bone index, with no
        // per-frame name-hash lookup. A raw bone with no matching skeleton bone is simply never read
        // (warned about below, for author visibility) - retargeting across a skeleton-side rename is
        // out of scope for this pass.
        xerr BuildCompiledClip
        ( const raw_clip&                                    Raw
        , const xanim_package_desc::clip*                    pOv
        , const xskeleton_desc::bone_manifest&                Manifest
        , bool&                                               Skip
        , xanim_package::clip&                                Out
        , std::vector<xmath::transform3>&                     AllKeyFrames
        , std::vector<xmath::fvec3>&                          AllRootMotion
        , const std::vector<xmath::transform3>*               pComposed = nullptr
        , std::vector<xanim_package::event>*                  pAllEvents = nullptr
        )
        {
            const auto& ResolvedBones = Manifest.m_Bones;

            Skip = pOv && pOv->m_bDelete;
            if (Skip) return {};

            const std::string  CompiledName = (pOv && !pOv->m_Name.empty()) ? pOv->m_Name : Raw.m_Anim.m_Name;
            const bool          bLoop        = pOv && pOv->m_bLoop;
            const auto          RootMotion   = pOv ? pOv->m_RootMotion : xanim_package::root_motion_mode::NONE;
            const int           DownsampleFPS  = pOv ? pOv->m_DownsampleFPS : 0;
            const int           TrimStartFrame = pOv ? pOv->m_TrimStartFrame : -1;
            const int           TrimEndFrame   = pOv ? pOv->m_TrimEndFrame   : -1;

            const int SourceFPS     = Raw.m_Anim.m_FPS;
            const int SourceFrames  = Raw.m_Anim.m_nFrames;
            const int nRawBones     = static_cast<int>(Raw.m_Anim.m_Bone.size());
            const int nSkelBones    = static_cast<int>(ResolvedBones.size());

            if (DownsampleFPS > SourceFPS)
                LogMessage(xresource_pipeline::msg_type::WARNING
                    , std::format("Clip '{}' asked to downsample to {} fps, which is higher than the imported rate ({}) - keeping the imported rate (no upsampling).", CompiledName, DownsampleFPS, SourceFPS));

            const int FinalFPS = (DownsampleFPS > 0 && DownsampleFPS <= SourceFPS) ? DownsampleFPS : SourceFPS;
            const int ResampledFrameCount = (FinalFPS == SourceFPS) ? SourceFrames : std::max(1, static_cast<int>(std::lround(static_cast<double>(SourceFrames) * FinalFPS / SourceFPS)));

            // Trim bounds are frame indices directly in the FINAL (post-resample) domain - no unit
            // conversion needed, just clamp into range.
            int StartFrame = (TrimStartFrame < 0) ? 0 : std::clamp(TrimStartFrame, 0, ResampledFrameCount - 1);
            const int EndFrame   = (TrimEndFrame   < 0) ? (ResampledFrameCount - 1) : std::clamp(TrimEndFrame, StartFrame, ResampledFrameCount - 1);
            int OutFrames  = EndFrame - StartFrame + 1;

            Out.m_NameHash        = xstrtool::CRC32(CompiledName);
            Out.m_FPS             = FinalFPS;
            Out.m_nFrames         = OutFrames;
            Out.m_bLoop           = bLoop;
            Out.m_RootMotionMode  = RootMotion;
            Out.m_iFirstKeyFrame  = static_cast<std::uint32_t>(AllKeyFrames.size());
            Out.m_iFirstRootMotion = static_cast<std::uint32_t>(AllRootMotion.size());
            Out.m_LoopDisplacement = xmath::fvec3::fromZero();

            // Map each skeleton bone (by name) to its raw-clip bone slot, if any - a small, one-time,
            // compile-time lookup, not a runtime cost.
            std::vector<int> SkelToRaw(nSkelBones, -1);
            for (int s = 0; s < nSkelBones; ++s)
            {
                for (int r = 0; r < nRawBones; ++r)
                {
                    if (Raw.m_Anim.m_Bone[r].m_Name == ResolvedBones[s].m_Name) { SkelToRaw[s] = r; break; }
                }
            }
            for (int r = 0; r < nRawBones; ++r)
            {
                const bool bFound = std::find(SkelToRaw.begin(), SkelToRaw.end(), r) != SkelToRaw.end();
                if (!bFound)
                    LogMessage(xresource_pipeline::msg_type::WARNING
                        , std::format("Clip '{}' references bone '{}' which was not found in the referenced skeleton.", CompiledName, Raw.m_Anim.m_Bone[r].m_Name));
            }

            const std::size_t KeyFrameBase = AllKeyFrames.size();
            AllKeyFrames.resize(KeyFrameBase + static_cast<std::size_t>(OutFrames) * nSkelBones);
            for (int f = 0; f < OutFrames; ++f)
            {
                const int SrcFrame = std::clamp(ResampleFrameIndex(StartFrame + f, SourceFPS, FinalFPS), 0, SourceFrames - 1);
                if (pComposed)          // layers baked: the frames are already in the skeleton's order and space
                {
                    std::copy_n(pComposed->begin() + static_cast<std::ptrdiff_t>(SrcFrame) * nSkelBones, nSkelBones, AllKeyFrames.begin() + static_cast<std::ptrdiff_t>(KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones));
                    continue;
                }
                for (int s = 0; s < nSkelBones; ++s)
                {
                    const int iRaw = SkelToRaw[s];
                    AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + s] = (iRaw != -1)
                        ? Raw.m_Anim.m_KeyFrame[static_cast<std::size_t>(SrcFrame) * nRawBones + iRaw]
                        : xmath::transform3
                          { .m_Scale    = ResolvedBones[s].m_RestScale
                          , .m_Rotation = xmath::fquat(ResolvedBones[s].m_RestRotX, ResolvedBones[s].m_RestRotY, ResolvedBones[s].m_RestRotZ, ResolvedBones[s].m_RestRotW)
                          , .m_Position = ResolvedBones[s].m_RestPosition
                          };
                }
            }

            // Identify the SKELETON's own root bone slot (m_iParent < 0, per the manifest - NOT the
            // raw clip's own hierarchy) - shared by the PreTransform correction below and root-motion
            // extraction further down. Deliberately NOT FindRootBone(Raw.m_Anim): assimp/FBX imports
            // frequently wrap the real, named root under a synthetic "RootNode" container, so the raw
            // clip's own m_iParent==-1 bone is often that wrapper, not the skeleton's actual root -
            // matching against it either silently corrects the wrong bone or (when the wrapper's name
            // matches nothing in the skeleton, as here) never corrects anything at all. Only meaningful
            // if that skeleton bone's slot actually came from this raw clip's animated data (see the
            // SkelToRaw construction above) - never the rest-pose fallback.
            int iSkelRoot = -1;
            for (int s = 0; s < nSkelBones; ++s)
                if (ResolvedBones[s].m_iParent < 0) { iSkelRoot = s; break; }
            const bool bHaveAnimatedRoot = (iSkelRoot != -1) && (SkelToRaw[iSkelRoot] != -1);

            // The root bone's curve is put in the skeleton's space: first the skeleton's PostTransform (the artist rotated the model while animating it), then
            // the skeleton's PreTransform, which mirrors xskeleton_compiler.cpp's ApplyPreTransform() exactly (root-only; every descendant inherits the
            // correction for free through the normal parent-multiply FK chain at playback). This clip's raw import never passes through the skeleton's own
            // import step, so without the PreTransform the animated root stays in raw-import space while the skeleton's rest pose (and every unanimated
            // bone's fallback here, both already-corrected via the manifest) sit in engine space - a visible scale/offset mismatch between the two. Only
            // meaningful if that skeleton bone's slot actually came from this raw clip's animated data (see the SkelToRaw construction above) - never the
            // rest-pose fallback.
            const auto ApplyToRoot = [&](const char* pName, const xmath::fvec3& Scale, const xmath::fvec3& RotationDeg, const xmath::fvec3& Translation)
            {
                if (!bHaveAnimatedRoot || (Scale == xmath::fvec3::fromOne() && RotationDeg == xmath::fvec3::fromZero() && Translation == xmath::fvec3::fromZero())) return;
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the skeleton's {} is applied to its root (scale {:.4f} {:.4f} {:.4f}, rotation {:.2f} {:.2f} {:.2f}, translation {:.4f} {:.4f} {:.4f})"
                    , CompiledName, pName, Scale.m_X, Scale.m_Y, Scale.m_Z, RotationDeg.m_X, RotationDeg.m_Y, RotationDeg.m_Z, Translation.m_X, Translation.m_Y, Translation.m_Z));

                xmath::radian3 Rot;
                Rot.m_Roll  = xmath::radian{ xmath::DegToRad(RotationDeg.m_Z) };
                Rot.m_Pitch = xmath::radian{ xmath::DegToRad(RotationDeg.m_X) };
                Rot.m_Yaw   = xmath::radian{ xmath::DegToRad(RotationDeg.m_Y) };
                const xmath::fquat Quat(Rot);
                const xmath::fmat4 M(Scale, Quat, Translation);

                for (int f = 0; f < OutFrames; ++f)
                {
                    auto& RootKey      = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot];
                    RootKey.m_Position = M * RootKey.m_Position;
                    RootKey.m_Rotation = Quat * RootKey.m_Rotation;
                    RootKey.m_Scale    = Scale * RootKey.m_Scale;
                }
            };
            if (!Raw.m_bInSkeletonSpace && !pComposed)        // (a retargeted clip is already in this skeleton's space: the retarget put it there; composed frames too: SkeletonFrames did it)
            {
                ApplyToRoot("PostTransform", Manifest.m_PostTransformScale, Manifest.m_PostTransformRotationDeg, Manifest.m_PostTransformTranslation);
                ApplyToRoot("PreTransform", Manifest.m_PreTransformScale, Manifest.m_PreTransformRotationDeg, Manifest.m_PreTransformTranslation);
            }
            // the clip's events (T5): composition ticks (the source's time) -> the compiled clip's seconds; what the trim cuts is dropped, an
            // interval is clipped to the clip; sorted by time (the runtime's crossing walk reads them in order)
            if (pAllEvents && pOv)
            {
                Out.m_iFirstEvent = static_cast<std::uint32_t>(pAllEvents->size());
                const double T0 = double(StartFrame) / FinalFPS, Len = double(OutFrames - 1) / FinalFPS;
                std::vector<xanim_package::event> Ev;
                for (const auto& E : pOv->m_Events)
                {
                    double A = double(E.m_Tick) / 6000.0 - T0, B = E.m_EndTick >= E.m_Tick ? double(E.m_EndTick) / 6000.0 - T0 : A;
                    if (B < 0 || A > Len + 1e-6) { LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': event '{}' is outside the trimmed clip: dropped", CompiledName, E.m_Name)); continue; }
                    A = std::clamp(A, 0.0, Len); B = std::clamp(B, A, Len);
                    xanim_package::event R{};
                    R.m_NameHash = xstrtool::CRC32(E.m_Name); R.m_Kind = E.m_Kind; R.m_Time = float(A); R.m_Duration = E.m_EndTick >= E.m_Tick ? float(B - A) : 0.0f;
                    R.m_iBone = -1;
                    if (!E.m_Bone.empty())
                    {
                        for (int s = 0; s < static_cast<int>(ResolvedBones.size()); ++s) if (ResolvedBones[s].m_Name == E.m_Bone) { R.m_iBone = static_cast<std::int16_t>(s); break; }
                        if (R.m_iBone < 0) LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': event '{}' names bone '{}', which the skeleton does not have", CompiledName, E.m_Name, E.m_Bone));
                    }
                    Ev.push_back(R);
                }
                if (!pOv->m_bMirrorable)      // (the clip's Mirrorable tick off: a "NoMirror" marker the runtime reads, as "NoMatch" - no format change)
                { xanim_package::event R{}; R.m_NameHash = xstrtool::CRC32("NoMirror"); R.m_Kind = xanim_package::event_kind::MARKER; R.m_Time = 0; R.m_Duration = 0; R.m_iBone = -1; Ev.push_back(R); }
                std::stable_sort(Ev.begin(), Ev.end(), [](const auto& a, const auto& b) { return a.m_Time < b.m_Time; });
                Out.m_nEvents = static_cast<std::uint32_t>(Ev.size());
                pAllEvents->insert(pAllEvents->end(), Ev.begin(), Ev.end());
            }

            if (bHaveAnimatedRoot)
            {
                // where the clip puts the root on its first frame, next to where the frozen pose has it: the two agree when the skeleton, its frozen pose and its animations are in the same space
                const auto& Root0 = AllKeyFrames[KeyFrameBase + iSkelRoot].m_Position;
                const auto& Rest  = ResolvedBones[iSkelRoot].m_RestPosition;
                const auto& Rot0  = AllKeyFrames[KeyFrameBase + iSkelRoot].m_Rotation;
                const auto  RestQ = xmath::fquat(ResolvedBones[iSkelRoot].m_RestRotX, ResolvedBones[iSkelRoot].m_RestRotY, ResolvedBones[iSkelRoot].m_RestRotZ, ResolvedBones[iSkelRoot].m_RestRotW);
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the root is at {:.4f} {:.4f} {:.4f} on its first frame, the frozen pose has it at {:.4f} {:.4f} {:.4f}", CompiledName, Root0.m_X, Root0.m_Y, Root0.m_Z, Rest.m_X, Rest.m_Y, Rest.m_Z));
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the root turns (quaternion {:.4f} {:.4f} {:.4f} {:.4f}) on its first frame, the frozen pose has it turned {:.4f} {:.4f} {:.4f} {:.4f}", CompiledName, Rot0.m_X, Rot0.m_Y, Rot0.m_Z, Rot0.m_W, RestQ.m_X, RestQ.m_Y, RestQ.m_Z, RestQ.m_W));
                // the bones where the first frame of the clip is not the rest pose: the angle between the two rotations of the bone (the first few; a bone the clip does not animate is the rest pose)
                {
                    std::string Worst;
                    int nShown = 0;
                    for (int b = 0; b < nSkelBones && nShown < 6; ++b)
                    {
                        const auto& Q  = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(b)].m_Rotation;
                        const xmath::fquat R(ResolvedBones[b].m_RestRotX, ResolvedBones[b].m_RestRotY, ResolvedBones[b].m_RestRotZ, ResolvedBones[b].m_RestRotW);
                        const float Dot = std::min(1.0f, std::abs(R.m_X * Q.m_X + R.m_Y * Q.m_Y + R.m_Z * Q.m_Z + R.m_W * Q.m_W));
                        const float Deg = 2.0f * std::acos(Dot) * 57.29578f;
                        if (Deg > 5.0f) { Worst += std::format(" [{} '{}' {:.1f} deg]", b, ResolvedBones[b].m_Name, Deg); ++nShown; }
                    }
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': bones whose first frame is more than 5 degrees from the frozen pose (first six):{}", CompiledName, Worst.empty() ? std::string(" none") : Worst));
                }
                // how far the root's rotation goes from the first frame over the clip (a clip whose root turns while it plays is not at the frozen pose's orientation after its first frame)
                {
                    float MaxDeg = 0.0f; int MaxFrame = 0; xmath::fquat MaxQ = Rot0;
                    for (int f = 1; f < OutFrames; ++f)
                    {
                        const auto& Q   = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot].m_Rotation;
                        const float Dot = std::min(1.0f, std::abs(Rot0.m_X * Q.m_X + Rot0.m_Y * Q.m_Y + Rot0.m_Z * Q.m_Z + Rot0.m_W * Q.m_W));
                        const float Deg = 2.0f * std::acos(Dot) * 57.29578f;
                        if (Deg > MaxDeg) { MaxDeg = Deg; MaxFrame = f; MaxQ = Q; }
                    }
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': over {} frames the root's rotation goes at most {:.2f} degrees from its first frame (at frame {}, quaternion {:.4f} {:.4f} {:.4f} {:.4f})", CompiledName, OutFrames, MaxDeg, MaxFrame, MaxQ.m_X, MaxQ.m_Y, MaxQ.m_Z, MaxQ.m_W));
                }
            }

            if (RootMotion != xanim_package::root_motion_mode::NONE)
            {
                if (!bHaveAnimatedRoot)
                {
                    LogMessage(xresource_pipeline::msg_type::WARNING
                        , std::format("Clip '{}' asked for root-motion extraction but no root bone (m_iParent==-1) was found - skipping extraction.", CompiledName));
                }
                else
                {
                    const std::size_t RootMotionBase = AllRootMotion.size();
                    AllRootMotion.resize(RootMotionBase + OutFrames);

                    const auto Frame0Pos = AllKeyFrames[KeyFrameBase + 0 * nSkelBones + iSkelRoot].m_Position;

                    for (int f = 0; f < OutFrames; ++f)
                    {
                        auto& RootKey = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot];
                        xmath::fvec3 Delta = RootKey.m_Position - Frame0Pos;

                        if (RootMotion == xanim_package::root_motion_mode::XZ_ONLY)
                        {
                            AllRootMotion[RootMotionBase + f] = xmath::fvec3(Delta.m_X, 0.f, Delta.m_Z);
                            RootKey.m_Position.m_X = Frame0Pos.m_X;
                            RootKey.m_Position.m_Z = Frame0Pos.m_Z;
                        }
                        else // XYZ
                        {
                            AllRootMotion[RootMotionBase + f] = Delta;
                            RootKey.m_Position = Frame0Pos;
                        }
                    }

                    Out.m_LoopDisplacement = AllRootMotion[RootMotionBase + OutFrames - 1];

                    // The travel from the hips (2026-11-17, K2/K4): a rig whose root bone stays while the pelvis walks (retargeted and Kimodo clips) gives
                    // no root motion from the root. Then the hips' horizontal travel is the root motion and the root is moved back by it, so the body
                    // stays over the character's origin (their height, the walk's bob, stays in the pose)
                    float RootTravel = 0;
                    for (int f = 0; f < OutFrames; ++f) RootTravel = std::max(RootTravel, AllRootMotion[RootMotionBase + f].Length());
                    int iHips = -1;
                    if (RootTravel < 1e-3f && OutFrames > 1)
                    {
                        std::vector<std::string_view> Names(static_cast<std::size_t>(nSkelBones)); std::vector<std::int32_t> Par(static_cast<std::size_t>(nSkelBones)); xanimation::pose RestP; RestP.resize(nSkelBones);
                        for (int b = 0; b < nSkelBones; ++b)
                        {
                            Names[b] = ResolvedBones[b].m_Name; Par[b] = ResolvedBones[b].m_iParent;
                            RestP.m_Position[b] = ResolvedBones[b].m_RestPosition; RestP.m_Scale[b] = ResolvedBones[b].m_RestScale;
                            RestP.m_Rotation[b] = xmath::fquat(ResolvedBones[b].m_RestRotX, ResolvedBones[b].m_RestRotY, ResolvedBones[b].m_RestRotZ, ResolvedBones[b].m_RestRotW).NormalizeSafeCopy();
                        }
                        const auto Roles = xanimation::BodyRoles(xanimation::skeleton_view{ Par }, Names, RestP);
                        for (int b = 0; b < nSkelBones && iHips < 0; ++b) if (Roles[b] == std::uint8_t(xanimation::bone_slot::HIPS)) iHips = b;
                        if (iHips >= 0)
                        {
                            std::vector<int> Chain; for (int b = iHips; b >= 0; b = Par[b]) Chain.push_back(b);
                            auto HipsAt = [&](int f)
                            {
                                xmath::fvec3 P(0, 0, 0); xmath::fquat R = xmath::fquat::fromIdentity(); xmath::fvec3 S(1, 1, 1);
                                for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
                                {
                                    const auto& K = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + *It];
                                    P = P + R * xmath::fvec3(S.m_X * K.m_Position.m_X, S.m_Y * K.m_Position.m_Y, S.m_Z * K.m_Position.m_Z);
                                    R = (R * K.m_Rotation).NormalizeSafeCopy(); S = xmath::fvec3(S.m_X * K.m_Scale.m_X, S.m_Y * K.m_Scale.m_Y, S.m_Z * K.m_Scale.m_Z);
                                }
                                return P;
                            };
                            const auto H0 = HipsAt(0);
                            std::vector<xmath::fvec3> Travel(static_cast<std::size_t>(OutFrames));
                            float Far = 0;
                            for (int f = 0; f < OutFrames; ++f) { const auto H = HipsAt(f); Travel[f] = xmath::fvec3(H.m_X - H0.m_X, 0, H.m_Z - H0.m_Z); Far = std::max(Far, Travel[f].Length()); }
                            if (Far > 0.05f)
                            {
                                // (moved back below the root: the root's child on the hips' chain. Moving the root itself left the root bone a
                                // clip's travel away from the character - skin weighted to it stretched there: C70, the 'wrong-way shadow' wedge)
                                const int iBack = Chain.size() >= 2 ? Chain[Chain.size() - 2] : iSkelRoot;
                                // (2026-11-20: the hips' frame-0 offset from the root is taken out too - Kimodo Walk / Jog / Turn clips start with the hips
                                // 0.24-1.37 m off the root, kept for the whole clip: the character turned about a point a metre from its body and every
                                // match into those clips teleported the hips. Root motion stays the travel; the pose moves by travel + that offset.)
                                const auto R0 = AllKeyFrames[KeyFrameBase + iSkelRoot].m_Position;
                                const xmath::fvec3 Off0(H0.m_X - R0.m_X, 0, H0.m_Z - R0.m_Z);
                                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the hips start {:.2f} units off the root (x {:.2f}, z {:.2f}): taken out of the pose", CompiledName, Off0.Length(), Off0.m_X, Off0.m_Z));
                                for (int f = 0; f < OutFrames; ++f)
                                {
                                    AllRootMotion[RootMotionBase + f] = Travel[f];
                                    const auto Mv = Travel[f] + Off0;
                                    auto& K = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iBack];
                                    if (iBack == iSkelRoot) { K.m_Position -= Mv; continue; }
                                    const auto& R = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot];
                                    const auto L = R.m_Rotation.ConjugateCopy() * Mv;      // (into the root's frame; Chain ends at the root)
                                    K.m_Position -= xmath::fvec3(L.m_X / R.m_Scale.m_X, L.m_Y / R.m_Scale.m_Y, L.m_Z / R.m_Scale.m_Z);
                                }
                                Out.m_LoopDisplacement = Travel[OutFrames - 1];
                                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the root bone stays; root motion from the hips ('{}'): {:.2f} units over the clip, the root moved back by it", CompiledName, Names[iHips], Travel[OutFrames - 1].Length()));
                            }
                        }
                    }
                }
            }

            // The root turn (2026-11-17, K4): with root motion the body's facing turn about +Y (the hips' turn applied to the rest's forward, from the
            // body roles' legs) is taken out of the pose too - the root key turned back about the origin, where the hips now stand - and stored per
            // frame next to the root motion, so a turning clip turns the entity. A clip that keeps its facing (within 2 deg) stores zeros.
            if (RootMotion != xanim_package::root_motion_mode::NONE && bHaveAnimatedRoot)
            {
                const std::size_t Base = Out.m_iFirstRootMotion;
                if (m_RootTurn.size() < Base) m_RootTurn.resize(Base, 0.0f);
                m_RootTurn.resize(Base + static_cast<std::size_t>(OutFrames), 0.0f);
                std::vector<std::string_view> Names(static_cast<std::size_t>(nSkelBones)); std::vector<std::int32_t> Par(static_cast<std::size_t>(nSkelBones)); xanimation::pose RestP; RestP.resize(nSkelBones);
                for (int b = 0; b < nSkelBones; ++b)
                {
                    Names[b] = ResolvedBones[b].m_Name; Par[b] = ResolvedBones[b].m_iParent;
                    RestP.m_Position[b] = ResolvedBones[b].m_RestPosition; RestP.m_Scale[b] = ResolvedBones[b].m_RestScale;
                    RestP.m_Rotation[b] = xmath::fquat(ResolvedBones[b].m_RestRotX, ResolvedBones[b].m_RestRotY, ResolvedBones[b].m_RestRotZ, ResolvedBones[b].m_RestRotW).NormalizeSafeCopy();
                }
                const xanimation::skeleton_view SV{ Par };
                const auto Roles = xanimation::BodyRoles(SV, Names, RestP);
                const auto Rig = xanimation::FindWarpRig(SV, Roles);
                if (Rig.valid() && Rig.m_Legs.size() >= 2)
                {
                    xanimation::details::rigid_worlds W; xanimation::details::Worlds(SV, RestP, W);
                    auto Side = W.m_P[Rig.m_Legs.front().m_Thigh] - W.m_P[Rig.m_Legs.back().m_Thigh]; Side.m_Y = 0;
                    const auto Fwd = Side.Cross(xmath::fvec3(0, 1, 0)).NormalizeSafeCopy();
                    const auto RestHipsInv = W.m_R[Rig.m_Hips].ConjugateCopy();
                    xanimation::pose P; P.resize(nSkelBones);
                    int Arm0 = -1, Arm1 = -1;          // (the upper arms, Arm0 on the side of the first leg; none: the hips alone)
                    for (int b = 0; b < nSkelBones; ++b) if (Roles[static_cast<std::size_t>(b)] == 7) { if (Arm0 < 0) Arm0 = b; else if (Arm1 < 0) Arm1 = b; }
                    if (Arm1 < 0) Arm0 = -1;
                    else if ((W.m_P[Arm0] - W.m_P[Arm1]).Dot(Side) < 0) std::swap(Arm0, Arm1);
                    std::vector<float> Yaw(static_cast<std::size_t>(OutFrames));
                    float Prev = 0, Acc = 0, Far = 0;
                    for (int f = 0; f < OutFrames; ++f)
                    {
                        for (int b = 0; b < nSkelBones; ++b) P.set(b, AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + b]);
                        xanimation::details::Worlds(SV, P, W);
                        // the body's heading from the hips' and the shoulders' lines together (Unity's body orientation): the hips alone twist
                        // with the gait (a side-step opens them toward the travel)
                        auto F = (W.m_R[Rig.m_Hips] * RestHipsInv) * Fwd; F.m_Y = 0;
                        if (Arm0 >= 0)
                        {
                            auto S1 = W.m_P[Rig.m_Legs.front().m_Thigh] - W.m_P[Rig.m_Legs.back().m_Thigh], S2 = W.m_P[Arm0] - W.m_P[Arm1]; S1.m_Y = 0; S2.m_Y = 0;
                            const auto S = S1.NormalizeSafeCopy() + S2.NormalizeSafeCopy();
                            if (S.LengthSq() > 1e-6f) F = S.Cross(xmath::fvec3(0, 1, 0));
                        }
                        const float A = std::atan2(F.m_X, F.m_Z);
                        if (f == 0) Prev = A;
                        float d = A - Prev; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;     // (unwrapped)
                        Acc += d; Prev = A; Yaw[static_cast<std::size_t>(f)] = Acc;
                    }
                    // the facing is the hips' heading without the gait's sway: a centred average over a stride (1 s); past the clip's ends the
                    // heading goes on along a line fitted to its first / last stride (a steady turn stays exact), then the whole is shifted so frame
                    // 0 faces 0 (the entity faces the smoothed heading; the pose keeps its hips' twist). Raw, a side-step's hips swing 12-42 deg
                    // every step: the entity wiggled with them and motion matching read the swing as facing (AnimDemo's strafer kept Stand)
                    {
                        const int H = std::max(1, int(std::lround(0.5 * double(FinalFPS)))), N = OutFrames;
                        auto Line = [&](int a, int b) { double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;          // least squares over frames a..b
                            for (int k = a; k <= b; ++k) { const double y = Yaw[static_cast<std::size_t>(k)]; n += 1; sx += k; sy += y; sxx += double(k) * k; sxy += k * y; }
                            const double d = n * sxx - sx * sx, m = d > 1e-9 ? (n * sxy - sx * sy) / d : 0.0; return std::pair{ m, (sy - m * sx) / n }; };
                        const auto L0 = Line(0, std::min(N - 1, 2 * H)), L1 = Line(std::max(0, N - 1 - 2 * H), N - 1);
                        // a loop goes round instead (frame N is frame 0 one pass on: the seam's pose twist then matches - a line there left the
                        // TurnLeft loop's hips 15-18 m/s off at every wrap)
                        auto At = [&](int k) {
                            if (bLoop) { const int q = int(std::floor(double(k) / N)); return double(Yaw[static_cast<std::size_t>(k - q * N)]) + q * double(Yaw[static_cast<std::size_t>(N - 1)]); }
                            return k < 0 ? L0.first * k + L0.second : k >= N ? L1.first * k + L1.second : double(Yaw[static_cast<std::size_t>(k)]); };
                        std::vector<float> Sm(Yaw.size());
                        for (int f = 0; f < N; ++f) { double S = 0; for (int k = f - H; k <= f + H; ++k) S += At(k); Sm[static_cast<std::size_t>(f)] = float(S / double(2 * H + 1)); }
                        const float Zero = Sm.empty() ? 0.0f : Sm.front();
                        double Sq = 0; float Worst = 0;             // (data check: how far the body's heading swings around its facing)
                        for (int f = 0; f < N; ++f) { const float d = Yaw[static_cast<std::size_t>(f)] - Sm[static_cast<std::size_t>(f)]; Sq += double(d) * d; Worst = std::max(Worst, std::abs(d)); }
                        const float Rms = float(std::sqrt(Sq / std::max(1, N))) * 57.29578f; Worst *= 57.29578f;
                        LogMessage(Worst > 20.0f ? xresource_pipeline::msg_type::WARNING : xresource_pipeline::msg_type::INFO
                            , std::format("Clip '{}': the body's heading swings up to {:.1f} deg (rms {:.1f}) around its facing{}", CompiledName, Worst, Rms
                            , Worst > 20.0f ? " - more than a gait's sway: motion matching reads its facing poorly (a strafe or a turn generated without a path/heading constraint?)" : ""));
                        for (auto& y : Sm) y -= Zero;
                        Yaw.swap(Sm);
                        for (float y : Yaw) Far = std::max(Far, std::abs(y));
                    }
                    if (Far > 0.035f)
                    {
                        for (int f = 0; f < OutFrames; ++f)
                        {
                            const float T = Yaw[static_cast<std::size_t>(f)];
                            m_RootTurn[Base + static_cast<std::size_t>(f)] = T;
                            const xmath::fquat Q(0.0f, std::sin(-0.5f * T), 0.0f, std::cos(-0.5f * T));
                            auto& K = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot];
                            K.m_Rotation = (Q * K.m_Rotation).NormalizeSafeCopy();
                            K.m_Position = Q * K.m_Position;
                        }
                        LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': the root turn taken out of the pose: {:.1f} deg over the clip", CompiledName, Yaw.back() * 57.29578f));
                    }
                }
            }

            // C127: the seam closed in the pose (the root motion is out of it now), then the last frame - the next pass's first - dropped: the loop
            // is frames 0..N-1 and the wrap N-1 -> 0 a real step (its travel and turn the pass's: m_LoopDisplacement, m_LoopTurn at that frame)
            // C127 (Tomas 2026-11-23: a clip marked Loop loops cleanly by itself unless it already does; the loop-baking method's phase 1). Every
            // Loop clip gets a verdict in this log, after the root motion is out of the pose:
            //  ALREADY VALID - its first and last frames match (pose; the velocity step no larger than its own; the feet's plant): the last frame
            //    is the next pass's first and is dropped;
            //  REPAIRED - of the candidate cycles (xanimation::FindLoopSeams: up to three one-stride support cycles and a two-stride one; an
            //    in-place clip, its feet planted throughout: the idle path, D = I), the least-edited one whose closure (CloseLoopSeam, 0.15 s)
            //    stays inside every bone's budget (max(its body part's, 12 % of its range of motion in the clip)) and passes the gates
            //    (GateLoop: planted-toe drift, skate, acceleration) is cut out and closed; m_LoopDisplacement / m_LoopTurn are the root's
            //    travel and turn at its end (the next pass's first, dropped); the clip's events are carried by phase;
            //  REFUSED - with every candidate's reason: the clip compiles as it did (its last frame taken as the next pass's first).
            if (bLoop)
            {
                std::vector<std::string_view> Names(static_cast<std::size_t>(nSkelBones)); std::vector<std::int32_t> Par(static_cast<std::size_t>(nSkelBones)); xanimation::pose RestP; RestP.resize(nSkelBones);
                for (int b = 0; b < nSkelBones; ++b)
                {
                    Names[b] = ResolvedBones[b].m_Name; Par[b] = ResolvedBones[b].m_iParent;
                    RestP.m_Position[b] = ResolvedBones[b].m_RestPosition; RestP.m_Scale[b] = ResolvedBones[b].m_RestScale;
                    RestP.m_Rotation[b] = xmath::fquat(ResolvedBones[b].m_RestRotX, ResolvedBones[b].m_RestRotY, ResolvedBones[b].m_RestRotZ, ResolvedBones[b].m_RestRotW).NormalizeSafeCopy();
                }
                const xanimation::skeleton_view SV{ Par };
                const auto Roles = xanimation::BodyRoles(SV, Names, RestP);
                const auto Rig = xanimation::FindWarpRig(SV, Roles);
                const std::size_t RB = Out.m_iFirstRootMotion;
                const bool bRM = AllRootMotion.size() >= RB + static_cast<std::size_t>(OutFrames), bRT = m_RootTurn.size() >= RB + static_cast<std::size_t>(OutFrames);
                auto RMf = [&](int f) { return bRM ? AllRootMotion[RB + static_cast<std::size_t>(f)] : xmath::fvec3(0, 0, 0); };
                auto RTf = [&](int f) { return bRT ? m_RootTurn[RB + static_cast<std::size_t>(f)] : 0.0f; };
                const std::span<xmath::transform3> Keys(AllKeyFrames.data() + KeyFrameBase, static_cast<std::size_t>(OutFrames) * nSkelBones);
                const auto Rom = xanimation::LoopRangeOfMotionDeg(Keys, nSkelBones);
                std::vector<float> Budget(static_cast<std::size_t>(nSkelBones));
                for (int b = 0; b < nSkelBones; ++b) Budget[static_cast<std::size_t>(b)] = xanimation::LoopBudgetDeg(Roles[static_cast<std::size_t>(b)], Rom[static_cast<std::size_t>(b)]);
                // experiment knobs (C127 phase 2, Fable's call before they become rules): the pelvis budget (fraction of the height) and the coupled
                // solve's half window (seconds); defaults = the contract's 1 % and 0.105 s
                static const float s_Pelvis = [] { const char* e = xanimation::Env("XLION_LOOP_PELVIS"); return e ? float(std::atof(e)) : 0.01f; }();
                static const float s_SolveWin = [] { const char* e = xanimation::Env("XLION_LOOP_SOLVE_S"); return e ? float(std::atof(e)) : 0.105f; }();
                {
                    static bool s_Said = false;
                    std::string Knobs; for (const char* k : { "XLION_LOOP_PELVIS", "XLION_LOOP_SOLVE_S", "XLION_LOOP_WINDOW", "XLION_LOOP_LOCK", "XLION_LOOP_LOCK_FLAT", "XLION_LOOP_VELW" }) if (const char* e = xanimation::Env(k)) Knobs += std::format(" {}={}", k, e);
                    if (!Knobs.empty() && !s_Said) { s_Said = true; LogMessage(xresource_pipeline::msg_type::WARNING, "Loop baker experiment knobs set (not part of the compiler's version - recompile without them before shipping):" + Knobs); }
                }
                std::vector<xmath::fvec3> RMv(static_cast<std::size_t>(OutFrames)); std::vector<float> RTv(static_cast<std::size_t>(OutFrames));
                for (int f = 0; f < OutFrames; ++f) { RMv[static_cast<std::size_t>(f)] = RMf(f); RTv[static_cast<std::size_t>(f)] = RTf(f); }
                // a clip without root motion passes no track (a walk in place is then read against its belt, N1)
                const std::span<const xmath::fvec3> RMs = (bRM || bRT) ? std::span<const xmath::fvec3>(RMv) : std::span<const xmath::fvec3>{};
                const std::span<const float> RTs = (bRM || bRT) ? std::span<const float>(RTv) : std::span<const float>{};
                const auto Now = xanimation::MeasureLoopSeam(SV, Rig, RestP, Keys, nSkelBones, FinalFPS, RMs, RTs);
                Out.m_LoopTurn = RTf(OutFrames - 1);            // (as it was: the last frame is the next pass's first)
                int Bake = 0, First = 0;
                // already valid: the seam's pose within 0.6 % of the body's height, its velocity steps (the body's, the bones', the root's travel
                // and turn under D) no larger than one frame of the clip's own, the feet's plant equal
                const bool bValidNow = Now.m_bValid && Now.m_PosError < 0.006f * Now.m_H && Now.m_VelError <= Now.m_VelInside && Now.m_AngVelError <= Now.m_AngVelInside
                                     && Now.m_RootVelError <= Now.m_RootVelInside + 1e-4f * Now.m_H && Now.m_RootTurnError <= Now.m_RootTurnInside + 1e-4f && Now.m_bContacts;
                auto Row = [&](const char* pVerdict, const std::string& What) { m_LoopReport += std::format("{}\t{}\t{}\t{:.1f}\t{:.0f}\t{}\n", CompiledName, pVerdict, OutFrames, Now.m_PosError * 1000.0f, Now.m_AngVelError, What); };
                if (!Now.m_bValid)
                {
                    LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': Loop: REFUSED - {}: it compiles as it is (its last frame taken as the next pass's first)", CompiledName, OutFrames < 3 ? "fewer than 3 frames" : "no legs to find its cycle by"));
                    Row("REFUSED", OutFrames < 3 ? "fewer than 3 frames" : "no legs");
                }
                else if (bValidNow)
                {
                    Bake = 1; Row("ALREADY VALID", std::format("frames 0..{} kept, the last dropped; root {:.3f} units/s, {:.0f} deg/s at the seam", OutFrames - 2, Now.m_RootVelError, Now.m_RootTurnError * 57.29578f));
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': Loop: ALREADY VALID (seam {:.1f} mm, {:.0f} deg-per-s against its own {:.0f}, root {:.3f} units-per-s / {:.0f} deg-per-s): frames 0..{} (its last, the next pass's first, dropped)"
                        , CompiledName, Now.m_PosError * 1000.0f, Now.m_AngVelError, Now.m_AngVelInside, Now.m_RootVelError, Now.m_RootTurnError * 57.29578f, OutFrames - 2));
                }
                else
                {
                    // the candidates: the artist's whole range first (kept when it passes: no silent shortening), then the found cycles
                    xanimation::loop_seam Whole = Now; Whole.m_First = 0; Whole.m_End = OutFrames - 1;
                    auto Found = xanimation::FindLoopSeams(SV, Rig, RestP, Keys, nSkelBones, FinalFPS, false, 0.48f, 1.55f, RMs, RTs, 20);      // (20 diverse cuts, ranked by the repair's result)
                    const bool bIdle = Found.empty() && xanimation::FeetPlantedThroughout(SV, Rig, RestP, Keys, nSkelBones, FinalFPS, RMs, RTs);
                    if (bIdle) Found = xanimation::FindLoopSeams(SV, Rig, RestP, Keys, nSkelBones, FinalFPS, true, 1.0f, 1.55f, RMs, RTs, 20);
                    std::vector<xanimation::loop_seam> Cands{ Whole };
                    for (const auto& c : Found) if (c.m_First != 0 || c.m_End != OutFrames - 1) Cands.push_back(c);
                    std::string Why; int iBest = -1; float BestRatio = 1e30f;
                    std::vector<xmath::transform3> BestKeys; std::vector<xmath::fvec3> BestRM; std::vector<float> BestRT; xanimation::loop_edit BestEdit; xanimation::loop_gate BestGate; bool BestMarginal = false; float BestWin = 0;
                    for (int c = 0; c < static_cast<int>(Cands.size()); ++c)
                    {
                        const auto& Cd = Cands[static_cast<std::size_t>(c)];
                        const int n = Cd.m_End - Cd.m_First;
                        const std::span<const xmath::transform3> Src(Keys.data() + static_cast<std::size_t>(Cd.m_First) * nSkelBones, static_cast<std::size_t>(n + 1) * nSkelBones);
                        // the cycle's root motion from its first frame (in its frame); the idle path: D = I (the drift over the cycle taken out evenly)
                        std::vector<xmath::fvec3> Rs(static_cast<std::size_t>(n + 1)); std::vector<float> Ts(static_cast<std::size_t>(n + 1));
                        const float T0 = RTf(Cd.m_First), c0 = std::cos(-T0), s0 = std::sin(-T0);
                        for (int f = 0; f <= n; ++f)
                        {
                            const auto d = RMf(Cd.m_First + f) - RMf(Cd.m_First);
                            Rs[static_cast<std::size_t>(f)] = xmath::fvec3(d.m_X * c0 + d.m_Z * s0, d.m_Y, -d.m_X * s0 + d.m_Z * c0); Ts[static_cast<std::size_t>(f)] = RTf(Cd.m_First + f) - T0;
                        }
                        if (bIdle) for (int f = 0; f <= n; ++f) { const float u = float(f) / float(n); Rs[static_cast<std::size_t>(f)] = Rs[static_cast<std::size_t>(f)] - Rs[static_cast<std::size_t>(n)] * u; Ts[static_cast<std::size_t>(f)] -= Ts[static_cast<std::size_t>(n)] * u; }
                        const bool bTracks = bRM || bRT;
                        auto Sp3 = [&](const std::vector<xmath::fvec3>& v) { return bTracks ? std::span<const xmath::fvec3>(v) : std::span<const xmath::fvec3>{}; };
                        auto Sp1 = [&](const std::vector<float>& v) { return bTracks ? std::span<const float>(v) : std::span<const float>{}; };
                        // one repair of the cut with a closure window: closure (two-sided) and foot lock alternated, ending on the closure; when that
                        // fails, the coupled solve (the best few cuts by seam score); judged by the edits and the gates
                        struct attempt { std::vector<xmath::transform3> m_Keys; std::vector<xmath::fvec3> m_RM; std::vector<float> m_RT; xanimation::loop_edit m_E; xanimation::loop_gate m_G;
                                         bool m_bPelvis = false, m_bReach = false, m_bRoot = false, m_bOk = false, m_bMarginal = false; float m_Win = 0, m_Ratio = 1e30f, m_SeamPos = 0, m_SeamVel = 0, m_SeamAng = 0; std::string m_Why; };
                        auto Try = [&](float Win)
                        {
                            attempt A; A.m_Win = Win; A.m_Keys.assign(Src.begin(), Src.end()); A.m_RM = Rs; A.m_RT = Ts;
                            xanimation::CloseLoopTrackTwoSided(A.m_RM, A.m_RT, FinalFPS, Win);
                            float RootMove = 0, RootTurn = 0;
                            for (int f = 0; f <= n; ++f) { RootMove = std::max(RootMove, (A.m_RM[static_cast<std::size_t>(f)] - Rs[static_cast<std::size_t>(f)]).Length()); RootTurn = std::max(RootTurn, std::fabs(A.m_RT[static_cast<std::size_t>(f)] - Ts[static_cast<std::size_t>(f)])); }
                            xanimation::loop_lock Lk;
                            static const int s_Lock = [] { const char* e = xanimation::Env("XLION_LOOP_LOCK"); return e ? std::atoi(e) : 1; }();      // (diagnosis: 0 = no foot lock)
                            for (int it = 0; it < 3; ++it) { xanimation::CloseLoopSeamTwoSided(A.m_Keys, nSkelBones, FinalFPS, Win, {}, &SV, Rig.m_Hips); if (!s_Lock) break; Lk = xanimation::LockLoopFeet(SV, Rig, RestP, Src, A.m_Keys, nSkelBones, FinalFPS, Sp3(Rs), Sp1(Ts), Sp3(A.m_RM), Sp1(A.m_RT), Win); }
                            xanimation::CloseLoopSeamTwoSided(A.m_Keys, nSkelBones, FinalFPS, Win, {}, &SV, Rig.m_Hips);
                            A.m_E = xanimation::LoopEditAgainstSource(SV, Rig.m_Hips, Src, A.m_Keys, nSkelBones, Budget);
                            A.m_bPelvis = A.m_E.m_HipsPos > s_Pelvis * Now.m_H; A.m_bReach = Lk.m_Miss > 0.001f * Now.m_H; A.m_bRoot = RootMove > 0.01f * Now.m_H || RootTurn > 0.035f;
                            // REPAIRED (MARGINAL): exactly one budget past by at most 20 % and everything else passing (Fable 2026-11-23)
                            auto Marginal = [&](const xanimation::loop_edit& E) { return E.m_bOver && E.m_nOver == 1 && E.m_Deg <= 1.2f * E.m_Budget; };
                            const bool bEditOk = !A.m_E.m_bOver || Marginal(A.m_E);
                            A.m_G = (!bEditOk || A.m_bPelvis || A.m_bReach || A.m_bRoot) ? xanimation::loop_gate{} : xanimation::GateLoop(SV, Rig, RestP, Src, A.m_Keys, Keys, nSkelBones, FinalFPS, Sp3(A.m_RM), Sp1(A.m_RT), Sp3(Rs), Sp1(Ts), Win);
                            std::string Coupled;
                            if ((!bEditOk || A.m_E.m_bOver || A.m_bPelvis || A.m_bReach || !A.m_G.m_bPass) && !A.m_bRoot && !bIdle && c <= 3)
                            {
                                std::vector<xmath::transform3> C2(Src.begin(), Src.end());
                                xanimation::CloseLoopSeamTwoSided(C2, nSkelBones, FinalFPS, Win, {}, &SV, Rig.m_Hips);
                                for (std::size_t f = 0; f < C2.size() / static_cast<std::size_t>(nSkelBones); ++f)          // (the legs back to the source: the solve owns them)
                                    for (const auto& Lg : Rig.m_Legs) for (int bb : { Lg.m_Thigh, Lg.m_Calf })
                                        C2[f * nSkelBones + static_cast<std::size_t>(bb)].m_Rotation = Src[f * nSkelBones + static_cast<std::size_t>(bb)].m_Rotation;
                                const auto Sol = xanimation::CoupledLoopSolve(SV, Rig, RestP, Src, C2, nSkelBones, FinalFPS, Sp3(Rs), Sp1(Ts), Sp3(A.m_RM), Sp1(A.m_RT), 1.4f * s_Pelvis, s_SolveWin);
                                xanimation::CloseLoopSeamTwoSided(C2, nSkelBones, FinalFPS, Win, {}, &SV, Rig.m_Hips);     // (the seam tied again after the solve: every bone, the ankle too - review 3, H1/H2)
                                const auto E2 = xanimation::LoopEditAgainstSource(SV, Rig.m_Hips, Src, C2, nSkelBones, Budget);
                                const bool bP2 = E2.m_HipsPos > s_Pelvis * Now.m_H, bE2 = !E2.m_bOver || Marginal(E2);
                                const auto G2 = (Sol.m_bBound || !bE2 || bP2) ? xanimation::loop_gate{} : xanimation::GateLoop(SV, Rig, RestP, Src, C2, Keys, nSkelBones, FinalFPS, Sp3(A.m_RM), Sp1(A.m_RT), Sp3(Rs), Sp1(Ts), std::max(Win, s_SolveWin));
                                const std::string B2 = E2.m_Bone >= 0 ? ResolvedBones[static_cast<std::size_t>(E2.m_Bone)].m_Name : std::string("-");
                                Coupled = std::format(" [coupled solve: {} iterations, contact {:.1f} mm, pelvis {:.1f} cm, edit {:.1f} deg at '{}' (budget {:.1f}): {}]", Sol.m_Iter, Sol.m_Contact * 1000.0f, Sol.m_Pelvis * 100.0f, E2.m_Deg, B2, E2.m_Budget
                                    , Sol.m_bBound ? "it ends on a bound" : !bE2 ? "an edit past its budget" : bP2 ? "the pelvis past its budget" : G2.m_bPass ? "gates pass" : G2.m_pWhy);
                                const float R1 = A.m_E.m_Deg / std::max(A.m_E.m_Budget, 1e-3f), R2 = E2.m_Deg / std::max(E2.m_Budget, 1e-3f);
                                const bool bFirstOk = bEditOk && !A.m_bPelvis && !A.m_bReach && A.m_G.m_bPass;
                                if (!Sol.m_bBound && bE2 && !bP2 && G2.m_bPass && (!bFirstOk || R2 < R1)) { A.m_Keys.swap(C2); A.m_E = E2; A.m_G = G2; A.m_bPelvis = false; A.m_bReach = false; }
                            }
                            A.m_bMarginal = A.m_E.m_bOver;
                            {
                                std::vector<xmath::transform3> Play(A.m_Keys.begin(), A.m_Keys.end() - std::ptrdiff_t(nSkelBones));
                                Play.insert(Play.end(), A.m_Keys.begin(), A.m_Keys.begin() + std::ptrdiff_t(nSkelBones));          // (as played: N-1 -> the next pass's 0)
                                const auto M = xanimation::MeasureLoopSeam(SV, Rig, RestP, Play, nSkelBones, FinalFPS, Sp3(A.m_RM), Sp1(A.m_RT));
                                A.m_SeamPos = M.m_PosError; A.m_SeamVel = M.m_VelError; A.m_SeamAng = M.m_AngVelError;
                                if (M.m_PosError > 1e-4f * Now.m_H || M.m_VelError > M.m_VelInside || M.m_AngVelError > M.m_AngVelInside) /* (the wrap's step change no larger than one frame of the clip's own, p95) */ A.m_G.m_bPass = false, A.m_G.m_pWhy = "the seam is not closed (measured on the result)";
                            }
                            A.m_bOk = (!A.m_E.m_bOver || Marginal(A.m_E)) && !A.m_bPelvis && !A.m_bReach && !A.m_bRoot && A.m_G.m_bPass;
                            A.m_Ratio = A.m_E.m_Deg / std::max(A.m_E.m_Budget, 1e-3f);
                            const std::string Bone = A.m_E.m_Bone >= 0 ? ResolvedBones[static_cast<std::size_t>(A.m_E.m_Bone)].m_Name : std::string("-");
                            const float Base = A.m_E.m_Bone >= 0 ? xanimation::LoopEditBudgetDeg(Roles[static_cast<std::size_t>(A.m_E.m_Bone)]) : 0.0f, Range = A.m_E.m_Bone >= 0 ? Rom[static_cast<std::size_t>(A.m_E.m_Bone)] : 0.0f;
                            const std::string Edit = std::format("edit {:.1f} deg at '{}' (budget {:.1f} = max({:.0f}, 12 % of its {:.0f} deg range){})", A.m_E.m_Deg, Bone, A.m_E.m_Budget, Base, Range
                                , A.m_E.m_bOver ? std::format(", {} bone(s) over, by {:.0f} %", A.m_E.m_nOver, 100.0f * (A.m_E.m_Deg / A.m_E.m_Budget - 1.0f)) : std::string());
                            A.m_Why = std::format("window {:.2f} s: {}", Win
                                , A.m_bOk ? Edit + (A.m_bMarginal ? ", MARGINAL" : "") + ", gates pass"
                                : (A.m_E.m_bOver && !Marginal(A.m_E)) ? "an " + Edit + " past it" : A.m_bReach ? std::format("a planted foot cannot reach its anchor ({:.1f} mm short)", Lk.m_Miss * 1000.0f)
                                : A.m_bRoot ? std::format("closing the root's path moves it {:.1f} cm / {:.1f} deg, past 1 % of the height / 2 deg", RootMove * 100.0f, RootTurn * 57.29578f)
                                : A.m_bPelvis ? std::format("the pelvis moved {:.1f} cm, past {:.1f} % of the height ({:.1f})", A.m_E.m_HipsPos * 100.0f, s_Pelvis * 100.0f, Now.m_H)
                                : std::string(A.m_G.m_pWhy) + std::format(" (toe drift {:.1f} mm, slide +{:.1f} mm, peak skate {:.3f} -> {:.3f}, step change {:.2f} / own p99 {:.2f})", A.m_G.m_ToeDrift * 1000.0f, A.m_G.m_Slide * 1000.0f, A.m_G.m_SkateBefore, A.m_G.m_SkateAfter, A.m_G.m_Acc, A.m_G.m_AccP99))
                                + std::format(" [as played: seam {:.2f} mm, {:.3f} units/s, {:.1f} deg/s] [lock pull {:.1f} mm]", A.m_SeamPos * 1000.0f, A.m_SeamVel, A.m_SeamAng, Lk.m_Pull * 1000.0f) + Coupled;
                            return A;
                        };
                        // the windows tried (each side of the seam), the best by final quality: a passing one first, then the smallest edit for its budget
                        static const float s_WinKnob = [] { const char* e = xanimation::Env("XLION_LOOP_WINDOW"); return e ? float(std::atof(e)) : 0.0f; }();
                        std::vector<float> Wins = s_WinKnob > 0 ? std::vector<float>{ s_WinKnob } : std::vector<float>{ 0.15f, 0.3f, 0.5f };
                        attempt Best; bool bHave = false; std::string Tried;
                        for (const float Win : Wins)
                        {
                            auto A = Try(Win);
                            Tried += (Tried.empty() ? "" : " | ") + A.m_Why;
                            if (!bHave || (A.m_bOk && !Best.m_bOk) || (A.m_bOk == Best.m_bOk && A.m_Ratio < Best.m_Ratio)) { Best = std::move(A); bHave = true; }
                        }
                        Why += std::format("{} {} {}..{} ({:.2f} s): {}", Why.empty() ? "" : ";", c == 0 ? "the whole range" : "cut", Cd.m_First, Cd.m_End, float(n) / FinalFPS, Tried);
                        // the pick: the whole range when it passes; an in-place clip (an idle) then the LONGEST passing cut - its performance (breath,
                        // weight shifts) kept, a shorter one only when every longer one fails; a moving clip the least edited one (a MARGINAL one only
                        // when no cut passes cleanly)
                        const float Rank = (Best.m_bMarginal ? 1e6f : 0.0f) + (bIdle ? -float(n) : Best.m_Ratio);
                        // (an in-place cut shorter than half the clip is not taken: it would throw away most of the performance - Fable 2026-11-23)
                        if (bIdle && 2 * n < OutFrames - 1) { Why += " (shorter than half the clip: not taken for an in-place clip)"; continue; }
                        if (!Best.m_bOk || Rank >= BestRatio) continue;
                        BestRatio = Rank; iBest = c; BestKeys.swap(Best.m_Keys); BestRM.swap(Best.m_RM); BestRT.swap(Best.m_RT); BestEdit = Best.m_E; BestGate = Best.m_G; BestMarginal = Best.m_bMarginal; BestWin = Best.m_Win;
                        if (c == 0 && !Best.m_bMarginal) break;                  // (the artist's range passes: it is kept)
                    }
                    if (iBest < 0) Row("REFUSED", Why.empty() ? std::string("no full support-to-support cycle") : Why);
                    if (iBest < 0)
                        LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': Loop: REFUSED - seam {:.1f} mm / {:.0f} deg-per-s as cut, and no candidate passes:{} - it compiles as it is", CompiledName, Now.m_PosError * 1000.0f, Now.m_AngVelError, Why.empty() ? std::string(" no full support-to-support cycle (a stationary hold, or a foot that never lifts and replants)") : " " + Why));
                    else
                    {
                        Bake = 2;
                        const auto& Cd = Cands[static_cast<std::size_t>(iBest)];
                        const int n = Cd.m_End - Cd.m_First, OldFrames = OutFrames, OldStart = StartFrame;
                        AllKeyFrames.resize(KeyFrameBase + static_cast<std::size_t>(n + 1) * nSkelBones);
                        std::copy(BestKeys.begin(), BestKeys.end(), AllKeyFrames.begin() + static_cast<std::ptrdiff_t>(KeyFrameBase));
                        if (bRM) { AllRootMotion.resize(RB + static_cast<std::size_t>(n + 1)); std::copy(BestRM.begin(), BestRM.end(), AllRootMotion.begin() + static_cast<std::ptrdiff_t>(RB)); }
                        if (bRT) { m_RootTurn.resize(RB + static_cast<std::size_t>(n + 1)); std::copy(BestRT.begin(), BestRT.end(), m_RootTurn.begin() + static_cast<std::ptrdiff_t>(RB)); }
                        Out.m_LoopDisplacement = bRM ? BestRM.back() : xmath::fvec3::fromZero(); Out.m_LoopTurn = BestRT.back();
                        First = Cd.m_First;
                        Row(BestMarginal ? (bIdle ? "REPAIRED (MARGINAL, in place)" : "REPAIRED (MARGINAL)") : bIdle ? "REPAIRED (in place)" : "REPAIRED", std::format("source frames {}..{} of {} -> {} frames, T {:.3f} s; D {:.3f} units, {:.1f} deg; seam after {:.1f} mm; edit {:.1f} deg at {} (budget {:.1f}); toe drift {:.1f} mm; slide +{:.1f} mm; peak skate {:.3f} -> {:.3f}"
                            , OldStart + Cd.m_First, OldStart + Cd.m_End, OldFrames, n, float(n) / FinalFPS, std::sqrt(BestRM.back().m_X * BestRM.back().m_X + BestRM.back().m_Z * BestRM.back().m_Z), BestRT.back() * 57.29578f
                            , xanimation::MeasureLoopSeam(SV, Rig, RestP, BestKeys, nSkelBones, FinalFPS, bRM || bRT ? std::span<const xmath::fvec3>(BestRM) : std::span<const xmath::fvec3>{}, bRM || bRT ? std::span<const float>(BestRT) : std::span<const float>{}).m_PosError * 1000.0f
                            , BestEdit.m_Deg, BestEdit.m_Bone >= 0 ? ResolvedBones[static_cast<std::size_t>(BestEdit.m_Bone)].m_Name : std::string("-"), BestEdit.m_Budget, BestGate.m_ToeDrift * 1000.0f, BestGate.m_Slide * 1000.0f, BestGate.m_SkateBefore, BestGate.m_SkateAfter));
                        StartFrame = OldStart + Cd.m_First; OutFrames = n + 1; Out.m_nFrames = OutFrames;
                        LogMessage(BestMarginal ? xresource_pipeline::msg_type::WARNING : xresource_pipeline::msg_type::INFO, std::format("Clip '{}': Loop: REPAIRED{}{} - seam {:.1f} mm / {:.0f} deg-per-s as cut; the cycle: frames {}..{} of the clip's {} (now {} frames, T {:.2f} s), D: travel {:.3f}, turn {:.1f} deg; "
                            "left to close {:.1f} mm / {:.0f} deg-per-s; {}; toe drift {:.1f} mm, skate {:.3f} -> {:.3f}; candidates: {}"
                            , CompiledName, BestMarginal ? std::format(" (MARGINAL: '{}' {:.1f} deg past its {:.1f} - an artist should look)", BestEdit.m_Bone >= 0 ? ResolvedBones[static_cast<std::size_t>(BestEdit.m_Bone)].m_Name : std::string("-"), BestEdit.m_Deg, BestEdit.m_Budget) : std::string(), bIdle ? " (in place, D = I)" : "", Now.m_PosError * 1000.0f, Now.m_AngVelError, OldStart + Cd.m_First, OldStart + Cd.m_End, OldFrames, n, float(n) / FinalFPS
                            , std::sqrt(Out.m_LoopDisplacement.m_X * Out.m_LoopDisplacement.m_X + Out.m_LoopDisplacement.m_Z * Out.m_LoopDisplacement.m_Z), Out.m_LoopTurn * 57.29578f
                            , Cd.m_PosError * 1000.0f, Cd.m_AngVelError, std::format("the largest edit {:.1f} deg at '{}' (budget {:.1f})", BestEdit.m_Deg, BestEdit.m_Bone >= 0 ? ResolvedBones[static_cast<std::size_t>(BestEdit.m_Bone)].m_Name : std::string("-"), BestEdit.m_Budget)
                            , BestGate.m_ToeDrift * 1000.0f, BestGate.m_SkateBefore, BestGate.m_SkateAfter, Why));
                    }
                }
                if (Bake)           // (the last frame is the next pass's first: dropped)
                {
                    --OutFrames; Out.m_nFrames = OutFrames;
                    AllKeyFrames.resize(KeyFrameBase + static_cast<std::size_t>(OutFrames) * nSkelBones);
                    if (bRM) AllRootMotion.resize(RB + static_cast<std::size_t>(OutFrames));
                    if (bRT) m_RootTurn.resize(RB + static_cast<std::size_t>(OutFrames));
                    // the events into the half-open cycle [0, T): those inside the cut keep their time (one at its end is the next pass's at 0);
                    // one the cut left out comes in at its phase only where the pass has none of its name within 0.1 T (strides are not exactly T
                    // long: folding every stride in would fire a footstep two or three times a pass)
                    if (pAllEvents && Out.m_nEvents)
                    {
                        const float T = float(OutFrames) / FinalFPS, Shift = float(First) / FinalFPS, Half = 0.5f / FinalFPS;
                        const auto* pE = pAllEvents->data() + Out.m_iFirstEvent;
                        std::vector<xanim_package::event> In, Outside;
                        for (std::uint32_t q = 0; q < Out.m_nEvents; ++q)
                        {
                            auto E = pE[q]; const float t = E.m_Time - Shift;
                            if (t > -Half && t < T + Half) { E.m_Time = t >= T - Half ? 0.0f : std::max(0.0f, t); E.m_Duration = std::min(E.m_Duration, T); In.push_back(E); }
                            else { float u = std::fmod(t, T); if (u < 0) u += T; if (u > T - Half) u = 0; E.m_Time = u; E.m_Duration = std::min(E.m_Duration, T); Outside.push_back(E); }
                        }
                        auto Near = [&](const xanim_package::event& E, float Tol) { for (const auto& K : In) { float d = std::fabs(K.m_Time - E.m_Time); d = std::min(d, T - d); if (K.m_NameHash == E.m_NameHash && K.m_Kind == E.m_Kind && d < Tol) return true; } return false; };
                        std::vector<xanim_package::event> Keep;
                        for (const auto& E : In) if (std::none_of(Keep.begin(), Keep.end(), [&](const auto& K) { return K.m_NameHash == E.m_NameHash && K.m_Kind == E.m_Kind && std::fabs(K.m_Time - E.m_Time) < Half; })) Keep.push_back(E);
                        In = Keep;
                        for (const auto& E : Outside) if (!Near(E, 0.1f * T)) In.push_back(E);
                        std::stable_sort(In.begin(), In.end(), [](const auto& a, const auto& b) { return a.m_Time < b.m_Time; });
                        auto First_ = pAllEvents->begin() + static_cast<std::ptrdiff_t>(Out.m_iFirstEvent);
                        pAllEvents->erase(First_, First_ + static_cast<std::ptrdiff_t>(Out.m_nEvents));
                        pAllEvents->insert(pAllEvents->begin() + static_cast<std::ptrdiff_t>(Out.m_iFirstEvent), In.begin(), In.end());
                        Out.m_nEvents = static_cast<std::uint32_t>(In.size());
                    }
                }
            }

            return {};
        }

        //--------------------------------------------------------------------------------------
        // Clip identity at runtime is a CRC32 of the (possibly renamed) name - two differently-named
        // clips hashing to the same identifier is a hard compile error, mirroring xskeleton's own
        // bone-name-hash collision check.
        xerr CheckClipNameCollisions(const std::vector<xanim_package::clip>& Clips, const std::vector<std::string>& Names)
        {
            std::unordered_map<std::uint32_t, std::string> Seen;
            for (int i = 0; i < static_cast<int>(Clips.size()); ++i)
            {
                if (auto It = Seen.find(Clips[i].m_NameHash); It != Seen.end() && It->second != Names[i])
                    return xerr::create_f<state, "Two differently-named clips hash to the same identifier - rename one of them">();
                Seen[Clips[i].m_NameHash] = Names[i];
            }
            return {};
        }

        //--------------------------------------------------------------------------------------

        // Folds the clip's nodes into the joints of a skeleton (the skeleton keeps only its runtime joints): the clip's other nodes' motion is baked into the joints below them
        void FoldRaw(raw_clip& Raw, const std::vector<xskeleton_desc::bone_manifest::bone_entry>& Joints)
        {
        // The skeleton keeps only its runtime joints (xraw3d anim::RuntimeJoints): fold the clip's other nodes the same way, so
        // their motion is baked into the joints below them (documentation/Editors/skeleton_runtime_folding_plan.md)
        {
            std::vector<bool> Keep(Raw.m_Anim.m_Bone.size(), false);
            for (std::size_t r = 0; r < Keep.size(); ++r)
                Keep[r] = std::any_of(Joints.begin(), Joints.end(), [&](const auto& B) { return B.m_Name == Raw.m_Anim.m_Bone[r].m_Name; });
            const bool bSyntheticRoot = !Joints.empty() && Joints[0].m_Name == xraw3d::anim::synthetic_root_name_v;
            const bool bFolds         = bSyntheticRoot || std::find(Keep.begin(), Keep.end(), false) != Keep.end();
            if (bFolds && std::find(Keep.begin(), Keep.end(), true) != Keep.end())
            {
                const auto nBefore = Raw.m_Anim.m_Bone.size();
                for (auto& Name : Raw.m_Anim.FoldBones(Keep, bSyntheticRoot))
                    LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': node '{}' kept, folding it would not fit translation/rotation/scale", Raw.m_Anim.m_Name, Name));
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': {} source nodes folded into the skeleton's joints", Raw.m_Anim.m_Name, nBefore - (Raw.m_Anim.m_Bone.size() - (bSyntheticRoot ? 1 : 0))));
            }
        }
        }

        //--------------------------------------------------------------------------------------
        //--------------------------------------------------------------------------------------
        // An import source that is another animation PACKAGE: its descriptor lists the animations (their names) and the skeleton they play on; its compiled clips are the
        // data (already in that skeleton's space: its Pre/PostTransform were applied when it was compiled). Each clip becomes a raw clip named by the descriptor, with the
        // bones of that package's skeleton; a clip that had its root motion taken out has it put back, so this package decides what to do with it.
        xerr LoadPackageSource(const xanim_package_desc::import_source& Source)
        {
            const std::uint64_t Key = Source.m_Package.m_Instance.m_Value;
            xanim_package_desc::descriptor Other;
            {
                xproperty::settings::context Context{};
                const std::wstring Path = std::format(L"{}/Descriptors/AnimPackage/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", m_ProjectPaths.m_Project, Key & 0xff, (Key & 0xff00) >> 8, Key);
                if (auto Err = Other.Serialize(true, Path, Context); Err)
                    return xerr::create_f<state, "The animation package of an import source could not be read (is it compiled? its skeleton?)">(Err);
            }
            if (Other.m_SkeletonRef.empty()) return xerr::create_f<state, "The animation package of an import source has no skeleton">();
            const std::uint64_t FromSkeleton = Other.m_SkeletonRef.m_Instance.m_Value;
            if (!Source.bDifferent() && FromSkeleton != m_Descriptor.m_SkeletonRef.m_Instance.m_Value)
                return xerr::create_f<state, "The package is on another skeleton: use the 'Different skeleton: animation package' kind and a Retarget">();
            m_Dependencies.m_Assets.push_back(xskeleton_desc::ManifestRelativePath(FromSkeleton));
            m_Dependencies.m_Resources.push_back({ Other.m_SkeletonRef.m_Instance, xrsc::skeleton_type_guid_v });

            // its compiled data, next to this resource's own output (the same platform folder)
            if (m_Target.empty()) return xerr::create_f<state, "No output target">();
            const std::filesystem::path Mine = std::filesystem::path(std::wstring(m_Target[0].m_DataPath));
            const std::wstring Compiled = (Mine.parent_path().parent_path().parent_path() / std::format(L"{:02X}", Key & 0xff) / std::format(L"{:02X}", (Key & 0xff00) >> 8) / std::format(L"{:X}", Key)).wstring();
            xanim_package::anim_package* pPackage = nullptr;
            xserializer::stream Stream;
            if (auto Err = Stream.Load(Compiled, pPackage); Err || !pPackage)
                return xerr::create_f<state, "The animation package of an import source is not compiled: compile it first">();
            struct free_it { xanim_package::anim_package* P; ~free_it() { xanim_package::FreeLoaded(P); } } Free{ pPackage };
            // its raw keys, exact as compiled from its sources (never its compressed tracks: lossy data is never an input)
            if (pPackage->m_nSidecarKeys && !xanim_package::LoadKeys(*pPackage, Compiled))
                return xerr::create_f<state, "The animation package of an import source has no raw keys next to it (.keys): compile it again">();

            const auto& Bones = Other.m_ResolvedSkeletonBones.m_Bones;
            if (Bones.size() != pPackage->m_nBones)
                return xerr::create_f<state, "The package and its skeleton disagree on the bones: compile the package again">();
            int nTaken = 0;
            for (auto& OtherSource : Other.m_ImportSources)
                for (auto& C : OtherSource.m_Clips)
                {
                    if (C.m_bDelete) continue;
                    const std::string Name = C.m_Name.empty() ? C.m_OriginalName : C.m_Name;
                    const int iClip = pPackage->findClipIndex(xstrtool::CRC32(Name));
                    if (iClip < 0) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Package clip '{}' is not in its compiled data (compile that package again)", Name)); continue; }
                    const auto& PC = pPackage->getClips()[iClip];
                    xraw3d::anim A;
                    A.m_Name = Name; A.m_FPS = PC.m_FPS; A.m_nFrames = PC.m_nFrames;
                    A.m_Bone.resize(Bones.size());
                    for (std::size_t b = 0; b < Bones.size(); ++b) { A.m_Bone[b] = {}; A.m_Bone[b].m_Name = Bones[b].m_Name; A.m_Bone[b].m_iParent = Bones[b].m_iParent; }
                    A.m_KeyFrame.resize(static_cast<std::size_t>(PC.m_nFrames) * Bones.size());
                    int iRoot = -1; for (std::size_t b = 0; b < Bones.size(); ++b) if (Bones[b].m_iParent < 0) { iRoot = static_cast<int>(b); break; }
                    const auto RootMotion = pPackage->getClipRootMotion(iClip);
                    for (int f = 0; f < PC.m_nFrames; ++f)
                    {
                        const auto Frame = pPackage->getClipFrame(iClip, f);
                        for (std::size_t b = 0; b < Bones.size(); ++b) A.m_KeyFrame[static_cast<std::size_t>(f) * Bones.size() + b] = Frame[b];
                        if (!RootMotion.empty() && iRoot >= 0) A.m_KeyFrame[static_cast<std::size_t>(f) * Bones.size() + iRoot].m_Position += RootMotion[f];   // the motion the package took out of the root
                    }
                    m_RawClips.push_back(raw_clip{ .m_Anim = std::move(A), .m_SourceFile = Source.key(), .m_bInSkeletonSpace = true, .m_FromSkeleton = FromSkeleton });
                    ++nTaken;
                }
            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Animation package source: {} clips taken", nTaken));
            return {};
        }

        // A clip made on ANOTHER skeleton (its import source names a Retarget): the clip's keys go into the SOURCE skeleton's space with that skeleton's own
        // PostTransform then PreTransform (the corrections that link a skeleton and its animations), are transferred to the DESTINATION skeleton (this package's)
        // by the Retarget's mapping, and come out already in the destination's space. The clip then replaces Raw, named after the destination's bones.
        xerr RetargetRaw(raw_clip& Raw, const xanim_package_desc::import_source& Source)
        {
            const std::uint64_t Key = Source.m_Retarget.m_Instance.m_Value;
            auto& Slot = m_Retargets[Key];
            if (!Slot)
            {
                auto Ctx = std::make_unique<retarget_context>();
                xproperty::settings::context Context{};
                const std::wstring Path = std::format(L"{}/Descriptors/Retarget/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", m_ProjectPaths.m_Project, Key & 0xff, (Key & 0xff00) >> 8, Key);
                if (auto Err = Ctx->m_Descriptor.Serialize(true, Path, Context); Err)
                    return xerr::create_f<state, "The Retarget of an import source could not be read (is it compiled? are its skeletons?)">(Err);
                std::vector<std::string> Errors; Ctx->m_Descriptor.Validate(Errors);
                if (!Errors.empty()) { for (auto& E : Errors) LogMessage(xresource_pipeline::msg_type::ERROR, std::move(E)); return xerr::create_f<state, "The Retarget of an import source is not valid">(); }
                if (Ctx->m_Descriptor.m_Destination.m_Instance.m_Value != m_Descriptor.m_SkeletonRef.m_Instance.m_Value)
                    return xerr::create_f<state, "The Retarget's destination skeleton is not this package's skeleton">();
                if (auto Err = xretarget_desc::MakeMapping(Ctx->m_Descriptor, Ctx->m_Mapping); Err) return Err;
                for (const auto& W : Ctx->m_Mapping.m_Auto.m_Warnings) LogMessage(xresource_pipeline::msg_type::WARNING, "Retarget: " + W);
                Ctx->m_Retargeter.Init(Ctx->m_Mapping.sourceCalibrated(), Ctx->m_Mapping.destinationCalibrated(), Ctx->m_Mapping.m_Auto.m_Map);
                // the package follows the Retarget and the skeleton it moves from: it compiles again when either does
                m_Dependencies.m_Resources.push_back({ Source.m_Retarget.m_Instance, xrsc::retarget_type_guid_v });
                m_Dependencies.m_Resources.push_back({ Ctx->m_Descriptor.m_Source.m_Instance, xrsc::skeleton_type_guid_v });
                m_Dependencies.m_Assets.push_back(xskeleton_desc::ManifestRelativePath(Ctx->m_Descriptor.m_Source.m_Instance.m_Value));
                Slot = std::move(Ctx);
            }
            auto& Ctx = *Slot;
            const auto& SrcManifest = Ctx.m_Descriptor.m_ResolvedSource;
            const auto& DstManifest = Ctx.m_Descriptor.m_ResolvedDestination;
            const int nSrc = static_cast<int>(SrcManifest.m_Bones.size()), nDst = static_cast<int>(DstManifest.m_Bones.size());

            if (!Raw.m_bInSkeletonSpace) FoldRaw(Raw, SrcManifest.m_Bones);             // the clip's nodes into the SOURCE skeleton's joints (a package clip has them: it IS the skeleton's)
            else if (Raw.m_FromSkeleton != Ctx.m_Descriptor.m_Source.m_Instance.m_Value)
                return xerr::create_f<state, "The package's skeleton is not the Retarget's source skeleton">();

            // the source skeleton's frames: the clip's key where it has one (by bone name), the skeleton's rest pose where it has not
            const int nRaw = static_cast<int>(Raw.m_Anim.m_Bone.size()), nFrames = Raw.m_Anim.m_nFrames;
            std::vector<int> SrcToRaw(nSrc, -1);
            for (int s = 0; s < nSrc; ++s)
                for (int r = 0; r < nRaw; ++r) if (Raw.m_Anim.m_Bone[r].m_Name == SrcManifest.m_Bones[s].m_Name) { SrcToRaw[s] = r; break; }
            std::vector<xmath::transform3> Frames(static_cast<std::size_t>(nFrames) * nSrc);
            for (int f = 0; f < nFrames; ++f)
                for (int s = 0; s < nSrc; ++s)
                {
                    const auto& B = SrcManifest.m_Bones[s];
                    Frames[static_cast<std::size_t>(f) * nSrc + s] = SrcToRaw[s] != -1 ? Raw.m_Anim.m_KeyFrame[static_cast<std::size_t>(f) * nRaw + SrcToRaw[s]]
                        : xmath::transform3{ .m_Scale = B.m_RestScale, .m_Rotation = xmath::fquat(B.m_RestRotX, B.m_RestRotY, B.m_RestRotZ, B.m_RestRotW), .m_Position = B.m_RestPosition };
                }
            int iRoot = -1; for (int s = 0; s < nSrc; ++s) if (SrcManifest.m_Bones[s].m_iParent < 0) { iRoot = s; break; }
            if (Raw.m_bInSkeletonSpace) {}                                              // a package clip: already in its skeleton's space (its own Pre/PostTransform were applied when it was compiled)
            else if (iRoot != -1 && SrcToRaw[iRoot] != -1)                              // the skeleton's own root, animated by the clip: raw import space -> the skeleton's space
            {
                auto Corr = [](const xmath::fvec3& S, const xmath::fvec3& R, const xmath::fvec3& T) { xanimation::root_correction C; C.m_Scale = S; C.m_RotationDeg = R; C.m_Translation = T; return C; };
                xanimation::ToSkeletonSpace(Frames, nSrc, iRoot
                    , Corr(SrcManifest.m_PostTransformScale, SrcManifest.m_PostTransformRotationDeg, SrcManifest.m_PostTransformTranslation)
                    , Corr(SrcManifest.m_PreTransformScale,  SrcManifest.m_PreTransformRotationDeg,  SrcManifest.m_PreTransformTranslation));
            }
            else LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}' does not animate the source skeleton's root: the skeleton's corrections were not applied", Raw.m_Anim.m_Name));

            auto Baked = xanimation::BakeFrames(Ctx.m_Retargeter, xanimation::clip_view{ Frames, nSrc, nFrames, Raw.m_Anim.m_FPS, false }, nDst);

            // a numeric look at the result (the log is how a retarget is judged without eyes): the hands and feet, relative to the hips and in hip heights, for the source
            // (in its skeleton space) and the destination (baked), on a few frames. A transfer that works gives the two nearly the same numbers.
            {
                auto Worlds = [&](const std::vector<std::int32_t>& Parents, const std::vector<xmath::transform3>& Fr, int nB, int f)
                {
                    xanimation::pose P; P.resize(nB); for (int b = 0; b < nB; ++b) P.set(b, Fr[static_cast<std::size_t>(f) * nB + b]);
                    std::vector<xmath::fquat> R; std::vector<xmath::fvec3> Pos;
                    xanimation::details::WorldRP({ std::span<const std::int32_t>(Parents) }, P, R, Pos);
                    return Pos;
                };
                const auto& Sk = Ctx.m_Mapping.m_Source; const auto& Dk = Ctx.m_Mapping.m_Destination;
                std::vector<xmath::fquat> Rr; std::vector<xmath::fvec3> SRest, DRest;
                xanimation::details::WorldRP({ std::span<const std::int32_t>(Sk.m_Parent) }, Ctx.m_Mapping.m_SourceCalibration, Rr, SRest);
                xanimation::details::WorldRP({ std::span<const std::int32_t>(Dk.m_Parent) }, Ctx.m_Mapping.m_DestinationCalibration, Rr, DRest);
                const int HS = Ctx.m_Mapping.m_Auto.m_Map.m_SrcHips, HD = Ctx.m_Mapping.m_Auto.m_Map.m_TgtHips;
                if (HS >= 0 && HD >= 0 && nFrames > 0)
                {
                    const float Hs = xanimation::details::HipHeight(SRest, HS), Hd = xanimation::details::HipHeight(DRest, HD);
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Retarget report [{}]: hip height source {:.3f}, destination {:.3f} (ratio {:.4f}); {} frames", Raw.m_Anim.m_Name, Hs, Hd, Hd / std::max(Hs, 1e-9f), nFrames));
                    for (const int f : { 0, nFrames / 4, nFrames / 2, 3 * nFrames / 4 })
                    {
                        auto SP = Worlds(Sk.m_Parent, Frames, nSrc, f); const auto DP = Worlds(Dk.m_Parent, Baked, nDst, f);
                        for (auto& P : SP) P = Ctx.m_Mapping.m_Auto.m_Map.m_Registration * P;       // the source seen through the registration, in the destination's frame
                        std::string Line = std::format("  frame {:3d}: hips src ({:.3f} {:.3f} {:.3f}) dst ({:.3f} {:.3f} {:.3f}) |", f, SP[HS].m_X, SP[HS].m_Y, SP[HS].m_Z, DP[HD].m_X, DP[HD].m_Y, DP[HD].m_Z);
                        for (const auto& Row : Ctx.m_Mapping.m_Auto.m_Rows)
                        {
                            const bool bEnd = Row.m_Slot == xanimation::bone_slot::HAND || Row.m_Slot == xanimation::bone_slot::FOOT;
                            if (!bEnd || Row.m_Side == xanimation::bone_side::NONE) continue;
                            const auto a = (SP[Row.m_Source] - SP[HS]) * (1.0f / Hs), b = (DP[Row.m_Target] - DP[HD]) * (1.0f / Hd);
                            Line += std::format(" {}{} src ({:.2f} {:.2f} {:.2f}) dst ({:.2f} {:.2f} {:.2f})", Row.m_Side == xanimation::bone_side::LEFT ? "L" : "R", Row.m_Slot == xanimation::bone_slot::HAND ? "hand" : "foot", a.m_X, a.m_Y, a.m_Z, b.m_X, b.m_Y, b.m_Z);
                        }
                        LogMessage(xresource_pipeline::msg_type::INFO, Line);
                    }

                    // feet planted? Each foot's support intervals (low and slow, in hip heights) found in the source and in the baked clip, and how far the foot slides while in support
                    // (hip heights per second): a retarget that keeps the contacts has about the same intervals and no more sliding than the source had.
                    for (const auto& Row : Ctx.m_Mapping.m_Auto.m_Rows)
                    {
                        if (Row.m_Slot != xanimation::bone_slot::FOOT || Row.m_Side == xanimation::bone_side::NONE) continue;
                        std::vector<xmath::fvec3> SF, DF; SF.reserve(nFrames); DF.reserve(nFrames);
                        for (int f = 0; f < nFrames; ++f)
                        {
                            auto SP = Worlds(Sk.m_Parent, Frames, nSrc, f); const auto DP = Worlds(Dk.m_Parent, Baked, nDst, f);
                            SF.push_back(Ctx.m_Mapping.m_Auto.m_Map.m_Registration * SP[Row.m_Source] * (1.0f / Hs)); DF.push_back(DP[Row.m_Target] * (1.0f / Hd));
                        }
                        const auto Sc = xanimation::FindContacts(SF, float(Raw.m_Anim.m_FPS)), Dc = xanimation::FindContacts(DF, float(Raw.m_Anim.m_FPS));
                        auto Mean = [](const std::vector<xanimation::contact_interval>& C) { float T = 0; for (const auto& I : C) T += I.m_Skate; return C.empty() ? 0.0f : T / static_cast<float>(C.size()); };
                        const char* pSide = Row.m_Side == xanimation::bone_side::LEFT ? "left" : "right";
                        LogMessage(xresource_pipeline::msg_type::INFO, std::format("  {} foot contacts: source {} (skate {:.3f}/s), destination {} (skate {:.3f}/s)", pSide, Sc.size(), Mean(Sc), Dc.size(), Mean(Dc)));

                        // the contact fix-up (the Retarget's PlantFeet): the destination foot held over the SOURCE's contacts (the intent), then measured again
                        // over those same intervals (skate in hip heights per second, like above)
                        if (Ctx.m_Descriptor.m_bPlantFeet && !Sc.empty())
                        {
                            xanimation::leg_chain Leg; Leg.m_Foot = Row.m_Target;
                            for (const auto& R2 : Ctx.m_Mapping.m_Auto.m_Rows)
                                if (R2.m_Side == Row.m_Side) { if (R2.m_Slot == xanimation::bone_slot::THIGH) Leg.m_Thigh = R2.m_Target; else if (R2.m_Slot == xanimation::bone_slot::CALF) Leg.m_Calf = R2.m_Target; }
                            auto Skate = [&](const std::vector<xmath::fvec3>& P)
                            {
                                float T = 0; for (const auto& I : Sc) { float Path = 0; for (int f = I.m_First + 1; f <= I.m_Last; ++f) Path += std::hypot(P[f].m_X - P[f - 1].m_X, P[f].m_Z - P[f - 1].m_Z);
                                                                        T += I.m_Last > I.m_First ? Path * Raw.m_Anim.m_FPS / float(I.m_Last - I.m_First) : 0.0f; }
                                return T / float(Sc.size());
                            };
                            const float Before = Skate(DF);
                            const auto Res = xanimation::PlantFoot(Baked, nDst, Dk.m_Parent, Leg, Sc);
                            if (!Res.m_bOk) LogMessage(xresource_pipeline::msg_type::WARNING, std::format("  {} foot: PlantFeet skipped (the leg is not thigh -> calf -> foot in the destination)", pSide));
                            else
                            {
                                for (int f = 0; f < nFrames; ++f) DF[f] = Worlds(Dk.m_Parent, Baked, nDst, f)[Row.m_Target] * (1.0f / Hd);
                                Raw.m_bPlanted = true;
                                LogMessage(xresource_pipeline::msg_type::INFO, std::format("  {} foot planted over the source's {} contacts: skate {:.4f}/s -> {:.4f}/s; {} frames solved, largest miss {:.4f} hip heights"
                                    , pSide, Sc.size(), Before, Skate(DF), Res.m_nFrames, Res.m_MaxMiss / Hd));
                            }
                        }
                    }
                }
            }

            // the clip becomes a clip of the destination skeleton: its bones are the destination's, its keys the baked frames
            xraw3d::anim Out;
            Out.m_Name = Raw.m_Anim.m_Name; Out.m_FPS = Raw.m_Anim.m_FPS; Out.m_nFrames = nFrames;
            Out.m_Bone.resize(nDst);
            for (int d = 0; d < nDst; ++d) { Out.m_Bone[d] = {}; Out.m_Bone[d].m_Name = DstManifest.m_Bones[d].m_Name; Out.m_Bone[d].m_iParent = DstManifest.m_Bones[d].m_iParent; }
            Out.m_KeyFrame = Baked;
            Raw.m_Anim = std::move(Out);
            Raw.m_bInSkeletonSpace = true;
            return {};
        }

        // E58 (Tomas 2026-11-18: "feet planting etc... is general for all our animations, no matter where the source comes from"): the clean-up
        // of ANY source on this skeleton - each leg's contacts found on the clip itself (low and slow, in hip heights) and its skate logged;
        // with PlantFeet the foot is held over them (xanimation PlantFoot, the same solver the Retarget uses) and measured again. The clip is
        // left in this skeleton's space (its Pre/PostTransform applied, as SkeletonFrames does).
        void CleanUpRaw(raw_clip& Raw, const xskeleton_desc::bone_manifest& Manifest, bool bPlant)
        {
            const auto& Bones = Manifest.m_Bones;
            const int nB = static_cast<int>(Bones.size()), nF = Raw.m_Anim.m_nFrames;
            if (nB == 0 || nF < 2) return;
            std::vector<std::int32_t> Parents(nB); std::vector<std::string_view> Names(nB); xanimation::pose Rest; Rest.resize(nB);
            for (int b = 0; b < nB; ++b)
            {
                Parents[b] = Bones[b].m_iParent; Names[b] = Bones[b].m_Name;
                Rest.set(b, xmath::transform3{ .m_Scale = Bones[b].m_RestScale, .m_Rotation = xmath::fquat(Bones[b].m_RestRotX, Bones[b].m_RestRotY, Bones[b].m_RestRotZ, Bones[b].m_RestRotW), .m_Position = Bones[b].m_RestPosition });
            }
            const xanimation::skeleton_view SV{ std::span<const std::int32_t>(Parents) };
            const auto Roles = xanimation::BodyRoles(SV, Names, Rest);
            const auto Rig = xanimation::FindWarpRig(SV, Roles, &Rest);
            if (!Rig.valid()) return;                                   // (no legs: nothing to plant or measure)
            std::vector<xmath::fquat> R; std::vector<xmath::fvec3> P;
            xanimation::details::WorldRP(SV, Rest, R, P);
            const float Hip = std::max(P[static_cast<std::size_t>(Rig.m_Hips)].m_Y, 1e-3f);
            auto Frames = SkeletonFrames(Raw, Manifest);
            auto Feet = [&](int Foot)
            {
                std::vector<xmath::fvec3> F(static_cast<std::size_t>(nF)); xanimation::pose Q; Q.resize(nB);
                for (int f = 0; f < nF; ++f) { for (int b = 0; b < nB; ++b) Q.set(b, Frames[static_cast<std::size_t>(f) * nB + b]); xanimation::details::WorldRP(SV, Q, R, P); F[static_cast<std::size_t>(f)] = P[static_cast<std::size_t>(Foot)] * (1.0f / Hip); }
                return F;
            };
            const float Fps = static_cast<float>(std::max(1, Raw.m_Anim.m_FPS));
            bool bChanged = false;
            for (std::size_t l = 0; l < Rig.m_Legs.size(); ++l)
            {
                const auto& Lg = Rig.m_Legs[l];
                auto F = Feet(Lg.m_Foot);
                const auto C = xanimation::FindContacts(F, Fps);
                auto Skate = [&](const std::vector<xmath::fvec3>& Q) { float T = 0; for (const auto& I : C) { float Path = 0; for (int f = I.m_First + 1; f <= I.m_Last; ++f) Path += std::hypot(Q[f].m_X - Q[f - 1].m_X, Q[f].m_Z - Q[f - 1].m_Z); T += Path * Hip; } return T; };
                const float Before = Skate(F);
                if (!bPlant || C.empty()) { LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': leg {} - {} contacts, the planted foot slides {:.1f} cm in all", Raw.m_Anim.m_Name, l, C.size(), Before * 100)); continue; }
                xanimation::leg_chain Leg; Leg.m_Thigh = Lg.m_Thigh; Leg.m_Calf = Lg.m_Calf; Leg.m_Foot = Lg.m_Foot;
                const auto Res = xanimation::PlantFoot(Frames, nB, Parents, Leg, C);
                if (!Res.m_bOk) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': leg {} - PlantFeet skipped (not thigh -> calf -> foot)", Raw.m_Anim.m_Name, l)); continue; }
                bChanged = true;
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': leg {} planted over {} contacts - the foot slides {:.1f} cm -> {:.1f} cm in all; {} frames solved, largest miss {:.1f} cm"
                    , Raw.m_Anim.m_Name, l, C.size(), Before * 100, Skate(Feet(Lg.m_Foot)) * 100, Res.m_nFrames, Res.m_MaxMiss * 100));
            }
            if (!bChanged) return;
            xraw3d::anim Out; Out.m_Name = Raw.m_Anim.m_Name; Out.m_FPS = Raw.m_Anim.m_FPS; Out.m_nFrames = nF; Out.m_Bone.resize(nB);
            for (int b = 0; b < nB; ++b) { Out.m_Bone[b] = {}; Out.m_Bone[b].m_Name = Bones[b].m_Name; Out.m_Bone[b].m_iParent = Bones[b].m_iParent; }
            Out.m_KeyFrame = std::move(Frames);
            Raw.m_Anim = std::move(Out); Raw.m_bInSkeletonSpace = true;
        }

        const xanim_package_desc::import_source* FindSource(const std::wstring& Path) const noexcept
        {
            for (auto& S : m_Descriptor.m_ImportSources) if (S.key() == Path) return &S;
            return nullptr;
        }

        // The demonstrated range of every joint over the package's clips, plus a 5 degree margin (posing v2 step R: contract 7.2's inferred
        // limits): JointRanges.txt next to the log, for review. Editor posing measures the same from LayerSources.bin; the limits get a home in
        // a resource once reviewed.
        void WriteJointRanges(const xskeleton_desc::bone_manifest& Manifest)
        {
            const int N = static_cast<int>(Manifest.m_Bones.size());
            if (!N) return;
            std::vector<std::int32_t> Parents(N);
            xanimation::pose Rest; Rest.resize(N);
            for (int b = 0; b < N; ++b)
            {
                const auto& B = Manifest.m_Bones[b];
                Parents[b] = B.m_iParent; Rest.m_Position[b] = B.m_RestPosition; Rest.m_Scale[b] = B.m_RestScale;
                Rest.m_Rotation[b] = xmath::fquat(B.m_RestRotX, B.m_RestRotY, B.m_RestRotZ, B.m_RestRotW).NormalizeSafeCopy();
            }
            std::vector<xmath::transform3> All; int nFrames = 0;
            for (const auto& S : m_SourceFrames)
                if (static_cast<int>(S.m_Frames.size()) == S.m_nFrames * N) { All.insert(All.end(), S.m_Frames.begin(), S.m_Frames.end()); nFrames += S.m_nFrames; }
            if (All.empty()) return;
            // the evidence the skeleton's "Analyze clips" reads (every clip's local rotations, keyed by the skeleton and its bone names)
            {
                xskeleton_joint_evidence::evidence E;
                E.m_Skeleton = m_Descriptor.m_SkeletonRef.m_Instance.m_Value;
                for (const auto& B : Manifest.m_Bones) E.m_Bones.push_back(B.m_Name);
                for (const auto& S : m_SourceFrames)
                {
                    if (static_cast<int>(S.m_Frames.size()) != S.m_nFrames * N) continue;
                    auto& C = E.m_Clips.emplace_back();
                    C.m_Name = S.m_Clip; C.m_nFrames = S.m_nFrames;
                    C.m_Rotations.reserve(S.m_Frames.size());
                    for (const auto& T : S.m_Frames) C.m_Rotations.push_back(T.m_Rotation);
                }
                if (!xskeleton_joint_evidence::Write(std::format(L"{}\\JointEvidence.bin", m_ResourceLogPath), E))
                    LogMessage(xresource_pipeline::msg_type::WARNING, "JointEvidence.bin could not be written: the skeleton's Analyze clips will not see this package");
            }
            std::vector<xanimation::joint_limit> L(N);
            std::vector<xanimation::frame_confidence> Conf(N);
            constexpr float D = 180.0f / 3.14159265f;
            xanimation::MeasureJointRanges(Parents, Rest, All, 5.0f / D, L, Conf);
            std::string Txt = std::format("# Demonstrated joint ranges over {} clips, {} frames (+5 deg margin): evidence, not accepted limits. Degrees, in each\n"
                                          "# joint's suggested frame (x along the bone, z the bend axis), from the rest pose.\n"
                                          "# bone\tframe\ttwist centre +- half\tswing centre (y, z)\tswing radii (y, z)\n", m_SourceFrames.size(), nFrames);
            for (int b = 0; b < N; ++b)
            {
                if (L[b].m_Type == xanimation::joint_type::FREE) { Txt += std::format("{}\tfree (root)\n", Manifest.m_Bones[b].m_Name); continue; }
                Txt += std::format("{}\t{}\t{:.1f} +- {:.1f}\t{:.1f}, {:.1f}\t{:.1f}, {:.1f}\n", Manifest.m_Bones[b].m_Name
                                  , Conf[b] == xanimation::frame_confidence::SUGGESTED ? "suggested" : "needs confirmation"
                                  , L[b].m_TwistCentre * D, L[b].m_TwistHalf * D, L[b].m_SwingCy * D, L[b].m_SwingCz * D, L[b].m_SwingRy * D, L[b].m_SwingRz * D);
            }
            if (std::FILE* f = xanimation::OpenW(std::format(L"{}\\JointRanges.txt", m_ResourceLogPath).c_str(), L"wb")) { std::fwrite(Txt.data(), 1, Txt.size(), f); std::fclose(f); }
            else LogMessage(xresource_pipeline::msg_type::WARNING, "JointRanges.txt could not be written");
        }

        xerr BuildFinalPackage()
        {
            std::unordered_map<std::wstring, std::unordered_map<std::string, const xanim_package_desc::clip*>> Overrides;
            CollectOverrides(Overrides);

            auto& Manifest      = m_Descriptor.m_ResolvedSkeletonBones;
            auto& ResolvedBones = Manifest.m_Bones;

            std::vector<xanim_package::clip>       Clips;
            std::vector<std::string>               Names, OrigNames;
            std::vector<std::pair<std::string, int>> Spaces;      // (per compiled clip: its motion space partner by name, samples)
            std::vector<xmath::transform3>          AllKeyFrames;
            std::vector<xmath::fvec3>               AllRootMotion;
            m_RootTurn.clear(); m_LoopReport.clear();
            std::vector<xanim_package::event>       AllEvents;

            // every raw clip on this skeleton first (retargeted or folded): a clip's layers may take any other clip of the package as their source
            for (auto& Raw : m_RawClips)
            {
                if (const auto* pSource = FindSource(Raw.m_SourceFile); pSource && pSource->bDifferent())
                {
                    if (auto Err = RetargetRaw(Raw, *pSource); Err) return Err;           // (a package clip is in its skeleton's space already: RetargetRaw knows)
                }
                else if (!Raw.m_bInSkeletonSpace) FoldRaw(Raw, ResolvedBones);            // an asset on this skeleton: its nodes folded into this skeleton's joints
                const auto* pSrc = FindSource(Raw.m_SourceFile);
                CleanUpRaw(Raw, Manifest, pSrc && pSrc->m_bPlantFeet && !Raw.m_bPlanted);      // E58: every source: its feet measured, planted when asked
            }

            // the skeleton frames of every raw clip: what layers compose from, and the editor's live preview reads the same (LayerSources.bin)
            m_SourceFrames.clear();
            for (const auto& Raw : m_RawClips)
                m_SourceFrames.push_back({ .m_Key = Raw.m_SourceFile, .m_Clip = Raw.m_Anim.m_Name, .m_FPS = std::max(1, Raw.m_Anim.m_FPS), .m_nFrames = Raw.m_Anim.m_nFrames, .m_Frames = SkeletonFrames(Raw, Manifest) });
            if (!xanim_package_layers::WriteSourcesCache(std::format(L"{}\\LayerSources.bin", m_ResourceLogPath), m_SourceFrames, static_cast<int>(ResolvedBones.size())))
                LogMessage(xresource_pipeline::msg_type::WARNING, "The layer-sources cache (LayerSources.bin) could not be written: the editor cannot preview layers live");
            WriteJointRanges(Manifest);

            for (auto& Raw : m_RawClips)
            {
                const xanim_package_desc::clip* pOv = nullptr;
                if (auto SourceIt = Overrides.find(Raw.m_SourceFile); SourceIt != Overrides.end())
                {
                    if (auto ClipIt = SourceIt->second.find(Raw.m_Anim.m_Name); ClipIt != SourceIt->second.end())
                        pOv = ClipIt->second;
                }

                // the clip's layers (the layered edit mode) baked onto its source frames, before its trim / downsample / root motion
                std::vector<xmath::transform3> Composed;
                const bool bLayers = pOv && !pOv->m_bDelete && std::any_of(pOv->m_Layers.begin(), pOv->m_Layers.end(), [](const auto& L) { return L.m_bEnabled; });
                if (bLayers)
                    if (auto Err = ComposeClip(Raw, *pOv, Manifest, Composed); Err) return Err;

                bool                  Skip = false;
                xanim_package::clip   Compiled{};
                if (auto Err = BuildCompiledClip(Raw, pOv, Manifest, Skip, Compiled, AllKeyFrames, AllRootMotion, bLayers ? &Composed : nullptr, &AllEvents); Err)
                    return Err;
                if (Skip) continue;

                Names.push_back((pOv && !pOv->m_Name.empty()) ? pOv->m_Name : Raw.m_Anim.m_Name); OrigNames.push_back(Raw.m_Anim.m_Name);
                Spaces.push_back(pOv ? std::pair{ pOv->m_SpaceWith, pOv->m_SpaceSamples } : std::pair{ std::string(), 0 });
                Clips.emplace_back(std::move(Compiled));
            }

            if (Clips.empty())
                return xerr::create_f<state, "No clips survived import/ignore - nothing to compile">();

            if (auto Err = CheckClipNameCollisions(Clips, Names); Err) return Err;
            for (std::size_t i = 0; i < Clips.size(); ++i)      // (motion spaces: the other loop by its compiled name; both must loop)
            {
                const auto& [With, n] = Spaces[i];
                if (With.empty()) continue;
                const auto It = std::find(Names.begin(), Names.end(), With);
                if (It == Names.end()) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': SpaceWith '{}' is not a clip of this package - no motion space", Names[i], With)); continue; }
                const auto j = std::size_t(It - Names.begin());
                if (!Clips[i].m_bLoop || !Clips[j].m_bLoop || j == i) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Clip '{}': a motion space needs two different LOOPS ('{}' with '{}') - none", Names[i], Names[i], With)); continue; }
                Clips[i].m_SpaceWith = std::int32_t(j); Clips[i].m_SpaceSamples = std::clamp(n, 2, 16);
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Motion space: '{}' <-> '{}', {} samples", Names[i], With, Clips[i].m_SpaceSamples));
            }
            // Smart Primitive step 5: the accepted entries, one record each (the interaction's clip and the entry's take by name: compiled, then original)
            {
                auto Find = [&](const std::string& N) { for (std::size_t i = 0; i < Names.size(); ++i) if (Names[i] == N) return int(i); for (std::size_t i = 0; i < OrigNames.size(); ++i) if (OrigNames[i] == N) return int(i); return -1; };
                std::vector<xanim_package::interaction_entry> Ie;
                for (const auto& I : m_Descriptor.m_Interactions)
                {
                    const int iClip = Find(I.m_Clip);
                    if (iClip < 0) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Interaction '{}': its clip '{}' is not in this package - no runtime entries", I.m_Name, I.m_Clip)); continue; }
                    for (const auto& E : I.m_Entries)
                    {
                        const int iTake = Find(E.m_Take);
                        if (iTake < 0) { LogMessage(xresource_pipeline::msg_type::WARNING, std::format("Interaction '{}': the take '{}' of an entry is not a clip of this package - entry dropped", I.m_Name, E.m_Take)); continue; }
                        xanim_package::interaction_entry R{}; R.m_NameHash = xstrtool::CRC32(I.m_Name); R.m_iClip = std::int16_t(iClip); R.m_iTake = std::int16_t(iTake);
                        R.m_X = E.m_X; R.m_Z = E.m_Z; R.m_Yaw = E.m_Yaw; R.m_bRevalidate = E.m_bRevalidate ? 1 : 0; Ie.push_back(R);
                    }
                }
                if (!Ie.empty())
                {
                    m_FinalPackage.m_nInteractions = std::uint32_t(Ie.size()); m_FinalPackage.m_pInteractions = new xanim_package::interaction_entry[Ie.size()];
                    std::memset(static_cast<void*>(m_FinalPackage.m_pInteractions), 0, sizeof(xanim_package::interaction_entry) * Ie.size());      // padding: the file follows from the content only
                    for (std::size_t i = 0; i < Ie.size(); ++i) { auto& D = m_FinalPackage.m_pInteractions[i]; const auto& S = Ie[i]; D.m_NameHash = S.m_NameHash; D.m_iClip = S.m_iClip; D.m_iTake = S.m_iTake; D.m_X = S.m_X; D.m_Z = S.m_Z; D.m_Yaw = S.m_Yaw; D.m_bRevalidate = S.m_bRevalidate; }
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Interaction entries: {}", Ie.size()));
                }
            }
            // C127: the loop report (one row per clip marked Loop: clip, verdict, frames as cut, seam as cut mm / deg-per-s, what was done or why not)
            if (!m_LoopReport.empty())
            {
                const std::string Txt = "clip\tverdict\tframes\tseam mm\tseam deg/s\twhat\n" + m_LoopReport;
                if (std::FILE* f = xanimation::OpenW(std::format(L"{}\\LoopReport.txt", m_ResourceLogPath).c_str(), L"wb")) { std::fwrite(Txt.data(), 1, Txt.size(), f); std::fclose(f); }
                else LogMessage(xresource_pipeline::msg_type::WARNING, "LoopReport.txt could not be written");
            }

            m_FinalPackage.m_nClips = static_cast<std::uint16_t>(Clips.size());
            // zeroed, then field by field: the file's bytes follow from the content only (xserializer copies a struct's raw bytes before its
            // fields, so padding - after the enum, the vector's unused lane - would carry whatever memory held)
            m_FinalPackage.m_pClips = new xanim_package::clip[Clips.size()];
            std::memset(static_cast<void*>(m_FinalPackage.m_pClips), 0, sizeof(xanim_package::clip) * Clips.size());
            for (std::size_t i = 0; i < Clips.size(); ++i)
            {
                auto& D = m_FinalPackage.m_pClips[i]; const auto& S = Clips[i];
                D.m_NameHash = S.m_NameHash; D.m_FPS = S.m_FPS; D.m_nFrames = S.m_nFrames; D.m_bLoop = S.m_bLoop; D.m_RootMotionMode = S.m_RootMotionMode;
                D.m_LoopDisplacement.m_X = S.m_LoopDisplacement.m_X; D.m_LoopDisplacement.m_Y = S.m_LoopDisplacement.m_Y; D.m_LoopDisplacement.m_Z = S.m_LoopDisplacement.m_Z; D.m_LoopTurn = S.m_LoopTurn;
                D.m_iFirstKeyFrame = S.m_iFirstKeyFrame; D.m_iFirstRootMotion = S.m_iFirstRootMotion; D.m_iFirstEvent = S.m_iFirstEvent; D.m_nEvents = S.m_nEvents;
                D.m_SpaceWith = S.m_SpaceWith; D.m_SpaceSamples = S.m_SpaceSamples;
                // what the clip does (soccer answer 4): measured on its final frames, with the travel its root motion took out
                {
                    const int nB = static_cast<int>(ResolvedBones.size());
                    std::vector<std::int32_t> Par(nB); std::vector<std::string_view> Nm(nB);
                    for (int b = 0; b < nB; ++b) { Par[b] = ResolvedBones[b].m_iParent; Nm[b] = ResolvedBones[b].m_Name; }
                    xanimation::clip_view CV; CV.m_nBones = nB; CV.m_nFrames = S.m_nFrames; CV.m_FPS = S.m_FPS; CV.m_bLoop = S.m_bLoop;
                    CV.m_Frames = { AllKeyFrames.data() + S.m_iFirstKeyFrame, std::size_t(S.m_nFrames) * nB };
                    std::span<const xmath::fvec3> RM; if (S.m_RootMotionMode != xanim_package::root_motion_mode::NONE) RM = { AllRootMotion.data() + S.m_iFirstRootMotion, std::size_t(S.m_nFrames) };
                    std::span<const float> RT; if (!RM.empty() && m_RootTurn.size() >= S.m_iFirstRootMotion + std::size_t(S.m_nFrames)) RT = { m_RootTurn.data() + S.m_iFirstRootMotion, std::size_t(S.m_nFrames) };
                    const auto Tr = xanimation::MeasureClip(xanimation::skeleton_view{ Par }, Nm, CV, RM, RT);
                    D.m_Speed = Tr.m_Speed; D.m_Direction = Tr.m_Direction; D.m_TurnRate = Tr.m_TurnRate; D.m_LoopError = Tr.m_LoopError; D.m_Height = Tr.m_Height;
                    LogMessage(xresource_pipeline::msg_type::INFO, std::format("Clip '{}': moves {:.2f} units/s at {:.0f} deg from its facing, turns {:.1f} deg/s, its ends {:.1f} deg apart{}, centroid height {:.2f}"
                        , Names[i], Tr.m_Speed, Tr.m_Direction, Tr.m_TurnRate, Tr.m_LoopError, Tr.m_LoopError < 5 ? " (loops)" : "", Tr.m_Height));                }
            }

            m_FinalPackage.m_nBones = static_cast<std::uint32_t>(ResolvedBones.size());
            m_FinalPackage.m_pBoneNameHashes = new std::uint32_t[ResolvedBones.size()];
            for (std::size_t i = 0; i < ResolvedBones.size(); ++i)
                m_FinalPackage.m_pBoneNameHashes[i] = ResolvedBones[i].m_NameHash;

            // the raw keys go to the sidecar (<package>.keys, written with the package); the package itself carries the compressed tracks
            m_FinalPackage.m_nTotalKeyFrames = 0; m_FinalPackage.m_pKeyFrame = nullptr;
            m_FinalPackage.m_nSidecarKeys = static_cast<std::uint32_t>(AllKeyFrames.size());
            m_SidecarKeys = AllKeyFrames;

            m_FinalPackage.m_nTotalRootMotionFrames = static_cast<std::uint32_t>(AllRootMotion.size());
            // an empty array is nullptr: new T[0] is a real heap address, and xserializer wrote it into the file as 8 raw bytes (count 0) - two
            // compiles of the same package without root motion differed by those 8 bytes (2026-11-15)
            m_FinalPackage.m_pRootMotion = AllRootMotion.empty() ? nullptr : new xmath::fvec3[AllRootMotion.size()];
            std::copy(AllRootMotion.begin(), AllRootMotion.end(), m_FinalPackage.m_pRootMotion);
            // K5: every clip compressed too (from the keys just built from the source assets - never from a package: lossy data is never an input),
            // its track indices made absolute into the package's arrays
            {
                std::vector<xanimation::compressed_clip::bone> Tracks; std::vector<std::uint64_t> Rot; std::vector<std::uint16_t> Q; std::vector<float> F;
                std::size_t Raw = 0;
                for (const auto& C : Clips)
                {
                    xanimation::clip_view V; V.m_Frames = { AllKeyFrames.data() + C.m_iFirstKeyFrame, std::size_t(C.m_nFrames) * m_FinalPackage.m_nBones };
                    V.m_nBones = int(m_FinalPackage.m_nBones); V.m_nFrames = C.m_nFrames; V.m_FPS = C.m_FPS; V.m_bLoop = C.m_bLoop;
                    auto Cc = xanimation::CompressClip(V);
                    for (auto& B : Cc.m_Bones)
                    {
                        if (B.m_Rot == xanimation::compressed_clip::TRACK_ANIM) B.m_iRot += std::uint32_t(Rot.size());
                        B.m_iPos   += std::uint32_t(B.m_Pos   == xanimation::compressed_clip::TRACK_RAW ? F.size() : Q.size());
                        B.m_iScale += std::uint32_t(B.m_Scale == xanimation::compressed_clip::TRACK_RAW ? F.size() : Q.size());
                        Tracks.push_back(B);
                    }
                    Rot.insert(Rot.end(), Cc.m_Rot64.begin(), Cc.m_Rot64.end()); Q.insert(Q.end(), Cc.m_Q16.begin(), Cc.m_Q16.end()); F.insert(F.end(), Cc.m_F32.begin(), Cc.m_F32.end());
                    Raw += V.m_Frames.size() * sizeof(xmath::transform3);
                }
                auto Copy = [](auto& Src, auto*& Dst, std::uint32_t& n) { n = std::uint32_t(Src.size()); if (!Src.empty()) { Dst = new std::remove_reference_t<decltype(Src[0])>[Src.size()]; std::copy(Src.begin(), Src.end(), Dst); } };
                Copy(Tracks, m_FinalPackage.m_pCTracks, m_FinalPackage.m_nCTracks); Copy(Rot, m_FinalPackage.m_pRot64, m_FinalPackage.m_nRot64);
                Copy(Q, m_FinalPackage.m_pQ16, m_FinalPackage.m_nQ16); Copy(F, m_FinalPackage.m_pF32, m_FinalPackage.m_nF32);
                const std::size_t Comp = Tracks.size() * 80 + Rot.size() * 8 + Q.size() * 2 + F.size() * 4;
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Compressed clips: {} -> {} bytes ({:.1f}x; the raw keys are kept for the editors until they decode on open)", Raw, Comp, Comp ? double(Raw) / double(Comp) : 0.0));
            }
            m_RootTurn.resize(AllRootMotion.size(), 0.0f);
            if (!AllRootMotion.empty())
            {
                m_FinalPackage.m_nRootTurnFrames = static_cast<std::uint32_t>(m_RootTurn.size());
                m_FinalPackage.m_pRootTurn = new float[m_RootTurn.size()];
                std::copy(m_RootTurn.begin(), m_RootTurn.end(), m_FinalPackage.m_pRootTurn);
            }
            m_FinalPackage.m_nTotalEvents = static_cast<std::uint32_t>(AllEvents.size());
            if (!AllEvents.empty())
            {
                m_FinalPackage.m_pEvents = new xanim_package::event[AllEvents.size()];
                std::memset(static_cast<void*>(m_FinalPackage.m_pEvents), 0, sizeof(xanim_package::event) * AllEvents.size());      // padding: the file follows from the content only
                for (std::size_t i = 0; i < AllEvents.size(); ++i)
                {
                    auto& D = m_FinalPackage.m_pEvents[i]; const auto& S = AllEvents[i];
                    D.m_NameHash = S.m_NameHash; D.m_Kind = S.m_Kind; D.m_iBone = S.m_iBone; D.m_Time = S.m_Time; D.m_Duration = S.m_Duration;
                }
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Events: {}", AllEvents.size()));
            }

            // the content's fingerprint, field by field (never the struct bytes: padding): two compiles of the same input print the same numbers
            {
                std::uint64_t H = 1469598103934665603ull;
                auto Mix = [&H](const void* p, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { H ^= static_cast<const std::uint8_t*>(p)[i]; H *= 1099511628211ull; } };
                auto F = [&](float v) { Mix(&v, sizeof(v)); };
                std::uint64_t HClips, HKeys, HRoot;
                H = 1469598103934665603ull;
                for (const auto& C : Clips) { Mix(&C.m_NameHash, 4); Mix(&C.m_FPS, 4); Mix(&C.m_nFrames, 4); Mix(&C.m_bLoop, 1); Mix(&C.m_RootMotionMode, 1); F(C.m_LoopDisplacement.m_X); F(C.m_LoopDisplacement.m_Y); F(C.m_LoopDisplacement.m_Z); F(C.m_LoopTurn); Mix(&C.m_iFirstKeyFrame, 4); Mix(&C.m_iFirstRootMotion, 4); }
                HClips = H; H = 1469598103934665603ull;
                for (const auto& K : AllKeyFrames) { F(K.m_Scale.m_X); F(K.m_Scale.m_Y); F(K.m_Scale.m_Z); F(K.m_Rotation.m_X); F(K.m_Rotation.m_Y); F(K.m_Rotation.m_Z); F(K.m_Rotation.m_W); F(K.m_Position.m_X); F(K.m_Position.m_Y); F(K.m_Position.m_Z); }
                HKeys = H; H = 1469598103934665603ull;
                for (const auto& R : AllRootMotion) { F(R.m_X); F(R.m_Y); F(R.m_Z); }
                HRoot = H;
                LogMessage(xresource_pipeline::msg_type::INFO, std::format("Package content: {} clips {:016X}, {} keyframes {:016X}, {} root motion {:016X}", Clips.size(), HClips, AllKeyFrames.size(), HKeys, AllRootMotion.size(), HRoot));
            }

            return {};
        }

        //--------------------------------------------------------------------------------------

        xerr onCompile(void) noexcept override
        {
            //
            // Read the descriptor file (this also triggers descriptor::Serialize()'s cross-plugin
            // read of the referenced skeleton's bone manifest - see xanim_package_descriptor.h)
            //
            displayProgressBar("Loading Descriptor", 0);
            {
                xproperty::settings::context    Context{};
                auto                             DescriptorFileName = std::format(L"{}/{}/Descriptor.txt", m_ProjectPaths.m_Project, m_InputSrcDescriptorPath);

                if (auto Err = m_Descriptor.Serialize(true, DescriptorFileName, Context); Err)
                    return Err;
            }

            //
            // Do a quick validation of the descriptor
            //
            {
                std::vector<std::string> Errors;
                m_Descriptor.Validate(Errors);
                if (not Errors.empty())
                {
                    for (auto& E : Errors)
                        LogMessage(xresource_pipeline::msg_type::ERROR, std::move(E));

                    return xerr::create_f<state, "Validation Errors">();
                }
            }
            displayProgressBar("Loading Descriptor", 1);

            //
            // Load the source data
            //
            displayProgressBar("Importing Animations", 0);

            // Every source file is a dependency of this resource, registered BEFORE loading them (a broken link still shows as a dependent of the file): it is what the Assets tab counts
            // (the #N of the file) and lists (Find Resource / Open Resource), and what makes a change of a file compile this resource again.
            for (auto& Source : m_Descriptor.m_ImportSources)
            {
                if (Source.bAsset() && !Source.m_Path.empty()) m_Dependencies.m_Assets.push_back(Source.m_Path);
                if (Source.bPackage() && !Source.m_Package.empty()) m_Dependencies.m_Resources.push_back({ Source.m_Package.m_Instance, xrsc::anim_package_type_guid_v });     // it follows the package it takes animations from
            }

            // The package follows its skeleton (its PreTransform is applied to the root's keys, its bones are what the curves bind to): it compiles again whenever the skeleton does
            if (!m_Descriptor.m_SkeletonRef.empty())
            {
                m_Dependencies.m_Resources.push_back({ m_Descriptor.m_SkeletonRef.m_Instance, xrsc::skeleton_type_guid_v });
                m_Dependencies.m_Assets.push_back(xskeleton_desc::ManifestRelativePath(m_Descriptor.m_SkeletonRef.m_Instance.m_Value));
            }

            if (auto Err = LoadRaw(); Err)
                return Err;
            displayProgressBar("Importing Animations", 1);

            //
            // Fill the detail structure (raw import, pre-curation - for editor display only)
            //
            ComputeDetailStructure();

            //
            // OK, time to compile
            //
            try
            {
                m_FinalPackage.Initialize();

                displayProgressBar("Building AnimPackage", 0);
                if (auto Err = BuildFinalPackage(); Err)
                    return Err;
                displayProgressBar("Building AnimPackage", 1);
            }
            catch (std::runtime_error Error)
            {
                LogMessage(xresource_pipeline::msg_type::ERROR, std::format("{}", Error.what()));
                return xerr::create_f<state, "Exception thrown">();
            }

            //
            // Serialize the details structure
            //
            {
                xtextfile::stream File;
                if (auto Err = File.Open(false, std::format(L"{}\\Details.txt", m_ResourceLogPath), xtextfile::file_type::TEXT); Err)
                    return xerr::create_f<state, "Failed while opening the details.txt so it can't be saved">(Err);

                xproperty::settings::context C{};
                if (auto Err = xproperty::sprop::serializer::Stream(File, m_Details, C); Err)
                    return xerr::create_f<state, "Failed while serializing details.txt">(Err);
            }

            //
            // Export
            //
            int Count = 0;
            for (auto& T : m_Target)
            {
                displayProgressBar("Serializing", Count++ / (float)m_Target.size());

                if (T.m_bValid)
                {
                    Serialize(T.m_DataPath);
                }
            }
            displayProgressBar("Serializing", 1);
            return {};
        }

        //--------------------------------------------------------------------------------------

        void Serialize(const std::wstring_view FilePath)
        {
            {   // the sidecar first (a package whose keys are missing is refused by its readers)
                xanim_package::anim_keys K; K.m_nKeys = static_cast<std::uint32_t>(m_SidecarKeys.size()); K.m_pKeyFrame = m_SidecarKeys.empty() ? nullptr : m_SidecarKeys.data();
                xserializer::stream S;
                if (auto Err = S.Save(std::wstring(FilePath) + L".keys", K, xserializer::compression_level::FAST); Err) throw(std::runtime_error(std::string(Err.getMessage())));
            }
            xserializer::stream Serializer;
            if (auto Err = Serializer.Save
                ( FilePath
                , m_FinalPackage
                , m_OptimizationType == optimization_type::O0 ? xserializer::compression_level::FAST : m_OptimizationType == optimization_type::O1 ? xserializer::compression_level::MEDIUM : xserializer::compression_level::HIGH
                ); Err)
            {
                throw(std::runtime_error(std::string(Err.getMessage())));
            }
        }

        xanim_package_desc::details      m_Details;
        xanim_package_desc::descriptor   m_Descriptor;

        xanim_package::anim_package      m_FinalPackage;
        std::vector<raw_clip>            m_RawClips;
    };

    //------------------------------------------------------------------------------------

    std::unique_ptr<instance> instance::Create(void)
    {
        return std::make_unique<implementation>();
    }
}
