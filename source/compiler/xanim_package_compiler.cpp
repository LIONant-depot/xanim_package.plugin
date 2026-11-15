#include "xanim_package_compiler.h"

#include "dependencies/xraw3d/source/xraw3d.h"
#include "dependencies/xraw3d/source/details/xraw3d_assimp_import_v3.h"

#include "dependencies/xstrtool/source/xstrtool.h"

#include "../xanim_package_descriptor.h"
#include "plugins/xretarget.plugin/source/xretarget_mapping.h"
#include "dependencies/xAnimation/source/xanimation.h"
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
            const int StartFrame = (TrimStartFrame < 0) ? 0 : std::clamp(TrimStartFrame, 0, ResampledFrameCount - 1);
            const int EndFrame   = (TrimEndFrame   < 0) ? (ResampledFrameCount - 1) : std::clamp(TrimEndFrame, StartFrame, ResampledFrameCount - 1);
            const int OutFrames  = EndFrame - StartFrame + 1;

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
                }
            }

            // Loop only flags the clip: the clip's frames are the source's, untouched. Wrapping from the last frame to the first is the player's job
            // (a clip that does not end where it began is not made to: that would change every frame of it)

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
            struct free_it { xanim_package::anim_package* P; ~free_it() { if (P) xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, P); } } Free{ pPackage };

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

            const auto Baked = xanimation::BakeFrames(Ctx.m_Retargeter, xanimation::clip_view{ Frames, nSrc, nFrames, Raw.m_Anim.m_FPS, false }, nDst);

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
                        const auto Sc = xanimation::FindContacts(SF, Raw.m_Anim.m_FPS), Dc = xanimation::FindContacts(DF, Raw.m_Anim.m_FPS);
                        auto Mean = [](const std::vector<xanimation::contact_interval>& C) { float T = 0; for (const auto& I : C) T += I.m_Skate; return C.empty() ? 0.0f : T / static_cast<float>(C.size()); };
                        LogMessage(xresource_pipeline::msg_type::INFO, std::format("  {} foot contacts: source {} (skate {:.3f}/s), destination {} (skate {:.3f}/s)", Row.m_Side == xanimation::bone_side::LEFT ? "left" : "right", Sc.size(), Mean(Sc), Dc.size(), Mean(Dc)));
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

        const xanim_package_desc::import_source* FindSource(const std::wstring& Path) const noexcept
        {
            for (auto& S : m_Descriptor.m_ImportSources) if (S.key() == Path) return &S;
            return nullptr;
        }

        xerr BuildFinalPackage()
        {
            std::unordered_map<std::wstring, std::unordered_map<std::string, const xanim_package_desc::clip*>> Overrides;
            CollectOverrides(Overrides);

            auto& Manifest      = m_Descriptor.m_ResolvedSkeletonBones;
            auto& ResolvedBones = Manifest.m_Bones;

            std::vector<xanim_package::clip>       Clips;
            std::vector<std::string>               Names;
            std::vector<xmath::transform3>          AllKeyFrames;
            std::vector<xmath::fvec3>               AllRootMotion;

            // every raw clip on this skeleton first (retargeted or folded): a clip's layers may take any other clip of the package as their source
            for (auto& Raw : m_RawClips)
            {
                if (const auto* pSource = FindSource(Raw.m_SourceFile); pSource && pSource->bDifferent())
                {
                    if (auto Err = RetargetRaw(Raw, *pSource); Err) return Err;           // (a package clip is in its skeleton's space already: RetargetRaw knows)
                }
                else if (!Raw.m_bInSkeletonSpace) FoldRaw(Raw, ResolvedBones);            // an asset on this skeleton: its nodes folded into this skeleton's joints
            }

            // the skeleton frames of every raw clip: what layers compose from, and the editor's live preview reads the same (LayerSources.bin)
            m_SourceFrames.clear();
            for (const auto& Raw : m_RawClips)
                m_SourceFrames.push_back({ .m_Key = Raw.m_SourceFile, .m_Clip = Raw.m_Anim.m_Name, .m_FPS = std::max(1, Raw.m_Anim.m_FPS), .m_nFrames = Raw.m_Anim.m_nFrames, .m_Frames = SkeletonFrames(Raw, Manifest) });
            if (!xanim_package_layers::WriteSourcesCache(std::format(L"{}\\LayerSources.bin", m_ResourceLogPath), m_SourceFrames, static_cast<int>(ResolvedBones.size())))
                LogMessage(xresource_pipeline::msg_type::WARNING, "The layer-sources cache (LayerSources.bin) could not be written: the editor cannot preview layers live");

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
                if (auto Err = BuildCompiledClip(Raw, pOv, Manifest, Skip, Compiled, AllKeyFrames, AllRootMotion, bLayers ? &Composed : nullptr); Err)
                    return Err;
                if (Skip) continue;

                Names.push_back((pOv && !pOv->m_Name.empty()) ? pOv->m_Name : Raw.m_Anim.m_Name);
                Clips.emplace_back(std::move(Compiled));
            }

            if (Clips.empty())
                return xerr::create_f<state, "No clips survived import/ignore - nothing to compile">();

            if (auto Err = CheckClipNameCollisions(Clips, Names); Err) return Err;

            m_FinalPackage.m_nClips = static_cast<std::uint16_t>(Clips.size());
            // zeroed, then field by field: the file's bytes follow from the content only (xserializer copies a struct's raw bytes before its
            // fields, so padding - after the enum, the vector's unused lane - would carry whatever memory held)
            m_FinalPackage.m_pClips = new xanim_package::clip[Clips.size()];
            std::memset(static_cast<void*>(m_FinalPackage.m_pClips), 0, sizeof(xanim_package::clip) * Clips.size());
            for (std::size_t i = 0; i < Clips.size(); ++i)
            {
                auto& D = m_FinalPackage.m_pClips[i]; const auto& S = Clips[i];
                D.m_NameHash = S.m_NameHash; D.m_FPS = S.m_FPS; D.m_nFrames = S.m_nFrames; D.m_bLoop = S.m_bLoop; D.m_RootMotionMode = S.m_RootMotionMode;
                D.m_LoopDisplacement.m_X = S.m_LoopDisplacement.m_X; D.m_LoopDisplacement.m_Y = S.m_LoopDisplacement.m_Y; D.m_LoopDisplacement.m_Z = S.m_LoopDisplacement.m_Z;
                D.m_iFirstKeyFrame = S.m_iFirstKeyFrame; D.m_iFirstRootMotion = S.m_iFirstRootMotion;
            }

            m_FinalPackage.m_nBones = static_cast<std::uint32_t>(ResolvedBones.size());
            m_FinalPackage.m_pBoneNameHashes = new std::uint32_t[ResolvedBones.size()];
            for (std::size_t i = 0; i < ResolvedBones.size(); ++i)
                m_FinalPackage.m_pBoneNameHashes[i] = ResolvedBones[i].m_NameHash;

            m_FinalPackage.m_nTotalKeyFrames = static_cast<std::uint32_t>(AllKeyFrames.size());
            m_FinalPackage.m_pKeyFrame = new xmath::transform3[AllKeyFrames.size()];
            std::copy(AllKeyFrames.begin(), AllKeyFrames.end(), m_FinalPackage.m_pKeyFrame);

            m_FinalPackage.m_nTotalRootMotionFrames = static_cast<std::uint32_t>(AllRootMotion.size());
            // an empty array is nullptr: new T[0] is a real heap address, and xserializer wrote it into the file as 8 raw bytes (count 0) - two
            // compiles of the same package without root motion differed by those 8 bytes (2026-11-15)
            m_FinalPackage.m_pRootMotion = AllRootMotion.empty() ? nullptr : new xmath::fvec3[AllRootMotion.size()];
            std::copy(AllRootMotion.begin(), AllRootMotion.end(), m_FinalPackage.m_pRootMotion);

            // the content's fingerprint, field by field (never the struct bytes: padding): two compiles of the same input print the same numbers
            {
                std::uint64_t H = 1469598103934665603ull;
                auto Mix = [&H](const void* p, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { H ^= static_cast<const std::uint8_t*>(p)[i]; H *= 1099511628211ull; } };
                auto F = [&](float v) { Mix(&v, sizeof(v)); };
                std::uint64_t HClips, HKeys, HRoot;
                H = 1469598103934665603ull;
                for (const auto& C : Clips) { Mix(&C.m_NameHash, 4); Mix(&C.m_FPS, 4); Mix(&C.m_nFrames, 4); Mix(&C.m_bLoop, 1); Mix(&C.m_RootMotionMode, 1); F(C.m_LoopDisplacement.m_X); F(C.m_LoopDisplacement.m_Y); F(C.m_LoopDisplacement.m_Z); Mix(&C.m_iFirstKeyFrame, 4); Mix(&C.m_iFirstRootMotion, 4); }
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
