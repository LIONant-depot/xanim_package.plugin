#ifndef XANIM_PACKAGE_DESCRIPTOR_H
#define XANIM_PACKAGE_DESCRIPTOR_H
#pragma once

#include "plugins/xskeleton.plugin/source/xskeleton_xgpu_rsc_loader.h"
#include "plugins/xskeleton.plugin/source/xskeleton_bone_manifest.h"
#include "xanim_package.h"
#include "xanim_package_details.h"
#include "dependencies/xstrtool/source/xstrtool.h"
#include <functional>
#include <format>
#include <algorithm>

namespace xanim_package_desc
{
    inline static constexpr auto    resource_type_guid_v    = xresource::type_guid(xresource::guid_generator::Instance64FromString("xAnimPackage"));

    static constexpr wchar_t        mesh_filter_v[]         = L"Animation\0 *.fbx; *.obj\0Any Thing\0 *.*\0";

    static constexpr auto root_motion_mode_v = std::array
    { xproperty::settings::enum_item("None",    xanim_package::root_motion_mode::NONE)
    , xproperty::settings::enum_item("XZ Only", xanim_package::root_motion_mode::XZ_ONLY)
    , xproperty::settings::enum_item("XYZ",     xanim_package::root_motion_mode::XYZ)
    };

    // A sparse override entry for one imported clip, matched BY m_OriginalName against its OWNING
    // import_source's own details::source::m_ClipList - a raw clip's name is only ever guaranteed
    // unique WITHIN the file it came from (see xanim_package_details.h's own comment on why), so this
    // lives nested inside import_source below, never in a flat cross-file list.
    struct clip
    {
        std::string                       m_OriginalName   = {};   // raw imported name - the stable match key across re-imports; never shown or edited directly, only ever read by MergeWithDetails/findClip
        std::string                       m_Name           = {};   // user-facing/compiled name - initialized to m_OriginalName when this entry is first created (see MergeWithDetails), freely renamable afterward; THIS gets CRC32'd into the compiled clip's name hash
        bool                              m_bDelete        = false;// excluded from the compiled output entirely - still visible in Details, so it can be re-enabled later
        bool                              m_bLoop          = false;
        int                               m_DownsampleFPS  = 0;    // 0 = keep the imported rate (the importer always samples at 60fps) - lowering this (e.g. to 30) only ever trades quality for memory, never fixes anything; no upsampling, nothing to invent above the imported rate
        int                               m_TrimStartFrame = -1;   // frame index, in the FINAL (post-resample) domain; -1 = from the start
        int                               m_TrimEndFrame   = -1;   // frame index; -1 = to the end
        xanim_package::root_motion_mode   m_RootMotion     = xanim_package::root_motion_mode::NONE;

        XPROPERTY_DEF
        ( "Clip", clip
        , obj_member<"OriginalName",    &clip::m_OriginalName, member_flags<flags::SHOW_READONLY> >
        , obj_member<"Name",            &clip::m_Name >
        , obj_member<"Delete",          &clip::m_bDelete >
        , obj_member<"Loop",            &clip::m_bLoop >
        , obj_member<"DownsampleFPS",   &clip::m_DownsampleFPS >
        , obj_member<"TrimStartFrame",  &clip::m_TrimStartFrame >
        , obj_member<"TrimEndFrame",    &clip::m_TrimEndFrame >
        , obj_member<"RootMotion",      &clip::m_RootMotion, member_enum_span<root_motion_mode_v> >
        )
    };
    XPROPERTY_REG(clip)

    // One entry in the "official list" of source files this package collects clips from - a single
    // FBX can contain multiple baked-in animation clips, all of which get their own sparse override
    // entry here (see MergeWithDetails), curated per-file rather than in one cross-file pool.
    struct import_source
    {
        std::wstring       m_Path  = {};
        std::vector<clip>  m_Clips = {};   // every clip found in THIS file, curated

        XPROPERTY_DEF
        ( "ImportSource", import_source
        , obj_member<"Path",    &import_source::m_Path, member_ui<std::wstring>::file_dialog<mesh_filter_v, true, 1> >
        , obj_member<"Clips",   &import_source::m_Clips >
        )
    };
    XPROPERTY_REG(import_source)

    struct descriptor : xresource_pipeline::descriptor::base
    {
        using parent = xresource_pipeline::descriptor::base;

        void SetupFromSource(std::string_view FileName) override
        {
        }

        void Validate(std::vector<std::string>& Errors) const noexcept override
        {
            if (m_SkeletonRef.empty())
            {
                Errors.push_back(std::format("The skeleton reference is NULL"));
                return;
            }

            if (m_ImportSources.empty())
            {
                Errors.push_back(std::format("No import sources - nothing to compile"));
            }
        }

        int findImportSource(std::wstring_view Path) const noexcept
        {
            for (auto& E : m_ImportSources)
                if (E.m_Path == Path) return static_cast<int>(&E - m_ImportSources.data());
            return -1;
        }

        // Reconciles each import source's sparse per-clip override list against a freshly
        // (re-)imported set of clips (details) - same job as xskeleton_desc::descriptor::
        // MergeWithDetails, scoped per-file so a clip is only ever matched against clips FROM THE
        // SAME FILE: a clip missing from a fresh import of its own file gets its override pruned
        // (with a warning), a newly-found clip gets a fresh, un-curated override entry appended -
        // m_Name seeded from m_OriginalName so it starts out displaying the imported name, freely
        // renamable afterward. An import source with no matching Details entry (nothing imported from
        // it yet, or the file failed to import) is left untouched - not an error, just nothing to
        // reconcile yet.
        std::vector<std::string> MergeWithDetails(const details& Details)
        {
            std::vector<std::string> Messages;

            for (auto& Source : m_ImportSources)
            {
                const int iDetailsSource = Details.findSource(Source.m_Path);
                if (iDetailsSource == -1) continue;
                const auto& DetailsSource = Details.m_Sources[iDetailsSource];

                for (int i = 0; i < static_cast<int>(Source.m_Clips.size()); ++i)
                {
                    if (DetailsSource.findClip(Source.m_Clips[i].m_OriginalName) != -1) continue;

                    Messages.push_back(std::format("WARNING: Clip [{}] no longer found in [{}] - removing its override.", Source.m_Clips[i].m_OriginalName, xstrtool::To(Source.m_Path)));
                    Source.m_Clips.erase(Source.m_Clips.begin() + i);
                    --i;
                }

                for (auto& Src : DetailsSource.m_ClipList)
                {
                    const bool bFound = std::any_of(Source.m_Clips.begin(), Source.m_Clips.end(), [&](const clip& C) { return C.m_OriginalName == Src.m_Name; });
                    if (!bFound)
                        Source.m_Clips.push_back(clip{ .m_OriginalName = Src.m_Name, .m_Name = Src.m_Name });
                }
            }

            return Messages;
        }

        // Mirrors xmaterial_instance::descriptor::getTemplatePathFromDescriptorPath() exactly - same
        // "find the project root, then the standard GUID-sharding formula" convention, pointed at the
        // referenced Skeleton's own log folder instead of a Descriptors folder. "Skeleton" here is a
        // hardcoded literal matching that plugin's own fixed PipelinePlugin/TypeName.
        xerr getSkeletonBoneManifestPath(std::wstring& Path, std::wstring_view DescriptorFileName) const noexcept
        {
            auto Pos = DescriptorFileName.rfind(L".lionprj");
            if (Pos != std::wstring_view::npos) Pos += sizeof(".lionprj");
            else if (Pos = DescriptorFileName.rfind(L".lionlib"); Pos != std::wstring_view::npos) Pos += sizeof(".lionlib");
            else return xerr::create_f<xresource_pipeline::state, "Unable to locate the project path, fail to load the referenced skeleton's bone manifest">();

            const auto ProjectPath = DescriptorFileName.substr(0, Pos - 1);
            Path = std::format(L"{}/Cache/Resources/Logs/Skeleton/{:02X}/{:02X}/{:X}.log/AnimPackage.txt"
                , ProjectPath
                , m_SkeletonRef.m_Instance.m_Value & 0xff
                , (m_SkeletonRef.m_Instance.m_Value & 0xff00) >> 8
                , m_SkeletonRef.m_Instance.m_Value
            );

            return {};
        }

        xerr Serialize(bool isReading, std::wstring_view FileName, xproperty::settings::context& Context) noexcept override
        {
            if (auto Err = xresource_pipeline::descriptor::base::Serialize(isReading, FileName, Context); Err)
                return Err;

            if (not isReading) return {};
            if (m_SkeletonRef.empty()) return {};

            // Cross-plugin dependency: read the Skeleton compiler's own bone-manifest export (its
            // final, post-merge bone order/hashes) - not its compiled binary. RunAfter in this
            // plugin's Plugin.config guarantees this file already exists by the time we get here.
            std::wstring ManifestPath;
            if (auto Err = getSkeletonBoneManifestPath(ManifestPath, FileName); Err)
                return Err;

            xtextfile::stream                File;
            xproperty::settings::context    C;

            if (auto Err = File.Open(true, ManifestPath, xtextfile::file_type::TEXT); Err)
                return xerr::create_f<xresource_pipeline::state, "Error opening the referenced skeleton's bone manifest - has it been compiled yet?">(Err);

            if (auto Err = xproperty::sprop::serializer::Stream(File, m_ResolvedSkeletonBones, C); Err)
                return xerr::create_f<xresource_pipeline::state, "Error reading the referenced skeleton's bone manifest">(Err);

            return {};
        }

        std::vector<import_source>     m_ImportSources         = {};   // the "official list" of source files - each owns its own clips now (see import_source)
        xrsc::skeleton                  m_SkeletonRef           = {};   // required - Validate() errors if empty

        xskeleton_desc::bone_manifest   m_ResolvedSkeletonBones = {};   // NOT saved - populated in Serialize() above

        XPROPERTY_VDEF
        ( "AnimPackage", descriptor
        , obj_member<"ImportSources",           &descriptor::m_ImportSources >
        , obj_member<"SkeletonRef",             &descriptor::m_SkeletonRef >
        , obj_member<"ResolvedSkeletonBones",   &descriptor::m_ResolvedSkeletonBones, member_flags<flags::DONT_SHOW, flags::DONT_SAVE> >
        )
    };
    XPROPERTY_VREG(descriptor)

    //--------------------------------------------------------------------------------------

    struct factory final : xresource_pipeline::factory_base
    {
        using xresource_pipeline::factory_base::factory_base;

        std::unique_ptr<xresource_pipeline::descriptor::base> CreateDescriptor(void) const noexcept override
        {
            return std::make_unique<descriptor>();
        };

        xresource::type_guid ResourceTypeGUID(void) const noexcept override
        {
            return resource_type_guid_v;
        }

        const char* ResourceTypeName(void) const noexcept override
        {
            return "AnimPackage";
        }

        const xproperty::type::object& ResourceXPropertyObject(void) const noexcept override
        {
            return *xproperty::getObjectByType<descriptor>();
        }
    };

    inline static factory g_Factory{};
}
#endif
