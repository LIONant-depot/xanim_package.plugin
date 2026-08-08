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
#include <unordered_set>
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
        void ComputeDetailStructure()
        {
            m_Details.m_ClipList.clear();
            m_Details.m_NumSourceFiles = static_cast<int>(m_Descriptor.m_ImportSources.size());

            for (auto& Raw : m_RawClips)
            {
                auto& D = m_Details.m_ClipList.emplace_back();
                D.m_Name              = Raw.m_Anim.m_Name;
                D.m_SourceFile        = Raw.m_SourceFile;
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
            m_Details.m_NumClips = static_cast<int>(m_Details.m_ClipList.size());
        }

        //--------------------------------------------------------------------------------------

        void CollectOverrides(std::unordered_map<std::string, const xanim_package_desc::clip*>& Map)
        {
            for (auto& C : m_Descriptor.m_Clips)
                Map[C.m_Name] = &C;
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
        // Returns false (via Skip) if the clip is ignored. Bone identity is hashed from the RAW
        // imported bone name - if the referenced skeleton later renames a bone (xskeleton_desc::
        // bone::m_Rename), this clip's hash for that bone will no longer match the skeleton's
        // compiled hash and it simply won't bind at runtime. Retargeting across a rename is out of
        // scope for this pass; BuildCompiledClip only warns when a raw bone name isn't found in the
        // referenced skeleton's manifest at all.
        xerr BuildCompiledClip
        ( const raw_clip&                         Raw
        , const xanim_package_desc::clip*         pOv
        , const std::unordered_set<std::string>&  SkeletonBoneNames
        , bool&                                   Skip
        , xanim_package::clip&                    Out
        , std::vector<std::uint32_t>&             AllBoneHashes
        , std::vector<xmath::transform3>&         AllKeyFrames
        , std::vector<xmath::fvec3>&               AllRootMotion
        )
        {
            Skip = pOv && pOv->m_bIgnore;
            if (Skip) return {};

            const std::string  CompiledName = (pOv && !pOv->m_Rename.empty()) ? pOv->m_Rename : Raw.m_Anim.m_Name;
            const bool          bLoop        = pOv && pOv->m_bLoop;
            const auto          RootMotion   = pOv ? pOv->m_RootMotion : xanim_package::root_motion_mode::NONE;
            const int           TargetFPS    = pOv ? pOv->m_TargetFPS : 0;
            const float         TrimStart    = pOv ? pOv->m_TrimStartTime : -1.f;
            const float         TrimEnd      = pOv ? pOv->m_TrimEndTime : -1.f;

            const int SourceFPS     = Raw.m_Anim.m_FPS;
            const int SourceFrames  = Raw.m_Anim.m_nFrames;
            const int nBones        = static_cast<int>(Raw.m_Anim.m_Bone.size());

            if (TargetFPS > SourceFPS)
                LogMessage(xresource_pipeline::msg_type::WARNING
                    , std::format("Clip '{}' asked for a target FPS ({}) higher than the imported rate ({}) - keeping the imported rate (no upsampling).", CompiledName, TargetFPS, SourceFPS));

            const int FinalFPS = (TargetFPS > 0 && TargetFPS <= SourceFPS) ? TargetFPS : SourceFPS;
            const int ResampledFrameCount = (FinalFPS == SourceFPS) ? SourceFrames : std::max(1, static_cast<int>(std::lround(static_cast<double>(SourceFrames) * FinalFPS / SourceFPS)));

            // Trim bounds are evaluated in the FINAL (post-resample) frame domain.
            const int StartFrame = (TrimStart < 0.f) ? 0 : std::clamp(static_cast<int>(std::lround(TrimStart * FinalFPS)), 0, ResampledFrameCount - 1);
            const int EndFrame   = (TrimEnd   < 0.f) ? (ResampledFrameCount - 1) : std::clamp(static_cast<int>(std::lround(TrimEnd * FinalFPS)), StartFrame, ResampledFrameCount - 1);
            const int OutFrames  = EndFrame - StartFrame + 1;

            Out.m_NameHash        = xstrtool::CRC32(CompiledName);
            Out.m_FPS             = FinalFPS;
            Out.m_nFrames         = OutFrames;
            Out.m_bLoop           = bLoop;
            Out.m_RootMotionMode  = RootMotion;
            Out.m_nBones          = static_cast<std::uint16_t>(nBones);
            Out.m_iFirstBone      = static_cast<std::uint32_t>(AllBoneHashes.size());
            Out.m_iFirstKeyFrame  = static_cast<std::uint32_t>(AllKeyFrames.size());
            Out.m_iFirstRootMotion = static_cast<std::uint32_t>(AllRootMotion.size());
            Out.m_LoopDisplacement = xmath::fvec3::fromZero();

            for (int b = 0; b < nBones; ++b)
            {
                const auto& Name = Raw.m_Anim.m_Bone[b].m_Name;
                AllBoneHashes.push_back(xstrtool::CRC32(Name));

                if (SkeletonBoneNames.find(Name) == SkeletonBoneNames.end())
                    LogMessage(xresource_pipeline::msg_type::WARNING
                        , std::format("Clip '{}' references bone '{}' which was not found in the referenced skeleton.", CompiledName, Name));
            }

            const std::size_t KeyFrameBase = AllKeyFrames.size();
            AllKeyFrames.resize(KeyFrameBase + static_cast<std::size_t>(OutFrames) * nBones);
            for (int f = 0; f < OutFrames; ++f)
            {
                const int SrcFrame = std::clamp(ResampleFrameIndex(StartFrame + f, SourceFPS, FinalFPS), 0, SourceFrames - 1);
                for (int b = 0; b < nBones; ++b)
                    AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nBones + b] = Raw.m_Anim.m_KeyFrame[static_cast<std::size_t>(SrcFrame) * nBones + b];
            }

            if (RootMotion != xanim_package::root_motion_mode::NONE)
            {
                const int iRoot = FindRootBone(Raw.m_Anim);
                if (iRoot == -1)
                {
                    LogMessage(xresource_pipeline::msg_type::WARNING
                        , std::format("Clip '{}' asked for root-motion extraction but no root bone (m_iParent==-1) was found - skipping extraction.", CompiledName));
                }
                else
                {
                    const std::size_t RootMotionBase = AllRootMotion.size();
                    AllRootMotion.resize(RootMotionBase + OutFrames);

                    const auto Frame0Pos = AllKeyFrames[KeyFrameBase + 0 * nBones + iRoot].m_Position;

                    for (int f = 0; f < OutFrames; ++f)
                    {
                        auto& RootKey = AllKeyFrames[KeyFrameBase + static_cast<std::size_t>(f) * nBones + iRoot];
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
            std::unordered_map<std::string, const xanim_package_desc::clip*> Overrides;
            CollectOverrides(Overrides);

            std::unordered_set<std::string> SkeletonBoneNames;
            for (auto& B : m_Descriptor.m_ResolvedSkeletonBones.m_Bones)
                SkeletonBoneNames.insert(B.m_Name);

            std::vector<xanim_package::clip>       Clips;
            std::vector<std::string>               Names;
            std::vector<std::uint32_t>              AllBoneHashes;
            std::vector<xmath::transform3>          AllKeyFrames;
            std::vector<xmath::fvec3>               AllRootMotion;

            for (auto& Raw : m_RawClips)
            {
                auto  It  = Overrides.find(Raw.m_Anim.m_Name);
                auto* pOv = (It == Overrides.end()) ? nullptr : It->second;

                bool                  Skip = false;
                xanim_package::clip   Compiled{};
                if (auto Err = BuildCompiledClip(Raw, pOv, SkeletonBoneNames, Skip, Compiled, AllBoneHashes, AllKeyFrames, AllRootMotion); Err)
                    return Err;
                if (Skip) continue;

                Names.push_back((pOv && !pOv->m_Rename.empty()) ? pOv->m_Rename : Raw.m_Anim.m_Name);
                Clips.emplace_back(std::move(Compiled));
            }

            if (Clips.empty())
                return xerr::create_f<state, "No clips survived import/ignore - nothing to compile">();

            if (auto Err = CheckClipNameCollisions(Clips, Names); Err) return Err;

            m_FinalPackage.m_nClips = static_cast<std::uint16_t>(Clips.size());
            m_FinalPackage.m_pClips = new xanim_package::clip[Clips.size()];
            std::copy(Clips.begin(), Clips.end(), m_FinalPackage.m_pClips);

            m_FinalPackage.m_nTotalBones = static_cast<std::uint32_t>(AllBoneHashes.size());
            m_FinalPackage.m_pBoneNameHashes = new std::uint32_t[AllBoneHashes.size()];
            std::copy(AllBoneHashes.begin(), AllBoneHashes.end(), m_FinalPackage.m_pBoneNameHashes);

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
