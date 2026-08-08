#ifndef XANIM_PACKAGE_DESCRIPTOR_H
#define XANIM_PACKAGE_DESCRIPTOR_H
#pragma once

#include "plugins/xskeleton.plugin/source/xskeleton_xgpu_rsc_loader.h"
#include "plugins/xskeleton.plugin/source/xskeleton_bone_manifest.h"
#include "xanim_package.h"
#include "xanim_package_details.h"
#include <functional>
#include <format>

namespace xanim_package_desc
{
    inline static constexpr auto    resource_type_guid_v    = xresource::type_guid(xresource::guid_generator::Instance64FromString("xAnimPackage"));

    static constexpr wchar_t        mesh_filter_v[]         = L"Animation\0 *.fbx; *.obj\0Any Thing\0 *.*\0";

    static constexpr auto root_motion_mode_v = std::array
    { xproperty::settings::enum_item("None",    xanim_package::root_motion_mode::NONE)
    , xproperty::settings::enum_item("XZ Only", xanim_package::root_motion_mode::XZ_ONLY)
    , xproperty::settings::enum_item("XYZ",     xanim_package::root_motion_mode::XYZ)
    };

    // One entry in the "official list" of source files this package collects clips from - a single
    // FBX can contain multiple baked-in animation clips, all of which land in `details` for the
    // descriptor's sparse per-clip overrides (below) to curate.
    struct import_source
    {
        std::wstring    m_Path  = {};

        XPROPERTY_DEF
        ( "ImportSource", import_source
        , obj_member<"Path",    &import_source::m_Path, member_ui<std::wstring>::file_dialog<mesh_filter_v, true, 1> >
        )
    };
    XPROPERTY_REG(import_source)

    // A sparse override entry for one imported clip, matched BY NAME against details::m_ClipList -
    // same by-name-matching pattern as xskeleton's bone tree, but flat (clips have no hierarchy, so
    // no reparenting concerns).
    struct clip
    {
        std::string                       m_Name          = {};   // raw imported name - SHOW_READONLY, stable match key across re-imports
        std::string                       m_Rename        = {};   // if set, THIS gets CRC32'd into the compiled clip's name hash
        bool                              m_bIgnore       = false;// excluded from the compiled output entirely - still visible in Details, so it can be re-enabled later
        bool                              m_bLoop         = false;
        int                               m_TargetFPS     = 0;    // 0 = keep the imported base FPS; must be <= imported (no upsampling - nothing to invent)
        float                             m_TrimStartTime = -1.f; // seconds, in the FINAL (post-resample) domain; -1 = from the start
        float                             m_TrimEndTime   = -1.f; // seconds; -1 = to the end
        xanim_package::root_motion_mode   m_RootMotion    = xanim_package::root_motion_mode::NONE;

        XPROPERTY_DEF
        ( "Clip", clip
        , obj_member<"Name",          &clip::m_Name, member_flags< flags::SHOW_READONLY> >
        , obj_member<"Rename",        &clip::m_Rename >
        , obj_member<"Ignore",        &clip::m_bIgnore >
        , obj_member<"Loop",          &clip::m_bLoop >
        , obj_member<"TargetFPS",     &clip::m_TargetFPS >
        , obj_member<"TrimStartTime", &clip::m_TrimStartTime >
        , obj_member<"TrimEndTime",   &clip::m_TrimEndTime >
        , obj_member<"RootMotion",    &clip::m_RootMotion, member_enum_span<root_motion_mode_v> >
        )
    };
    XPROPERTY_REG(clip)

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

        int findClip(std::string_view Name) const noexcept
        {
            for (auto& E : m_Clips)
                if (E.m_Name == Name) return static_cast<int>(&E - m_Clips.data());
            return -1;
        }

        // Reconciles the sparse per-clip override list against a freshly (re-)imported set of clips
        // (details) - same job as xskeleton_desc::descriptor::MergeWithDetails, but flat (no tree/
        // reparenting): a clip missing from a fresh import gets its override pruned (with a warning),
        // a newly-found clip gets a fresh, un-curated override entry appended.
        std::vector<std::string> MergeWithDetails(const details& Details)
        {
            std::vector<std::string> Messages;

            for (int i = 0; i < static_cast<int>(m_Clips.size()); ++i)
            {
                if (Details.findClip(m_Clips[i].m_Name) != -1) continue;

                Messages.push_back(std::format("WARNING: Clip [{}] no longer found in the imported sources - removing its override.", m_Clips[i].m_Name));
                m_Clips.erase(m_Clips.begin() + i);
                --i;
            }

            for (auto& Src : Details.m_ClipList)
            {
                if (findClip(Src.m_Name) == -1)
                    m_Clips.push_back(clip{ .m_Name = Src.m_Name });
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

        std::vector<import_source>     m_ImportSources         = {};   // the "official list" of source files
        xrsc::skeleton                  m_SkeletonRef           = {};   // required - Validate() errors if empty
        std::vector<clip>               m_Clips                 = {};   // sparse per-clip overrides

        xskeleton_desc::bone_manifest   m_ResolvedSkeletonBones = {};   // NOT saved - populated in Serialize() above

        XPROPERTY_VDEF
        ( "AnimPackage", descriptor
        , obj_member<"ImportSources",           &descriptor::m_ImportSources >
        , obj_member<"SkeletonRef",             &descriptor::m_SkeletonRef >
        , obj_member<"Clips",                   &descriptor::m_Clips >
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
