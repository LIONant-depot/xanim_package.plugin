#include "xanim_package_compiler.h"

#include "dependencies/xraw3d/source/xraw3d.h"
#include "dependencies/xraw3d/source/details/xraw3d_assimp_import_v3.h"

#include "dependencies/xstrtool/source/xstrtool.h"

#include "../xanim_package_descriptor.h"
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
        };

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
                    m_RawClips.push_back(raw_clip{ .m_Anim = std::move(Anim), .m_SourceFile = Source.m_Path });
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
                m_Details.m_Sources.push_back({ .m_Path = Source.m_Path });

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
                auto& SourceMap = Map[Source.m_Path];
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
        // Builds one compiled clip from a raw import + its (possibly null) descriptor override.
        // Returns false (via Skip) if the clip is ignored.
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

            // Apply the skeleton's own author-time PreTransform to the animated root bone's curve -
            // mirrors xskeleton_compiler.cpp's ApplyPreTransform() exactly (root-only; every
            // descendant inherits the correction for free through the normal parent-multiply FK chain
            // at playback). This clip's raw import never passes through the skeleton's own import
            // step, so without this the animated root stays in raw-import space while the skeleton's
            // rest pose (and every unanimated bone's fallback here, both already-corrected via the
            // manifest) sit in engine space - a visible scale/offset mismatch between the two.
            if (bHaveAnimatedRoot
                && (Manifest.m_PreTransformScale != xmath::fvec3::fromOne()
                 || Manifest.m_PreTransformRotationDeg != xmath::fvec3::fromZero()
                 || Manifest.m_PreTransformTranslation != xmath::fvec3::fromZero()))
            {
                xmath::radian3 Rot;
                Rot.m_Roll  = xmath::radian{ xmath::DegToRad(Manifest.m_PreTransformRotationDeg.m_Z) };
                Rot.m_Pitch = xmath::radian{ xmath::DegToRad(Manifest.m_PreTransformRotationDeg.m_X) };
                Rot.m_Yaw   = xmath::radian{ xmath::DegToRad(Manifest.m_PreTransformRotationDeg.m_Y) };
                const xmath::fquat PreQuat(Rot);
                const xmath::fmat4 M(Manifest.m_PreTransformScale, PreQuat, Manifest.m_PreTransformTranslation);

                for (int f = 0; f < OutFrames; ++f)
                {
                    auto& RootKey      = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + iSkelRoot];
                    RootKey.m_Position = M * RootKey.m_Position;
                    RootKey.m_Rotation = PreQuat * RootKey.m_Rotation;
                    RootKey.m_Scale    = Manifest.m_PreTransformScale * RootKey.m_Scale;
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

            // Seamless-loop correction: raw mocap/imported clips almost never end exactly where they
            // started, which pops visibly when a looping clip wraps back to frame 0 (the same problem
            // every major engine's "Loop Pose" / loop-compensation feature exists to fix). Distribute
            // the frame-0-vs-last-frame discrepancy linearly across every frame (t = f/(nFrames-1)) so
            // frame 0 is untouched, the last frame exactly matches frame 0, and everything in between
            // is nudged by an imperceptible, smoothly ramping amount rather than a sudden correction
            // crammed into just the last few frames. A no-op for any bone that's constant across the
            // whole clip (rest-pose fallback, or genuinely static) since First == Last already.
            // Root-motion translation is already pinned flat by the extraction above, so this is a
            // no-op there too - only rotation/scale (and position on non-root bones) actually move.
            if (bLoop && OutFrames > 1)
            {
                for (int s = 0; s < nSkelBones; ++s)
                {
                    const auto  First    = AllKeyFrames[KeyFrameBase + 0 * nSkelBones + s];
                    const auto  Last     = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(OutFrames - 1) * nSkelBones + s];
                    const xmath::fvec3 PosDelta   = First.m_Position - Last.m_Position;
                    const xmath::fvec3 ScaleDelta = First.m_Scale    - Last.m_Scale;
                    const xmath::fquat RotDelta   = First.m_Rotation * Last.m_Rotation.InverseCopy();

                    for (int f = 1; f < OutFrames; ++f)
                    {
                        const float t = static_cast<float>(f) / static_cast<float>(OutFrames - 1);
                        auto& Key = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nSkelBones + s];
                        Key.m_Position += PosDelta * t;
                        Key.m_Scale    += ScaleDelta * t;
                        Key.m_Rotation  = xmath::fquat::Slerp(xmath::fquat::fromIdentity(), RotDelta, t) * Key.m_Rotation;
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

            for (auto& Raw : m_RawClips)
            {
                const xanim_package_desc::clip* pOv = nullptr;
                if (auto SourceIt = Overrides.find(Raw.m_SourceFile); SourceIt != Overrides.end())
                {
                    if (auto ClipIt = SourceIt->second.find(Raw.m_Anim.m_Name); ClipIt != SourceIt->second.end())
                        pOv = ClipIt->second;
                }

                bool                  Skip = false;
                xanim_package::clip   Compiled{};
                if (auto Err = BuildCompiledClip(Raw, pOv, Manifest, Skip, Compiled, AllKeyFrames, AllRootMotion); Err)
                    return Err;
                if (Skip) continue;

                Names.push_back((pOv && !pOv->m_Name.empty()) ? pOv->m_Name : Raw.m_Anim.m_Name);
                Clips.emplace_back(std::move(Compiled));
            }

            if (Clips.empty())
                return xerr::create_f<state, "No clips survived import/ignore - nothing to compile">();

            if (auto Err = CheckClipNameCollisions(Clips, Names); Err) return Err;

            m_FinalPackage.m_nClips = static_cast<std::uint16_t>(Clips.size());
            m_FinalPackage.m_pClips = new xanim_package::clip[Clips.size()];
            std::copy(Clips.begin(), Clips.end(), m_FinalPackage.m_pClips);

            m_FinalPackage.m_nBones = static_cast<std::uint32_t>(ResolvedBones.size());
            m_FinalPackage.m_pBoneNameHashes = new std::uint32_t[ResolvedBones.size()];
            for (std::size_t i = 0; i < ResolvedBones.size(); ++i)
                m_FinalPackage.m_pBoneNameHashes[i] = ResolvedBones[i].m_NameHash;

            m_FinalPackage.m_nTotalKeyFrames = static_cast<std::uint32_t>(AllKeyFrames.size());
            m_FinalPackage.m_pKeyFrame = new xmath::transform3[AllKeyFrames.size()];
            std::copy(AllKeyFrames.begin(), AllKeyFrames.end(), m_FinalPackage.m_pKeyFrame);

            m_FinalPackage.m_nTotalRootMotionFrames = static_cast<std::uint32_t>(AllRootMotion.size());
            m_FinalPackage.m_pRootMotion = new xmath::fvec3[AllRootMotion.size()];
            std::copy(AllRootMotion.begin(), AllRootMotion.end(), m_FinalPackage.m_pRootMotion);

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
