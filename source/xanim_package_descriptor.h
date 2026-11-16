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
#include <map>
#include <limits>

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
    // NEW_ANIMATION (keyframe batch, 2026-11-16): no file - the skeleton's rest pose held for Seconds at FPS, named by Path: a clip made from nothing, keyed with Pose layers
    enum class import_kind : std::uint8_t { SAME_SKELETON_ASSET, DIFFERENT_SKELETON_ASSET, SAME_SKELETON_PACKAGE, DIFFERENT_SKELETON_PACKAGE, NEW_ANIMATION };
    static constexpr auto import_kind_v = std::array
    { xproperty::settings::enum_item("Same skeleton: animation asset",         import_kind::SAME_SKELETON_ASSET)
    , xproperty::settings::enum_item("Different skeleton: animation asset",    import_kind::DIFFERENT_SKELETON_ASSET)
    , xproperty::settings::enum_item("Same skeleton: animation package",       import_kind::SAME_SKELETON_PACKAGE)
    , xproperty::settings::enum_item("Different skeleton: animation package",  import_kind::DIFFERENT_SKELETON_PACKAGE)
    , xproperty::settings::enum_item("New animation (rest pose)",              import_kind::NEW_ANIMATION)
    };

    //----------------------------------------------------------------------------------------------
    // Animation layers (the layered edit mode, xLION_Docs animation_layered_edit_design_v2_2026-11-15.md; evaluation: xAnimation
    // blend/xanimation_layers.h). A clip's layers sit on top of the clip as imported (its SOURCE, never a compiled output) and are baked into
    // it by the compile, before its trim / downsample / root motion. A clip with no layers compiles exactly as before.
    // L1: Clip layers only; a layer's source is a clip of one of THIS package's import sources (so a file, a Retargeted file or another
    // package's clips: whatever the package imports); masks are bone entries (skeleton regions later).
    //----------------------------------------------------------------------------------------------
    enum class layer_mode : std::uint8_t { OVERRIDE, ADDITIVE };
    static constexpr auto layer_mode_v = std::array
    { xproperty::settings::enum_item("Override", layer_mode::OVERRIDE)
    , xproperty::settings::enum_item("Additive", layer_mode::ADDITIVE)
    };
    enum class additive_reference : std::uint8_t { REST_POSE, SOURCE_FRAME };
    static constexpr auto additive_reference_v = std::array
    { xproperty::settings::enum_item("Rest pose",             additive_reference::REST_POSE)
    , xproperty::settings::enum_item("A frame of the source", additive_reference::SOURCE_FRAME)
    };
    enum class layer_root : std::uint8_t { INHERIT_BELOW, FROM_LAYER };
    static constexpr auto layer_root_v = std::array
    { xproperty::settings::enum_item("Inherit below", layer_root::INHERIT_BELOW)
    , xproperty::settings::enum_item("From layer",    layer_root::FROM_LAYER)
    };

    enum class layer_kind : std::uint8_t { CLIP, POSE, FOLDER, GROUP, KIMODO };      // FOLDER: pass-through organization; GROUP: isolated (contract v3)
    static constexpr auto layer_kind_v = std::array
    { xproperty::settings::enum_item("Clip", layer_kind::CLIP)
    , xproperty::settings::enum_item("Pose", layer_kind::POSE)
    , xproperty::settings::enum_item("Folder", layer_kind::FOLDER)
    , xproperty::settings::enum_item("Group", layer_kind::GROUP)
    , xproperty::settings::enum_item("Kimodo", layer_kind::KIMODO)     // a generated take (its accepted take = an import source, sampled like a Clip layer; none yet = nothing)
    };

    // A Pose layer's key: one bone's local rotation at a time. Absolute target by default (the pose to reach: in an Additive layer its offset is
    // derived against the layers below AT THE KEY'S TICK, so moving the key keeps the pose); Offset: the stored rotation offset itself.
    struct layer_pose_key
    {
        std::int64_t    m_Tick    = 0;
        std::string     m_Bone    = {};
        float           m_X = 0, m_Y = 0, m_Z = 0, m_W = 1;     // quaternion
        bool            m_bOffset = false;
        bool            m_bStep   = false;
        bool            m_bPosition = false;                    // a position key too (the hips' shift, or any bone): local, absolute or with Offset the offset
        float           m_PX = 0, m_PY = 0, m_PZ = 0;
        bool            m_bLinear = false;                      // the interval to the next key is linear even in a smooth layer

        XPROPERTY_DEF
        ( "PoseKey", layer_pose_key
        , obj_member<"Tick",   &layer_pose_key::m_Tick >
        , obj_member<"Bone",   &layer_pose_key::m_Bone >
        , obj_member<"X",      &layer_pose_key::m_X >
        , obj_member<"Y",      &layer_pose_key::m_Y >
        , obj_member<"Z",      &layer_pose_key::m_Z >
        , obj_member<"W",      &layer_pose_key::m_W >
        , obj_member<"Offset", &layer_pose_key::m_bOffset >
        , obj_member<"Step",   &layer_pose_key::m_bStep >
        , obj_member<"Positioned", &layer_pose_key::m_bPosition >
        , obj_member<"PX",     &layer_pose_key::m_PX >
        , obj_member<"PY",     &layer_pose_key::m_PY >
        , obj_member<"PZ",     &layer_pose_key::m_PZ >
        , obj_member<"Linear", &layer_pose_key::m_bLinear, member_help<"The interval to the next key is linear (in a smooth layer)">>
        )
    };
    XPROPERTY_REG(layer_pose_key)

    // An IK key (keyframe batch 2026-11-16): an effector's target (the handle: bone origin + Attach) in the skeleton's model space at a tick;
    // optional rotation; Pinned holds it until the next key. The compose solves the body on every frame (body_ik) and bakes rotations.
    struct layer_ik_key
    {
        std::int64_t    m_Tick      = 0;
        std::string     m_Effector  = {};                       // the bone (hand, foot, head, hips)
        float           m_PX = 0, m_PY = 0, m_PZ = 0;           // model-space target of the handle
        bool            m_bRotation = false;
        float           m_X = 0, m_Y = 0, m_Z = 0, m_W = 1;     // model rotation of the bone
        bool            m_bPinned   = false;
        bool            m_bStep     = false;
        bool            m_bLinear   = false;
        float           m_AX = 0, m_AY = 0, m_AZ = 0;           // the handle in the bone's frame (palm, sole)

        XPROPERTY_DEF
        ( "IkKey", layer_ik_key
        , obj_member<"Tick",     &layer_ik_key::m_Tick >
        , obj_member<"Effector", &layer_ik_key::m_Effector >
        , obj_member<"PX", &layer_ik_key::m_PX >, obj_member<"PY", &layer_ik_key::m_PY >, obj_member<"PZ", &layer_ik_key::m_PZ >
        , obj_member<"Rotated",  &layer_ik_key::m_bRotation >
        , obj_member<"X", &layer_ik_key::m_X >, obj_member<"Y", &layer_ik_key::m_Y >, obj_member<"Z", &layer_ik_key::m_Z >, obj_member<"W", &layer_ik_key::m_W >
        , obj_member<"Pinned",   &layer_ik_key::m_bPinned >
        , obj_member<"Step",     &layer_ik_key::m_bStep >
        , obj_member<"Linear",   &layer_ik_key::m_bLinear >
        , obj_member<"AX", &layer_ik_key::m_AX >, obj_member<"AY", &layer_ik_key::m_AY >, obj_member<"AZ", &layer_ik_key::m_AZ >
        )
    };
    XPROPERTY_REG(layer_ik_key)

    enum class layer_space : std::uint8_t { LOCAL, SKELETON_MODEL };
    static constexpr auto layer_space_v = std::array
    { xproperty::settings::enum_item("Local",          layer_space::LOCAL)
    , xproperty::settings::enum_item("Skeleton model", layer_space::SKELETON_MODEL)
    };

    // A bone entry of a layer's mask: this bone (and, inheriting, its descendants until a nearer entry) at Value; BlendDepth > 0 ramps the
    // value in down the branch: Value * min(1, (depth + 1) / BlendDepth). Nothing listed = the layer touches nothing.
    struct layer_mask_entry
    {
        std::string     m_Bone       = {};
        float           m_Value      = 1.0f;
        bool            m_bInherit   = true;
        int             m_BlendDepth = 0;

        XPROPERTY_DEF
        ( "MaskEntry", layer_mask_entry
        , obj_member<"Bone",       &layer_mask_entry::m_Bone >
        , obj_member<"Value",      &layer_mask_entry::m_Value >
        , obj_member<"Inherit",    &layer_mask_entry::m_bInherit >
        , obj_member<"BlendDepth", &layer_mask_entry::m_BlendDepth >
        )
    };
    XPROPERTY_REG(layer_mask_entry)

    struct layer_weight_key
    {
        std::int64_t    m_Tick  = 0;            // 6000 per second (xanimation::ticks_per_second_v)
        float           m_Value = 1.0f;
        bool            m_bStep = false;        // hold this value until the next key (else linear)

        XPROPERTY_DEF
        ( "WeightKey", layer_weight_key
        , obj_member<"Tick",  &layer_weight_key::m_Tick >
        , obj_member<"Value", &layer_weight_key::m_Value >
        , obj_member<"Step",  &layer_weight_key::m_bStep >
        )
    };
    XPROPERTY_REG(layer_weight_key)

    // A generated take behind some of a layer's keys (Tomas 2026-11-15: any part of a Kimodo animation can be an ingredient of the final
    // clip): stored in the project's Cache\KimodoTakes\<package guid>\ (its BVH, the constraints JSON, a .take.txt), never regenerated;
    // the keys of [Start, End] came from it. The coming Kimodo layer kind shows these as segments.
    struct layer_take
    {
        std::string     m_File    = {};         // the take's stem in the package's takes folder (File.bvh, File.json, File.take.txt)
        std::string     m_Prompt  = {};
        int             m_Seed    = 1;
        int             m_Steps   = 25;
        std::int64_t    m_Start   = 0;          // the clip span [Start, End] (ticks) whose keys it produced
        std::int64_t    m_End     = 0;
        std::int64_t    m_Offset  = 0;          // the clip tick of the take's frame 0
        std::string     m_Why     = {};         // why it was made (e.g. "refine after Suggest Pose at 1.00 s, scope +-1 s: acceleration")

        XPROPERTY_DEF
        ( "Take", layer_take
        , obj_member<"File",   &layer_take::m_File >
        , obj_member<"Prompt", &layer_take::m_Prompt >
        , obj_member<"Seed",   &layer_take::m_Seed >
        , obj_member<"Steps",  &layer_take::m_Steps >
        , obj_member<"Start",  &layer_take::m_Start >
        , obj_member<"End",    &layer_take::m_End >
        , obj_member<"Offset", &layer_take::m_Offset >
        , obj_member<"Why",    &layer_take::m_Why >
        )
    };
    XPROPERTY_REG(layer_take)

    struct clip_layer
    {
        std::uint64_t       m_Id          = 0;          // stable identity (commands, undo, reports); 0 = not given yet (the editor gives one)
        std::string         m_Name        = {};
        bool                m_bEnabled    = true;       // a disabled layer is not in the build (solo is the editor's, never stored)
        bool                m_bLock       = false;      // edit lock (the timeline refuses edits of a locked layer); no effect on the build
        layer_mode          m_Mode        = layer_mode::OVERRIDE;
        additive_reference  m_Reference   = additive_reference::REST_POSE;
        std::int64_t        m_ReferenceTick = 0;        // SOURCE_FRAME: the source time of the reference frame (ticks)
        std::wstring        m_SourceKey   = {};         // the import source of this package (its key: the path, or package:HEX)
        std::string         m_SourceClip  = {};         // the clip there (its original name)
        std::int64_t        m_Start       = 0;          // composition span [Start, End) in ticks; End -1 = the clip's end
        std::int64_t        m_End         = -1;
        std::int64_t        m_TrimIn      = 0;          // the source span [TrimIn, TrimOut] in ticks; TrimOut -1 = the source's end
        std::int64_t        m_TrimOut     = -1;
        double              m_Rate        = 1.0;        // >= 0; 0 = hold
        bool                m_bReverse    = false;
        float               m_Weight      = 1.0f;       // used when there are no weight keys
        std::vector<layer_weight_key>   m_WeightKeys = {};
        std::vector<layer_mask_entry>   m_Mask       = {};
        bool                m_bRotation   = true;       // the channels the layer brings
        bool                m_bPosition   = false;
        layer_root          m_Root        = layer_root::INHERIT_BELOW;
        layer_space         m_Space       = layer_space::LOCAL;     // override rotations: per bone (Local) or relative to the skeleton (a masked arm keeps the layer's orientation)
        layer_kind          m_Kind        = layer_kind::CLIP;       // Clip: motion from SourceKey/SourceClip; Pose: the Keys below
        std::vector<layer_pose_key> m_Keys = {};                    // Pose layers: sparse keys (a track per bone); their mask, when empty, is the whole body
        std::vector<layer_ik_key>   m_IkKeys = {};                  // Pose layers: IK keys (a track per effector), solved per frame after the layer's own keys
        bool                m_bSmooth     = false;      // Pose layers: a smooth curve through the keys (new pose layers: on; off = linear, as layers made before)
        bool                m_bBoneLimits = true;       // Pose layers: the IK (posing, IK keys) respects the skeleton's joint limits; off for the rare impossible pose (a broken leg)
        float               m_Falloff     = 0.0f;       // Pose layers: seconds the layer eases in before its first key and out after its last (new pose layers: 0.5; 0 = ramp to the section ends)
        std::vector<layer_take> m_Takes   = {};         // Pose layers: the generated takes (Kimodo) some of the keys came from (provenance; nothing composes from it)
        std::uint64_t       m_Parent      = 0;          // the Id of the folder or group this layer is in; 0 = the top level (contract v3)
        std::string         m_SharedMask  = {};         // a skeleton mask group by name: replaces Mask; a name the skeleton lacks = the layer contributes nothing (an error)
        // Kimodo layers (T4): the request that makes its take; the accepted take is SourceKey/SourceClip (an import source); AcceptedSig = the request hash when it was accepted (request relation: matches or changed)
        std::string         m_Prompt      = {};
        int                 m_Seed        = 1;
        int                 m_Steps       = 25;
        float               m_Seconds     = 2.0f;
        std::string         m_AcceptedSig = {};
        // Clip and Kimodo layers: the timeline row this layer is a segment of (layers with the same Row and parent are ONE row; where two
        // overlap they crossfade - the upper one eases in or out over the overlap; three at once is an error). 0 = a row of its own.
        std::uint64_t       m_Row         = 0;

        XPROPERTY_DEF
        ( "Layer", clip_layer
        , obj_member<"Id",            &clip_layer::m_Id, member_flags<flags::SHOW_READONLY> >
        , obj_member<"Name",          &clip_layer::m_Name >
        , obj_member<"Enabled",       &clip_layer::m_bEnabled >
        , obj_member<"Lock",          &clip_layer::m_bLock >
        , obj_member<"Mode",          &clip_layer::m_Mode, member_enum_span<layer_mode_v> >
        , obj_member<"Reference",     &clip_layer::m_Reference, member_enum_span<additive_reference_v> >
        , obj_member<"ReferenceTick", &clip_layer::m_ReferenceTick >
        , obj_member<"SourceKey",     &clip_layer::m_SourceKey >
        , obj_member<"SourceClip",    &clip_layer::m_SourceClip >
        , obj_member<"Start",         &clip_layer::m_Start >
        , obj_member<"End",           &clip_layer::m_End >
        , obj_member<"TrimIn",        &clip_layer::m_TrimIn >
        , obj_member<"TrimOut",       &clip_layer::m_TrimOut >
        , obj_member<"Rate",          &clip_layer::m_Rate >
        , obj_member<"Reverse",       &clip_layer::m_bReverse >
        , obj_member<"Weight",        &clip_layer::m_Weight >
        , obj_member<"WeightKeys",    &clip_layer::m_WeightKeys >
        , obj_member<"Mask",          &clip_layer::m_Mask >
        , obj_member<"Rotation",      &clip_layer::m_bRotation >
        , obj_member<"Position",      &clip_layer::m_bPosition >
        , obj_member<"Root",          &clip_layer::m_Root, member_enum_span<layer_root_v> >
        , obj_member<"Space",         &clip_layer::m_Space, member_enum_span<layer_space_v> >
        , obj_member<"Kind",          &clip_layer::m_Kind, member_enum_span<layer_kind_v> >
        , obj_member<"Keys",          &clip_layer::m_Keys >
        , obj_member<"IkKeys",        &clip_layer::m_IkKeys >
        , obj_member<"Smooth",        &clip_layer::m_bSmooth >
        , obj_member<"BoneLimits",    &clip_layer::m_bBoneLimits, member_help<"The IK respects the skeleton's joint limits (posing and IK keys). Off only for an impossible pose (a broken leg)"> >
        , obj_member<"Falloff",       &clip_layer::m_Falloff >
        , obj_member<"Takes",         &clip_layer::m_Takes >
        , obj_member<"Parent",        &clip_layer::m_Parent >
        , obj_member<"SharedMask",    &clip_layer::m_SharedMask >
        , obj_member<"Prompt",        &clip_layer::m_Prompt >
        , obj_member<"Seed",          &clip_layer::m_Seed >
        , obj_member<"Steps",         &clip_layer::m_Steps >
        , obj_member<"Seconds",       &clip_layer::m_Seconds >
        , obj_member<"AcceptedSig",   &clip_layer::m_AcceptedSig, member_flags<flags::SHOW_READONLY> >
        , obj_member<"Row",           &clip_layer::m_Row >
        )
    };
    XPROPERTY_REG(clip_layer)

    // A row's segments (layers with the same Row and parent): at most two may overlap at any time (they crossfade). End -1 = the clip's end.
    // Returns the first row where three overlap ("" when none). Segments that are off do not count.
    template< typename T_LAYERS >
    inline std::string RowOverlapError(const T_LAYERS& Layers, std::int64_t ClipEnd = std::numeric_limits<std::int64_t>::max())
    {
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::pair<std::int64_t, int>>> Ev;
        for (const auto& L : Layers)
            if (L.m_Row && L.m_bEnabled && (L.m_Kind == layer_kind::CLIP || L.m_Kind == layer_kind::KIMODO))      // a segment that is off plays nothing: it never conflicts
            {
                auto& E = Ev[{ L.m_Parent, L.m_Row }];
                E.push_back({ L.m_Start, +1 }); E.push_back({ L.m_End >= 0 ? L.m_End : ClipEnd, -1 });
            }
        for (auto& [K, E] : Ev)
        {
            std::sort(E.begin(), E.end());          // an end before a start at the same tick: touching is not overlapping
            int n = 0;
            for (const auto& [T, d] : E) if ((n += d) > 2) return std::format("row {}: three segments overlap at {:.3f} s (two at most: they crossfade)", K.second, double(T) / 6000.0);
        }
        return {};
    }

    static constexpr auto event_kind_v = std::array
    { xproperty::settings::enum_item("Marker",   xanim_package::event_kind::MARKER)
    , xproperty::settings::enum_item("Footstep", xanim_package::event_kind::FOOTSTEP)
    , xproperty::settings::enum_item("Contact",  xanim_package::event_kind::CONTACT)
    , xproperty::settings::enum_item("Sync",     xanim_package::event_kind::SYNC)
    };

    // A marker on the clip's timeline (T5): a point, or an interval (a contact bar: EndTick >= Tick). In the clip's composition ticks (the
    // layers' time: the source's own, before the trim); the compiler moves them into the compiled clip's time and drops what the trim cuts.
    struct clip_event
    {
        std::uint64_t               m_Id      = 0;          // stable identity (commands, undo)
        std::string                 m_Name    = {};
        xanim_package::event_kind   m_Kind    = xanim_package::event_kind::MARKER;
        std::int64_t                m_Tick    = 0;          // 6000 per second
        std::int64_t                m_EndTick = -1;         // -1: a point
        std::string                 m_Bone    = {};         // the bone it is about (a foot for a footstep); empty: none

        XPROPERTY_DEF
        ( "Event", clip_event
        , obj_member<"Id",      &clip_event::m_Id, member_flags<flags::SHOW_READONLY> >
        , obj_member<"Name",    &clip_event::m_Name >
        , obj_member<"Kind",    &clip_event::m_Kind, member_enum_span<event_kind_v> >
        , obj_member<"Tick",    &clip_event::m_Tick >
        , obj_member<"EndTick", &clip_event::m_EndTick, member_help<"-1: a point; else the end of the interval (a contact)"> >
        , obj_member<"Bone",    &clip_event::m_Bone >
        )
    };
    XPROPERTY_REG(clip_event)

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
        std::vector<clip_layer>           m_Layers         = {};   // the layered edit mode: baked into this clip before its trim/downsample/root motion
        std::vector<clip_event>           m_Events         = {};   // markers, footsteps, contacts, sync points (T5)

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
        , obj_member<"Layers",          &clip::m_Layers >
        , obj_member<"Events",          &clip::m_Events >
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
        float              m_Seconds = 2.0f;    // NEW_ANIMATION: its length
        int                m_FPS     = 30;      // NEW_ANIMATION: its frame rate

        // The kind as it counts: a source saved before kinds existed that names a Retarget is a different-skeleton asset
        import_kind kind(void) const noexcept { return (m_Kind == import_kind::SAME_SKELETON_ASSET && !m_Retarget.empty()) ? import_kind::DIFFERENT_SKELETON_ASSET : m_Kind; }
        bool bAsset  (void) const noexcept { return kind() == import_kind::SAME_SKELETON_ASSET || kind() == import_kind::DIFFERENT_SKELETON_ASSET; }
        bool bPackage(void) const noexcept { return kind() == import_kind::SAME_SKELETON_PACKAGE || kind() == import_kind::DIFFERENT_SKELETON_PACKAGE; }
        bool bNew    (void) const noexcept { return kind() == import_kind::NEW_ANIMATION; }
        bool bDifferent(void) const noexcept { return kind() == import_kind::DIFFERENT_SKELETON_ASSET || kind() == import_kind::DIFFERENT_SKELETON_PACKAGE; }

        // What names this source in the details (the compiler's list of what each source holds): the file, or the package
        std::wstring key(void) const { return bAsset() ? m_Path : bNew() ? L"new:" + m_Path : std::format(L"package:{:X}", m_Package.m_Instance.m_Value); }

        XPROPERTY_DEF
        ( "ImportSource", import_source
        , obj_member<"Type", &import_source::m_Kind, member_enum_span<import_kind_v>, member_help<"Where this source's animations come from: an animation asset or another animation package, made on this package's skeleton or on another one (then a Retarget moves them)">>
        , obj_member<"Path", &import_source::m_Path, member_ui<std::wstring>::file_dialog<mesh_filter_v, true, 1>
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bAsset() && !O.bNew(); return F; }> >
        , obj_member<"Seconds", &import_source::m_Seconds, member_help<"New animation: its length in seconds">
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bNew(); return F; }> >
        , obj_member<"FPS", &import_source::m_FPS, member_help<"New animation: its frame rate">
            , member_dynamic_flags<+[](const import_source& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.bNew(); return F; }> >
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
                if (S.bNew() && (S.m_Path.empty() || !(S.m_Seconds > 0) || S.m_FPS < 1 || S.m_FPS > 240)) Errors.push_back(std::format("Import source {}: a new animation needs a name (Path), Seconds > 0 and FPS 1..240", i));
                if (S.bDifferent() && S.m_Retarget.empty())   Errors.push_back(std::format("Import source {}: a different-skeleton kind needs its Retarget", i));
                for (const auto& C : S.m_Clips)
                {
                    if (auto E = RowOverlapError(C.m_Layers); !E.empty()) Errors.push_back(std::format("Clip '{}', {}", C.m_Name, E));
                    for (const auto& L : C.m_Layers)
                    {
                        const auto Where = std::format("Clip '{}', layer '{}'", C.m_Name, L.m_Name);
                        if (L.m_Kind == layer_kind::CLIP || (L.m_Kind == layer_kind::KIMODO && !L.m_SourceKey.empty()))
                        {
                            if (L.m_SourceKey.empty() || L.m_SourceClip.empty())   Errors.push_back(Where + ": no source clip (SourceKey and SourceClip)");
                            else if (findImportSource(L.m_SourceKey) == -1)        Errors.push_back(Where + ": its source is not one of this package's import sources");
                        }
                        if (!(L.m_Rate >= 0.0))                                Errors.push_back(Where + ": a negative rate (use Reverse to play backwards)");
                        if (L.m_End >= 0 && L.m_End <= L.m_Start)              Errors.push_back(Where + ": it ends before it starts");
                        if (L.m_TrimOut >= 0 && L.m_TrimOut < L.m_TrimIn)      Errors.push_back(Where + ": trim out is before trim in");
                    }
                }
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
            return LoadSkeletonManifest(FileName);
        }

        // Cross-plugin dependency: read the Skeleton compiler's own bone-manifest export (its final, post-merge bone order/hashes, its mask
        // groups) - not its compiled binary. RunAfter in this plugin's Plugin.config guarantees this file already exists by the time we
        // get here. The editor calls it again after a compile: the skeleton may have changed (a mask group edit, contract v3).
        xerr LoadSkeletonManifest(std::wstring_view FileName) noexcept
        {
            if (m_SkeletonRef.empty()) return {};
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
