#ifndef XANIM_PACKAGE_DESCRIPTOR_H
#define XANIM_PACKAGE_DESCRIPTOR_H
#pragma once

#include "plugins/xskeleton.plugin/source/xskeleton_xgpu_rsc_loader.h"
#include "plugins/xskeleton.plugin/source/xskeleton_bone_manifest.h"
#include "plugins/xretarget.plugin/source/xretarget_ref.h"
#include "xanim_package.h"
#include "xanim_package_xgpu_rsc_loader.h"
#include "xanim_package_details.h"
#include "dependencies/xstrtool/source/xstrtool.h"
#include <functional>
#include <format>
#include <algorithm>

namespace xanim_package_desc
{
    inline static constexpr auto    resource_type_guid_v    = xresource::type_guid(xresource::guid_generator::Instance64FromString("xAnimPackage"));

    static constexpr wchar_t        mesh_filter_v[]         = L"Animation\0 *.fbx; *.obj; *.blend\0Any Thing\0 *.*\0";

    static constexpr auto root_motion_mode_v = std::array
    { xproperty::settings::enum_item("None",    xanim_package::root_motion_mode::NONE)
    , xproperty::settings::enum_item("XZ Only", xanim_package::root_motion_mode::XZ_ONLY)
    , xproperty::settings::enum_item("XYZ",     xanim_package::root_motion_mode::XYZ)
    };

    // Where an import source's animations come from (Tomas 2026-11-15). Each entry of the package's input list says which:
    //   an animation ASSET (an fbx or any file the importer reads) made on THIS package's skeleton, or on ANOTHER skeleton (then a Retarget resource moves it);
    //   another animation PACKAGE (its descriptor lists the animations, its compiled clips are the data) on this package's skeleton, or on another (a Retarget again).
    enum class import_kind : std::uint8_t { SAME_SKELETON_ASSET, DIFFERENT_SKELETON_ASSET, SAME_SKELETON_PACKAGE, DIFFERENT_SKELETON_PACKAGE };
    static constexpr auto import_kind_v = std::array
    { xproperty::settings::enum_item("Same skeleton: animation asset",         import_kind::SAME_SKELETON_ASSET)
    , xproperty::settings::enum_item("Different skeleton: animation asset",    import_kind::DIFFERENT_SKELETON_ASSET)
    , xproperty::settings::enum_item("Same skeleton: animation package",       import_kind::SAME_SKELETON_PACKAGE)
    , xproperty::settings::enum_item("Different skeleton: animation package",  import_kind::DIFFERENT_SKELETON_PACKAGE)
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
        import_kind        m_Kind    = import_kind::SAME_SKELETON_ASSET;
        std::wstring       m_Path    = {};      // the animation asset (the two asset kinds)
        xrsc::anim_package m_Package = {};      // the animation package (the two package kinds): its descriptor lists the animations, its compiled clips are the data
        xrsc::retarget     m_Retarget = {};     // the two DIFFERENT kinds: the Retarget from the skeleton the animations were made on to this package's skeleton
        std::vector<clip>  m_Clips   = {};      // every clip found in THIS source, curated

        // The kind as it counts: a source saved before kinds existed that names a Retarget is a different-skeleton asset
        import_kind kind(void) const noexcept { return (m_Kind == import_kind::SAME_SKELETON_ASSET && !m_Retarget.empty()) ? import_kind::DIFFERENT_SKELETON_ASSET : m_Kind; }
        bool bAsset  (void) const noexcept { return kind() == import_kind::SAME_SKELETON_ASSET || kind() == import_kind::DIFFERENT_SKELETON_ASSET; }
        bool bPackage(void) const noexcept { return !bAsset(); }
        bool bDifferent(void) const noexcept { return kind() == import_kind::DIFFERENT_SKELETON_ASSET || kind() == import_kind::DIFFERENT_SKELETON_PACKAGE; }

        // What names this source in the details (the compiler's list of what each source holds): the file, or the package
        std::wstring key(void) const { return bAsset() ? m_Path : std::format(L"package:{:X}", m_Package.m_Instance.m_Value); }

        XPROPERTY_DEF
        ( "ImportSource", import_source
        , obj_member<"Type", &import_source::m_Kind, member_enum_span<import_kind_v>, member_help<"Where this source's animations come from: an animation asset or another animation package, made on this package's skeleton or on another one (then a Retarget moves them)">>
        , obj_member<"Path", &import_source::m_Path, member_ui<std::wstring>::file_dialog<mesh_filter_v, true, 1>
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bAsset(); return F; }> >
        , obj_member<"Package", &import_source::m_Package, member_help<"The animation package whose animations are brought in (its descriptor lists them)">
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bPackage(); return F; }> >
        , obj_member<"Retarget", &import_source::m_Retarget, member_help<"The Retarget resource that moves these animations from the skeleton they were made on (its Source) to this package's skeleton (its Destination)">
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bDifferent(); return F; }> >
        , obj_member<"Clips",   &import_source::m_Clips >
        )
    };
    XPROPERTY_REG(import_source)

    // Generating clips from text with NVIDIA Kimodo (the editor's Kimodo panel, through xkimodo.exe): the generated motion is on Kimodo's
    // SOMA 77-bone skeleton, so it needs a Retarget from that skeleton to this package's; an accepted clip is saved as a .bvh in Folder and
    // added as a "Different skeleton: animation asset" source with that Retarget. Nothing here changes what the package compiles.
    static constexpr wchar_t        folder_filter_v[]       = L"All\0*\0";
    struct kimodo_settings
    {
        xrsc::retarget  m_Retarget = {};
        std::wstring    m_Folder   = L"Assets\\Kimodo";

        XPROPERTY_DEF
        ( "Kimodo", kimodo_settings
        , obj_member<"Retarget", &kimodo_settings::m_Retarget, member_help<"The Retarget from Kimodo's SOMA 77-bone skeleton (its Source) to this package's skeleton (its Destination): generated clips are moved with it">>
        , obj_member<"Folder",   &kimodo_settings::m_Folder, member_ui<std::wstring>::folder_dialog<folder_filter_v, true, 1>, member_help<"The asset folder an accepted generated clip is saved in (as a .bvh, with a .kimodo.txt beside it: the prompt, seed and length)">>
        )
    };
    XPROPERTY_REG(kimodo_settings)

    struct descriptor : xresource_pipeline::descriptor::base
    {
        using parent = xresource_pipeline::descriptor::base;

        void SetupFromSource(std::string_view FileName) override
        {
        }

        std::string SuggestedName() const noexcept override { return m_ImportSources.empty() ? std::string{} : NameOfFile(m_ImportSources.front().m_Path); }

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
            for (int i = 0; i < static_cast<int>(m_ImportSources.size()); ++i)
            {
                const auto& S = m_ImportSources[i];
                if (S.bAsset() && S.m_Path.empty())           Errors.push_back(std::format("Import source {}: an animation asset kind needs its Path", i));
                if (S.bPackage() && S.m_Package.empty())      Errors.push_back(std::format("Import source {}: an animation package kind needs its Package", i));
                if (S.bDifferent() && S.m_Retarget.empty())   Errors.push_back(std::format("Import source {}: a different-skeleton kind needs its Retarget", i));
            }
        }

        int findImportSource(std::wstring_view Path) const noexcept
        {
            for (auto& E : m_ImportSources)
                if (E.key() == Path) return static_cast<int>(&E - m_ImportSources.data());
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
                const int iDetailsSource = Details.findSource(Source.key());
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
        kimodo_settings                 m_Kimodo                = {};   // editor only: where generated clips go and how they are retargeted

        xskeleton_desc::bone_manifest   m_ResolvedSkeletonBones = {};   // NOT saved - populated in Serialize() above

        XPROPERTY_VDEF
        ( "AnimPackage", descriptor
        , obj_member<"SkeletonRef",             &descriptor::m_SkeletonRef >          // first: the skeleton every source ends up on
        , obj_member<"ImportSources",           &descriptor::m_ImportSources >
        , obj_member<"Kimodo",                  &descriptor::m_Kimodo >
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
