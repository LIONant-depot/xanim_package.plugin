#ifndef XANIM_PACKAGE_EDITOR_H
#define XANIM_PACKAGE_EDITOR_H
#pragma once

// The Anim Package editor: opens an animation package from the asset browser in its own window. The clips the compiler found in the import
// files are listed with their options (name, delete, loop, down sample, trim, root motion), edited in the table or through commands, all
// undoable. A clip plays on the skeleton it is bound to, in a 3D view, with a transport bar and a timeline. Hosts include this header and open
// editors through xeditor::open_resource_editors.
#include "source/Tools/Editor/xeditor_descriptor_editor.h"
#include "source/Tools/Editor/xeditor_camera_toolbar.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_inspector_pickers.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_resources.h"
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_viewport.h"
#include "plugins/xanim_package.plugin/source/xanim_package.h"
#include "plugins/xanim_package.plugin/source/xanim_package_descriptor.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit
#include "source/Tools/xgpu_imgui_timeline.h"
#include "plugins/xretarget.plugin/source/xretarget_descriptor.h"
#include "plugins/xretarget.plugin/source/xretarget_mapping.h"
#include "dependencies/xAnimation/source/xanimation.h"
#include "plugins/xanim_package.plugin/source/Editor/xanim_package_kimodo.h"
#include "plugins/xanim_package.plugin/source/xanim_package_layers.h"
#include "plugins/xskeleton.plugin/source/xskeleton_joints.h"

#include <charconv>

namespace xanim_package_editor
{
    // Segoe MDL2 trash can: the header of the Delete column
    inline constexpr const char* g_DeleteIcon = "\xEE\x9D\x8D";

    inline const char* RootMotionModeName(xanim_package::root_motion_mode Mode) noexcept
    {
        switch (Mode)
        {
        case xanim_package::root_motion_mode::NONE:    return "None";
        case xanim_package::root_motion_mode::XZ_ONLY: return "XZ Only";
        case xanim_package::root_motion_mode::XYZ:     return "XYZ";
        }
        return "?";
    }

    //--------------------------------------------------------------------------------------------
    // Playback commands: which clip plays, and where. Playing is view state, not part of the resource, so these are queries: not undoable and
    // they never dirty the descriptor.
    //--------------------------------------------------------------------------------------------
    struct session;

    struct playback_cmd : xundo::query_command_base
    {
        enum class kind { select_clip, play, pause, seek, set_speed, info, list_clips, kimodo_generate, kimodo_accept, kimodo_discard, kimodo_info, layer_add, layer_mask, layers_info, pose_key, suggest_pose, suggest_pose_accept };

        session&    m_Session;
        kind        m_Kind;
        const char* m_pHelp;

        playback_cmd(xundo::system& System, session& Session, kind Kind, const char* pName, const char* pHelp) noexcept
            : query_command_base(System, pName, nullptr), m_Session(Session), m_Kind(Kind), m_pHelp(pHelp) { RegisterArguments(); }

        const char* getCommandHelp() const noexcept override { return m_pHelp; }
        void RegisterArguments() noexcept override
        {
            switch (m_Kind)
            {
            case kind::select_clip: m_hA = m_Parser.addOption("Source", "Import source index",       true, 1); m_hB = m_Parser.addOption("Clip", "Clip index in that source", true, 1); break;
            case kind::seek:        m_hA = m_Parser.addOption("Time",   "Seconds from the clip start", true, 1); break;
            case kind::set_speed:   m_hA = m_Parser.addOption("Index",  "Playback speed step (0.25x .. 3x)", true, 1); break;
            case kind::kimodo_generate:
                m_hA = m_Parser.addOption("Prompt",  "What the motion is, in words", true, 1);
                m_hB = m_Parser.addOption("Seconds", "Length in seconds (default 5)", false, 1);
                m_hC = m_Parser.addOption("Seed",    "Seed (default 1): another seed, another take", false, 1);
                break;
            case kind::kimodo_accept: m_hA = m_Parser.addOption("Name", "The clip's file name (default: from the prompt)", false, 1); break;
            case kind::layer_add:
                m_hA = m_Parser.addOption("Source", "The import source's key (its path, as ListClips shows it); not for -Kind Pose", false, 1);
                m_hB = m_Parser.addOption("Clip",   "The clip there (its original name); not for -Kind Pose", false, 1);
                m_hC = m_Parser.addOption("Name",   "The layer's name", false, 1);
                m_hD = m_Parser.addOption("Kind",   "Clip (default) or Pose", false, 1);
                break;
            case kind::pose_key:
                m_hA = m_Parser.addOption("Layer",  "The Pose layer's index in the selected clip", true, 1);
                m_hB = m_Parser.addOption("Bone",   "The bone (its name)", true, 1);
                m_hC = m_Parser.addOption("Time",   "Seconds", true, 1);
                m_hD = m_Parser.addOption("Turn",   "\"x y z\" degrees: the key's pose = this turn (parent side) applied to the layers below at that time (default 0 0 0: the pose below)", false, 1);
                m_hE = m_Parser.addOption("Offset", "1: store the turn itself as an offset key (additive layers) instead of the absolute pose", false, 1);
                m_hF = m_Parser.addOption("Move",   "\"x y z\" model units: also a position key - the position below moved by this (the hips' shift; any bone); with -Offset the move itself", false, 1);
                break;
            case kind::suggest_pose:
                m_hA = m_Parser.addOption("Time",   "Seconds: the key", true, 1);
                m_hB = m_Parser.addOption("Move",   "\"Bone dx dy dz\": a handle moved, centimetres in the model frame (Bone: a bone name or LeftHand/RightHand/LeftFoot/RightFoot)", false, 1);
                m_hC = m_Parser.addOption("Pin",    "Handles held where they are (default \"LeftFoot RightFoot\"; \"none\" for none)", false, 1);
                m_hD = m_Parser.addOption("Seeds",  "\"1 2 3\": one candidate per seed (default 1)", false, 1);
                m_hE = m_Parser.addOption("Prompt", "What the pose is, in words (default: none)", false, 1);
                break;
            case kind::suggest_pose_accept:
                m_hA = m_Parser.addOption("Layer",     "The Pose layer the keys go on (the top layer of the clip)", true, 1);
                m_hB = m_Parser.addOption("Candidate", "Which candidate (default 0)", false, 1);
                m_hC = m_Parser.addOption("Refine",    "0: no Refine Motion (Kimodo regenerating +-0.5 s around the key so it moves naturally); default 1", false, 1);
                m_hD = m_Parser.addOption("MaxWindow", "Seconds per Kimodo window (default 10, Kimodo's limit): a longer refine scope is generated as overlapping windows stitched together; a smaller value tests the stitching", false, 1);
                break;
            case kind::layer_mask:
                m_hA = m_Parser.addOption("Layer", "The layer's index in the selected clip", true, 1);
                m_hB = m_Parser.addOption("Bone",  "The bone (its name): added with its branch, or removed when it is in the mask", true, 1);
                m_hC = m_Parser.addOption("Only",  "1: this bone only (not its branch)", false, 1);
                break;
            default: break;
            }
        }
        std::string Query() noexcept override;

        xcmdline::parser::handle m_hA, m_hB, m_hC, m_hD, m_hE, m_hF;
    };

    //--------------------------------------------------------------------------------------------
    // The editor
    //--------------------------------------------------------------------------------------------
    // The Animation Package editor's own actions (Preview/...).
    struct anim_actions
    {
        session* m_pS = nullptr;
        anim_actions() noexcept = default;
        explicit anim_actions(session& S) noexcept : m_pS(&S) {}

        void        PlayPause() noexcept;       const char* WhyNoPlayPause() const noexcept;

        XPROPERTY_DEF
        ( "AnimPackage", anim_actions
        , obj_scope<"Preview"
            , obj_action<"PlayPause", &anim_actions::PlayPause
                , member_help<"Plays or pauses the selected clip in the preview">
                , ximgui::actions::member_keys<"P">
                , member_dynamic_reason<+[](const anim_actions& A) noexcept -> const char* { return A.WhyNoPlayPause(); }> >
            >
        )
    };
    XPROPERTY_REG(anim_actions)
    XIMGUI_ACTIONS_OWNER(anim_actions)

    struct session : xeditor::descriptor_editor
    {
        using desc = xanim_package_desc::descriptor;

        playback_cmd                                    m_SelectClip, m_Play, m_Pause, m_Seek, m_SetSpeed, m_Info, m_ListClips;
        playback_cmd                                    m_KimodoGenerate, m_KimodoAccept, m_KimodoDiscard, m_KimodoInfo;
        playback_cmd                                    m_LayerAdd, m_LayerMask, m_LayersInfo, m_PoseKey, m_SuggestPose, m_SuggestPoseAccept;

        // Layers (the layered edit mode): the compiler's layer-sources cache, the selected clip's live composition (same function as the compile)
        std::vector<xanim_package_layers::source_frames> m_LayerSources;
        std::vector<xmath::transform3>                  m_LayerComposed;        // the selected clip's layers composed now (source frames, before trim/downsample/root motion)
        int                                             m_LayerComposedFPS = 30;
        int                                             m_LayerComposedFrames = 0;
        std::string                                     m_LayerSig;             // what m_LayerComposed was made from
        xanim_package_layers::report                    m_LayerReport;
        std::string                                     m_LayerError;
        int                                             m_iLayer = -1;          // the layer the panel edits
        int                                             m_iSolo = -1;           // a layer auditioned alone (with the clip below it): editor state only, never in the build
        double                                          m_LayerComposeMs = 0;   // the last live recompose (the preview budget is < 100 ms)
        bool                                            m_bLayerLive = true;    // the viewport shows the live composition (not the compiled clip)
        bool                                            m_bMaskEdit = false;    // a click on a bone adds/removes it from the selected layer's mask
        std::set<int>                                   m_MaskShown;            // the selected layer's masked bones (drawn selected)

        // Kimodo (text to motion): one generation at a time; its result previews until accepted or discarded
        struct kimodo_state
        {
            kimodo::job                                 m_Job;
            kimodo::bvh                                 m_Preview;              // the generated motion, not in the assets yet
            bool                                        m_bPreview = false;
            float                                       m_Time = 0.0f;
            bool                                        m_bPlaying = true;
            char                                        m_Prompt[1024] = "A person walks forward.";
            char                                        m_Name[128] = "";
            float                                       m_Seconds = 5.0f;
            int                                         m_Seed = 1;
            std::string                                 m_PromptUsed;           // what the preview was made from
            float                                       m_SecondsUsed = 0.0f;
            int                                         m_SeedUsed = 0;
            std::string                                 m_Status;               // the last result, said in words
            bool                                        m_bStatusError = false;
            xrsc::skeleton                              m_SkeletonRef;          // the Kimodo Retarget's Source (SOMA 77): the preview draws on it
            std::vector<int>                            m_BoneToJoint;          // that skeleton's bone -> the BVH joint of the same name
            std::vector<xskeleton_editor::bone_world>   m_Bones;
            std::vector<float>                          m_Pos, m_World;

            // the preview on THIS package's skeleton: the compiler's own retarget path (the Retarget's mapping, ToSkeletonSpace, BakeFrames) run on the BVH
            struct retarget_ctx
            {
                xretarget_desc::descriptor              m_Descriptor;   // the mapping keeps views into its manifests: it lives as long as the mapping
                xretarget_desc::mapping                 m_Mapping;
                xanimation::retargeter                  m_Retargeter;
                xanimation::retarget_map                m_ReverseMap;   // Suggest Pose: this package's skeleton -> SOMA (the same links reversed)
                xanimation::retargeter                  m_Reverse;
                std::uint64_t                           m_Key = 0;
            };
            std::unique_ptr<retarget_ctx>               m_pRetarget;
            std::vector<xmath::transform3>              m_Baked;                // [frame][package skeleton bone], local
            int                                         m_nDst = 0;
            std::string                                 m_RetargetError;        // why the preview is on Kimodo's skeleton only
            bool                                        m_bShowSource = false;  // show Kimodo's own skeleton instead of this package's
            std::vector<xmath::fmat4>                   m_DstWorlds;
            std::vector<xskeleton_editor::bone_world>   m_DstBones;
        } m_K;

        // Suggest Pose (posing v2 step S): candidates only - nothing reaches the descriptor before SuggestPoseAccept
        struct suggest_candidate
        {
            int                                 m_Seed = 0;
            std::vector<xmath::transform3>      m_Pose;         // [bone] local: Kimodo's pose, corrected on this skeleton (pins, goals, limits)
            std::string                         m_Report;
        };
        struct suggest_state
        {
            std::vector<suggest_candidate>      m_Candidates;
            std::vector<std::uint8_t>           m_Owned;        // the bones a candidate may key (mapped to SOMA, rotation only)
            std::vector<xmath::transform3>      m_Original;     // the clip's pose at the key
            double                              m_Time = 0;
            std::string                         m_Sig;          // the clip and its layers when asked: a candidate is stale once they change
            std::string                         m_MovedBone;
            xmath::fvec3                        m_Target{ 0, 0, 0 };
            int                                 m_iHips = -1;   // the bone Kimodo's root drives: its translation is keyed too
        } m_Suggest;
        int m_RefineMaxFrames = 300;        // frames per Kimodo window (30 fps; Kimodo's 10 s limit): longer scopes are stitched windows

        xskeleton_editor::skeleton_viewport             m_Viewport;             // the shared 3D skeleton panel (the scene is m_Viewport.m_Scene)
        xeditor::camera_toolbar                         m_CameraToolbar;
        xeditor::camera_cmds                            m_CameraCmds;
        xrsc::anim_package                              m_Ref;
        xrsc::skeleton                                  m_SkeletonRef;
        xanim_package_desc::details                     m_Details;              // what the compiler found in the import files, last time it ran
        std::string                                     m_ErrorMessage;         // why nothing can be shown, when so
        bool                                            m_bShowable = false;    // the package and its skeleton are loaded and agree

        std::vector<xmath::fmat4>                       m_RestWorlds, m_PoseWorlds;
        std::vector<xskeleton_editor::bone_world>       m_PoseBones;
        std::vector<e19::draw_vert>                     m_Lines;

        int                                             m_iSelectedClip = -1;   // in the compiled package: drives playback
        int                                             m_iSelectedSource = -1, m_iSelectedDescriptorClip = -1;
        int                                             m_iRenameSource = -1, m_iRenameClip = -1;
        bool                                            m_bRenameStarted = false;
        std::string                                     m_RenameBuffer;
        int                                             m_IntBefore = 0;        // an integer field's value when its editing began

        float                                           m_TimeSeconds = 0.0f;
        int                                             m_LoopsElapsed = 0;
        bool                                            m_bPlaying = false;
        anim_actions                                    m_EditorActions{ *this };       // this editor's own keys (Preview/...)
        void RegisterActions(ximgui::actions::context& Ctx) noexcept override { Ctx.Scope(m_EditorActions, "Preview"); }
        int                                             m_iSpeedIndex = xgpu::tools::editors::g_DefaultSpeedIndex;
        xgpu::tools::imgui::timeline::state             m_Timeline;

        session(xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : descriptor_editor("AnimPackage", Guid, LibraryGuid, pDevice)
            , m_SelectClip(m_Undo, *this, playback_cmd::kind::select_clip, "SelectClip", "Selects a clip to preview. Usage: SelectClip -Source index -Clip index (see ListClips)")
            , m_Play      (m_Undo, *this, playback_cmd::kind::play,        "Play",       "Plays the selected clip. Usage: Play")
            , m_Pause     (m_Undo, *this, playback_cmd::kind::pause,       "Pause",      "Pauses the playback. Usage: Pause")
            , m_Seek      (m_Undo, *this, playback_cmd::kind::seek,        "Seek",       "Moves the playback to a time in seconds. Usage: Seek -Time seconds")
            , m_SetSpeed  (m_Undo, *this, playback_cmd::kind::set_speed,   "SetSpeed",   "Sets the playback speed step (0 = 0.25x, 3 = 1x, 7 = 3x). Usage: SetSpeed -Index step")
            , m_Info      (m_Undo, *this, playback_cmd::kind::info,        "PlaybackInfo","The selected clip, the time and whether it plays. Usage: PlaybackInfo")
            , m_ListClips (m_Undo, *this, playback_cmd::kind::list_clips,  "ListClips",  "Every clip with its options and whether it is compiled. Usage: ListClips")
            , m_KimodoGenerate(m_Undo, *this, playback_cmd::kind::kimodo_generate, "KimodoGenerate", "Generates a motion from text with Kimodo; it previews until KimodoAccept or KimodoDiscard. Usage: KimodoGenerate -Prompt \"A person jumps.\" [-Seconds 5] [-Seed 1]")
            , m_KimodoAccept  (m_Undo, *this, playback_cmd::kind::kimodo_accept,   "KimodoAccept",   "Saves the previewed Kimodo motion in the package's Kimodo folder and adds it as a source (undoable). Usage: KimodoAccept [-Name ClipName]")
            , m_KimodoDiscard (m_Undo, *this, playback_cmd::kind::kimodo_discard,  "KimodoDiscard",  "Drops the previewed Kimodo motion (nothing was saved). Usage: KimodoDiscard")
            , m_KimodoInfo    (m_Undo, *this, playback_cmd::kind::kimodo_info,     "KimodoInfo",     "Kimodo's state: running, previewing, the last result, the Retarget and folder. Usage: KimodoInfo")
            , m_LayerAdd      (m_Undo, *this, playback_cmd::kind::layer_add,       "LayerAdd",       "Adds a layer to the selected clip from another clip of this package (its mask is empty: it touches nothing yet). Usage: LayerAdd -Source key -Clip name [-Name text]")
            , m_LayerMask     (m_Undo, *this, playback_cmd::kind::layer_mask,      "LayerMask",      "Adds a bone (with its branch) to a layer's mask, or removes it when it is there. Usage: LayerMask -Layer n -Bone name [-Only 1]")
            , m_LayersInfo    (m_Undo, *this, playback_cmd::kind::layers_info,     "LayersInfo",     "The selected clip's layers, the live composition's isolation report and its parity with the compiled clip. Usage: LayersInfo")
            , m_PoseKey       (m_Undo, *this, playback_cmd::kind::pose_key,        "PoseKey",        "Sets a key on a Pose layer: the bone's pose at a time = the layers below turned by -Turn (absolute target), or the turn as an offset key. Usage: PoseKey -Layer n -Bone name -Time s [-Turn \"x y z\"] [-Offset 1] [-Move \"x y z\"]")
            , m_SuggestPose   (m_Undo, *this, playback_cmd::kind::suggest_pose,    "SuggestPose",    "Suggests a natural pose at a time with Kimodo for the handles moved and pinned: a CANDIDATE (nothing is written) and its report. Usage: SuggestPose -Time s [-Move \"LeftHand 0 0 15\"] [-Pin \"LeftFoot RightFoot\"] [-Seeds \"1 2\"] [-Prompt text]")
            , m_SuggestPoseAccept(m_Undo, *this, playback_cmd::kind::suggest_pose_accept, "SuggestPoseAccept", "Writes a Suggest Pose candidate as keys on a Pose layer (the bones it changes). Usage: SuggestPoseAccept -Layer n [-Candidate i]")
            , m_CameraCmds(m_Undo, m_Viewport.m_Scene.Camera())
        {
            // Reading the descriptor also cross-checks the skeleton's compiled bone list: not having it yet does not make the clips unusable
            m_Document.m_TolerateReadError = [](const xresource_pipeline::descriptor::base& D) { return !static_cast<const desc&>(D).m_SkeletonRef.empty(); };
            m_Document.Load();
            BindDescriptorInspector();
            xresource_editor::WireResourcePickerCallbacks(m_DescriptorInspector.m_Inspector);
            MergeDetails();

            AddPanel("Clips",                dock::right,  [this] { RenderClips(); });
            AddPanel("Kimodo",               dock::right,  [this] { RenderKimodo(); });
            AddPanel("Layers",               dock::right,  [this] { RenderLayers(); });
            if (pDevice) m_CameraToolbar.Init(m_Document.m_Guid.m_Instance.m_Value);
            AddPanel("AnimPackage Viewport", dock::center, [this] { RenderViewport(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            AddPanel("Playback",             dock::bottom, [this] { RenderPlayback(); });
            AddPanel("Description",          dock::left,   [this] { m_DescriptorInspector.Show(); });

            m_Viewport.m_pOpen = &m_bOpen;
            m_bReady = pDevice && m_Viewport.Init(*pDevice);
            Reload();
        }

        ~session() noexcept override
        {
            xresource::g_Mgr.ReleaseRef(m_Ref);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            xresource::g_Mgr.ReleaseRef(m_K.m_SkeletonRef);
            m_Viewport.Release();
        }

        bool m_bReady = false;

        desc* Desc() noexcept { return m_Document.isLoaded() ? static_cast<desc*>(m_Document.m_pDescriptor.get()) : nullptr; }

        //----------------------------------------------------------------------------------------
        // Loading
        //----------------------------------------------------------------------------------------

        // The descriptor keeps only what was changed about each clip; the compiler's list of what the files hold fills in the rest, so the table shows
        // every clip at once. In memory only: saving is what writes it.
        void MergeDetails() noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) return;
            m_Details = {};
            xtextfile::stream File;
            if (auto Err = File.Open(true, m_Document.m_LogPath + L"\\Details.txt", {}); Err) return;
            xproperty::settings::context Context;
            if (auto Err = xproperty::sprop::serializer::Stream(File, m_Details, Context); Err) { m_Details = {}; return; }

            const auto Before = m_Document.Snapshot();
            pDesc->MergeWithDetails(m_Details);
            if (m_Document.Snapshot() != Before) m_Document.m_bDirty = true;
        }

        void OnDescriptorReplaced() noexcept override { Reload(); }
        void OnCompileStarted()     noexcept override { LetGo(); }
        void OnCompiled()           noexcept override { MergeDetails(); Reload(); }

        std::uint64_t m_FramedSkeleton = ~0ull;      // the skeleton the camera was last framed for
        void LetGo() noexcept
        {
            xresource::g_Mgr.ReleaseRef(m_Ref);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            m_Ref.clear();
            m_SkeletonRef.clear();
            m_bShowable = false;
            m_iSelectedClip = -1;
            m_bPlaying = false;
        }

        // The compiled package, the skeleton it plays on, the rest pose that frames the view
        void Reload() noexcept
        {
            LetGo();
            m_ErrorMessage.clear();
            {   // the layers' sources, as the last compile saw them (the live composition reads them)
                int nBones = 0;
                if (!xanim_package_layers::ReadSourcesCache(m_Document.m_LogPath + L"\\LayerSources.bin", m_LayerSources, nBones)) m_LayerSources.clear();
                m_LayerSig.clear();
            }
            auto* pDesc = Desc();
            if (!pDesc) { m_ErrorMessage = "The descriptor could not be read."; return; }

            m_Ref.m_Instance = m_Document.m_Guid.m_Instance;
            auto* pPackage = std::filesystem::exists(m_Document.m_ResourcePath) ? xresource::g_Mgr.getResource(m_Ref) : nullptr;
            if (!pPackage) { m_Ref.clear(); m_ErrorMessage = "Not compiled yet: compile the package to preview it."; return; }

            if (pDesc->m_SkeletonRef.empty()) { m_ErrorMessage = "This package's descriptor has no Skeleton reference."; return; }
            m_SkeletonRef.m_Instance = pDesc->m_SkeletonRef.m_Instance;
            auto* pSkeleton = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkeleton) { m_SkeletonRef.clear(); m_ErrorMessage = "The referenced skeleton could not be loaded."; return; }

            if (pPackage->m_nBones != pSkeleton->getBones().size())
            {
                m_ErrorMessage = std::format("The package and the skeleton disagree on the bones ({} vs {}): was the skeleton recompiled after this package?", pPackage->m_nBones, pSkeleton->getBones().size());
                return;
            }

            xgpu::tools::editors::ComputeRestBoneWorlds(*pSkeleton, m_RestWorlds);
            xmath::fvec3 Center(0.0f, 0.0f, 0.0f);
            for (auto& M : m_RestWorlds) Center += M.ExtractPosition();
            if (!m_RestWorlds.empty()) Center /= float(m_RestWorlds.size());
            float Radius = 0.5f;
            for (auto& M : m_RestWorlds) Radius = std::max(Radius, (M.ExtractPosition() - Center).Length());
            // the camera is framed for a skeleton the first time it is shown, never because the package was saved or compiled: the person's view stays where they put it
            if (m_FramedSkeleton != pDesc->m_SkeletonRef.m_Instance.m_Value)
            {
                m_FramedSkeleton = pDesc->m_SkeletonRef.m_Instance.m_Value;
                m_Viewport.m_Scene.m_Center  = Center;
                m_Viewport.m_Scene.m_Radius  = Radius;
                m_Viewport.m_Scene.m_bReframe = true;
            }

            m_bShowable = true;
            if (m_iSelectedSource >= 0) SelectDescriptorClip(m_iSelectedSource, m_iSelectedDescriptorClip);
        }

        //----------------------------------------------------------------------------------------
        // Layers (the layered edit mode, L1c): the selected clip's layer stack, edited through SetProperty / ListOp (undoable, console
        // parity), previewed live with the compiler's own composition (xanim_package_layers.h) on the compiler's source frames.
        //----------------------------------------------------------------------------------------

        xanim_package_desc::import_source* SelectedSource() noexcept
        {
            auto* pDesc = Desc();
            return (pDesc && m_iSelectedSource >= 0 && m_iSelectedSource < int(pDesc->m_ImportSources.size())) ? &pDesc->m_ImportSources[m_iSelectedSource] : nullptr;
        }
        xanim_package_desc::clip* SelectedOv() noexcept
        {
            auto* pS = SelectedSource();
            return (pS && m_iSelectedDescriptorClip >= 0 && m_iSelectedDescriptorClip < int(pS->m_Clips.size())) ? &pS->m_Clips[m_iSelectedDescriptorClip] : nullptr;
        }
        std::string LayersPath() const { return std::format("AnimPackage/ImportSources[G:{}]/Clips[G:{}]/Layers", m_iSelectedSource, m_iSelectedDescriptorClip); }
        std::string LayerPath(int iLayer, const char* pField) const { return std::format("{}[G:{}]/{}", LayersPath(), iLayer, pField); }
        void SetLayer(int iLayer, const char* pField, const std::string& Value, const std::string& Before) noexcept
        {
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(LayerPath(iLayer, pField)), xeditor::Quote(Value), xeditor::Quote(Before)));
        }
        static std::string F(double v) { return std::format("{}", v); }

        const xanim_package_layers::source_frames* FindLayerSource(const std::wstring& Key, const std::string& Clip) const
        {
            for (const auto& S : m_LayerSources) if (S.m_Key == Key && S.m_Clip == Clip) return &S;
            return nullptr;
        }

        // Everything the composition depends on, as text: a change recomposes (cheap: a few layers)
        static std::string LayerSignature(const xanim_package_desc::clip& C)
        {
            std::string S;
            for (const auto& L : C.m_Layers)
            {
                S += std::format("{}|{}|", int(L.m_Space), int(L.m_Kind));
                for (const auto& K : L.m_Keys) S += std::format("p{},{},{},{},{},{},{},{},{},{},{},{};", K.m_Tick, K.m_Bone, K.m_X, K.m_Y, K.m_Z, K.m_W, K.m_bOffset, K.m_bStep, K.m_bPosition, K.m_PX, K.m_PY, K.m_PZ);
                S += std::format("s{},{}|", L.m_bSmooth, L.m_Falloff);
                S += std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|", L.m_bEnabled, int(L.m_Mode), int(L.m_Reference), L.m_ReferenceTick, xstrtool::To(L.m_SourceKey), L.m_SourceClip
                    , L.m_Start, L.m_End, L.m_TrimIn, L.m_TrimOut, L.m_Rate, L.m_bReverse, L.m_Weight, L.m_bRotation, L.m_bPosition, int(L.m_Root), L.m_Name);
                for (const auto& K : L.m_WeightKeys) S += std::format("k{},{},{};", K.m_Tick, K.m_Value, K.m_bStep);
                for (const auto& E : L.m_Mask) S += std::format("m{},{},{},{};", E.m_Bone, E.m_Value, E.m_bInherit, E.m_BlendDepth);
            }
            return S;
        }

        // The selected clip's live composition, remade when its layers changed (or the sources were reloaded)
        void UpdateLiveLayers() noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource(); auto* pDesc = Desc();
            const bool bAny = pOv && std::any_of(pOv->m_Layers.begin(), pOv->m_Layers.end(), [](const auto& L) { return L.m_bEnabled; });
            if (!bAny || !pDesc) { m_LayerComposed.clear(); m_LayerSig.clear(); m_LayerError.clear(); m_LayerReport = {}; return; }
            if (m_iSolo >= int(pOv->m_Layers.size())) m_iSolo = -1;
            const std::string Sig = std::format("{}:{}:{}:{}:{}", m_iSelectedSource, m_iSelectedDescriptorClip, m_LayerSources.size(), m_iSolo, LayerSignature(*pOv));
            if (Sig == m_LayerSig) return;
            m_LayerSig = Sig; m_LayerComposed.clear(); m_LayerError.clear();
            const auto* pBase = FindLayerSource(pS->key(), pOv->m_OriginalName);
            if (!pBase) { m_LayerError = m_LayerSources.empty() ? "no layer sources yet: compile the package once" : "this clip is not in the layer sources: compile the package"; return; }
            xanim_package_desc::clip Audition;                              // solo: a copy with only that layer on (the descriptor is untouched)
            const xanim_package_desc::clip* pUse = pOv;
            if (m_iSolo >= 0) { Audition = *pOv; for (int i = 0; i < int(Audition.m_Layers.size()); ++i) Audition.m_Layers[i].m_bEnabled = i == m_iSolo; pUse = &Audition; }
            std::vector<xmath::transform3> Out;
            const auto t0 = std::chrono::steady_clock::now();
            const bool bOk = xanim_package_layers::Compose(*pBase, *pUse, pDesc->m_ResolvedSkeletonBones, [this](const std::wstring& K, const std::string& C) { return FindLayerSource(K, C); }, Out, m_LayerReport, m_LayerError);
            m_LayerComposeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!bOk) return;
            m_LayerComposed = std::move(Out); m_LayerComposedFPS = pBase->m_FPS; m_LayerComposedFrames = pBase->m_nFrames;
        }
        bool LiveLayers() const noexcept { return m_bLayerLive && !m_LayerComposed.empty(); }

        // The live pose at the playhead: the composed source frames (blended between the two nearest), parent * local like a compiled clip
        void EvaluateLiveLayers(const xskeleton::skeleton& Skeleton) noexcept
        {
            const auto Bones = Skeleton.getBones();
            const int nB = int(Bones.size());
            if (int(m_LayerComposed.size()) != m_LayerComposedFrames * nB || m_LayerComposedFrames <= 0) { m_PoseWorlds = m_RestWorlds; return; }
            const float FT = std::max(0.0f, m_TimeSeconds) * float(m_LayerComposedFPS);
            const int A = std::clamp(int(FT), 0, m_LayerComposedFrames - 1), B = std::min(A + 1, m_LayerComposedFrames - 1);
            const float T = FT - std::floor(FT);
            m_PoseWorlds.resize(nB);
            for (int i = 0; i < nB; ++i)
            {
                const auto Local = xmath::transform3::fromBlend(m_LayerComposed[std::size_t(A) * nB + i], m_LayerComposed[std::size_t(B) * nB + i], T).toMatrix();
                m_PoseWorlds[i] = Bones[i].m_iParent < 0 ? Local : m_PoseWorlds[Bones[i].m_iParent] * Local;
            }
        }

        // The compiled clip against the live composition, frame by frame (only when the clip has no trim / downsample / root motion: then the
        // two are the same frames). Returns a sentence.
        std::string LayersParity() noexcept
        {
            auto* pOv = SelectedOv(); auto* pPackage = Package(); auto* pClip = SelectedClip();
            if (!pOv || m_LayerComposed.empty()) return "parity: no live composition";
            if (!pPackage || !pClip) return "parity: the clip is not compiled";
            if (pOv->m_DownsampleFPS > 0 || pOv->m_TrimStartFrame >= 0 || pOv->m_TrimEndFrame >= 0 || pOv->m_RootMotion != xanim_package::root_motion_mode::NONE)
                return "parity: skipped (the clip trims, downsamples or extracts root motion after the layers)";
            const int nB = int(pPackage->m_nBones);
            if (pClip->m_nFrames != m_LayerComposedFrames) return std::format("parity: frame counts differ (compiled {}, live {}): compile the package", pClip->m_nFrames, m_LayerComposedFrames);
            float Max = 0.0f; int nDiff = 0;
            for (int f = 0; f < pClip->m_nFrames; ++f)
            {
                const auto Frame = pPackage->getClipFrame(m_iSelectedClip, f);
                for (int b = 0; b < nB; ++b)
                {
                    const auto& C = Frame[b]; const auto& L = m_LayerComposed[std::size_t(f) * nB + b];
                    const float D = std::max({ std::abs(C.m_Position.m_X - L.m_Position.m_X), std::abs(C.m_Position.m_Y - L.m_Position.m_Y), std::abs(C.m_Position.m_Z - L.m_Position.m_Z)
                                             , std::abs(C.m_Rotation.m_X - L.m_Rotation.m_X), std::abs(C.m_Rotation.m_Y - L.m_Rotation.m_Y), std::abs(C.m_Rotation.m_Z - L.m_Rotation.m_Z), std::abs(C.m_Rotation.m_W - L.m_Rotation.m_W) });
                    Max = std::max(Max, D); nDiff += D > 0.0f;
                }
            }
            return std::format("parity: live vs compiled over {} frames: {} values differ, max difference {:.9g}", pClip->m_nFrames, nDiff, Max);
        }

        std::string LayerAdd(const std::wstring& Key, const std::string& Clip, std::string Name, bool bPose = false) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv) return "LayerAdd: select a clip first (SelectClip)";
            if (!bPose && pDesc->findImportSource(Key) == -1) return "LayerAdd: that source is not one of this package's import sources";
            const int n = int(pOv->m_Layers.size());
            std::uint64_t Id = 1;
            for (const auto& S : pDesc->m_ImportSources) for (const auto& C : S.m_Clips) for (const auto& L : C.m_Layers) Id = std::max(Id, L.m_Id + 1);
            if (Name.empty()) Name = bPose ? std::format("Pose {}", n) : std::format("{} ({})", Clip, std::filesystem::path(Key).stem().string());
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(LayersPath() + "[]"), xeditor::Quote(std::to_string(n + 1)), xeditor::Quote(std::to_string(n))));
            SetLayer(n, "Id", std::to_string(Id), "0");
            SetLayer(n, "Name", Name, "");
            m_iLayer = n;
            if (bPose)
            {   // additive: a key is a turn on top of the layers below (its absolute target re-derived when they change)
                SetLayer(n, "Kind", "Pose", "Clip");
                SetLayer(n, "Mode", "Additive", "Override");
                // the software's job, not the user's: a smooth curve through the keys, eased in and out over half a second
                SetLayer(n, "Smooth", "true", "false");
                SetLayer(n, "Falloff", "0.5", "0");
                return std::format("LayerAdd: pose layer {} '{}' (id {}); it has no keys: add them (PoseKey)", n, Name, Id);
            }
            SetLayer(n, "SourceKey", xstrtool::To(Key), "");
            SetLayer(n, "SourceClip", Clip, "");
            return std::format("LayerAdd: layer {} '{}' (id {}); its mask is empty: add bones (LayerMask) for it to do anything", n, Name, Id);
        }

        // The bone's local rotation composed from the layers under iLayer, at a time (the base when iLayer is 0)
        bool RotationBelow(int iLayer, int Bone, double Seconds, xmath::fquat& Q, std::string& Error) noexcept
        {
            xmath::fvec3 P;
            return TransformBelow(iLayer, Bone, Seconds, Q, P, Error);
        }
        // ... and its local position
        bool TransformBelow(int iLayer, int Bone, double Seconds, xmath::fquat& Q, xmath::fvec3& Pos, std::string& Error) noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource(); auto* pDesc = Desc();
            const auto* pBase = FindLayerSource(pS->key(), pOv->m_OriginalName);
            if (!pBase) { Error = "this clip is not in the layer sources: compile the package"; return false; }
            xanim_package_desc::clip Below = *pOv;
            Below.m_Layers.resize(iLayer);
            std::vector<xmath::transform3> Out; xanim_package_layers::report R;
            if (!xanim_package_layers::Compose(*pBase, Below, pDesc->m_ResolvedSkeletonBones, [this](const std::wstring& K, const std::string& C) { return FindLayerSource(K, C); }, Out, R, Error)) return false;
            const int nB = int(pDesc->m_ResolvedSkeletonBones.m_Bones.size()), nF = pBase->m_nFrames;
            const double FT = std::clamp(Seconds * std::max(1, pBase->m_FPS), 0.0, double(nF - 1));
            const int A = int(FT), B = std::min(A + 1, nF - 1);
            Q = xmath::fquat::Slerp(Out[std::size_t(A) * nB + Bone].m_Rotation, Out[std::size_t(B) * nB + Bone].m_Rotation, float(FT - A));
            const auto& PA = Out[std::size_t(A) * nB + Bone].m_Position; const auto& PB = Out[std::size_t(B) * nB + Bone].m_Position;
            Pos = PA + (PB - PA) * float(FT - A);
            return true;
        }

        // A key on a Pose layer: absolute (the pose below at that time turned by Turn, parent side) or the turn itself as an offset.
        // A key already on that bone at that tick is replaced.
        // pMove: also a position key - absolute: the position below moved by it (model units, the bone's parent space); offset: the move itself
        std::string PoseKey(int iLayer, const std::string& Bone, double Seconds, const xmath::fquat& Turn, bool bOffset, const xmath::fvec3* pMove = nullptr) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size())) return "PoseKey: no such layer in the selected clip";
            const auto& L = pOv->m_Layers[iLayer];
            if (L.m_Kind != xanim_package_desc::layer_kind::POSE) return "PoseKey: that layer is not a Pose layer";
            const auto& Bones = pDesc->m_ResolvedSkeletonBones.m_Bones;
            int b = -1;
            for (int s = 0; s < int(Bones.size()); ++s) if (Bones[s].m_Name == Bone) { b = s; break; }
            if (b < 0) return std::format("PoseKey: '{}' is not a bone of this skeleton", Bone);
            xmath::fquat Q = Turn;
            xmath::fvec3 Pos = pMove ? *pMove : xmath::fvec3(0, 0, 0);
            if (!bOffset)
            {
                xmath::fquat B; xmath::fvec3 BP; std::string Error;
                if (!TransformBelow(iLayer, b, Seconds, B, BP, Error)) return "PoseKey: " + Error;
                Q = (Turn * B).NormalizeCopy();
                Pos = BP + Pos;
            }
            const auto Tick = xanim_package_layers::ToTick(Seconds);
            int k = -1;
            for (int i = 0; i < int(L.m_Keys.size()); ++i) if (L.m_Keys[i].m_Bone == Bone && L.m_Keys[i].m_Tick == Tick) { k = i; break; }
            const bool bNew = k < 0;
            if (bNew) { k = int(L.m_Keys.size()); SetLayer(iLayer, "Keys[]", std::to_string(k + 1), std::to_string(k)); }
            const auto& Old = bNew ? xanim_package_desc::layer_pose_key{} : L.m_Keys[k];
            auto Field = [&](const char* p) { return std::format("Keys[G:{}]/{}", k, p); };
            if (bNew) { SetLayer(iLayer, Field("Tick").c_str(), std::to_string(Tick), "0"); SetLayer(iLayer, Field("Bone").c_str(), Bone, ""); }
            SetLayer(iLayer, Field("X").c_str(), F(Q.m_X), F(Old.m_X));
            SetLayer(iLayer, Field("Y").c_str(), F(Q.m_Y), F(Old.m_Y));
            SetLayer(iLayer, Field("Z").c_str(), F(Q.m_Z), F(Old.m_Z));
            SetLayer(iLayer, Field("W").c_str(), F(Q.m_W), F(Old.m_W));
            if (bOffset != Old.m_bOffset) SetLayer(iLayer, Field("Offset").c_str(), bOffset ? "true" : "false", Old.m_bOffset ? "true" : "false");
            if (pMove)
            {
                if (!Old.m_bPosition) SetLayer(iLayer, Field("Positioned").c_str(), "true", "false");
                SetLayer(iLayer, Field("PX").c_str(), F(Pos.m_X), F(Old.m_PX));
                SetLayer(iLayer, Field("PY").c_str(), F(Pos.m_Y), F(Old.m_PY));
                SetLayer(iLayer, Field("PZ").c_str(), F(Pos.m_Z), F(Old.m_PZ));
            }
            m_iLayer = iLayer;
            return std::format("PoseKey: layer {} key {} {} '{}' at tick {} ({}): {:.6f} {:.6f} {:.6f} {:.6f}{}", iLayer, k, bNew ? "added" : "replaced", Bone, Tick, bOffset ? "offset" : "absolute", Q.m_X, Q.m_Y, Q.m_Z, Q.m_W
                , pMove ? std::format(" position {:.5f} {:.5f} {:.5f}", Pos.m_X, Pos.m_Y, Pos.m_Z) : "");
        }

        // Adds the bone (with its branch, or alone) to the layer's mask, or removes its entry when it has one
        std::string LayerMaskToggle(int iLayer, const std::string& Bone, bool bOnly) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size())) return "LayerMask: no such layer in the selected clip";
            const auto& Mask = pOv->m_Layers[iLayer].m_Mask;
            for (int e = 0; e < int(Mask.size()); ++e)
                if (Mask[e].m_Bone == Bone)
                {
                    xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(iLayer, "Mask")), e));
                    return std::format("LayerMask: '{}' removed from layer {}'s mask", Bone, iLayer);
                }
            const int n = int(Mask.size());
            SetLayer(iLayer, "Mask[]", std::to_string(n + 1), std::to_string(n));
            SetLayer(iLayer, std::format("Mask[G:{}]/Bone", n).c_str(), Bone, "");
            if (bOnly) SetLayer(iLayer, std::format("Mask[G:{}]/Inherit", n).c_str(), "false", "true");
            return std::format("LayerMask: '{}' added to layer {}'s mask{}", Bone, iLayer, bOnly ? " (this bone only)" : " (with its branch)");
        }

        std::string LayersInfo() noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "LayersInfo: select a clip first";
            UpdateLiveLayers();
            std::string T = std::format("clip '{}': {} layer(s); layer sources {} clip(s){}\n", pOv->m_Name, pOv->m_Layers.size(), m_LayerSources.size(), m_LayerError.empty() ? "" : "; " + m_LayerError);
            for (int i = 0; i < int(pOv->m_Layers.size()); ++i)
            {
                const auto& L = pOv->m_Layers[i];
                T += std::format("  layer {} id {} '{}' {} {} source {}:{} weight {} keys {} mask {} entries [", i, L.m_Id, L.m_Name, L.m_bEnabled ? "on" : "off"
                    , L.m_Mode == xanim_package_desc::layer_mode::ADDITIVE ? "additive" : "override", xstrtool::To(L.m_SourceKey), L.m_SourceClip, L.m_Weight, L.m_WeightKeys.size(), L.m_Mask.size());
                for (const auto& E : L.m_Mask) T += std::format(" {}{}", E.m_Bone, E.m_bInherit ? "+" : "");
                T += " ]\n";
                if (L.m_Kind == xanim_package_desc::layer_kind::POSE) T += std::format("    {} curve, falloff {:.2f} s\n", L.m_bSmooth ? "smooth" : "linear", L.m_Falloff);
                if (L.m_Kind == xanim_package_desc::layer_kind::POSE)
                    for (const auto& K : L.m_Keys)
                    {
                        T += std::format("    pose key '{}' tick {} {} ({:.6f} {:.6f} {:.6f} {:.6f}){}", K.m_Bone, K.m_Tick, K.m_bOffset ? "offset" : "absolute", K.m_X, K.m_Y, K.m_Z, K.m_W
                            , K.m_bPosition ? std::format(" position ({:.5f} {:.5f} {:.5f})", K.m_PX, K.m_PY, K.m_PZ) : "");
                        // an absolute key's hit: the live pose there vs the key (exact when nothing above it turns that bone, weight 1)
                        const auto& Bones = Desc()->m_ResolvedSkeletonBones.m_Bones; const int nB = int(Bones.size());
                        int b = -1; for (int s = 0; s < nB; ++s) if (Bones[s].m_Name == K.m_Bone) { b = s; break; }
                        if (!K.m_bOffset && L.m_bEnabled && b >= 0 && !m_LayerComposed.empty() && int(m_LayerComposed.size()) == m_LayerComposedFrames * nB)
                        {
                            const double FT = std::clamp(xanimation::TicksToSeconds(K.m_Tick) * m_LayerComposedFPS, 0.0, double(m_LayerComposedFrames - 1));
                            const int A = int(FT), B = std::min(A + 1, m_LayerComposedFrames - 1);
                            const auto Q = xmath::fquat::Slerp(m_LayerComposed[std::size_t(A) * nB + b].m_Rotation, m_LayerComposed[std::size_t(B) * nB + b].m_Rotation, float(FT - A));
                            const double Dot = std::min(1.0, std::abs(double(Q.m_X) * K.m_X + double(Q.m_Y) * K.m_Y + double(Q.m_Z) * K.m_Z + double(Q.m_W) * K.m_W) / std::max(1e-12, std::sqrt(double(K.m_X) * K.m_X + double(K.m_Y) * K.m_Y + double(K.m_Z) * K.m_Z + double(K.m_W) * K.m_W)));
                            T += std::format(" live hit {:.4f} deg", 2.0 * std::acos(Dot) * 57.29577951308232);
                        }
                        T += "\n";
                    }
            }
            for (const auto& R : m_LayerReport.m_Layers) T += std::format("  live '{}': {} bones in its mask, bone-frames written {}, outside its mask {}\n", R.m_Name, R.m_nMaskBones, R.m_Written, R.m_WrittenOutside);
            T += std::format("  live recompose {:.3f} ms{}\n", m_LayerComposeMs, m_iSolo >= 0 ? std::format(" (solo layer {}: editor only)", m_iSolo) : std::string{});
            if (!m_LayerComposed.empty())
                T += std::format("  live isolation: bone-frames changed outside every mask {}{}; root travel change {:.6f} m\n  {}\n", m_LayerReport.m_ChangedOutside
                    , m_LayerReport.m_ChangedOutside ? " (first: " + m_LayerReport.m_FirstOutside + ")" : std::string{}, m_LayerReport.m_MaxRootMove, LayersParity());
            return T;
        }

        // The selected layer's mask, as the bones it touches (drawn selected in the viewport)
        void UpdateMaskShown() noexcept
        {
            m_MaskShown.clear();
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || !pDesc || m_iLayer < 0 || m_iLayer >= int(pOv->m_Layers.size())) return;
            const auto M = xanim_package_layers::LayerMask(pOv->m_Layers[m_iLayer], pDesc->m_ResolvedSkeletonBones);
            for (int b = 0; b < int(M.size()); ++b) if (M[b] > 0.0f) m_MaskShown.insert(b);
        }

        // A field edited in place: the widget edits a buffer while active (the descriptor value is re-read every frame) and the change is ONE
        // SetProperty when the edit ends. Returns true with the new value then.
        ImGuiID m_ActiveEdit = 0; double m_ActiveValue = 0;
        bool EditNumber(const char* pLabel, double Shown, double& Out, int Kind /*0 int, 1 float, 2 slider 0..1*/) noexcept
        {
            const ImGuiID Id = ImGui::GetID(pLabel);
            const bool bMine = m_ActiveEdit == Id;
            bool bDone = false;
            if (Kind == 0) { int V = int(bMine ? m_ActiveValue : Shown); ImGui::InputInt(pLabel, &V, 0); m_ActiveValue = ImGui::IsItemActive() ? V : m_ActiveValue; if (ImGui::IsItemDeactivatedAfterEdit()) { Out = V; bDone = true; } }
            else { float V = float(bMine ? m_ActiveValue : Shown); if (Kind == 1) ImGui::InputFloat(pLabel, &V, 0, 0, "%.3f"); else ImGui::SliderFloat(pLabel, &V, 0.0f, 1.0f, "%.2f");
                   m_ActiveValue = ImGui::IsItemActive() ? V : m_ActiveValue; if (ImGui::IsItemDeactivatedAfterEdit()) { Out = V; bDone = true; } }
            if (ImGui::IsItemActive()) m_ActiveEdit = Id; else if (bMine) m_ActiveEdit = 0;
            return bDone;
        }

        // a frame number of the clip's source <-> ticks (the panel shows frames)
        int    TicksToFrame(std::int64_t T) const { return int(std::llround(double(T) * m_LayerComposedFPS / double(xanimation::ticks_per_second_v))); }
        std::int64_t FrameToTicks(int F) const { return std::llround(double(F) * double(xanimation::ticks_per_second_v) / std::max(1, m_LayerComposedFPS)); }

        void RenderLayers() noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || !pDesc) { ImGui::TextDisabled("Select a clip (in Clips) to edit its layers."); return; }
            UpdateLiveLayers();
            ImGui::Text("Clip '%s'", pOv->m_Name.c_str());
            ImGui::SameLine(); ImGui::Checkbox("Live", &m_bLayerLive);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("On: the viewport shows the layers composed now (the compile's own function, on the source frames before\nthe clip's trim / downsample / root motion). Off: the compiled clip.");
            ImGui::SameLine(); ImGui::Checkbox("Edit mask", &m_bMaskEdit);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("A click on a bone adds it (with its branch) to the selected layer's mask, or removes it.\nShift-click: this bone only.");
            if (!m_LayerError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", m_LayerError.c_str());
            ImGui::Separator();

            // + Layer: any clip of this package's import sources (its source frames, not a compiled output: no cycles)
            if (ImGui::BeginCombo("##addlayer", "+ Layer from a clip...", ImGuiComboFlags_HeightLarge))
            {
                for (const auto& S : pDesc->m_ImportSources)
                    for (const auto& C : S.m_Clips)
                    {
                        const auto Label = std::format("{}  ({})", C.m_OriginalName, xstrtool::To(S.key()));
                        if (ImGui::Selectable(Label.c_str())) LayerAdd(S.key(), C.m_OriginalName, {});
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Pose layer")) LayerAdd({}, {}, {}, true);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("A layer of keyed poses (additive: each key turns the bone on top of the layers below).\nKeys: the PoseKey command (the gizmo comes next).");

            constexpr ImGuiTableFlags TF = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
            const int n = int(pOv->m_Layers.size());
            if (n && ImGui::BeginTable("###Layers", 5, TF))
            {
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 26.0f);
                ImGui::TableSetupColumn("Layer (top first)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 74.0f);
                ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("##ops", ImGuiTableColumnFlags_WidthFixed, 86.0f);
                ImGui::TableHeadersRow();
                for (int i = n - 1; i >= 0; --i)          // top layer first, like every layer list
                {
                    const auto& L = pOv->m_Layers[i];
                    ImGui::PushID(i); ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    { bool V = L.m_bEnabled; if (ImGui::Checkbox("##on", &V)) SetLayer(i, "Enabled", V ? "true" : "false", L.m_bEnabled ? "true" : "false"); }
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(L.m_Name.empty() ? "(unnamed)" : L.m_Name.c_str(), m_iLayer == i)) m_iLayer = i;
                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    { int M = int(L.m_Mode); if (ImGui::Combo("##mode", &M, "Override\0Additive\0")) SetLayer(i, "Mode", M ? "Additive" : "Override", L.m_Mode == xanim_package_desc::layer_mode::ADDITIVE ? "Additive" : "Override"); }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (double W; EditNumber("##w", L.m_Weight, W, 2)) SetLayer(i, "Weight", F(W), F(L.m_Weight));
                    ImGui::TableSetColumnIndex(4);
                    {
                        const bool bSolo = m_iSolo == i;
                        if (bSolo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.1f, 1.0f));
                        if (ImGui::SmallButton("S")) m_iSolo = bSolo ? -1 : i;
                        if (bSolo) ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) xeditor::hint::Text("Solo: see this layer alone on the clip (the preview only: never saved, never in the build)");
                        ImGui::SameLine();
                    }
                    ImGui::BeginDisabled(i == n - 1); if (ImGui::SmallButton("Up")) { xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Move -Index {} -ToIndex {}", xeditor::Quote(LayersPath()), i, i + 1)); m_iLayer = i + 1; } ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) { xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayersPath()), i)); if (m_iLayer >= i) --m_iLayer; }
                    if (ImGui::IsItemHovered()) xeditor::hint::Text("Delete this layer (undoable)");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!n) { ImGui::TextDisabled("No layers: this clip compiles exactly as imported."); return; }
            if (m_iLayer < 0 || m_iLayer >= n) m_iLayer = n - 1;

            // the selected layer's details
            const auto& L = pOv->m_Layers[m_iLayer];
            ImGui::Separator();
            ImGui::Text("Layer %d: %s:%s", m_iLayer, xstrtool::To(std::filesystem::path(L.m_SourceKey).filename().wstring()).c_str(), L.m_SourceClip.c_str());
            { bool R = L.m_bRotation; if (ImGui::Checkbox("Rotations", &R)) SetLayer(m_iLayer, "Rotation", R ? "true" : "false", L.m_bRotation ? "true" : "false"); }
            ImGui::SameLine(); { bool P = L.m_bPosition; if (ImGui::Checkbox("Positions", &P)) SetLayer(m_iLayer, "Position", P ? "true" : "false", L.m_bPosition ? "true" : "false"); }
            if (ImGui::IsItemHovered()) xeditor::hint::Text("The channels the layer brings. The root's position (its travel) is never taken from a layer.");
            {
                int Sp = int(L.m_Space); ImGui::SetNextItemWidth(150.0f);
                if (ImGui::Combo("Rotation space", &Sp, "Local\0Skeleton model\0")) SetLayer(m_iLayer, "Space", Sp ? "Skeleton model" : "Local", L.m_Space == xanim_package_desc::layer_space::SKELETON_MODEL ? "Skeleton model" : "Local");
                if (ImGui::IsItemHovered()) xeditor::hint::Text("Override rotations. Local: each bone keeps its pose relative to its parent (a masked arm follows the body below).\nSkeleton model: relative to the character, so a masked arm keeps the layer's orientation whatever the spine below does\n(not a world lock; additive layers are always local).");
            }
            auto FrameField = [&](const char* pLabel, const char* pField, std::int64_t Value)        // shown in frames, stored in ticks (-1 stays -1)
            {
                ImGui::SetNextItemWidth(90.0f);
                if (double V; EditNumber(pLabel, Value < 0 ? -1.0 : double(TicksToFrame(Value)), V, 0))
                    SetLayer(m_iLayer, pField, std::to_string(V < 0 ? -1 : FrameToTicks(int(V))), std::to_string(Value));
            };
            FrameField("Start", "Start", L.m_Start); ImGui::SameLine(); FrameField("End (-1: clip end)", "End", L.m_End);
            FrameField("Trim in", "TrimIn", L.m_TrimIn); ImGui::SameLine(); FrameField("Trim out (-1: source end)", "TrimOut", L.m_TrimOut);
            ImGui::TextDisabled("(frames at %d fps)", m_LayerComposedFPS);
            ImGui::SetNextItemWidth(90.0f);
            if (double R; EditNumber("Rate", L.m_Rate, R, 1)) SetLayer(m_iLayer, "Rate", F(std::max(0.0, R)), F(L.m_Rate));
            ImGui::SameLine(); { bool V = L.m_bReverse; if (ImGui::Checkbox("Reverse", &V)) SetLayer(m_iLayer, "Reverse", V ? "true" : "false", L.m_bReverse ? "true" : "false"); }

            // weight keys
            ImGui::Separator();
            ImGui::Text("Weight keys (%d)", int(L.m_WeightKeys.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Key at playhead"))
            {
                const int k = int(L.m_WeightKeys.size());
                SetLayer(m_iLayer, "WeightKeys[]", std::to_string(k + 1), std::to_string(k));
                SetLayer(m_iLayer, std::format("WeightKeys[G:{}]/Tick", k).c_str(), std::to_string(xanim_package_layers::ToTick(m_TimeSeconds)), "0");
                SetLayer(m_iLayer, std::format("WeightKeys[G:{}]/Value", k).c_str(), F(L.m_Weight), "1");
            }
            for (int k = 0; k < int(L.m_WeightKeys.size()); ++k)
            {
                const auto& K = L.m_WeightKeys[k];
                ImGui::PushID(1000 + k);
                ImGui::Text("%.2fs", xanimation::TicksToSeconds(K.m_Tick)); ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                if (double V; EditNumber("##kv", K.m_Value, V, 2)) SetLayer(m_iLayer, std::format("WeightKeys[G:{}]/Value", k).c_str(), F(V), F(K.m_Value));
                ImGui::SameLine(); if (ImGui::SmallButton("X")) xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(m_iLayer, "WeightKeys")), k));
                ImGui::PopID();
            }

            // mask entries
            ImGui::Separator();
            ImGui::Text("Mask (%d entries)", int(L.m_Mask.size()));
            if (L.m_Mask.empty()) ImGui::TextDisabled("Empty: the layer touches nothing. Tick 'Edit mask' and click bones.");
            for (int e = 0; e < int(L.m_Mask.size()); ++e)
            {
                const auto& E = L.m_Mask[e];
                ImGui::PushID(2000 + e);
                ImGui::Text("%s%s", E.m_Bone.c_str(), E.m_bInherit ? " (+branch)" : ""); ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                if (double V; EditNumber("##mv", E.m_Value, V, 2)) SetLayer(m_iLayer, std::format("Mask[G:{}]/Value", e).c_str(), F(V), F(E.m_Value));
                ImGui::SameLine(); ImGui::SetNextItemWidth(60.0f);
                if (double D; EditNumber("depth", E.m_BlendDepth, D, 0)) SetLayer(m_iLayer, std::format("Mask[G:{}]/BlendDepth", e).c_str(), std::to_string(std::max(0, int(D))), std::to_string(E.m_BlendDepth));
                if (ImGui::IsItemHovered()) xeditor::hint::Text("Blend depth: 0 = the whole branch at the value; N = it ramps in over N bones down the branch");
                ImGui::SameLine(); if (ImGui::SmallButton("X")) xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(m_iLayer, "Mask")), e));
                ImGui::PopID();
            }

            // the live isolation report
            if (!m_LayerComposed.empty())
            {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                for (const auto& R : m_LayerReport.m_Layers) ImGui::TextDisabled("'%s': %d bones in its mask, %zu bone-frames written (outside its mask: %zu)", R.m_Name.c_str(), R.m_nMaskBones, R.m_Written, R.m_WrittenOutside);
                const bool bLeak = m_LayerReport.m_ChangedOutside > 0;
                ImGui::TextColored(bLeak ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f), "Outside every mask: %zu bone-frames changed; root travel change %.4f m", m_LayerReport.m_ChangedOutside, m_LayerReport.m_MaxRootMove);
                ImGui::TextDisabled("Composed in %.2f ms%s", m_LayerComposeMs, m_iSolo >= 0 ? "  (SOLO: one layer auditioned; the build uses every enabled layer)" : "");
                ImGui::PopTextWrapPos();
            }
        }

        //----------------------------------------------------------------------------------------
        // Kimodo: text -> preview -> Accept (asset + descriptor entry, saved) or Discard
        //----------------------------------------------------------------------------------------

        std::filesystem::path ProjectPath() const noexcept { return std::filesystem::path(m_Document.m_TempDir).parent_path().parent_path(); }    // <project>\Cache\Temp

        // Why nothing can be generated (nullptr: ready)
        const char* KimodoWhyNot() noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) return "the descriptor is not loaded";
            if (pDesc->m_SkeletonRef.empty()) return "set this package's SkeletonRef first";
            if (pDesc->m_Kimodo.m_Retarget.empty()) return "set Kimodo/Retarget in the Description panel: a Retarget from Kimodo's SOMA 77 skeleton (KimodoSOMA77) to this package's skeleton";
            if (pDesc->m_Kimodo.m_Folder.empty()) return "set Kimodo/Folder in the Description panel: the asset folder accepted clips go to";
            return nullptr;
        }

        // The Retarget's Source skeleton (SOMA 77): read from the Retarget's descriptor, like the compiler does
        bool KimodoResolveSkeleton(std::string& Error) noexcept
        {
            auto* pDesc = Desc();
            const std::uint64_t Key = pDesc->m_Kimodo.m_Retarget.m_Instance.m_Value;
            const auto Path = ProjectPath() / std::format(L"Descriptors/Retarget/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", Key & 0xff, (Key & 0xff00) >> 8, Key);
            xretarget_desc::descriptor R;
            xproperty::settings::context Context{};
            if (auto Err = R.Serialize(true, Path.wstring(), Context); Err || R.m_Source.empty()) { Error = "the Kimodo Retarget could not be read (is it saved and are its skeletons compiled?)"; return false; }
            if (R.m_Destination.m_Instance.m_Value != pDesc->m_SkeletonRef.m_Instance.m_Value) { Error = "the Kimodo Retarget's Destination is not this package's skeleton"; return false; }
            if (m_K.m_SkeletonRef.m_Instance.m_Value != R.m_Source.m_Instance.m_Value)
            {
                xresource::g_Mgr.ReleaseRef(m_K.m_SkeletonRef);
                m_K.m_SkeletonRef.m_Instance = R.m_Source.m_Instance;
            }
            if (!xresource::g_Mgr.getResource(m_K.m_SkeletonRef)) { m_K.m_SkeletonRef.clear(); Error = "the Kimodo Retarget's Source skeleton could not be loaded (compile it)"; return false; }
            return true;
        }

        std::string KimodoGenerate(const std::string& Prompt, float Seconds, int Seed) noexcept
        {
            if (auto* pWhy = KimodoWhyNot()) return std::string("KimodoGenerate: ") + pWhy;
            if (m_K.m_Job.m_bRunning) return "KimodoGenerate: a generation is already running";
            const auto Exe = kimodo::FindExe();
            if (Exe.empty()) return "KimodoGenerate: xkimodo.exe not found (build dependencies/xKimodo, or set XKIMODO_EXE)";
            std::string Err;
            if (!KimodoResolveSkeleton(Err)) return "KimodoGenerate: " + Err;
            if (!m_K.m_Job.Start(Exe, m_Document.m_TempDir, Prompt, std::clamp(Seconds, 0.5f, 60.0f), Seed, Err)) return "KimodoGenerate: " + Err;
            m_K.m_PromptUsed = Prompt; m_K.m_SecondsUsed = Seconds; m_K.m_SeedUsed = Seed;
            m_K.m_Status = "generating..."; m_K.m_bStatusError = false;
            return "KimodoGenerate: started";
        }

        // Once a frame: a finished generation becomes the preview
        void KimodoPoll() noexcept
        {
            bool Ok = false; std::string Msg;
            if (!m_K.m_Job.Poll(Ok, Msg)) return;
            m_K.m_bStatusError = !Ok;
            m_K.m_Status = Msg.empty() ? (Ok ? "done" : "xkimodo failed (no message)") : Msg;
            if (!Ok) return;
            kimodo::bvh B; std::string Err;
            if (!B.Load(m_K.m_Job.m_Bvh, Err)) { m_K.m_bStatusError = true; m_K.m_Status = "the generated BVH could not be read: " + Err; return; }
            auto* pSkel = m_K.m_SkeletonRef.empty() ? nullptr : xresource::g_Mgr.getResource(m_K.m_SkeletonRef);
            if (!pSkel) { m_K.m_bStatusError = true; m_K.m_Status = "the SOMA skeleton is not loaded"; return; }
            m_K.m_BoneToJoint.assign(pSkel->getBones().size(), -1);
            int Missing = 0;
            for (int j = 0; j < B.nJoints(); ++j)
                if (const int b = pSkel->findBoneIndex(xstrtool::CRC32(B.m_Names[j])); b >= 0) m_K.m_BoneToJoint[b] = j;
            for (int v : m_K.m_BoneToJoint) Missing += v < 0;
            if (Missing) { m_K.m_bStatusError = true; m_K.m_Status = std::format("the SOMA skeleton has {} bones the generated motion does not (is the Retarget's Source Kimodo's skeleton?)", Missing); return; }
            m_K.m_Preview = std::move(B); m_K.m_bPreview = true; m_K.m_Time = 0.0f; m_K.m_bPlaying = true;
            std::snprintf(m_K.m_Name, sizeof(m_K.m_Name), "%s", kimodo::NameFromPrompt(m_K.m_PromptUsed).c_str());
            m_K.m_RetargetError.clear();
            if (!KimodoRetargetPreview(m_K.m_RetargetError)) m_K.m_Baked.clear();
        }

        // The generated motion moved onto this package's skeleton exactly as the compile will move it (xanim_package_compiler.cpp RetargetRaw):
        // the BVH's joints as the importer reads them (centimetres, OFFSET for the rest, the root's channels absolute), into the Retarget source
        // skeleton's space (its Pre/PostTransform), then baked by the Retarget's retargeter.
        // The package's Kimodo Retarget, read once: its mapping, the retargeter (SOMA -> this skeleton) and the reverse one (this skeleton -> SOMA)
        kimodo_state::retarget_ctx* KimodoRetargetCtx(std::string& Error) noexcept
        {
            auto* pDesc = Desc();
            const std::uint64_t Key = pDesc->m_Kimodo.m_Retarget.m_Instance.m_Value;
            if (!m_K.m_pRetarget || m_K.m_pRetarget->m_Key != Key)
            {
                auto Ctx = std::make_unique<kimodo_state::retarget_ctx>();
                const auto Path = ProjectPath() / std::format(L"Descriptors/Retarget/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", Key & 0xff, (Key & 0xff00) >> 8, Key);
                xproperty::settings::context Context{};
                if (auto Err = Ctx->m_Descriptor.Serialize(true, Path.wstring(), Context); Err) { Error = "the Retarget could not be read"; return nullptr; }
                if (auto Err = xretarget_desc::MakeMapping(Ctx->m_Descriptor, Ctx->m_Mapping); Err) { Error = "the Retarget's mapping could not be made"; return nullptr; }
                Ctx->m_Retargeter.Init(Ctx->m_Mapping.sourceCalibrated(), Ctx->m_Mapping.destinationCalibrated(), Ctx->m_Mapping.m_Auto.m_Map);
                Ctx->m_ReverseMap = xanimation::ReverseMap(Ctx->m_Mapping.m_Auto.m_Map, int(Ctx->m_Descriptor.m_ResolvedSource.m_Bones.size()));
                Ctx->m_Reverse.Init(Ctx->m_Mapping.destinationCalibrated(), Ctx->m_Mapping.sourceCalibrated(), Ctx->m_ReverseMap);
                Ctx->m_Key = Key;
                m_K.m_pRetarget = std::move(Ctx);
            }
            return m_K.m_pRetarget.get();
        }

        bool KimodoRetargetPreview(std::string& Error) noexcept
        {
            if (!KimodoRetargetCtx(Error)) return false;
            auto& Ctx = *m_K.m_pRetarget;
            const auto& Src = Ctx.m_Descriptor.m_ResolvedSource;
            const auto& Dst = Ctx.m_Descriptor.m_ResolvedDestination;
            const int nSrc = int(Src.m_Bones.size()), nDst = int(Dst.m_Bones.size());
            auto* pSkel = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkel || int(pSkel->getBones().size()) != nDst) { Error = "this package's skeleton is not loaded (compile the package once)"; return false; }

            const auto& B = m_K.m_Preview;
            std::vector<int> SrcToJoint(nSrc, -1);
            for (int s = 0; s < nSrc; ++s)
                for (int j = 0; j < B.nJoints(); ++j) if (B.m_Names[j] == Src.m_Bones[s].m_Name) { SrcToJoint[s] = j; break; }
            int iRoot = -1;
            for (int s = 0; s < nSrc; ++s) if (Src.m_Bones[s].m_iParent < 0) { iRoot = s; break; }
            if (iRoot < 0 || SrcToJoint[iRoot] < 0) { Error = "the Retarget's Source skeleton is not Kimodo's (no Hips)"; return false; }

            const int nFrames = B.m_nFrames;
            std::vector<xmath::transform3> Frames(std::size_t(nFrames) * nSrc);
            for (int f = 0; f < nFrames; ++f)
                for (int s = 0; s < nSrc; ++s)
                {
                    auto& T = Frames[std::size_t(f) * nSrc + s];
                    const auto& SB = Src.m_Bones[s];
                    const int j = SrcToJoint[s];
                    if (j < 0) { T = xmath::transform3{ .m_Scale = SB.m_RestScale, .m_Rotation = xmath::fquat(SB.m_RestRotX, SB.m_RestRotY, SB.m_RestRotZ, SB.m_RestRotW), .m_Position = SB.m_RestPosition }; continue; }
                    float q[4]; kimodo::MatToQuat(&B.m_Rots[(std::size_t(f) * B.nJoints() + j) * 9], q);
                    const float* p = (B.m_Parents[j] < 0 && B.m_bRootChannels) ? &B.m_Root[f * 3] : &B.m_Offsets[j * 3];
                    T = xmath::transform3{ .m_Scale = xmath::fvec3(1, 1, 1), .m_Rotation = xmath::fquat(q[0], q[1], q[2], q[3]), .m_Position = xmath::fvec3(p[0] * 100.0f, p[1] * 100.0f, p[2] * 100.0f) };
                }
            auto Corr = [](const xmath::fvec3& S, const xmath::fvec3& R, const xmath::fvec3& T) { xanimation::root_correction C; C.m_Scale = S; C.m_RotationDeg = R; C.m_Translation = T; return C; };
            xanimation::ToSkeletonSpace(Frames, nSrc, iRoot
                , Corr(Src.m_PostTransformScale, Src.m_PostTransformRotationDeg, Src.m_PostTransformTranslation)
                , Corr(Src.m_PreTransformScale,  Src.m_PreTransformRotationDeg,  Src.m_PreTransformTranslation));
            m_K.m_Baked = xanimation::BakeFrames(Ctx.m_Retargeter, xanimation::clip_view{ Frames, nSrc, nFrames, int(std::lround(1.0f / B.m_FrameTime)), false }, nDst);
            m_K.m_nDst = nDst;
            return true;
        }

        void KimodoEvaluateRetargeted() noexcept
        {
            auto* pSkel = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkel) return;
            const auto Bones = pSkel->getBones();
            const int f = std::clamp(int(m_K.m_Time / m_K.m_Preview.m_FrameTime), 0, m_K.m_Preview.m_nFrames - 1);
            m_K.m_DstWorlds.resize(Bones.size()); m_K.m_DstBones.resize(Bones.size());
            for (int i = 0; i < int(Bones.size()); ++i)
            {
                const auto LocalMat = m_K.m_Baked[std::size_t(f) * m_K.m_nDst + i].toMatrix();
                m_K.m_DstWorlds[i] = Bones[i].m_iParent < 0 ? LocalMat : m_K.m_DstWorlds[Bones[i].m_iParent] * LocalMat;
                m_K.m_DstBones[i].m_Position = m_K.m_DstWorlds[i].ExtractPosition();
                m_K.m_DstBones[i].m_Right    = m_K.m_DstWorlds[i].Right();
                m_K.m_DstBones[i].m_Up       = m_K.m_DstWorlds[i].Up();
            }
        }

        void KimodoEvaluate() noexcept
        {
            auto& P = m_K.m_Preview;
            const int f = std::clamp(int(m_K.m_Time / P.m_FrameTime), 0, P.m_nFrames - 1);
            P.Pose(f, m_K.m_Pos, m_K.m_World);
            m_K.m_Bones.resize(m_K.m_BoneToJoint.size());
            for (std::size_t b = 0; b < m_K.m_Bones.size(); ++b)
            {
                const int j = m_K.m_BoneToJoint[b];
                const float* p = &m_K.m_Pos[j * 3]; const float* R = &m_K.m_World[j * 9];
                m_K.m_Bones[b].m_Position = xmath::fvec3(p[0], p[1], p[2]);
                m_K.m_Bones[b].m_Right    = xmath::fvec3(R[0], R[3], R[6]);
                m_K.m_Bones[b].m_Up       = xmath::fvec3(R[1], R[4], R[7]);
            }
        }

        //----------------------------------------------------------------------------------------
        // Suggest Pose (posing v2 step S): the handles (one moved, some pinned) at a key -> a Kimodo window around it with the key's hands and feet
        // as end-effector constraints, two sparse full-body anchors from the clip and the clip's root path -> Kimodo's pose at the key, brought
        // back to this skeleton and corrected by body_ik (pins exact, the moved handle as a goal, the skeleton's joint limits) -> a CANDIDATE with
        // its report. Rotation only: Pose layer keys do not carry the hips' translation, so the clip's translation is kept.
        //----------------------------------------------------------------------------------------

        std::string SuggestSignature() noexcept
        {
            auto* pOv = SelectedOv();
            return pOv ? std::format("{}:{}:{}", m_iSelectedSource, m_iSelectedDescriptorClip, LayerSignature(*pOv)) : std::string{};
        }

        // The selected clip as the authoring stack has it: its live composition, or its source frames when it has no layer on
        bool SuggestClipFrames(const std::vector<xmath::transform3>*& pFrames, int& FPS, int& nFrames, std::string& Error) noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource();
            if (!pOv || !pS) { Error = "select a clip first (SelectClip)"; return false; }
            UpdateLiveLayers();
            if (!m_LayerError.empty() && m_LayerComposed.empty() && std::any_of(pOv->m_Layers.begin(), pOv->m_Layers.end(), [](const auto& L) { return L.m_bEnabled; })) { Error = m_LayerError; return false; }
            if (!m_LayerComposed.empty()) { pFrames = &m_LayerComposed; FPS = m_LayerComposedFPS; nFrames = m_LayerComposedFrames; return true; }
            const auto* pBase = FindLayerSource(pS->key(), pOv->m_OriginalName);
            if (!pBase) { Error = m_LayerSources.empty() ? "no layer sources yet: compile the package once" : "this clip is not in the layer sources: compile the package"; return false; }
            pFrames = &pBase->m_Frames; FPS = std::max(1, pBase->m_FPS); nFrames = pBase->m_nFrames;
            return true;
        }

        static std::vector<xmath::transform3> SamplePose(const std::vector<xmath::transform3>& Frames, int FPS, int nFrames, int nB, double Seconds) noexcept
        {
            const double FT = std::clamp(Seconds * FPS, 0.0, double(std::max(0, nFrames - 1)));
            const int A = int(FT), B = std::min(A + 1, nFrames - 1);
            const float T = float(FT - A);
            std::vector<xmath::transform3> P(nB);
            for (int i = 0; i < nB; ++i) P[i] = xmath::transform3::fromBlend(Frames[std::size_t(A) * nB + i], Frames[std::size_t(B) * nB + i], T);
            return P;
        }
        static float AngleDeg(const xmath::fquat& A, const xmath::fquat& B) noexcept
        {
            return 2.0f * std::acos(std::min(1.0f, std::abs(xmath::fquat::Dot(A.NormalizeCopy(), B.NormalizeCopy())))) * 57.29578f;
        }

        // Kimodo's space for a window of this skeleton's frames: the Retarget's links reversed into SOMA 77, the window's first root at the
        // origin facing +Z (Kimodo's generations start there)
        struct kimodo_space
        {
            xmath::fquat                    m_Norm, m_Denorm;
            xmath::fvec3                    m_Origin{ 0, 0, 0 };        // cm (the raw space)
            int                             m_iRoot = -1, m_nSrc = 0;
            xanimation::root_correction     m_Post, m_Pre;
        };
        bool ToKimodo(kimodo_state::retarget_ctx& Ctx, const std::vector<xmath::transform3>& Win, int W, int nB, kimodo_space& KS, std::vector<kimodo::soma_pose>& SP, std::string& Err) noexcept
        {
            const auto& Src = Ctx.m_Descriptor.m_ResolvedSource;
            const int nSrc = int(Src.m_Bones.size()); KS.m_nSrc = nSrc;
            auto Soma = xanimation::BakeFrames(Ctx.m_Reverse, xanimation::clip_view{ Win, nB, W, 30, false }, nSrc);
            KS.m_iRoot = -1;
            for (int s = 0; s < nSrc; ++s) if (Src.m_Bones[s].m_iParent < 0) { KS.m_iRoot = s; break; }
            if (KS.m_iRoot < 0) { Err = "the Retarget's Source skeleton has no root"; return false; }
            const int iRoot = KS.m_iRoot;
            auto Corr = [](const xmath::fvec3& S, const xmath::fvec3& R, const xmath::fvec3& T) { xanimation::root_correction C; C.m_Scale = S; C.m_RotationDeg = R; C.m_Translation = T; return C; };
            KS.m_Post = Corr(Src.m_PostTransformScale, Src.m_PostTransformRotationDeg, Src.m_PostTransformTranslation);
            KS.m_Pre  = Corr(Src.m_PreTransformScale,  Src.m_PreTransformRotationDeg,  Src.m_PreTransformTranslation);
            xanimation::FromSkeletonSpace(Soma, nSrc, iRoot, KS.m_Post, KS.m_Pre);
            const auto& R0 = Soma[iRoot];
            const auto F0 = R0.m_Rotation * xmath::fvec3(0, 0, 1);
            const float Heading = std::atan2(F0.m_X, F0.m_Z);
            KS.m_Norm = xanimation::details::AxisAngle(xmath::fvec3(0, 1, 0), -Heading); KS.m_Denorm = KS.m_Norm.ConjugateCopy();
            KS.m_Origin = xmath::fvec3(R0.m_Position.m_X, 0, R0.m_Position.m_Z);
            std::vector<int> SrcOfJoint(77, -1);
            for (int j = 0; j < 77; ++j) for (int s = 0; s < nSrc; ++s) if (Src.m_Bones[s].m_Name == kimodo::soma77_names_v[j]) { SrcOfJoint[j] = s; break; }
            if (SrcOfJoint[0] != iRoot) { Err = "the Retarget's Source is not Kimodo's SOMA 77 skeleton (its root is not Hips)"; return false; }
            SP.assign(W, {});
            for (int i = 0; i < W; ++i)
            {
                auto& P = SP[i];
                for (int j = 0; j < 77; ++j)
                {
                    const int s = SrcOfJoint[j];
                    if (s < 0) continue;
                    const auto& T = Soma[std::size_t(i) * nSrc + s];
                    const auto V = xanimation::details::LogQ(j == 0 ? (KS.m_Norm * T.m_Rotation) : T.m_Rotation);
                    P.m_Rot[j][0] = V.m_X; P.m_Rot[j][1] = V.m_Y; P.m_Rot[j][2] = V.m_Z;
                }
                const auto Rp = KS.m_Norm * (Soma[std::size_t(i) * nSrc + iRoot].m_Position - KS.m_Origin) * 0.01f;
                P.m_Root[0] = Rp.m_X; P.m_Root[1] = Rp.m_Y; P.m_Root[2] = Rp.m_Z;
            }
            return true;
        }
        // a generated BVH back on this skeleton ([frame][bone] local): Kimodo's frame undone, the compile's own retarget path
        std::vector<xmath::transform3> FromKimodo(kimodo_state::retarget_ctx& Ctx, const kimodo::bvh& B, const kimodo_space& KS, int nB) noexcept
        {
            const auto& Src = Ctx.m_Descriptor.m_ResolvedSource;
            const int nSrc = KS.m_nSrc;
            std::vector<int> SrcToJoint(nSrc, -1);
            for (int s = 0; s < nSrc; ++s) for (int j = 0; j < B.nJoints(); ++j) if (B.m_Names[j] == Src.m_Bones[s].m_Name) { SrcToJoint[s] = j; break; }
            std::vector<xmath::transform3> Gen(std::size_t(B.m_nFrames) * nSrc);
            for (int f = 0; f < B.m_nFrames; ++f)
                for (int s = 0; s < nSrc; ++s)
                {
                    auto& T = Gen[std::size_t(f) * nSrc + s];
                    const auto& SB = Src.m_Bones[s];
                    const int j = SrcToJoint[s];
                    if (j < 0) { T = xmath::transform3{ .m_Scale = SB.m_RestScale, .m_Rotation = xmath::fquat(SB.m_RestRotX, SB.m_RestRotY, SB.m_RestRotZ, SB.m_RestRotW), .m_Position = SB.m_RestPosition }; continue; }
                    float q[4]; kimodo::MatToQuat(&B.m_Rots[(std::size_t(f) * B.nJoints() + j) * 9], q);
                    xmath::fquat Q(q[0], q[1], q[2], q[3]);
                    const bool bRoot = B.m_Parents[j] < 0 && B.m_bRootChannels;
                    const float* p = bRoot ? &B.m_Root[f * 3] : &B.m_Offsets[j * 3];
                    xmath::fvec3 Pos(p[0] * 100.0f, p[1] * 100.0f, p[2] * 100.0f);
                    if (bRoot) { Q = KS.m_Denorm * Q; Pos = KS.m_Denorm * Pos + KS.m_Origin; }
                    T = xmath::transform3{ .m_Scale = xmath::fvec3(1, 1, 1), .m_Rotation = Q, .m_Position = Pos };
                }
            xanimation::ToSkeletonSpace(Gen, nSrc, KS.m_iRoot, KS.m_Post, KS.m_Pre);
            return xanimation::BakeFrames(Ctx.m_Retargeter, xanimation::clip_view{ Gen, nSrc, B.m_nFrames, 30, false }, nB);
        }

        std::string SuggestPose(double Seconds, const std::string& Move, const std::string& Pins, const std::string& Seeds, const std::string& Prompt) noexcept
        {
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();
            auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            auto* pDesc = Desc();
            if (!pDesc) return "SuggestPose: nothing loaded";
            if (auto* pWhy = KimodoWhyNot()) return std::string("SuggestPose: ") + pWhy;
            const auto Exe = kimodo::FindExe();
            if (Exe.empty()) return "SuggestPose: xkimodo.exe not found (build dependencies/xKimodo, or set XKIMODO_EXE)";
            std::string Err;
            auto* pCtx = KimodoRetargetCtx(Err);
            if (!pCtx) return "SuggestPose: " + Err;
            auto& Ctx = *pCtx;
            const std::vector<xmath::transform3>* pFrames = nullptr; int FPS = 30, nF = 0;
            if (!SuggestClipFrames(pFrames, FPS, nF, Err)) return "SuggestPose: " + Err;

            // this skeleton
            const auto& Man = pDesc->m_ResolvedSkeletonBones.m_Bones;
            const int nB = int(Man.size());
            if (nB == 0 || int(pFrames->size()) != nF * nB || int(Ctx.m_Descriptor.m_ResolvedDestination.m_Bones.size()) != nB) return "SuggestPose: the clip, the skeleton and the Retarget disagree on the bones (compile the package)";
            std::vector<std::int32_t> Parents(nB); std::vector<std::string> Names(nB); std::vector<std::string_view> Views(nB); xanimation::pose Rest; Rest.resize(nB);
            for (int b = 0; b < nB; ++b)
            {
                Parents[b] = Man[b].m_iParent; Names[b] = Man[b].m_Name;
                Rest.m_Position[b] = Man[b].m_RestPosition; Rest.m_Scale[b] = Man[b].m_RestScale;
                Rest.m_Rotation[b] = xmath::fquat(Man[b].m_RestRotX, Man[b].m_RestRotY, Man[b].m_RestRotZ, Man[b].m_RestRotW).NormalizeSafeCopy();
            }
            for (int b = 0; b < nB; ++b) Views[b] = Names[b];
            const xanimation::skeleton_view SV{ Parents };
            const auto Slots = xanimation::details::BuildSlots(SV, Views, Rest);
            auto SlotBone = [&](xanimation::bone_slot S, xanimation::bone_side D) { const auto It = Slots.m_Bones.find({ S, D }); return It == Slots.m_Bones.end() ? -1 : It->second[0]; };
            auto Handle = [&](std::string_view N) -> int
            {
                using xanimation::bone_slot; using xanimation::bone_side;
                if (N == "LeftHand")  return SlotBone(bone_slot::HAND, bone_side::LEFT);
                if (N == "RightHand") return SlotBone(bone_slot::HAND, bone_side::RIGHT);
                if (N == "LeftFoot")  return SlotBone(bone_slot::FOOT, bone_side::LEFT);
                if (N == "RightFoot") return SlotBone(bone_slot::FOOT, bone_side::RIGHT);
                for (int b = 0; b < nB; ++b) if (Names[b] == N) return b;
                return -1;
            };
            const int LH = Handle("LeftHand"), RH = Handle("RightHand"), LF = Handle("LeftFoot"), RF = Handle("RightFoot");
            if (LH < 0 || RH < 0 || LF < 0 || RF < 0) return "SuggestPose: this skeleton's hands and feet were not recognized from the bone names";

            // the request: the key pose, the moved handle and the pins
            const auto P0 = SamplePose(*pFrames, FPS, nF, nB, Seconds);
            xanimation::body_ik Solver;
            std::vector<xanimation::joint_limit> Limits;
            if (auto* pSk = xresource::g_Mgr.getResource(m_SkeletonRef); pSk && int(pSk->getJoints().size()) == nB)
            {
                Limits.resize(nB);
                for (int b = 0; b < nB; ++b) Limits[b] = xskeleton_joints::FromRuntime(pSk->getJoints()[b], Rest.m_Rotation[b]);
            }
            std::vector<xanimation::two_bone_chain> Limbs;
            for (int E : { LH, RH, LF, RF }) if (const int p = Parents[E]; p >= 0 && Parents[p] >= 0) Limbs.push_back({ Parents[p], p, E });
            Solver.Setup(Parents, Limits, {}, Limbs);
            std::vector<std::uint8_t> Owned(nB, 0);
            for (int t = 0; t < nB; ++t) Owned[t] = t < int(Ctx.m_Mapping.m_Auto.m_Map.m_SourceOf.size()) && Ctx.m_Mapping.m_Auto.m_Map.m_SourceOf[t] >= 0;
            xanimation::pose Pose0; Pose0.resize(nB); for (int b = 0; b < nB; ++b) Pose0.set(b, P0[b]);
            Solver.ComputeWorld(Pose0);

            std::vector<xanimation::ik_task> Tasks; std::vector<std::string> TaskName;
            int iMoved = -1; xmath::fvec3 Target(0, 0, 0);
            if (!Move.empty())
            {
                char Bone[128] = {}; float d[3] = {};
                if (std::sscanf(Move.c_str(), "%127s %f %f %f", Bone, &d[0], &d[1], &d[2]) != 4) return "SuggestPose: -Move is \"Bone dx dy dz\" (centimetres)";
                iMoved = Handle(Bone);
                if (iMoved < 0) return std::format("SuggestPose: no bone or handle '{}'", Bone);
                Target = Solver.WorldPosition(iMoved) + xmath::fvec3(d[0], d[1], d[2]) * 0.01f;
                xanimation::ik_task T; T.m_Bone = iMoved; T.m_bPosition = true; T.m_Position = Target;
                Tasks.push_back(T); TaskName.push_back(std::format("{} (moved)", Names[iMoved]));
            }
            if (Pins != "none")
            {
                std::istringstream In(Pins); std::string N;
                while (In >> N)
                {
                    const int b = Handle(N);
                    if (b < 0) return std::format("SuggestPose: no bone or handle '{}' to pin", N);
                    if (b == iMoved) continue;
                    xanimation::ik_task T; T.m_Bone = b; T.m_bPosition = true; T.m_Position = Solver.WorldPosition(b);
                    T.m_bOrientation = true; T.m_Orientation = Solver.WorldRotation(b); T.m_bHard = true;
                    Tasks.push_back(T); TaskName.push_back(std::format("{} (pinned)", Names[b]));
                }
            }
            if (Tasks.empty()) return "SuggestPose: nothing to do (no -Move and no pins)";
            std::vector<xanimation::ik_task_report> Rep(Tasks.size());
            xanimation::pose PoseIK;
            const auto ResIK = Solver.Solve(Pose0, Owned, Tasks, Rep, PoseIK);
            const auto t1 = clock::now();

            // the Kimodo window: 2 s at 30 fps centred on the key (clamped at the clip's ends: those frames repeat the first / last pose)
            constexpr int W = 60, K = 30, A0 = K - 22, A1 = K + 22;
            std::vector<xmath::transform3> Win(std::size_t(W) * nB);
            int nClamped = 0;
            const double Len = double(std::max(1, nF - 1)) / FPS;
            for (int i = 0; i < W; ++i)
            {
                const double t = Seconds + double(i - K) / 30.0;
                nClamped += t < 0 || t > Len;
                const auto P = i == K ? std::vector<xmath::transform3>{} : SamplePose(*pFrames, FPS, nF, nB, t);
                for (int b = 0; b < nB; ++b) Win[std::size_t(i) * nB + b] = i == K ? PoseIK.get(b) : P[b];
            }
            kimodo_space KS; std::vector<kimodo::soma_pose> SP;
            if (!ToKimodo(Ctx, Win, W, nB, KS, SP, Err)) return "SuggestPose: " + Err;
            const int iRoot = KS.m_iRoot;
            std::vector<kimodo::constraint_set> Sets;
            for (const char* Type : { "left-hand", "right-hand", "left-foot", "right-foot" })
                Sets.push_back({ Type, { K }, { SP[K] }, { { SP[K].m_Root[0], SP[K].m_Root[2] } } });
            Sets.push_back({ "fullbody", { A0, A1 }, { SP[A0], SP[A1] }, { { SP[A0].m_Root[0], SP[A0].m_Root[2] }, { SP[A1].m_Root[0], SP[A1].m_Root[2] } } });
            kimodo::constraint_set Path{ "root2d", {}, {}, {} };
            for (int i = 0; i < W; ++i) { Path.m_Frames.push_back(i); Path.m_Smooth.push_back({ SP[i].m_Root[0], SP[i].m_Root[2] }); }
            Sets.push_back(std::move(Path));
            static int s_Count = 0;
            const auto Stem = std::format(L"{}\\suggest_{}_{}", m_Document.m_TempDir, GetCurrentProcessId(), ++s_Count);
            std::filesystem::create_directories(m_Document.m_TempDir);
            if (!kimodo::WriteConstraints(Stem + L".json", Sets, Err)) return "SuggestPose: " + Err;
            const auto t2 = clock::now();

            std::string Text = std::format("SuggestPose at {:.3f} s ({} fps clip, {} bones; {} of them keyable)\n", Seconds, FPS, nB, std::count(Owned.begin(), Owned.end(), 1));
            Text += std::format("request (Kimodo, SOMA 77, 2 s at 30 fps, key = frame {}{}):\n", K, nClamped ? std::format("; {} frames past the clip's ends repeat its first/last pose", nClamped) : "");
            Text += std::format("  explicit: left-hand, right-hand, left-foot, right-foot at frame {} from the IK-only pose (moved: {}; pinned: {})\n", K, iMoved >= 0 ? Names[iMoved] : "none", Pins);
            Text += std::format("  implicit: root path (x, z) at all {} frames from the clip; root height and heading at the key from the hand/foot sets; full-body anchors at frames {} and {} (+-0.73 s) from the clip\n", W, A0, A1);
            Text += std::format("  prompt: {}; 10 steps; raw output (no foot clean-up)\n", Prompt.empty() ? "(none)" : "\"" + Prompt + "\"");
            Text += std::format("IK-only pose (the baseline): {} iterations, all reached {}\n", ResIK.m_Iterations, ResIK.m_bAllReached);

            // the candidates
            m_Suggest = {};
            m_Suggest.m_Owned = Owned; m_Suggest.m_Original = P0; m_Suggest.m_Time = Seconds; m_Suggest.m_Sig = SuggestSignature();
            m_Suggest.m_MovedBone = iMoved >= 0 ? Names[iMoved] : ""; m_Suggest.m_Target = Target;
            for (int t = 0; t < nB && m_Suggest.m_iHips < 0; ++t) if (t < int(Ctx.m_Mapping.m_Auto.m_Map.m_SourceOf.size()) && Ctx.m_Mapping.m_Auto.m_Map.m_SourceOf[t] == iRoot) m_Suggest.m_iHips = t;
            xanimation::body_ik_settings CorrSet; CorrSet.m_TranslateBone = m_Suggest.m_iHips;     // the correction may shift the hips too (they are keyed)
            std::istringstream SeedIn(Seeds); int Seed = 0;
            while (SeedIn >> Seed)
            {
                const auto s0 = clock::now();
                const auto Bvh = std::format(L"{}_s{}.bvh", Stem, Seed);
                std::wstring P = xstrtool::To(Prompt.empty() ? std::string("A person.") : Prompt);
                for (auto& c : P) if (c == L'"' || c == L'\n' || c == L'\r') c = L' ';
                const auto Args = std::format(L"gen --prompt \"{}\" --frames {} --steps 10 --seed {} --no-postprocess --constraints \"{}.json\" --bvh \"{}\"", P, W, Seed, Stem, Bvh);
                if (!kimodo::RunBlocking(Exe, Args, Stem + L".log", 180000, Err)) return Text + "SuggestPose: " + Err;
                const auto s1 = clock::now();
                kimodo::bvh B;
                if (!B.Load(Bvh, Err)) return Text + "SuggestPose: the generated BVH could not be read: " + Err;
                if (B.m_nFrames <= K) return Text + "SuggestPose: the generated window is too short";

                // back: Kimodo's frame undone, into the SOMA skeleton's space, retargeted by the compile's own path, frame K
                const auto Back = FromKimodo(Ctx, B, KS, nB);

                // Kimodo's rotations and its hip shift (positioned keys carry it), then corrected on this skeleton
                xanimation::pose Kim = Pose0;
                for (int b = 0; b < nB; ++b) if (Owned[b]) Kim.m_Rotation[b] = Back[std::size_t(K) * nB + b].m_Rotation;
                if (m_Suggest.m_iHips >= 0) Kim.m_Position[m_Suggest.m_iHips] = Back[std::size_t(K) * nB + m_Suggest.m_iHips].m_Position;
                Solver.ComputeWorld(Kim);
                std::vector<float> KimMiss(Tasks.size());
                for (std::size_t t = 0; t < Tasks.size(); ++t) KimMiss[t] = (Solver.WorldPosition(Tasks[t].m_Bone) - Tasks[t].m_Position).Length();
                xanimation::pose Fixed;
                const auto ResC = Solver.Solve(Kim, Owned, Tasks, Rep, Fixed, CorrSet);
                float CMax = 0, CSum = 0; int CN = 0; std::string CBone;
                for (int b = 0; b < nB; ++b) if (Owned[b]) { const float a = AngleDeg(Kim.m_Rotation[b], Fixed.m_Rotation[b]); CSum += a; ++CN; if (a > CMax) { CMax = a; CBone = Names[b]; } }

                // continuity: the key frame replaced in the clip; the second difference (acceleration) around it against the clip's own
                auto Positions = [&](const xanimation::pose& P) { Solver.ComputeWorld(P); std::vector<xmath::fvec3> V(nB); for (int b = 0; b < nB; ++b) V[b] = Solver.WorldPosition(b); return V; };
                auto PoseAt = [&](double t) { xanimation::pose P; P.resize(nB); const auto S = SamplePose(*pFrames, FPS, nF, nB, t); for (int b = 0; b < nB; ++b) P.set(b, S[b]); return P; };
                const double dt = 1.0 / FPS;
                const auto Pm = Positions(PoseAt(Seconds - dt)), Pp = Positions(PoseAt(Seconds + dt)), Po = Positions(Pose0), Pc = Positions(Fixed);
                float AccO = 0, AccC = 0, Jump = 0; std::string AccBone;
                for (int b = 0; b < nB; ++b)
                {
                    if (!Owned[b]) continue;
                    const float ao = (Pp[b] - Po[b] * 2.0f + Pm[b]).Length() / float(dt * dt), ac = (Pp[b] - Pc[b] * 2.0f + Pm[b]).Length() / float(dt * dt);
                    AccO = std::max(AccO, ao);
                    if (ac > AccC) { AccC = ac; AccBone = Names[b]; }
                    Jump = std::max(Jump, (Pc[b] - Po[b]).Length());
                }
                const auto s2 = clock::now();

                suggest_candidate C; C.m_Seed = Seed;
                C.m_Pose.resize(nB); for (int b = 0; b < nB; ++b) C.m_Pose[b] = Fixed.get(b);
                std::string R = std::format("candidate {} (seed {}): Kimodo {:.0f} ms, back + correction {:.0f} ms\n", m_Suggest.m_Candidates.size(), Seed, ms(s0, s1), ms(s1, s2));
                for (std::size_t t = 0; t < Tasks.size(); ++t)
                    R += std::format("  {}: Kimodo's pose misses by {:.2f} cm; after correction {:.3f} cm{}\n", TaskName[t], KimMiss[t] * 100.0f, Rep[t].m_PosMiss * 100.0f
                        , Rep[t].m_Status == xanimation::ik_status::REACHED ? "" : std::format(" (MISSED, reasons {})", int(Rep[t].m_Reasons)));
                R += std::format("  correction: max {:.1f} deg ({}), mean {:.2f} deg over {} bones; all reached {}\n", CMax, CBone, CN ? CSum / CN : 0.0f, CN, ResC.m_bAllReached);
                R += std::format("  continuity: the key's acceleration (key replaced, neighbours from the clip) {:.1f} m/s^2 at {} (the clip's own: {:.1f}); largest move from the clip's pose {:.1f} cm\n", AccC, AccBone, AccO, Jump * 100.0f);
                C.m_Report = R;
                Text += R;
                m_Suggest.m_Candidates.push_back(std::move(C));
            }
            if (m_Suggest.m_Candidates.empty()) return Text + "SuggestPose: no seed given";
            Text += std::format("stages: IK-only {:.0f} ms, request {:.0f} ms, total {:.0f} ms. A candidate is nothing yet: SuggestPoseAccept -Layer n writes it.", ms(t0, t1), ms(t1, t2), ms(t0, clock::now()));
            return Text;
        }

        std::string m_AccAt;      // where the last MaxAcceleration found its largest value
        // the bones' model positions (metres: the skeleton's scales applied) of one frame
        std::vector<xmath::fvec3> ModelPositions(const xmath::transform3* pFrame) noexcept
        {
            const auto& Man = Desc()->m_ResolvedSkeletonBones.m_Bones; const int nB = int(Man.size());
            std::vector<std::int32_t> Par(nB); for (int b = 0; b < nB; ++b) Par[b] = Man[b].m_iParent;
            xanimation::body_ik FK; FK.Setup(Par);
            xanimation::pose P; P.resize(nB); for (int b = 0; b < nB; ++b) P.set(b, pFrame[b]);
            FK.ComputeWorld(P);
            std::vector<xmath::fvec3> V(nB); for (int b = 0; b < nB; ++b) V[b] = FK.WorldPosition(b);
            return V;
        }
        // max acceleration (m/s^2, model positions, owned bones) over the frames [f0, f1]: of the clip A, or (B given) of the motion A adds to B
        float MaxAcceleration(const std::vector<xmath::transform3>& A, int FPS, int nF, int f0, int f1, const std::vector<xmath::transform3>* pB = nullptr, int OnlyBone = -1) noexcept
        {
            const int nB = int(Desc()->m_ResolvedSkeletonBones.m_Bones.size());
            auto At = [&](int f)
            {
                auto V = ModelPositions(&A[std::size_t(f) * nB]);
                if (pB) { const auto W = ModelPositions(&(*pB)[std::size_t(f) * nB]); for (int b = 0; b < nB; ++b) V[b] = V[b] - W[b]; }
                return V;
            };
            f0 = std::max(1, f0); f1 = std::min(nF - 2, f1);
            float Max = 0; const float dt = 1.0f / float(FPS);
            if (f1 < f0) return 0;
            auto Pm = At(f0 - 1), Po = At(f0);
            for (int f = f0; f <= f1; ++f)
            {
                auto Pp = At(f + 1);
                for (int b = 0; b < nB; ++b) if (OnlyBone >= 0 ? b == OnlyBone : (m_Suggest.m_Owned.empty() || m_Suggest.m_Owned[b])) { const float a = (Pp[b] - Po[b] * 2.0f + Pm[b]).Length() / (dt * dt); if (a > Max) { Max = a; m_AccAt = std::format("frame {} {}", f, Desc()->m_ResolvedSkeletonBones.m_Bones[b].m_Name); } }
                Pm = std::move(Po); Po = std::move(Pp);
            }
            return Max;
        }

        // Refine Motion (automatic after an accept): Kimodo regenerates +-0.5 s around the key so the motion there looks natural, not just
        // interpolated. Constraints: the accepted key pose and the clip's poses at and beyond the window's edges (full body), the feet over
        // their contact frames, the root path; 25 steps, raw. Brought back, the feet pinned per frame where in contact (body_ik), written as
        // absolute keys every 3 frames on the same layer (rotations of the keyable bones + the hips' position) with the window's edges keyed
        // at the pose below (neutral), so the layer joins the clip exactly there. Writes into the layer directly: the caller makes it part of
        // its one undo step. Report: skate in contact and acceleration in the window, before and after; residuals; time.
        // Scope: [Key - Lo, Key + Hi] seconds (the adaptive caller grows it). Every other key of this layer inside the scope is an anchor:
        // a full-body constraint, kept exactly (never rewritten), and the blend meets it. The Kimodo window is stored as a take
        // (Cache\KimodoTakes\<package>\) and listed on the layer (provenance).
        bool SuggestRefine(int iLayer, int Seed, double Lo, double Hi, const std::string& Why, const std::vector<xmath::transform3>& Own, std::string& Report, std::string& Err) noexcept
        {
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();
            auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            auto* pOv = SelectedOv(); auto* pS = SelectedSource(); auto* pDesc = Desc();
            if (!pOv || !pS || !pDesc) { Err = "select a clip first"; return false; }
            const auto Exe = kimodo::FindExe();
            if (Exe.empty()) { Err = "xkimodo.exe not found"; return false; }
            auto* pCtx = KimodoRetargetCtx(Err);
            if (!pCtx) return false;
            const std::vector<xmath::transform3>* pFrames = nullptr; int FPS = 30, nF = 0;
            if (!SuggestClipFrames(pFrames, FPS, nF, Err)) return false;
            const auto Accepted = *pFrames;                                   // the clip with the accepted key (before the refine)
            const auto& Man = pDesc->m_ResolvedSkeletonBones.m_Bones;
            const int nB = int(Man.size());
            std::vector<std::int32_t> Parents(nB); std::vector<std::string> Names(nB); std::vector<std::string_view> Views(nB); xanimation::pose Rest; Rest.resize(nB);
            for (int b = 0; b < nB; ++b)
            {
                Parents[b] = Man[b].m_iParent; Names[b] = Man[b].m_Name;
                Rest.m_Position[b] = Man[b].m_RestPosition; Rest.m_Scale[b] = Man[b].m_RestScale;
                Rest.m_Rotation[b] = xmath::fquat(Man[b].m_RestRotX, Man[b].m_RestRotY, Man[b].m_RestRotZ, Man[b].m_RestRotW).NormalizeSafeCopy();
            }
            for (int b = 0; b < nB; ++b) Views[b] = Names[b];
            const auto Slots = xanimation::details::BuildSlots(xanimation::skeleton_view{ Parents }, Views, Rest);
            auto SlotBone = [&](xanimation::bone_slot S, xanimation::bone_side D) { const auto It = Slots.m_Bones.find({ S, D }); return It == Slots.m_Bones.end() ? -1 : It->second[0]; };
            const int Feet[2] = { SlotBone(xanimation::bone_slot::FOOT, xanimation::bone_side::LEFT), SlotBone(xanimation::bone_slot::FOOT, xanimation::bone_side::RIGHT) };
            if (Feet[0] < 0 || Feet[1] < 0) { Err = "the feet were not recognized from the bone names"; return false; }
            const int iHips = m_Suggest.m_iHips;

            // the window at 30 fps: the scope [A, B] around the key K plus a 0.5 s margin each side; every frame keyed (the feet held)
            constexpr int Mg = 15, Step = 1;
            int Rl = std::max(2, int(std::lround(Lo * 30.0))), Rh = std::max(2, int(std::lround(Hi * 30.0)));
            // a window longer than Kimodo's limit is generated as overlapping windows, stitched (below)
            const int W = Rl + Rh + 2 * Mg + 1, K = Mg + Rl, A = K - Rl, B = K + Rh;
            const double Key = m_Suggest.m_Time;
            // the layer's other keys inside the scope: anchors (kept, constrained, met by the blend)
            std::set<std::int64_t> UserTicks; std::vector<int> Anchors = { A, K, B };
            for (const auto& k : pOv->m_Layers[iLayer].m_Keys)
            {
                const int f = K + int(std::lround((double(k.m_Tick) / double(xanim_package_layers::ToTick(1.0)) - Key) * 30.0));
                if (std::abs(double(k.m_Tick) / double(xanim_package_layers::ToTick(1.0)) - Key) < 1e-6) continue;   // the accepted key
                if (f <= A || f >= B) continue;
                UserTicks.insert(k.m_Tick); Anchors.push_back(f);
            }
            std::sort(Anchors.begin(), Anchors.end()); Anchors.erase(std::unique(Anchors.begin(), Anchors.end()), Anchors.end());
            std::vector<xmath::transform3> Win(std::size_t(W) * nB);
            for (int i = 0; i < W; ++i)
            {
                const auto P = SamplePose(Accepted, FPS, nF, nB, Key + double(i - K) / 30.0);
                std::copy(P.begin(), P.end(), Win.begin() + std::size_t(i) * nB);
            }
            // the feet's model positions over the window, in hip heights (the contacts' units)
            xanimation::body_ik FK; FK.Setup(Parents);
            auto World = [&](const xmath::transform3* pF, std::vector<xmath::fvec3>& P, std::vector<xmath::fquat>* pR = nullptr)
            {
                xanimation::pose Q; Q.resize(nB); for (int b = 0; b < nB; ++b) Q.set(b, pF[b]);
                FK.ComputeWorld(Q); P.resize(nB); if (pR) pR->resize(nB);
                for (int b = 0; b < nB; ++b) { P[b] = FK.WorldPosition(b); if (pR) (*pR)[b] = FK.WorldRotation(b); }
            };
            std::vector<xmath::fvec3> P; World(&Win[std::size_t(K) * nB], P);
            const float HipH = std::max(1e-3f, iHips >= 0 ? P[iHips].m_Y - std::min(P[Feet[0]].m_Y, P[Feet[1]].m_Y) : 1.0f);
            std::vector<std::vector<std::uint8_t>> InContact(2, std::vector<std::uint8_t>(W, 0));
            std::vector<xmath::fvec3> FootPos[2]; std::vector<xmath::fquat> FootRot[2];
            for (int s = 0; s < 2; ++s) { FootPos[s].resize(W); FootRot[s].resize(W); }
            for (int i = 0; i < W; ++i)
            {   // the feet where the clip itself had them (before the accept: its falloff slides them), the pins and the contacts
                const auto Po = Own.size() == Accepted.size() ? SamplePose(Own, FPS, nF, nB, Key + double(i - K) / 30.0) : std::vector<xmath::transform3>(Win.begin() + std::size_t(i) * nB, Win.begin() + std::size_t(i + 1) * nB);
                std::vector<xmath::fquat> Rw; World(Po.data(), P, &Rw); for (int s = 0; s < 2; ++s) { FootPos[s][i] = P[Feet[s]]; FootRot[s][i] = Rw[Feet[s]]; }
            }
            auto Skate = [&](const std::vector<xmath::fvec3>& Pos, std::vector<std::uint8_t>* pMark) -> float     // mean skate (hip heights/s) of the contacts inside the refined frames
            {
                std::vector<xmath::fvec3> N(Pos.size()); for (std::size_t i = 0; i < Pos.size(); ++i) N[i] = Pos[i] * (1.0f / HipH);
                float Sum = 0; int n = 0;
                for (const auto& C : xanimation::FindContacts(N, 30.0f))
                {
                    if (pMark) for (int f = C.m_First; f <= C.m_Last; ++f) (*pMark)[f] = 1;
                    if (C.m_Last < A || C.m_First > B) continue;
                    Sum += C.m_Skate; ++n;
                }
                return n ? Sum / n : 0.0f;
            };
            const float SkateBefore = 0.5f * (Skate(FootPos[0], &InContact[0]) + Skate(FootPos[1], &InContact[1]));

            // the clip's own poses every 0.5 s away from the key: constraints AND blend anchors (Kimodo meets a constraint with a local jerk;
            // anchored, the motion meets the clip there smoothly), so a wide scope keeps the clip's intent instead of drifting into another motion
            const auto KeyAnchors = Anchors;                                               // the edges, the key, the user's other keys
            for (int f = A + 15; f < B - 7; f += 15) if (std::abs(f - K) >= 15) Anchors.push_back(f);
            std::sort(Anchors.begin(), Anchors.end()); Anchors.erase(std::unique(Anchors.begin(), Anchors.end()), Anchors.end());

            // The windows: one when the scope fits Kimodo's limit, else overlapping windows (a quarter of a window of overlap, at most 2 s),
            // each generated on its own (the constraints of the frames it covers, its own Kimodo space) and stored as a take; the overlaps
            // crossfade with a zero-slope weight. Kimodo's 10 s is per take, never per result.
            const int MaxW = std::max(30, m_RefineMaxFrames);
            std::vector<std::pair<int, int>> Chunks;
            if (W <= MaxW) Chunks.push_back({ 0, W });
            else
            {
                const int Ov = std::min(60, MaxW / 4), Stride = MaxW - Ov;
                for (int c0 = 0;; c0 += Stride) { const int s = std::min(c0, W - MaxW); Chunks.push_back({ s, s + MaxW }); if (s + MaxW >= W) break; }
            }
            static int s_Count = 0;
            std::filesystem::create_directories(m_Document.m_TempDir);
            const auto t1 = clock::now();
            std::vector<std::vector<xmath::transform3>> Gen(Chunks.size());
            int nFootFrames = 0;
            for (std::size_t ci = 0; ci < Chunks.size(); ++ci)
            {
                const int c0 = Chunks[ci].first, c1 = Chunks[ci].second, n = c1 - c0;
                const std::vector<xmath::transform3> Wc(Win.begin() + std::size_t(c0) * nB, Win.begin() + std::size_t(c1) * nB);
                kimodo_space KS; std::vector<kimodo::soma_pose> SP;
                if (!ToKimodo(*pCtx, Wc, n, nB, KS, SP, Err)) return false;
                auto Root2 = [&](int i) { return std::array<float, 2>{ SP[i].m_Root[0], SP[i].m_Root[2] }; };
                std::vector<kimodo::constraint_set> Sets;
                // full body inside this window, at most 20 (Kimodo's guidance): the edges just outside the scope, the key and the user's keys
                // first, then the clip every 0.5 s
                std::vector<int> Full;
                for (int f : { A - 3, B + 3 }) if (f >= c0 && f < c1) Full.push_back(f);
                for (int f : KeyAnchors) if (f >= c0 && f < c1) Full.push_back(f);
                for (int f : Anchors) if (f >= c0 && f < c1 && Full.size() < 20) Full.push_back(f);
                std::sort(Full.begin(), Full.end()); Full.erase(std::unique(Full.begin(), Full.end()), Full.end());
                kimodo::constraint_set FB{ "fullbody", {}, {}, {} };
                for (int f : Full) { FB.m_Frames.push_back(f - c0); FB.m_Poses.push_back(SP[f - c0]); FB.m_Smooth.push_back(Root2(f - c0)); }
                if (!FB.m_Frames.empty()) Sets.push_back(std::move(FB));
                const int a = std::max(A, c0), b = std::min(B, c1 - 1);
                const int FootStride = std::max(2, (b - a) / 16 + 1);                      // at most ~16 contact frames per foot (Kimodo's guidance)
                for (int s = 0; s < 2; ++s)
                {
                    kimodo::constraint_set Set{ s ? "right-foot" : "left-foot", {}, {}, {} };
                    for (int f = a; f <= b && Set.m_Frames.size() < 16; f += FootStride) if (InContact[s][f] && f != K) { Set.m_Frames.push_back(f - c0); Set.m_Poses.push_back(SP[f - c0]); Set.m_Smooth.push_back(Root2(f - c0)); }
                    nFootFrames += int(Set.m_Frames.size());
                    if (!Set.m_Frames.empty()) Sets.push_back(std::move(Set));
                }
                kimodo::constraint_set Path{ "root2d", {}, {}, {} };
                for (int i = 0; i < n; ++i) { Path.m_Frames.push_back(i); Path.m_Smooth.push_back(Root2(i)); }
                Sets.push_back(std::move(Path));
                const auto Stem = std::format(L"{}\\refine_{}_{}", m_Document.m_TempDir, GetCurrentProcessId(), ++s_Count);
                if (!kimodo::WriteConstraints(Stem + L".json", Sets, Err)) return false;
                const auto Bvh = Stem + L".bvh";
                const int TakeSeed = std::max(1, Seed) + int(ci);
                const auto Args = std::format(L"gen --prompt \"A person.\" --frames {} --steps 25 --seed {} --no-postprocess --constraints \"{}.json\" --bvh \"{}\"", n, TakeSeed, Stem, Bvh);
                if (!kimodo::RunBlocking(Exe, Args, Stem + L".log", 180000, Err)) return false;
                kimodo::bvh Bv;
                if (!Bv.Load(Bvh, Err)) return false;
                if (Bv.m_nFrames < n) { Err = "the generated window is too short"; return false; }
                Gen[ci] = FromKimodo(*pCtx, Bv, KS, nB);

                // the window is a take: stored (never regenerated) and listed on the layer (provenance; the Kimodo layer kind shows it later)
                const auto Dir = ProjectPath() / L"Cache" / L"KimodoTakes" / std::format(L"{:016X}", m_Document.m_Guid.m_Instance.m_Value);
                std::error_code Ec; std::filesystem::create_directories(Dir, Ec);
                const auto Name = std::format("refine_{:%Y%m%d_%H%M%S}_{}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()), s_Count);
                std::filesystem::copy_file(Bvh, Dir / (xstrtool::To(Name) + L".bvh"), std::filesystem::copy_options::overwrite_existing, Ec);
                std::filesystem::copy_file(Stem + L".json", Dir / (xstrtool::To(Name) + L".json"), std::filesystem::copy_options::overwrite_existing, Ec);
                const double T0 = Key + double(c0 - K) / 30.0, Ta = Key + double(a - K) / 30.0, Tb = Key + double(b - K) / 30.0;
                xanim_package_desc::layer_take Tk;
                Tk.m_File = Name; Tk.m_Prompt = "A person."; Tk.m_Seed = TakeSeed; Tk.m_Steps = 25;
                Tk.m_Start = xanim_package_layers::ToTick(std::max(0.0, Ta)); Tk.m_End = xanim_package_layers::ToTick(Tb);
                Tk.m_Offset = xanim_package_layers::ToTick(T0);
                Tk.m_Why = Chunks.size() > 1 ? std::format("{} (window {} of {}, stitched)", Why, ci + 1, Chunks.size()) : Why;
                if (std::FILE* fp = _wfopen((Dir / (xstrtool::To(Name) + L".take.txt")).c_str(), L"wb"))
                {
                    const auto T = std::format("prompt: {}\nseed: {}\nsteps: {}\nframes: {} at 30 fps (frame 0 = clip {:.3f} s)\nkeys made: clip {:.3f} .. {:.3f} s\nclip: {}\nwhy: {}\n"
                        , Tk.m_Prompt, Tk.m_Seed, Tk.m_Steps, n, T0, Ta, Tb, pOv->m_OriginalName, Tk.m_Why);
                    std::fwrite(T.data(), 1, T.size(), fp); std::fclose(fp);
                }
                if (Ec) { Err = std::format("the take could not be stored in {}: {}", xstrtool::To(Dir.wstring()), Ec.message()); return false; }
                pOv->m_Layers[iLayer].m_Takes.push_back(Tk);
            }
            const auto t2 = clock::now();
            // stitched: a frame covered by two windows crossfades from the earlier to the later with a zero-slope (smootherstep) weight
            std::vector<xmath::transform3> Back(std::size_t(W) * nB);
            for (int f = 0; f < W; ++f)
            {
                int j = 0; while (j + 1 < int(Chunks.size()) && Chunks[j + 1].first <= f) ++j;
                const auto* pB = &Gen[j][std::size_t(f - Chunks[j].first) * nB];
                auto* pOut = &Back[std::size_t(f) * nB];
                if (j > 0 && f < Chunks[j - 1].second)
                {
                    const int s = Chunks[j].first, e = Chunks[j - 1].second;
                    float u = float(f - s + 1) / float(e - s + 1); u = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
                    const auto* pA = &Gen[j - 1][std::size_t(f - Chunks[j - 1].first) * nB];
                    for (int b = 0; b < nB; ++b)
                    {
                        pOut[b] = pB[b];
                        pOut[b].m_Rotation = xmath::fquat::Slerp(pA[b].m_Rotation, pB[b].m_Rotation, u).NormalizeCopy();
                        pOut[b].m_Position = pA[b].m_Position + (pB[b].m_Position - pA[b].m_Position) * u;
                    }
                }
                else std::copy(pB, pB + nB, pOut);
            }

            // per refined frame: Kimodo's rotations (keyable bones) and hips, the feet pinned where in contact
            xanimation::body_ik Solver;
            std::vector<xanimation::two_bone_chain> Limbs;
            for (int E : Feet) if (const int p = Parents[E]; p >= 0 && Parents[p] >= 0) Limbs.push_back({ Parents[p], p, E });
            Solver.Setup(Parents, {}, {}, Limbs);
            xanimation::body_ik_settings Set;     // the hips stay: a per-frame hips solve jitters (each pinned frame alone); the legs reach
            const auto& Owned = m_Suggest.m_Owned;
            auto& Keys = pOv->m_Layers[iLayer].m_Keys;
            auto PutKey = [&](const std::string& Bone, std::int64_t Tick, const xmath::fquat& Q, const xmath::fvec3* pPos)
            {
                xanim_package_desc::layer_pose_key* pK = nullptr;
                for (auto& k : Keys) if (k.m_Bone == Bone && k.m_Tick == Tick) { pK = &k; break; }
                if (!pK) { Keys.emplace_back(); pK = &Keys.back(); pK->m_Tick = Tick; pK->m_Bone = Bone; }
                const auto N = Q.NormalizeCopy();
                pK->m_X = N.m_X; pK->m_Y = N.m_Y; pK->m_Z = N.m_Z; pK->m_W = N.m_W; pK->m_bOffset = false; pK->m_bStep = false;
                if (pPos) { pK->m_bPosition = true; pK->m_PX = pPos->m_X; pK->m_PY = pPos->m_Y; pK->m_PZ = pPos->m_Z; }
            };
            // The refined motion before the pins, frames [A, B] (the rest of Ref is the clip):
            // 1. offset blending: Kimodo's constraints are soft (cm off the anchors); its error at the anchors (the edges, the key, the user's
            //    other keys) is spread between them linearly, then the difference is faded in from each anchor with a zero-slope envelope, so
            //    the motion keeps Kimodo's shape and meets every anchor exactly;
            // 2. the difference smoothed in time ([1 2 1]/4, 3 passes, the anchors held): raw diffusion frames jitter, and a second
            //    difference (acceleration) amplifies that
            std::vector<xmath::transform3> Ref = Win;
            {
                auto Smooth5 = [](float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f); };
                std::vector<std::uint8_t> IsAnchor(W, 0); for (int f : Anchors) IsAnchor[f] = 1;
                for (int b = 0; b < nB; ++b)
                {
                    if (!Owned[b] && b != iHips) continue;
                    std::vector<xmath::fvec3> E(W), EP(W), D(W), DP(W);
                    for (int f = A; f <= B; ++f)
                    {
                        const auto& Bb = Back[std::size_t(f) * nB + b]; const auto& Wb = Win[std::size_t(f) * nB + b];
                        E[f] = xanimation::details::LogQ(Bb.m_Rotation * Wb.m_Rotation.ConjugateCopy()); EP[f] = Bb.m_Position - Wb.m_Position;
                    }
                    for (std::size_t s = 0; s + 1 < Anchors.size(); ++s)
                    {
                        const int a = Anchors[s], c = Anchors[s + 1];
                        for (int f = a; f <= c; ++f)
                        {
                            const float u = float(f - a) / float(std::max(1, c - a));
                            const float Env = Smooth5(u / 0.4f) * Smooth5((1.0f - u) / 0.4f);
                            D[f]  = (E[f]  - E[a]  * (1.0f - u) - E[c]  * u) * (Owned[b] ? Env : 0.0f);
                            DP[f] = (EP[f] - EP[a] * (1.0f - u) - EP[c] * u) * (b == iHips ? Env : 0.0f);
                        }
                    }
                    for (int pass = 0; pass < 3; ++pass)
                    {
                        auto D2 = D, DP2 = DP;
                        for (int f = A + 1; f < B; ++f)
                        {
                            if (IsAnchor[f]) continue;
                            D2[f] = (D[f - 1] + D[f] * 2.0f + D[f + 1]) * 0.25f; DP2[f] = (DP[f - 1] + DP[f] * 2.0f + DP[f + 1]) * 0.25f;
                        }
                        D = D2; DP = DP2;
                    }
                    for (int f = A + 1; f < B; ++f)
                    {
                        if (IsAnchor[f]) continue;
                        auto& O = Ref[std::size_t(f) * nB + b]; const auto& Wb = Win[std::size_t(f) * nB + b];
                        if (Owned[b]) O.m_Rotation = (xanimation::details::ExpV(D[f]) * Wb.m_Rotation).NormalizeCopy();
                        if (b == iHips) O.m_Position = Wb.m_Position + DP[f];
                    }
                }
            }
            float PinMax = 0; int nKeys = 0, nFrames = 0;
            for (int f = A; f <= B; f += Step)
            {
                const double t = Key + double(f - K) / 30.0;
                if (t < 0.0) continue;                                           // before the clip's start
                const auto Tick = xanim_package_layers::ToTick(t);
                if (f == K || UserTicks.count(Tick)) continue;                  // the accepted key and the user's keys stay as they are
                if (f == A || f == B)
                {   // the edges: the motion as it plays there now (the layer joins it exactly)
                    for (int b = 0; b < nB; ++b) if (Owned[b] || b == iHips) { const auto& Wb = Win[std::size_t(f) * nB + b]; PutKey(Names[b], Tick, Wb.m_Rotation, b == iHips ? &Wb.m_Position : nullptr); ++nKeys; }
                    continue;
                }
                xanimation::pose Kim; Kim.resize(nB); for (int b = 0; b < nB; ++b) Kim.set(b, Ref[std::size_t(f) * nB + b]);
                std::vector<xanimation::ik_task> Tasks;
                for (int s = 0; s < 2; ++s)
                    if (InContact[s][f]) { xanimation::ik_task T; T.m_Bone = Feet[s]; T.m_bPosition = true; T.m_Position = FootPos[s][f]; T.m_bOrientation = true; T.m_Orientation = FootRot[s][f]; T.m_bHard = true; Tasks.push_back(T); }
                xanimation::pose Fixed = Kim;
                if (!Tasks.empty())
                {
                    std::vector<xanimation::ik_task_report> Rep(Tasks.size());
                    Solver.Solve(Kim, Owned, Tasks, Rep, Fixed, Set);
                    for (const auto& r : Rep) PinMax = std::max(PinMax, r.m_PosMiss);
                }
                for (int b = 0; b < nB; ++b) if (Owned[b] || b == iHips) { PutKey(Names[b], Tick, Fixed.m_Rotation[b], b == iHips ? &Fixed.m_Position[b] : nullptr); ++nKeys; }
                ++nFrames;
            }
            const auto t3 = clock::now();

            // after: the clip as the stack plays it now
            const std::vector<xmath::transform3>* pAfter = nullptr; int FPSa = 30, nFa = 0;
            if (!SuggestClipFrames(pAfter, FPSa, nFa, Err)) return false;
            std::vector<xmath::fvec3> AfterFeet[2];
            for (int s = 0; s < 2; ++s) AfterFeet[s].resize(W);
            float KeyMiss = 0;
            for (int i = 0; i < W; ++i)
            {
                const auto Pp = SamplePose(*pAfter, FPSa, nFa, nB, Key + double(i - K) / 30.0);
                World(Pp.data(), P);
                for (int s = 0; s < 2; ++s) AfterFeet[s][i] = P[Feet[s]];
                if (i == K) for (int b = 0; b < nB; ++b) if (Owned[b]) KeyMiss = std::max(KeyMiss, AngleDeg(Pp[b].m_Rotation, Win[std::size_t(K) * nB + b].m_Rotation));
            }
            const float SkateAfter = 0.5f * (Skate(AfterFeet[0], nullptr) + Skate(AfterFeet[1], nullptr));
            Report = std::format("refine scope -{:.2f} .. +{:.2f} s ({} frames as {} Kimodo window(s) of up to {} frames{}, 25 steps, {} anchors): {} frames keyed ({} keys), feet pinned on {} contact frames (pins within {:.3f} cm); "
                                 "skate in contact {:.4f} -> {:.4f} hip heights/s; the key kept within {:.4f} deg; time: request {:.0f} ms, Kimodo {:.0f} ms, back + keys {:.0f} ms"
                , double(K - A) / 30.0, double(B - K) / 30.0, W, Chunks.size(), MaxW, Chunks.size() > 1 ? ", stitched" : "", Anchors.size(), nFrames, nKeys, nFootFrames, PinMax * 100.0f, SkateBefore, SkateAfter, KeyMiss, ms(t0, t1), ms(t1, t2), ms(t2, t3));
            return true;
        }

        // A candidate as keys on a Pose layer: every keyable bone it turns by more than 0.05 degrees from the pose below that layer, and the
        // hips' translation (Kimodo's shift). One undo step. The layer's falloff and smooth curve blend the change in and out.
        std::string SuggestPoseAccept(int iLayer, int iCand, bool bRefine = true) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || !pDesc) return "SuggestPoseAccept: select a clip first";
            if (iCand < 0 || iCand >= int(m_Suggest.m_Candidates.size())) return "SuggestPoseAccept: no such candidate (SuggestPose first)";
            if (m_Suggest.m_Sig != SuggestSignature()) return "SuggestPoseAccept: the candidate is stale (the clip or its layers changed since): SuggestPose again";
            if (iLayer < 0 || iLayer >= int(pOv->m_Layers.size()) || pOv->m_Layers[iLayer].m_Kind != xanim_package_desc::layer_kind::POSE) return "SuggestPoseAccept: -Layer is not a Pose layer of the selected clip";
            if (iLayer != int(pOv->m_Layers.size()) - 1) return "SuggestPoseAccept: the Pose layer must be the clip's top layer (a layer above it would change the result)";
            const auto& C = m_Suggest.m_Candidates[iCand];
            const auto& Man = pDesc->m_ResolvedSkeletonBones.m_Bones;
            const int nB = int(Man.size());
            if (int(C.m_Pose.size()) != nB) return "SuggestPoseAccept: the skeleton changed since the candidate";
            // One undo step for the whole accept: the keys are written one by one (each reads the stack the previous ones made), then those
            // steps are rolled back and replayed as one snapshot edit (the snapshot keeps enums since C57)
            // the motion around the key before the keys (for the continuity report)
            std::vector<xmath::transform3> FramesBefore; int FPSb = 30, nFb = 0;
            { const std::vector<xmath::transform3>* pF = nullptr; std::string E0; if (SuggestClipFrames(pF, FPSb, nFb, E0)) FramesBefore = *pF; }
            const int UndoAt = m_Undo.GetUndoIndex();
            xproperty::settings::context SnapCtx;
            const auto Before = xproperty::ui::undo::SnapshotToString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), SnapCtx);
            int nSteps = 0;
            int nKeys = 0; std::string Err;
            for (int b = 0; b < nB; ++b)
            {
                if (!m_Suggest.m_Owned[b]) continue;
                xmath::fquat Below;
                if (!RotationBelow(iLayer, b, m_Suggest.m_Time, Below, Err)) return "SuggestPoseAccept: " + Err;
                if (AngleDeg(Below, C.m_Pose[b].m_Rotation) < 0.05f) continue;
                const auto R = PoseKey(iLayer, Man[b].m_Name, m_Suggest.m_Time, (C.m_Pose[b].m_Rotation * Below.ConjugateCopy()).NormalizeCopy(), false);
                if (!R.starts_with("PoseKey: layer")) return "SuggestPoseAccept: " + R;
                ++nKeys; nSteps += R.find(" added ") != std::string::npos ? 7 : 4;      // PoseKey's SetProperty steps (a new key: size, tick, bone, x y z w)
            }
            std::string HipsNote;
            if (const int h = m_Suggest.m_iHips; h >= 0 && h < nB)
            {
                xmath::fquat BQ; xmath::fvec3 BP;
                if (!TransformBelow(iLayer, h, m_Suggest.m_Time, BQ, BP, Err)) return "SuggestPoseAccept: " + Err;
                const auto Move = C.m_Pose[h].m_Position - BP;                       // local units (the rig's: cm on a 0.01-scaled rig)
                // the same shift in model space (metres): the candidate with its hips and with the position below
                auto With = C.m_Pose; With[h].m_Position = BP;
                const float Model = (ModelPositions(C.m_Pose.data())[h] - ModelPositions(With.data())[h]).Length();
                if (Model > 1e-5f)
                {
                    const auto R = PoseKey(iLayer, Man[h].m_Name, m_Suggest.m_Time, (C.m_Pose[h].m_Rotation * BQ.ConjugateCopy()).NormalizeCopy(), false, &Move);
                    if (!R.starts_with("PoseKey: layer")) return "SuggestPoseAccept: " + R;
                    HipsNote = std::format("; hips moved {:.2f} cm", Model * 100.0f);
                }
            }
            // the motion around the key made natural (Kimodo), in the same undo step
            // the motion around the key made natural (Kimodo), as far as realism needs: the smallest scope (+-0.5 s, +-1, +-2, the whole
            // clip) whose motion is no worse than the clip's own (skate, acceleration, the hips' momentum, joint limits; balance advisory)
            std::string RefineNote;
            bool bRefined = false;
            {
                std::vector<std::int32_t> Par(nB); std::vector<std::string> Nm(nB); std::vector<std::string_view> Vw(nB); xanimation::pose RestP; RestP.resize(nB);
                for (int b = 0; b < nB; ++b)
                {
                    Par[b] = Man[b].m_iParent; Nm[b] = Man[b].m_Name; RestP.m_Position[b] = Man[b].m_RestPosition; RestP.m_Scale[b] = Man[b].m_RestScale;
                    RestP.m_Rotation[b] = xmath::fquat(Man[b].m_RestRotX, Man[b].m_RestRotY, Man[b].m_RestRotZ, Man[b].m_RestRotW).NormalizeSafeCopy();
                }
                for (int b = 0; b < nB; ++b) Vw[b] = Nm[b];
                const xanimation::skeleton_view SV{ Par };
                // the joint limits: the skeleton's own compiled limits when it has them (JointLimits On), else the Humanoid v1 preset
                std::vector<xanimation::joint_limit> Lim;
                std::string LimitsFrom = xanimation::humanoid_v1_name_v;
                if (auto* pSk = m_SkeletonRef.empty() ? nullptr : xresource::g_Mgr.getResource(m_SkeletonRef); pSk && pSk->m_nJoints == nB && pSk->m_nBones == nB)
                {
                    Lim.resize(nB);
                    for (int b = 0; b < nB; ++b) Lim[b] = xskeleton_joints::FromRuntime(pSk->m_pJoints[b], pSk->m_pBoneRests[b].m_RestPose.m_Rotation.NormalizeSafeCopy());
                    LimitsFrom = "the skeleton's own";
                }
                else Lim = xanimation::HumanoidJointPresetV1(SV, Vw, RestP);
                const auto Rig = xanimation::MakeHumanoidRealismRig(SV, Vw, RestP, std::move(Lim));
                const double Key = m_Suggest.m_Time, Dur = double(std::max(1, nFb - 1)) / double(std::max(1, FPSb));
                const int fk = int(std::lround(Key * FPSb));
                const auto OwnFrames = FramesBefore.empty() ? xanimation::realism_frames{} : xanimation::MeasureRealismFrames(Rig, FramesBefore, nB, nFb, float(FPSb));
                // judged frame by frame against the clip at the same moment (+-3 frames): a wider scope cannot hide the edit's own fault
                // behind a worse moment elsewhere in the clip. Margins: 25 % + a floor (noise, not realism).
                const bool bOwnLimits = LimitsFrom != xanimation::humanoid_v1_name_v;     // reviewed limits decide; the unreviewed preset only informs
                float LastScore = 0;                                                     // how far the last judged scope is from passing (0: passes)
                auto Judge = [&](double Lo, double Hi, std::string& Line) -> std::vector<std::string>
                {
                    const std::vector<xmath::transform3>* pA = nullptr; int FPSa = 30, nFa = 0; std::string E1;
                    std::vector<std::string> Fails;
                    if (FramesBefore.empty() || !SuggestClipFrames(pA, FPSa, nFa, E1) || nFa != nFb) { Line = "not measured"; return Fails; }
                    const int f0 = fk - int(std::lround(Lo * FPSb)) - 3, f1 = fk + int(std::lround(Hi * FPSb)) + 3;
                    // the user's key is intent: what its pose itself asks for (a joint excess, a lean) is allowed within the layer's 0.5 s
                    // falloff of it; the refine must not add to it
                    const auto Cmp = xanimation::CompareRealism(xanimation::MeasureRealismFrames(Rig, *pA, nB, nFa, float(FPSa)), OwnFrames, f0, f1, {}, fk, int(std::lround(0.5 * FPSb)));
                    auto M = [&](const xanimation::realism_margin& m) { return m.m_Frame < 0 ? std::string("-") : std::format("{:.3f} vs {:.3f} at {:.2f} s", m.m_Edit, m.m_Clip, double(m.m_Frame) / FPSb); };
                    Line = std::format("worst against the clip at the same moment - skate {} hip h/s, acceleration {} m/s^2, hips {} m/s^2, limits ({}) {} deg, balance [advisory] {} hip h"
                        , M(Cmp.m_Skate), M(Cmp.m_Acc), M(Cmp.m_Hips), LimitsFrom, M(Cmp.m_Limit), M(Cmp.m_Balance));
                    if (!bOwnLimits) Line += " (limits from the preset, not reviewed for this skeleton: advisory)";
                    LastScore = 0;
                    auto Fail = [&](const xanimation::realism_margin& m, float Floor, const char* pName) { if (m.m_Value > 0) { Fails.push_back(pName); LastScore += m.m_Value / Floor; } };
                    const xanimation::realism_floors Fl;
                    Fail(Cmp.m_Skate, Fl.m_Skate, "foot skate");
                    Fail(Cmp.m_Acc, Fl.m_Acc, "acceleration");
                    Fail(Cmp.m_Hips, Fl.m_Hips, "hips momentum");
                    if (bOwnLimits) Fail(Cmp.m_Limit, Fl.m_Limit, "joint limits");
                    Fail(Cmp.m_Balance, Fl.m_Balance, "balance");
                    return Fails;
                };
                auto Join = [](const std::vector<std::string>& V) { std::string S; for (const auto& s : V) S += (S.empty() ? "" : ", ") + s; return S; };
                if (!bRefine)
                {
                    std::string Line; const auto F = Judge(0.5, 0.5, Line);
                    RefineNote = std::format("; no refine; +-0.5 s: {}{}", Line, F.empty() ? "" : " - worse than the clip: " + Join(F));
                }
                else
                {
                    const auto KeysBefore = pOv->m_Layers[iLayer].m_Keys; const auto TakesBefore = pOv->m_Layers[iLayer].m_Takes;
                    struct scope { double Lo, Hi; std::string Name; };
                    std::vector<scope> Scopes;
                    for (double r : { 0.5, 1.0, 2.0 }) if (r < Key || r < Dur - Key) Scopes.push_back({ std::min(r, Key), std::min(r, Dur - Key), std::format("+-{} s", r) });
                    Scopes.push_back({ Key, Dur - Key, "the whole clip" });
                    std::string Why;
                    std::size_t Best = 0; float BestScore = 1e30f; std::string BestFails;
                    for (std::size_t i = 0; i < Scopes.size(); ++i)
                    {
                        const auto& S = Scopes[i];
                        pOv->m_Layers[iLayer].m_Keys = KeysBefore; pOv->m_Layers[iLayer].m_Takes = TakesBefore;      // each scope from the accepted state
                        std::string Rep, RErr, Line;
                        if (!SuggestRefine(iLayer, C.m_Seed, S.Lo, S.Hi, std::format("refine after Suggest Pose at {:.2f} s, scope {}{}", Key, S.Name, Why), FramesBefore, Rep, RErr))
                        {
                            pOv->m_Layers[iLayer].m_Keys = KeysBefore; pOv->m_Layers[iLayer].m_Takes = TakesBefore;
                            RefineNote += "; refine skipped: " + RErr; bRefined = false; break;
                        }
                        bRefined = true;
                        const auto F = Judge(S.Lo, S.Hi, Line);
                        RefineNote += std::format("; scope {}: {} - {} [{}]", S.Name, F.empty() ? "passes" : "worse than the clip: " + Join(F), Line, Rep);
                        if (F.empty()) { RefineNote += std::format("; chosen scope: {}", S.Name); break; }
                        if (LastScore < BestScore) { BestScore = LastScore; Best = i; BestFails = Join(F); }
                        Why = " (a smaller scope failed: " + Join(F) + ")";
                        if (i + 1 == Scopes.size())
                        {   // no scope passes: the least worse one (wider is not better when the refine itself adds the fault)
                            if (Best != i)
                            {
                                pOv->m_Layers[iLayer].m_Keys = KeysBefore; pOv->m_Layers[iLayer].m_Takes = TakesBefore;
                                std::string Rep2, RErr2;
                                if (!SuggestRefine(iLayer, C.m_Seed, Scopes[Best].Lo, Scopes[Best].Hi, std::format("refine after Suggest Pose at {:.2f} s, scope {} (no scope passed: the least worse)", Key, Scopes[Best].Name), FramesBefore, Rep2, RErr2))
                                { pOv->m_Layers[iLayer].m_Keys = KeysBefore; pOv->m_Layers[iLayer].m_Takes = TakesBefore; RefineNote += "; refine skipped: " + RErr2; bRefined = false; break; }
                            }
                            RefineNote += std::format("; chosen scope: {} (no scope passes; the least worse: {})", Scopes[Best].Name, BestFails);
                        }
                    }
                }
            }
            if (m_Undo.GetUndoIndex() > UndoAt || bRefined)
            {
                const auto After = xproperty::ui::undo::SnapshotToString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), SnapCtx);
                while (m_Undo.GetUndoIndex() > UndoAt) m_Undo.Undo();
                xproperty::ui::undo::ApplySnapshotFromString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), Before, SnapCtx);    // the refine wrote outside the history
                if (auto E = m_Undo.Execute(std::format("SnapshotEdit -Label {} -Before {} -After {}", xeditor::Quote(std::format("Suggest Pose: {} keys", nKeys)), xeditor::Quote(Before), xeditor::Quote(After))); !E.empty())
                    return "SuggestPoseAccept: the keys could not be made one undo step: " + E;
                nSteps = 1;
            }
            // what the stack now plays at the key, against the candidate
            const std::vector<xmath::transform3>* pFrames = nullptr; int FPS = 30, nF = 0;
            if (!SuggestClipFrames(pFrames, FPS, nF, Err)) return std::format("SuggestPoseAccept: {} keys written ({} undo steps); the check could not run: {}", nKeys, nSteps, Err);
            const auto Now = SamplePose(*pFrames, FPS, nF, nB, m_Suggest.m_Time);
            float Max = 0; std::string MaxBone;
            for (int b = 0; b < nB; ++b) if (m_Suggest.m_Owned[b]) { const float a = AngleDeg(Now[b].m_Rotation, C.m_Pose[b].m_Rotation); if (a > Max) { Max = a; MaxBone = Man[b].m_Name; } }
            float HipsMiss = 0;
            if (const int h = m_Suggest.m_iHips; h >= 0 && h < nB) HipsMiss = (ModelPositions(Now.data())[h] - ModelPositions(C.m_Pose.data())[h]).Length();
            // continuity: the largest acceleration within +-1 s of the key, before and after, and that of the motion the layer adds (its falloff
            // and smooth curve ease the change in and out)
            const int fk = int(std::lround(m_Suggest.m_Time * FPS));
            const bool bSame = FramesBefore.size() == pFrames->size();
            const float AccB = bSame ? MaxAcceleration(FramesBefore, FPSb, nFb, fk - FPSb, fk + FPSb) : -1.0f;
            const float AccA = MaxAcceleration(*pFrames, FPS, nF, fk - FPS, fk + FPS);
            const float AccD = bSame ? MaxAcceleration(*pFrames, FPS, nF, fk - FPS, fk + FPS, &FramesBefore) : -1.0f;
            const std::string AccDAt = m_AccAt;
            int iMovedBone = -1; for (int b = 0; b < nB; ++b) if (Man[b].m_Name == m_Suggest.m_MovedBone) iMovedBone = b;
            // locality: the largest change of any bone more than 0.6 s from the key (the falloff keeps a pose local; the old ramp reached the clip's ends)
            float Far = 0;
            if (bSame)
                for (int f = 0; f < nF; ++f)
                {
                    if (std::abs(f - fk) <= int(0.6 * FPS)) continue;
                    const auto Pa = ModelPositions(&(*pFrames)[std::size_t(f) * nB]), Pb = ModelPositions(&FramesBefore[std::size_t(f) * nB]);
                    for (int b = 0; b < nB; ++b) Far = std::max(Far, (Pa[b] - Pb[b]).Length());
                }
            std::string Moved = std::format("; change beyond 0.6 s of the key {:.3f} cm", Far * 100.0f);
            if (bSame && iMovedBone >= 0)
                Moved += std::format("; the moved {}: added motion max {:.2f} m/s^2 ({}), the clip's own {:.2f}", m_Suggest.m_MovedBone, MaxAcceleration(*pFrames, FPS, nF, fk - FPS, fk + FPS, &FramesBefore, iMovedBone), m_AccAt
                                   , MaxAcceleration(FramesBefore, FPSb, nFb, fk - FPSb, fk + FPSb, nullptr, iMovedBone));
            m_Suggest.m_Sig = SuggestSignature();      // the keys just written are the candidate's own
            return std::format("SuggestPoseAccept: {} keys on layer {} ({} undo steps){}; the stack at the key against the candidate: max {:.4f} deg ({}), hips {:.3f} cm; acceleration within 1 s of the key: before {:.1f}, after {:.1f} m/s^2; of the added motion {:.1f} m/s^2 ({}; key frame {})"
                , nKeys, iLayer, nSteps, HipsNote, Max, MaxBone, HipsMiss * 100.0f, AccB, AccA, AccD, AccDAt, fk) + Moved + RefineNote;
        }

        static std::string SafeFileName(std::string N) noexcept
        {
            for (auto& c : N) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
            return N.empty() ? "KimodoClip" : N;
        }

        // The preview goes into the assets: the .bvh (+ .kimodo.txt) in the Kimodo folder, an import source entry, the descriptor saved and compiled
        std::string KimodoAccept(std::string Name) noexcept
        {
            auto* pDesc = Desc();
            if (!m_K.m_bPreview || !pDesc) return "KimodoAccept: nothing to accept (generate first)";
            if (Name.empty()) Name = m_K.m_Name;
            Name = SafeFileName(Name);
            const std::wstring Folder = pDesc->m_Kimodo.m_Folder;
            const auto Dir = ProjectPath() / Folder;
            std::error_code Ec;
            std::filesystem::create_directories(Dir, Ec);
            std::wstring File = xstrtool::To(Name) + L".bvh";
            for (int i = 2; std::filesystem::exists(Dir / File); ++i) File = std::format(L"{}_{}.bvh", xstrtool::To(Name), i);
            std::filesystem::copy_file(m_K.m_Job.m_Bvh, Dir / File, Ec);
            if (Ec) return "KimodoAccept: could not write " + (Dir / File).string() + ": " + Ec.message();
            {
                std::ofstream Info(Dir / (std::filesystem::path(File).stem().wstring() + L".kimodo.txt"));
                Info << "generator: NVIDIA Kimodo (Kimodo-SOMA-RP-v1.2) through xkimodo\nprompt: " << m_K.m_PromptUsed << "\nseconds: " << m_K.m_SecondsUsed
                     << "\nseed: " << m_K.m_SeedUsed << "\nsteps: 100\ncfg: 2 2\n";
            }

            const int n = int(pDesc->m_ImportSources.size());
            const auto Base = std::format("AnimPackage/ImportSources[G:{}]/", n);
            const auto Rel  = xstrtool::To(Folder + L"\\" + File);
            const auto Rt   = std::format("{:016X}, {:016X}", pDesc->m_Kimodo.m_Retarget.m_Instance.m_Value, xrsc::retarget_type_guid_v.m_Value);
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote("AnimPackage/ImportSources[]"), xeditor::Quote(std::to_string(n + 1)), xeditor::Quote(std::to_string(n))));
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Type"), xeditor::Quote("Different skeleton: animation asset")));
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Path"), xeditor::Quote(Rel)));
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Retarget"), xeditor::Quote(Rt)));
            // the BVH's one take is "Motion": the clip is named after the file (the compile keeps a clip entry whose OriginalName it finds)
            const auto ClipName = xstrtool::To(std::filesystem::path(File).stem().wstring());
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[]"), xeditor::Quote("1")));
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[G:0]/OriginalName"), xeditor::Quote("Motion")));
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[G:0]/Name"), xeditor::Quote(ClipName)));
            (void)m_Undo.Query("Save");
            (void)m_Undo.Query("Compile");

            KimodoDiscard();
            m_K.m_bStatusError = false;
            m_K.m_Status = std::format("saved {} and added it as import source {}; the package is compiling", Rel, n);
            return "KimodoAccept: " + m_K.m_Status;
        }

        void KimodoDiscard() noexcept
        {
            m_K.m_bPreview = false; m_K.m_Preview = {};
            std::error_code Ec;
            if (!m_K.m_Job.m_Bvh.empty()) std::filesystem::remove(m_K.m_Job.m_Bvh, Ec);
        }

        void RenderKimodo() noexcept
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            if (auto* pWhy = KimodoWhyNot()) { ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "To generate: %s.", pWhy); ImGui::PopTextWrapPos(); return; }
            ImGui::TextDisabled("Describe a motion; it previews on this package's skeleton and goes into the assets only when you accept it.");
            ImGui::PopTextWrapPos();

            const bool bBusy = m_K.m_Job.m_bRunning;
            ImGui::BeginDisabled(bBusy);
            ImGui::InputTextMultiline("##prompt", m_K.m_Prompt, sizeof(m_K.m_Prompt), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
            if (ImGui::IsItemHovered()) xeditor::hint::Text("What the motion is, in words: \"A person jumps over an obstacle.\"");
            ImGui::SetNextItemWidth(120.0f); ImGui::SliderFloat("Seconds", &m_K.m_Seconds, 1.0f, 20.0f, "%.1f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(90.0f); ImGui::InputInt("Seed", &m_K.m_Seed);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Another seed, another take of the same prompt");
            if (ImGui::Button(m_K.m_bPreview ? "Generate again" : "Generate"))
            {
                if (m_K.m_bPreview && m_K.m_Seed == m_K.m_SeedUsed && m_K.m_PromptUsed == m_K.m_Prompt) ++m_K.m_Seed;       // "again" with nothing changed: the next take
                if (auto R = KimodoGenerate(m_K.m_Prompt, m_K.m_Seconds, m_K.m_Seed); R != "KimodoGenerate: started") { m_K.m_Status = R; m_K.m_bStatusError = true; }
            }
            ImGui::EndDisabled();
            if (bBusy)
            {
                ImGui::SameLine(); ImGui::Text("generating... %.0f s", ImGui::GetTime() - m_K.m_Job.m_StartTime);
                ImGui::SameLine(); if (ImGui::Button("Cancel")) { m_K.m_Job.Cancel(); m_K.m_Status = "cancelled"; }
            }

            if (m_K.m_bPreview)
            {
                ImGui::Separator();
                ImGui::TextWrapped("Previewing (not in the assets): \"%s\", %.1f s, seed %d", m_K.m_PromptUsed.c_str(), m_K.m_Preview.m_nFrames * m_K.m_Preview.m_FrameTime, m_K.m_SeedUsed);
                if (m_K.m_Baked.empty())
                {
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "On Kimodo's skeleton only: %s", m_K.m_RetargetError.c_str());
                    ImGui::PopTextWrapPos();
                }
                else
                {
                    ImGui::Checkbox("Show Kimodo's skeleton", &m_K.m_bShowSource);
                    if (ImGui::IsItemHovered()) xeditor::hint::Text("Off: the motion on this package's skeleton, moved by the Kimodo Retarget exactly as the compile will move it");
                }
                ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##name", m_K.m_Name, sizeof(m_K.m_Name));
                if (ImGui::IsItemHovered()) xeditor::hint::Text("The clip's file name in %s", xstrtool::To(Desc()->m_Kimodo.m_Folder).c_str());
                if (ImGui::Button("Accept")) { m_K.m_Status = KimodoAccept(m_K.m_Name); }
                if (ImGui::IsItemHovered()) xeditor::hint::Text("Saves it in the Kimodo folder, adds it to this package (Retarget: Kimodo/Retarget) and compiles");
                ImGui::SameLine(); if (ImGui::Button("Discard")) { KimodoDiscard(); m_K.m_Status = "discarded"; m_K.m_bStatusError = false; }
            }
            if (!m_K.m_Status.empty())
            {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                if (m_K.m_bStatusError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_K.m_Status.c_str());
                else ImGui::TextDisabled("%s", m_K.m_Status.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        //----------------------------------------------------------------------------------------
        // Playback
        //----------------------------------------------------------------------------------------

        xanim_package::anim_package* Package() noexcept { return m_bShowable && !m_Ref.empty() ? xresource::g_Mgr.getResource(m_Ref) : nullptr; }

        static int FindCompiledClip(const xanim_package::anim_package& Package, const xanim_package_desc::clip& Clip) noexcept
        {
            return Package.findClipIndex(xstrtool::CRC32(Clip.m_Name));
        }

        bool SelectDescriptorClip(int iSource, int iClip) noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc || iSource < 0 || iSource >= int(pDesc->m_ImportSources.size())) return false;
            auto& Clips = pDesc->m_ImportSources[iSource].m_Clips;
            if (iClip < 0 || iClip >= int(Clips.size())) return false;

            m_iSelectedSource         = iSource;
            m_iSelectedDescriptorClip = iClip;
            auto* pPackage = Package();
            // A clip marked for delete, or edited since the last compile, has no compiled counterpart to play
            m_iSelectedClip = (pPackage && !Clips[iClip].m_bDelete) ? FindCompiledClip(*pPackage, Clips[iClip]) : -1;
            m_TimeSeconds   = 0.0f;
            m_LoopsElapsed  = 0;
            m_Timeline      = {};
            return true;
        }

        const xanim_package::clip* SelectedClip() noexcept
        {
            auto* pPackage = Package();
            if (!pPackage || m_iSelectedClip < 0 || m_iSelectedClip >= int(pPackage->getClips().size())) return nullptr;
            return &pPackage->getClips()[m_iSelectedClip];
        }

        static float ClipLength(const xanim_package::clip& Clip) noexcept
        {
            return (Clip.m_FPS > 0 && Clip.m_nFrames > 0) ? float(Clip.m_nFrames) / float(Clip.m_FPS) : 0.0f;
        }

        // Play from where it is; a clip that played to its end starts again from 0 (the button, the P key and the Play command)
        void StartPlaying() noexcept
        {
            if (auto* pClip = SelectedClip(); pClip && m_TimeSeconds >= ClipLength(*pClip)) { m_TimeSeconds = 0.0f; m_LoopsElapsed = 0; }
            m_bPlaying = true;
        }
        void TogglePlay() noexcept { if (m_bPlaying) m_bPlaying = false; else StartPlaying(); }

        // Once a frame, whichever tab is showing: the time advances while playing
        void Render() noexcept override
        {
            KimodoPoll();
            if (m_K.m_bPreview && m_K.m_bPlaying)
            {
                const float Len = m_K.m_Preview.m_nFrames * m_K.m_Preview.m_FrameTime;
                m_K.m_Time += ImGui::GetIO().DeltaTime * xgpu::tools::editors::g_PlaybackSpeeds[m_iSpeedIndex];
                if (Len > 0.0f) m_K.m_Time = std::fmod(m_K.m_Time, Len);
            }
            if (m_bPlaying)
                if (auto* pClip = SelectedClip(); pClip && ClipLength(*pClip) > 0.0f)
                    xgpu::tools::editors::AdvancePlayback(m_TimeSeconds, m_LoopsElapsed, m_bPlaying, ClipLength(*pClip), pClip->m_bLoop, ImGui::GetIO().DeltaTime, xgpu::tools::editors::g_PlaybackSpeeds[m_iSpeedIndex]);
            descriptor_editor::Render();
        }

        //----------------------------------------------------------------------------------------
        // The clips table. Every change is a SetProperty command, so it is undoable and the same as a typed one.
        //----------------------------------------------------------------------------------------

        static std::string ClipPath(int iSource, int iClip, const char* pField) noexcept
        {
            return std::format("AnimPackage/ImportSources[G:{}]/Clips[G:{}]/{}", iSource, iClip, pField);
        }

        void SetClipProperty(int iSource, int iClip, const char* pField, const std::string& Value, const std::string& Before) noexcept
        {
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(ClipPath(iSource, iClip, pField)), xeditor::Quote(Value), xeditor::Quote(Before)));
        }

        // An integer field: typed straight into the descriptor while it is edited, reported as one command when the edit ends
        void IntField(const char* pId, int& Value, int iSource, int iClip, const char* pField) noexcept
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(pId, &Value, 0);
            if (ImGui::IsItemActivated()) m_IntBefore = Value;
            if (ImGui::IsItemDeactivatedAfterEdit() && Value != m_IntBefore) SetClipProperty(iSource, iClip, pField, std::to_string(Value), std::to_string(m_IntBefore));
        }

        void RenderClips() noexcept
        {
            if (!m_ErrorMessage.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_ErrorMessage.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
            }
            auto* pDesc = Desc();
            if (!pDesc) return;

            auto& Sources = pDesc->m_ImportSources;
            int Total = 0;
            for (auto& S : Sources) Total += static_cast<int>(S.m_Clips.size());
            ImGui::Text("%d clip(s) - click a name to preview, double-click to rename", Total);
            ImGui::Separator();

            constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("###ClipsTable", 8, TableFlags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 32.0f);
                ImGui::TableSetupColumn("Loop",   ImGuiTableColumnFlags_WidthFixed, 34.0f);
                ImGui::TableSetupColumn("DS",     ImGuiTableColumnFlags_WidthFixed, 42.0f);
                ImGui::TableSetupColumn("In",     ImGuiTableColumnFlags_WidthFixed, 42.0f);
                ImGui::TableSetupColumn("Out",    ImGuiTableColumnFlags_WidthFixed, 42.0f);
                ImGui::TableSetupColumn("RM",     ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("###spacer", ImGuiTableColumnFlags_WidthFixed, 1.0f);

                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                auto Header = [](int Column, const char* pLabel, const char* pTip)
                {
                    ImGui::TableSetColumnIndex(Column);
                    ImGui::TableHeader(pLabel);
                    if (ImGui::IsItemHovered()) xeditor::hint::Text("%s", pTip);
                };
                Header(0, "Name",       "Compiled clip name - hover a row's name for its original import stats and source file");
                Header(1, g_DeleteIcon, "Delete Clip - excluded from the compiled output entirely (still listed here, so it can be re-enabled)");
                Header(2, "Loop",       "Loop - the clip repeats when it reaches its end; its frames are not changed");
                Header(3, "DS",         "Down Sample - 0 = keep the imported 60fps rate; lower it (e.g. 30) only to trade quality for memory");
                Header(4, "In",         "Trim In - first frame to keep (post-resample), -1 = from the start");
                Header(5, "Out",        "Trim Out - last frame to keep (post-resample), -1 = to the end");
                Header(6, "RM",         "Root Motion - extract the root bone's translation into a separate channel instead of animating in place");

                for (int iSource = 0; iSource < static_cast<int>(Sources.size()); ++iSource)
                {
                    auto& Source = Sources[iSource];
                    ImGui::PushID(iSource);
                    for (int i = 0; i < static_cast<int>(Source.m_Clips.size()); ++i)
                    {
                        auto& Clip = Source.m_Clips[i];
                        ImGui::PushID(i);
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        const bool bSelected = m_iSelectedSource == iSource && m_iSelectedDescriptorClip == i;
                        if (m_iRenameSource == iSource && m_iRenameClip == i)
                        {
                            static char Buffer[128];     // one row renames at a time
                            if (m_bRenameStarted)
                            {
                                std::snprintf(Buffer, sizeof(Buffer), "%s", m_RenameBuffer.c_str());
                                ImGui::SetKeyboardFocusHere();
                                m_bRenameStarted = false;
                            }
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            const bool bEnter = ImGui::InputText("##name", Buffer, sizeof(Buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                            if (bEnter || ImGui::IsItemDeactivatedAfterEdit())
                            {
                                const std::string NewName = Buffer, OldName = Clip.m_Name;
                                m_iRenameSource = m_iRenameClip = -1;
                                if (NewName != OldName) SetClipProperty(iSource, i, "Name", NewName, OldName);
                            }
                            else if (ImGui::IsItemDeactivated()) m_iRenameSource = m_iRenameClip = -1;     // Escape: nothing changed
                        }
                        else
                        {
                            if (ImGui::Selectable(Clip.m_Name.c_str(), bSelected)) SelectDescriptorClip(iSource, i);
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            {
                                m_iRenameSource = iSource; m_iRenameClip = i; m_bRenameStarted = true; m_RenameBuffer = Clip.m_Name;
                            }
                            if (ImGui::IsItemHovered())
                            {
                                const auto FileName = std::filesystem::path(Source.m_Path).filename().string();
                                const int iDetailsSource = m_Details.findSource(Source.m_Path);
                                const int iDetailsClip   = iDetailsSource == -1 ? -1 : m_Details.m_Sources[iDetailsSource].findClip(Clip.m_OriginalName);
                                if (iDetailsClip != -1)
                                {
                                    auto& D = m_Details.m_Sources[iDetailsSource].m_ClipList[iDetailsClip];
                                    xeditor::hint::Text("Imported as \"%s\"\nSource: %s\n%d fps, %d frames, %.2fs\n(double-click to rename)", Clip.m_OriginalName.c_str(), FileName.empty() ? "(no path set)" : FileName.c_str(), D.m_OriginalFPS, D.m_OriginalFrameCount, D.m_DurationSeconds);
                                }
                                else
                                    xeditor::hint::Text("Imported as \"%s\"\nSource: %s (not in the last import)\n(double-click to rename)", Clip.m_OriginalName.c_str(), FileName.empty() ? "(no path set)" : FileName.c_str());
                            }
                        }

                        ImGui::TableSetColumnIndex(1);
                        { bool V = Clip.m_bDelete; if (ImGui::Checkbox("##delete", &V)) SetClipProperty(iSource, i, "Delete", V ? "true" : "false", Clip.m_bDelete ? "true" : "false"); }
                        ImGui::TableSetColumnIndex(2);
                        { bool V = Clip.m_bLoop;   if (ImGui::Checkbox("##loop",   &V)) SetClipProperty(iSource, i, "Loop",   V ? "true" : "false", Clip.m_bLoop   ? "true" : "false"); }
                        ImGui::TableSetColumnIndex(3); IntField("##downsamplefps", Clip.m_DownsampleFPS,  iSource, i, "DownsampleFPS");
                        ImGui::TableSetColumnIndex(4); IntField("##trimstart",     Clip.m_TrimStartFrame, iSource, i, "TrimStartFrame");
                        ImGui::TableSetColumnIndex(5); IntField("##trimend",       Clip.m_TrimEndFrame,   iSource, i, "TrimEndFrame");

                        ImGui::TableSetColumnIndex(6);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        int Mode = static_cast<int>(Clip.m_RootMotion);
                        if (ImGui::Combo("##rootmotion", &Mode, "None\0XZ Only\0XYZ\0")) SetClipProperty(iSource, i, "RootMotion", std::to_string(Mode), std::to_string(static_cast<int>(Clip.m_RootMotion)));

                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (Sources.empty()) ImGui::TextDisabled("No import sources yet: add one in the Description panel.");
        }

        //----------------------------------------------------------------------------------------
        // The 3D view and the transport bar
        //----------------------------------------------------------------------------------------

        // The pose to show: the selected clip at the current time when there is one (with its root motion), the rest pose otherwise
        void EvaluatePose(const xskeleton::skeleton& Skeleton) noexcept
        {
            auto* pPackage = Package();
            UpdateLiveLayers();
            if (LiveLayers()) EvaluateLiveLayers(Skeleton);        // the selected clip's layers composed now (what the compile will store)
            else if (auto* pClip = SelectedClip())
            {
                xgpu::tools::editors::ComputeAnimatedBoneWorlds(Skeleton, *pPackage, m_iSelectedClip, m_TimeSeconds, m_PoseWorlds);
                if (pClip->m_RootMotionMode != xanim_package::root_motion_mode::NONE)
                {
                    const auto Offset = xgpu::tools::editors::ComputeRootMotionOffset(*pClip, pPackage->getClipRootMotion(m_iSelectedClip), m_TimeSeconds, m_LoopsElapsed);
                    xgpu::tools::editors::ApplyWorldOffset(m_PoseWorlds, Offset);
                }
            }
            else m_PoseWorlds = m_RestWorlds;

            m_PoseBones.resize(m_PoseWorlds.size());
            for (std::size_t i = 0; i < m_PoseWorlds.size(); ++i)
            {
                m_PoseBones[i].m_Position = m_PoseWorlds[i].ExtractPosition();
                m_PoseBones[i].m_Right    = m_PoseWorlds[i].Right();
                m_PoseBones[i].m_Up       = m_PoseWorlds[i].Up();
            }
        }

        void RenderViewport() noexcept
        {
            m_CameraToolbar.Render(ImGui::GetContentRegionAvail(), m_Viewport.m_Scene.m_State, [this] { RenderViewportBody(); });
        }

        void RenderViewportBody() noexcept
        {
            auto* pHost   = xeditor::host::current();
            auto* pWindow = pHost ? pHost->find<xgpu::window>() : nullptr;
            if (!m_bReady || !pWindow) { ImGui::TextDisabled("The 3D view needs a GPU device (open from the editor)."); return; }
            if (m_K.m_bPreview && !m_K.m_bShowSource && !m_K.m_Baked.empty())
                if (auto* pOwn = xresource::g_Mgr.getResource(m_SkeletonRef))
                {
                    KimodoEvaluateRetargeted();
                    const xskeleton_editor::viewport_item Item{ .m_pSkeleton = pOwn, .m_Bones = m_K.m_DstBones };
                    m_Viewport.Render(std::span<const xskeleton_editor::viewport_item>(&Item, 1));
                    return;
                }
            if (m_K.m_bPreview)
                if (auto* pSoma = xresource::g_Mgr.getResource(m_K.m_SkeletonRef))
                {
                    KimodoEvaluate();
                    const xskeleton_editor::viewport_item Item{ .m_pSkeleton = pSoma, .m_Bones = m_K.m_Bones, .m_NormalColor = 0xFF40C0FFu };
                    m_Viewport.Render(std::span<const xskeleton_editor::viewport_item>(&Item, 1));
                    return;
                }
            if (!m_bShowable) { ImGui::TextWrapped("%s", m_ErrorMessage.empty() ? "Nothing to show." : m_ErrorMessage.c_str()); return; }
            auto* pSkeleton = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkeleton) return;

            // Play / pause is the AnimPackage/Preview/PlayPause action (P; Space belongs to the drawer).
            EvaluatePose(*pSkeleton);
            UpdateMaskShown();
            const xskeleton_editor::viewport_item Item{ .m_pSkeleton = pSkeleton, .m_Bones = m_PoseBones, .m_pSelected = m_MaskShown.empty() ? nullptr : &m_MaskShown };
            m_Viewport.Render(std::span<const xskeleton_editor::viewport_item>(&Item, 1));

            // mask editing: a click on a bone adds it (with its branch; Shift: alone) to the selected layer's mask, or removes it
            if (m_bMaskEdit && m_Viewport.Over() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                int iBone = -1; float T = 0;
                auto* pDesc = Desc();
                if (pDesc && m_Viewport.PickBone(Item, ImGui::GetMousePos(), iBone, T) && iBone >= 0 && iBone < int(pDesc->m_ResolvedSkeletonBones.m_Bones.size()))
                    LayerMaskToggle(m_iLayer, pDesc->m_ResolvedSkeletonBones.m_Bones[iBone].m_Name, ImGui::GetIO().KeyShift);
            }
        }

        void RenderPlayback() noexcept
        {
            if (m_K.m_bPreview)
            {
                namespace ed = xgpu::tools::editors;
                const float Length = m_K.m_Preview.m_nFrames * m_K.m_Preview.m_FrameTime;
                if (ImGui::Button(m_K.m_bPlaying ? ed::g_PauseIcon : ed::g_PlayIcon)) m_K.m_bPlaying = !m_K.m_bPlaying;
                ImGui::SameLine(); ImGui::SetNextItemWidth(140.0f);
                ImGui::SliderInt("##speed", &m_iSpeedIndex, 0, ed::g_NumPlaybackSpeeds - 1, ed::g_PlaybackSpeedLabels[m_iSpeedIndex]);
                ImGui::SameLine(); ImGui::TextColored(ImVec4(0.25f, 0.75f, 1.0f, 1.0f), "Kimodo preview - not in the assets until accepted");
                const float Footer = ImGui::GetTextLineHeightWithSpacing();
                xgpu::tools::imgui::timeline::Draw(m_Timeline, m_K.m_Time, Length, 1.0f / m_K.m_Preview.m_FrameTime, {}, "kimodo_timeline", m_K.m_Name, std::max(ImGui::GetContentRegionAvail().y - Footer, 0.0f));
                ImGui::Text("Frames: %d    FPS: %.0f    Prompt: %s", m_K.m_Preview.m_nFrames, 1.0f / m_K.m_Preview.m_FrameTime, m_K.m_PromptUsed.c_str());
                return;
            }
            auto* pClip = SelectedClip();
            if (!pClip) { ImGui::TextDisabled("Select a clip to preview."); return; }
            namespace ed = xgpu::tools::editors;
            const float Length = ClipLength(*pClip);

            if (ImGui::Button(m_bPlaying ? ed::g_PauseIcon : ed::g_PlayIcon)) TogglePlay();
            if (ImGui::IsItemHovered()) xeditor::hint::Text(m_bPlaying ? "Pause" : "Play");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToStartIcon)) { m_TimeSeconds = 0.0f; m_LoopsElapsed = 0; }
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Go to start");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToEndIcon)) m_TimeSeconds = Length;
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Go to end");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderInt("##speed", &m_iSpeedIndex, 0, ed::g_NumPlaybackSpeeds - 1, ed::g_PlaybackSpeedLabels[m_iSpeedIndex]);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Playback speed");

            const char* pName = nullptr;
            if (auto* pDesc = Desc(); pDesc && m_iSelectedSource >= 0 && m_iSelectedSource < int(pDesc->m_ImportSources.size()))
            {
                auto& Clips = pDesc->m_ImportSources[m_iSelectedSource].m_Clips;
                if (m_iSelectedDescriptorClip >= 0 && m_iSelectedDescriptorClip < int(Clips.size())) pName = Clips[m_iSelectedDescriptorClip].m_Name.c_str();
            }

            // The footer line stays at the bottom; the timeline takes what is above it
            const float Footer = ImGui::GetTextLineHeightWithSpacing();
            if (xgpu::tools::imgui::timeline::Draw(m_Timeline, m_TimeSeconds, Length, static_cast<float>(pClip->m_FPS), {}, "playback_timeline", pName, std::max(ImGui::GetContentRegionAvail().y - Footer, 0.0f)))
                m_LoopsElapsed = 0;     // scrubbed by hand: the count of elapsed loops means nothing now

            ImGui::Text("Zoom: %.0f%%    FPS: %d    Frames: %d    Loop: %s    Root Motion: %s"
                , xgpu::tools::imgui::timeline::GetZoomPercent(m_Timeline, Length), pClip->m_FPS, pClip->m_nFrames, pClip->m_bLoop ? "Yes" : "No", RootMotionModeName(pClip->m_RootMotionMode));
        }
    };

    inline std::string playback_cmd::Query() noexcept
    {
        auto& S = m_Session;
        std::string A, B;
        switch (m_Kind)
        {
        case kind::select_clip:
        {
            int iSource = -1, iClip = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, B)
             || std::from_chars(A.data(), A.data() + A.size(), iSource).ec != std::errc() || std::from_chars(B.data(), B.data() + B.size(), iClip).ec != std::errc()) return "SelectClip: bad arguments";
            if (!S.SelectDescriptorClip(iSource, iClip)) return "SelectClip: no such clip";
            return S.m_iSelectedClip >= 0 ? "SelectClip: selected" : "SelectClip: selected (it has no compiled clip to play: compile, or it is marked for delete)";
        }
        case kind::play:  if (!S.SelectedClip()) return "Play: no clip is selected"; S.StartPlaying();  return "Play: playing";
        case kind::pause: S.m_bPlaying = false; return "Pause: paused";
        case kind::seek:
        {
            float Time = 0;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || std::from_chars(A.data(), A.data() + A.size(), Time).ec != std::errc()) return "Seek: bad arguments";
            auto* pClip = S.SelectedClip();
            if (!pClip) return "Seek: no clip is selected";
            S.m_TimeSeconds = std::clamp(Time, 0.0f, session::ClipLength(*pClip));
            S.m_LoopsElapsed = 0;
            return std::format("Seek: {:.3f}s", S.m_TimeSeconds);
        }
        case kind::set_speed:
        {
            int Index = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || std::from_chars(A.data(), A.data() + A.size(), Index).ec != std::errc()) return "SetSpeed: bad arguments";
            if (Index < 0 || Index >= xgpu::tools::editors::g_NumPlaybackSpeeds) return std::format("SetSpeed: 0 to {}", xgpu::tools::editors::g_NumPlaybackSpeeds - 1);
            S.m_iSpeedIndex = Index;
            return std::format("SetSpeed: {}", xgpu::tools::editors::g_PlaybackSpeedLabels[Index]);
        }
        case kind::info:
        {
            auto* pClip = S.SelectedClip();
            std::string Text = std::format("compiled: {}\n", S.m_bShowable ? "yes" : "no");
            if (!S.m_ErrorMessage.empty()) Text += "note: " + S.m_ErrorMessage + "\n";
            Text += std::format("selected: source {} clip {} (compiled index {})\n", S.m_iSelectedSource, S.m_iSelectedDescriptorClip, S.m_iSelectedClip);
            if (pClip) Text += std::format("time: {:.3f} / {:.3f}s\nplaying: {}\nspeed: {}\nfps: {}  frames: {}  loop: {}  root motion: {}\n"
                , S.m_TimeSeconds, session::ClipLength(*pClip), S.m_bPlaying ? "yes" : "no", xgpu::tools::editors::g_PlaybackSpeedLabels[S.m_iSpeedIndex]
                , pClip->m_FPS, pClip->m_nFrames, pClip->m_bLoop ? "yes" : "no", RootMotionModeName(pClip->m_RootMotionMode));
            return Text;
        }
        case kind::list_clips:
        {
            auto* pDesc = S.Desc();
            if (!pDesc) return "ListClips: nothing loaded";
            auto* pPackage = S.Package();
            std::string Text;
            for (int iSource = 0; iSource < int(pDesc->m_ImportSources.size()); ++iSource)
            {
                auto& Source = pDesc->m_ImportSources[iSource];
                Text += std::format("source {}: {}\n", iSource, xstrtool::To(Source.m_Path));
                for (int i = 0; i < int(Source.m_Clips.size()); ++i)
                {
                    auto& C = Source.m_Clips[i];
                    Text += std::format("  clip {}: \"{}\" (imported as \"{}\") delete={} loop={} downsample={} trim={}..{} root motion={} compiled={}\n"
                        , i, C.m_Name, C.m_OriginalName, C.m_bDelete, C.m_bLoop, C.m_DownsampleFPS, C.m_TrimStartFrame, C.m_TrimEndFrame
                        , RootMotionModeName(C.m_RootMotion), (pPackage && !C.m_bDelete && S.FindCompiledClip(*pPackage, C) >= 0) ? "yes" : "no");
                }
            }
            return Text.empty() ? "(no import sources)" : Text;
        }
        case kind::kimodo_generate:
        {
            std::string Prompt, Secs = "5", Seed = "1";
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Prompt)) return "KimodoGenerate: -Prompt is required";
            xeditor::cmd_util::GetArg(m_Parser, m_hB, Secs); xeditor::cmd_util::GetArg(m_Parser, m_hC, Seed);
            float Seconds = 5.0f; int iSeed = 1;
            std::from_chars(Secs.data(), Secs.data() + Secs.size(), Seconds); std::from_chars(Seed.data(), Seed.data() + Seed.size(), iSeed);
            std::snprintf(S.m_K.m_Prompt, sizeof(S.m_K.m_Prompt), "%s", Prompt.c_str()); S.m_K.m_Seconds = Seconds; S.m_K.m_Seed = iSeed;
            return S.KimodoGenerate(Prompt, Seconds, iSeed);
        }
        case kind::kimodo_accept:
        {
            std::string Name; xeditor::cmd_util::GetArg(m_Parser, m_hA, Name);
            return S.KimodoAccept(Name);
        }
        case kind::kimodo_discard: S.KimodoDiscard(); return "KimodoDiscard: discarded";
        case kind::layer_add:
        {
            std::string Src, Clip, Name, Kind;
            xeditor::cmd_util::GetArg(m_Parser, m_hC, Name);
            xeditor::cmd_util::GetArg(m_Parser, m_hD, Kind);
            if (Kind == "Pose" || Kind == "pose") return S.LayerAdd({}, {}, Name, true);
            if (!Kind.empty() && Kind != "Clip" && Kind != "clip") return "LayerAdd: -Kind is Clip or Pose";
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Src) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Clip)) return "LayerAdd: -Source and -Clip are required";
            return S.LayerAdd(xstrtool::To(Src), Clip, Name);
        }
        case kind::pose_key:
        {
            std::string L, Bone, Time, Turn, Off; int iL = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Bone) || !xeditor::cmd_util::GetArg(m_Parser, m_hC, Time)
                || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "PoseKey: bad arguments";
            xeditor::cmd_util::GetArg(m_Parser, m_hD, Turn);
            xeditor::cmd_util::GetArg(m_Parser, m_hE, Off);
            double Secs = 0; float D[3] = {};
            if (std::from_chars(Time.data(), Time.data() + Time.size(), Secs).ec != std::errc() || Secs < 0) return "PoseKey: -Time is seconds (>= 0)";
            if (!Turn.empty() && std::sscanf(Turn.c_str(), "%f %f %f", &D[0], &D[1], &D[2]) != 3) return "PoseKey: -Turn is \"x y z\" degrees";
            constexpr float R = 3.14159265358979f / 180.0f;
            const xmath::fquat Q(xmath::radian3(xmath::radian{ D[0] * R }, xmath::radian{ D[1] * R }, xmath::radian{ D[2] * R }));
            std::string MoveArg; xeditor::cmd_util::GetArg(m_Parser, m_hF, MoveArg);
            xmath::fvec3 Mv(0, 0, 0);
            if (!MoveArg.empty() && std::sscanf(MoveArg.c_str(), "%f %f %f", &Mv.m_X, &Mv.m_Y, &Mv.m_Z) != 3) return "PoseKey: -Move is \"x y z\"";
            return S.PoseKey(iL, Bone, Secs, Q, Off == "1" || Off == "true", MoveArg.empty() ? nullptr : &Mv);
        }
        case kind::layer_mask:
        {
            std::string L, Bone, Only; int iL = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Bone) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "LayerMask: bad arguments";
            xeditor::cmd_util::GetArg(m_Parser, m_hC, Only);
            S.m_iLayer = iL;
            return S.LayerMaskToggle(iL, Bone, Only == "1" || Only == "true");
        }
        case kind::suggest_pose:
        {
            std::string Time, Move, Pin, Seeds, Prompt; double Secs = 0;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Time) || std::from_chars(Time.data(), Time.data() + Time.size(), Secs).ec != std::errc() || Secs < 0) return "SuggestPose: -Time is seconds (>= 0)";
            xeditor::cmd_util::GetArg(m_Parser, m_hB, Move); xeditor::cmd_util::GetArg(m_Parser, m_hC, Pin); xeditor::cmd_util::GetArg(m_Parser, m_hD, Seeds); xeditor::cmd_util::GetArg(m_Parser, m_hE, Prompt);
            return S.SuggestPose(Secs, Move, Pin.empty() ? "LeftFoot RightFoot" : Pin, Seeds.empty() ? "1" : Seeds, Prompt);
        }
        case kind::suggest_pose_accept:
        {
            std::string L, C; int iL = -1, iC = 0;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "SuggestPoseAccept: bad arguments";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, C) && std::from_chars(C.data(), C.data() + C.size(), iC).ec != std::errc()) return "SuggestPoseAccept: -Candidate is an index";
            std::string Rf; const bool bRefine = !(xeditor::cmd_util::GetArg(m_Parser, m_hC, Rf) && (Rf == "0" || Rf == "false"));
            std::string Mw; double MaxW = 10.0;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Mw) && (std::from_chars(Mw.data(), Mw.data() + Mw.size(), MaxW).ec != std::errc() || MaxW < 1.0)) return "SuggestPoseAccept: -MaxWindow is seconds (at least 1)";
            S.m_RefineMaxFrames = int(std::lround(std::min(MaxW, 10.0) * 30.0));
            return S.SuggestPoseAccept(iL, iC, bRefine);
        }
        case kind::layers_info: return S.LayersInfo();
        case kind::kimodo_info:
        {
            auto* pDesc = S.Desc();
            std::string Text = std::format("running: {}\npreview: {}\n", S.m_K.m_Job.m_bRunning ? "yes" : "no", S.m_K.m_bPreview ? "yes" : "no");
            if (S.m_K.m_bPreview)
            {
                Text += std::format("preview frames: {}  prompt: \"{}\"  seed: {}  name: {}\n", S.m_K.m_Preview.m_nFrames, S.m_K.m_PromptUsed, S.m_K.m_SeedUsed, S.m_K.m_Name);
                Text += S.m_K.m_Baked.empty() ? std::format("preview on: Kimodo's skeleton ({})\n", S.m_K.m_RetargetError)
                                              : std::format("preview on: {}\n", S.m_K.m_bShowSource ? "Kimodo's skeleton (Show Kimodo's skeleton is on)" : "this package's skeleton (retargeted)");
            }
            Text += std::format("status: {}{}\n", S.m_K.m_bStatusError ? "ERROR " : "", S.m_K.m_Status);
            if (auto* pWhy = S.KimodoWhyNot()) Text += std::format("not ready: {}\n", pWhy);
            if (pDesc) Text += std::format("retarget: {:016X}  folder: {}\n", pDesc->m_Kimodo.m_Retarget.m_Instance.m_Value, xstrtool::To(pDesc->m_Kimodo.m_Folder));
            const auto Exe = kimodo::FindExe();
            Text += std::format("xkimodo: {}\n", Exe.empty() ? std::string("NOT FOUND") : xstrtool::To(Exe));
            return Text;
        }
        }
        return {};
    }

    inline const char* anim_actions::WhyNoPlayPause() const noexcept { return m_pS->SelectedClip() ? nullptr : "select a clip first"; }
    inline void anim_actions::PlayPause() noexcept { m_pS->TogglePlay(); }

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::anim_package_type_guid_v
    , [](xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XANIM_PACKAGE_EDITOR_H
