#ifndef XANIM_PACKAGE_EDITOR_H
#define XANIM_PACKAGE_EDITOR_H
#pragma once

// The Anim Package editor: opens an animation package from the asset browser in its own window. The clips the compiler found in the import
// files are listed with their options (name, delete, loop, down sample, trim, root motion), edited in the table or through commands, all
// undoable. A clip plays on the skeleton it is bound to, in a 3D view, with a transport bar and a timeline. Hosts include this header and open
// editors through xeditor::open_resource_editors.
#include "source/Tools/Editor/xeditor_descriptor_editor.h"
#include "source/Tools/Editor/xeditor_camera_toolbar.h"
#include "source/Tools/Editor/xeditor_frame_recorder.h"      // Render video: the preview, frame by frame, to a video
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_inspector_pickers.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_resources.h"
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_viewport.h"
#include "plugins/xgeom_skin.plugin/source/Editor/xgeom_skin_editor_under.h"     // the skin under the bones (shared with the Ragdoll editor)
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_runtime.h"
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
#include "source/Tools/Editor/xeditor_timeline.h"
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_posing_handles.h"
#include "dependencies/ImGuizmo/src/ImGuizmo.h"

#include <charconv>
#include <functional>
#include <map>
#include <unordered_map>

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
        enum class kind { select_clip, play, pause, seek, set_speed, info, list_clips, kimodo_generate, kimodo_accept, kimodo_discard, kimodo_info, layer_add, layer_mask, layers_info, pose_key, suggest_pose, suggest_pose_accept, layer_timeline, layer_timeline_edit, layer_parent
                        , pose_tool, pose_drag, pose_pin, pose_mirror, pose_copy, pose_paste, pose_suggest, kimodo_layer_add, kimodo_regenerate, kimodo_accept_candidate, kimodo_show, bake_to_pose, kimodo_takes_to_layers, kimodo_take_check, pose_rotate, new_clip, ik_key, key_selected, auto_key, clip_event, detect_footsteps, ball_flight, render_video };

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
                m_hD = m_Parser.addOption("Path",    "\"vx vz\" metres per second: the root goes straight at this velocity (+Z ahead, +X to the left) while facing ahead the whole take - strafes, a backward walk (Kimodo's root path + heading constraints)", false, 1);
                break;
            case kind::kimodo_accept: m_hA = m_Parser.addOption("Name", "The clip's file name (default: from the prompt)", false, 1); break;
            case kind::layer_add:
                m_hA = m_Parser.addOption("Source", "The import source's key (its path, as ListClips shows it); not for -Kind Pose", false, 1);
                m_hB = m_Parser.addOption("Clip",   "The clip there (its original name); not for -Kind Pose", false, 1);
                m_hC = m_Parser.addOption("Name",   "The layer's name", false, 1);
                m_hD = m_Parser.addOption("Kind",   "Clip (default), Pose, Folder (organizes: pass-through) or Group (isolated: its children mix, then it blends with its weight and mask)", false, 1);
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
            case kind::layer_timeline:
                m_hA = m_Parser.addOption("Time",   "Seconds: the playhead (a seek: the descriptor is not touched)", false, 1);
                m_hB = m_Parser.addOption("Solo",   "\"row 0|1\": audition a layer row (editor only, never in the build)", false, 1);
                m_hC = m_Parser.addOption("Fold",   "A row to fold (view state)", false, 1);
                m_hD = m_Parser.addOption("Unfold", "A row to unfold (view state)", false, 1);
                m_hE = m_Parser.addOption("Frame",  "\"a b\" seconds: the view shows that span", false, 1);
                break;
            case kind::layer_timeline_edit:
                m_hA = m_Parser.addOption("Ops", "A timeline plan (\"rev N; seg row item start end offset ein eout; key row item tick\", ticks: 6000 a second) or row ops \"mute row 0|1; lock row 0|1; move row above_row; delete row item\"", true, 1);
                break;
            case kind::layer_mask:
                m_hA = m_Parser.addOption("Layer", "The layer's index in the selected clip", true, 1);
                m_hB = m_Parser.addOption("Bone",  "The bone (its name): added with its branch, or removed when it is in the mask", false, 1);
                m_hC = m_Parser.addOption("Only",  "1: this bone only (not its branch)", false, 1);
                m_hD = m_Parser.addOption("Shared", "A skeleton mask group by name: the layer uses it instead of its own entries (\"\" = back to its own)", false, 1);
                break;
            case kind::layer_parent:
                m_hA = m_Parser.addOption("Layer",  "The layer's index in the selected clip", true, 1);
                m_hB = m_Parser.addOption("Parent", "The folder's or group's index; -1 = the top level", true, 1);
                break;
            case kind::pose_tool:
                m_hA = m_Parser.addOption("On",   "1: the Pose tool on (handles in the viewport), 0: off", false, 1);
                m_hB = m_Parser.addOption("Bone", "Select a bone for FK (its tip handle turns it)", false, 1);
                break;
            case kind::pose_drag:
                m_hA = m_Parser.addOption("Effector", "LeftHand, RightHand, LeftFoot, RightFoot, Head, Hips or a bone name", true, 1);
                m_hB = m_Parser.addOption("Move",     "\"dx dy dz\" centimetres in the model frame", true, 1);
                m_hC = m_Parser.addOption("Pins",     "auto (default: the feet in contact at that time + the pins set), none, or handle names", false, 1);
                m_hD = m_Parser.addOption("Only",     "1: FK - only this bone turns (its tip goes toward the move)", false, 1);
                m_hE = m_Parser.addOption("Time",     "Seconds (default: the playhead)", false, 1);
                m_hF = m_Parser.addOption("Commit",   "0: preview only (nothing written); default 1: keys on the top Pose layer, one undo step", false, 1);
                break;
            case kind::pose_pin:
                m_hA = m_Parser.addOption("Handle", "A handle to pin or unpin (toggle)", false, 1);
                m_hB = m_Parser.addOption("Preset", "Feet, Hands, Both, None, or Auto (feet in contact pinned automatically on/off: \"Auto 1|0\")", false, 1);
                break;
            case kind::pose_mirror: case kind::pose_copy: case kind::pose_paste:
                m_hA = m_Parser.addOption("Time", "Seconds (default: the playhead)", false, 1);
                break;
            case kind::pose_suggest:
                m_hA = m_Parser.addOption("Seeds",  "\"1 2 3\": one candidate each (default)", false, 1);
                m_hB = m_Parser.addOption("Prompt", "What the pose is, in words", false, 1);
                m_hC = m_Parser.addOption("Accept", "A candidate's index: written as keys (one undo step, with Refine Motion)", false, 1);
                m_hD = m_Parser.addOption("Show",   "A candidate's index to preview (Tab in the viewport), or -1: none (Esc)", false, 1);
                break;
            case kind::kimodo_layer_add:
                m_hA = m_Parser.addOption("Prompt",  "What the motion is, in words", true, 1);
                m_hB = m_Parser.addOption("Seconds", "Its length (0.5 .. 10: one take; default 2)", false, 1);
                m_hC = m_Parser.addOption("Start",   "Where it starts in the clip, seconds (default: the playhead)", false, 1);
                m_hD = m_Parser.addOption("Seed",    "Seed (default 1)", false, 1);
                m_hE = m_Parser.addOption("Name",    "The layer's name", false, 1);
                break;
            case kind::kimodo_regenerate:
                m_hA = m_Parser.addOption("Layer", "The Kimodo layer's index in the selected clip", true, 1);
                m_hB = m_Parser.addOption("Seeds", "\"1 2 3\": one candidate per seed (alternatives); default: the layer's seed", false, 1);
                m_hC = m_Parser.addOption("Wait",  "1: wait for the background run to end (scripts)", false, 1);
                break;
            case kind::kimodo_accept_candidate: m_hA = m_Parser.addOption("Candidate", "Which candidate (default 0)", false, 1); break;
            case kind::kimodo_show:             m_hA = m_Parser.addOption("Candidate", "A candidate to preview at the playhead, -1: none", true, 1); break;
            case kind::bake_to_pose:
                m_hA = m_Parser.addOption("Layer", "The Kimodo layer's index", true, 1);
                m_hB = m_Parser.addOption("Every", "A key every N frames (default 1: every frame)", false, 1);
                break;
            case kind::new_clip:
                m_hA = m_Parser.addOption("Name",     "The new animation's name", true, 1);
                m_hB = m_Parser.addOption("Seconds",  "Its length (default 2)", false, 1);
                m_hC = m_Parser.addOption("FPS",      "Its frame rate (default 30)", false, 1);
                m_hD = m_Parser.addOption("FromPose", "rest (default) or playhead: start from the pose at the playhead (keys at 0)", false, 1);
                break;
            case kind::ik_key:
                m_hA = m_Parser.addOption("Effector", "LeftHand, RightHand, LeftFoot, RightFoot, Head, Hips or a bone name", true, 1);
                m_hB = m_Parser.addOption("Time",     "Seconds (default: the playhead)", false, 1);
                m_hC = m_Parser.addOption("Pos",      "\"x y z\" model-space target of the handle (default: where it is now)", false, 1);
                m_hD = m_Parser.addOption("Pin",      "1: pinned (held until the next key), 0: not (default: the handle's pin state)", false, 1);
                m_hE = m_Parser.addOption("Rot",      "1: key its rotation too (default 0)", false, 1);
                m_hF = m_Parser.addOption("Delete",   "1: delete the effector's IK key at that time", false, 1);
                m_hG = m_Parser.addOption("Probe",    "1: only report where the handle is at that time (model space, the composed animation), no key", false, 1);
                m_hH = m_Parser.addOption("Prop",     "A preview prop's name (the description's PreviewProps): the target is that prop's centre at that time (a foot on the ball)", false, 1);
                break;
            case kind::key_selected:
                m_hA = m_Parser.addOption("All",  "1: every bone the layer owns (Shift+K), else the selected bone or handle (K)", false, 1);
                m_hB = m_Parser.addOption("Time", "Seconds (default: the playhead)", false, 1);
                break;
            case kind::auto_key:
                m_hA = m_Parser.addOption("On", "1: edits write keys (default), 0: edits are a pending pose that K keys", true, 1);
                break;
            case kind::pose_rotate:
                m_hA = m_Parser.addOption("Bone", "The bone (its name)", true, 1);
                m_hB = m_Parser.addOption("Turn", "\"x y z\" degrees about the model's axes, applied to the bone's current world rotation", true, 1);
                m_hC = m_Parser.addOption("Time", "Seconds (default: the playhead)", false, 1);
                break;
            case kind::kimodo_takes_to_layers:  m_hA = m_Parser.addOption("Layer", "The Pose layer whose stored takes become Kimodo layers", true, 1); break;
            case kind::render_video:
                m_hA = m_Parser.addOption("Fps",     "Frames per second of the video (default 30)", false, 1);
                m_hB = m_Parser.addOption("Seconds", "How long (default: the clip's length)", false, 1);
                m_hC = m_Parser.addOption("Cancel",  "1: stop a recording", false, 1);
                break;
            case kind::ball_flight:
                m_hA = m_Parser.addOption("Effector", "What kicks it: RightFoot (default), LeftFoot, a hand or a bone", false, 1);
                m_hB = m_Parser.addOption("Time",     "Seconds: the contact (default: the playhead)", false, 1);
                m_hC = m_Parser.addOption("Prop",     "The ball: a preview prop's name (default Ball)", false, 1);
                m_hD = m_Parser.addOption("Off",      "1: hide the flight", false, 1);
                break;
            case kind::clip_event:
                m_hA = m_Parser.addOption("Add",    "A new event's name (at -Time, default the playhead)", false, 1);
                m_hB = m_Parser.addOption("Id",     "The event to change (its id, hex, as the list shows)", false, 1);
                m_hC = m_Parser.addOption("Delete", "The event to delete (its id)", false, 1);
                m_hD = m_Parser.addOption("Kind",   "Marker (default), Footstep, Contact (an interval) or Sync", false, 1);
                m_hE = m_Parser.addOption("Time",   "Seconds: where it is (a contact: where it starts)", false, 1);
                m_hF = m_Parser.addOption("End",    "Seconds: an interval's end (-1: a point)", false, 1);
                m_hG = m_Parser.addOption("Bone",   "The bone it is about (a foot, a hand); with -Id, \"-\" clears it", false, 1);
                m_hH = m_Parser.addOption("Name",   "With -Id: its new name", false, 1);
                break;            default: break;
            }
        }
        std::string Query() noexcept override;

        xcmdline::parser::handle m_hA, m_hB, m_hC, m_hD, m_hE, m_hF, m_hG, m_hH;
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
        playback_cmd                                    m_LayerAdd, m_LayerMask, m_LayersInfo, m_PoseKey, m_SuggestPose, m_SuggestPoseAccept, m_LayerTimeline, m_LayerTimelineEdit, m_LayerParent;
        playback_cmd                                    m_PoseTool, m_PoseDrag, m_PosePin, m_PoseMirror, m_PoseCopy, m_PosePaste, m_PoseSuggest;
        playback_cmd                                    m_KimodoLayerAdd, m_KimodoRegenerate, m_KimodoAcceptCandidate, m_KimodoShow, m_BakeToPose, m_KimodoTakesToLayers, m_KimodoTakeCheck, m_PoseRotate, m_NewClip, m_IkKeyCmd, m_KeySelected, m_AutoKey, m_ClipEvent, m_DetectFootsteps, m_BallFlight, m_RenderVideoCmd;
        // the last accepted candidate (its frames on this skeleton): KimodoTakeCheck compares the compiled take with it
        std::uint64_t m_KAcceptedLayer = 0; std::vector<xmath::transform3> m_KAcceptedFrames; int m_KAcceptedW = 0;

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
        xgeom_skin_editor::skin_under                   m_SkinUnder;            // the descriptor's PreviewSkin drawn under the bones (Skin button)
        bool                                            m_bShowSkin = true;
        bool                                            m_bFollow = true;       // the camera's target moves with the character (a walk stays in view)
        bool                                            m_bFollowHas = false; xmath::fvec3 m_FollowLast;
        xrsc::geom_skin                                 m_SkinRef;              // (our reference to the PreviewSkin: released on close or when it changes)
        xrsc::skeleton                                  m_SkinSkelRef;          // (the skin's own skeleton: its bones follow ours by name)
        std::vector<xmath::fmat4>                       m_SkinWorlds;
        xgpu::device*                                   m_pDevice = nullptr;
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
            , m_LayerMask     (m_Undo, *this, playback_cmd::kind::layer_mask,      "LayerMask",      "Adds a bone (with its branch) to a layer's mask, or removes it when it is there; or uses a skeleton mask group. Usage: LayerMask -Layer n -Bone name [-Only 1] | LayerMask -Layer n -Shared name")
            , m_LayerParent   (m_Undo, *this, playback_cmd::kind::layer_parent,    "LayerParent",    "Puts a layer into a folder or group (or back to the top level with -1): one undo step. Usage: LayerParent -Layer n -Parent m|-1")
            , m_LayersInfo    (m_Undo, *this, playback_cmd::kind::layers_info,     "LayersInfo",     "The selected clip's layers, the live composition's isolation report and its parity with the compiled clip. Usage: LayersInfo")
            , m_PoseKey       (m_Undo, *this, playback_cmd::kind::pose_key,        "PoseKey",        "Sets a key on a Pose layer: the bone's pose at a time = the layers below turned by -Turn (absolute target), or the turn as an offset key. Usage: PoseKey -Layer n -Bone name -Time s [-Turn \"x y z\"] [-Offset 1] [-Move \"x y z\"]")
            , m_SuggestPose   (m_Undo, *this, playback_cmd::kind::suggest_pose,    "SuggestPose",    "Suggests a natural pose at a time with Kimodo for the handles moved and pinned: a CANDIDATE (nothing is written) and its report. Usage: SuggestPose -Time s [-Move \"LeftHand 0 0 15\"] [-Pin \"LeftFoot RightFoot\"] [-Seeds \"1 2\"] [-Prompt text]")
            , m_SuggestPoseAccept(m_Undo, *this, playback_cmd::kind::suggest_pose_accept, "SuggestPoseAccept", "Writes a Suggest Pose candidate as keys on a Pose layer (the bones it changes). Usage: SuggestPoseAccept -Layer n [-Candidate i]")
            , m_LayerTimeline    (m_Undo, *this, playback_cmd::kind::layer_timeline,      "LayerTimeline",     "The layer timeline: its rows (top layer first, the clip at the bottom), their items, the revision, the playhead, solo, the selection and the layout. Usage: LayerTimeline [-Time s] [-Solo \"row 0|1\"] [-Fold row] [-Unfold row] [-Frame \"a b\"]")
            , m_LayerTimelineEdit(m_Undo, *this, playback_cmd::kind::layer_timeline_edit, "LayerTimelineEdit", "Edits the selected clip's layers as the timeline does (ONE undo step): a plan's text or row ops. Usage: LayerTimelineEdit -Ops \"rev N; seg ...; key ...\" | \"mute r 1; lock r 0; move r above; delete r i\"")
            , m_PoseTool   (m_Undo, *this, playback_cmd::kind::pose_tool,   "PoseTool",   "The Pose tool (posing handles in the viewport): on/off, a bone selected for FK; reports the handles (screen positions), pins, the last drag and Suggest candidates. Usage: PoseTool [-On 1|0] [-Bone name]")
            , m_PoseDrag   (m_Undo, *this, playback_cmd::kind::pose_drag,   "PoseDrag",   "Drags a posing handle: full-body IK (feet in contact pinned, the pins held, the skeleton's limits) from the pose at that time; the result is keys on the top Pose layer (made when missing), one undo step. Usage: PoseDrag -Effector LeftHand -Move \"dx dy dz\" [-Pins auto|none|names] [-Only 1] [-Time s] [-Commit 0]")
            , m_PosePin    (m_Undo, *this, playback_cmd::kind::pose_pin,    "PosePin",    "Pins or unpins a posing handle, or a preset. Usage: PosePin -Handle LeftHand | -Preset Feet|Hands|Both|None|\"Auto 1|0\"")
            , m_PoseMirror (m_Undo, *this, playback_cmd::kind::pose_mirror, "PoseMirror", "The pose at a time mirrored left/right, as keys (one undo step). Usage: PoseMirror [-Time s]")
            , m_PoseCopy   (m_Undo, *this, playback_cmd::kind::pose_copy,   "PoseCopy",   "Copies the pose at a time (editor clipboard). Usage: PoseCopy [-Time s]")
            , m_PosePaste  (m_Undo, *this, playback_cmd::kind::pose_paste,  "PosePaste",  "Pastes the copied pose at a time, as keys (one undo step). Usage: PosePaste [-Time s]")
            , m_PoseSuggest(m_Undo, *this, playback_cmd::kind::pose_suggest,"PoseSuggest","Suggest Pose for the current handles (the pins and the last dragged handle held) at the playhead: candidates; -Show previews one; -Accept writes it (one step, Refine Motion). Usage: PoseSuggest [-Seeds \"1 2 3\"] [-Prompt text] | -Show i | -Accept i")
            , m_KimodoLayerAdd       (m_Undo, *this, playback_cmd::kind::kimodo_layer_add,        "KimodoLayerAdd",       "Adds a Kimodo layer (a generated take as a layer: no take yet, it plays nothing until a candidate is accepted). Usage: KimodoLayerAdd -Prompt text [-Seconds 2] [-Start s] [-Seed 1] [-Name text]")
            , m_KimodoRegenerate     (m_Undo, *this, playback_cmd::kind::kimodo_regenerate,       "KimodoRegenerate",     "Generates the Kimodo layer's take from its request and the stack below (stated constraints): CANDIDATES (nothing written); several seeds = alternatives. Usage: KimodoRegenerate -Layer n [-Seeds \"1 2 3\"]")
            , m_KimodoAcceptCandidate(m_Undo, *this, playback_cmd::kind::kimodo_accept_candidate, "KimodoAcceptCandidate","Makes a candidate the layer's take (one undo step; the earlier take stays on disk), then saves and compiles. Usage: KimodoAcceptCandidate [-Candidate i]")
            , m_KimodoShow           (m_Undo, *this, playback_cmd::kind::kimodo_show,             "KimodoShow",           "Previews a Kimodo candidate at the playhead (-1: none). Usage: KimodoShow -Candidate i")
            , m_BakeToPose           (m_Undo, *this, playback_cmd::kind::bake_to_pose,            "BakeToPose",           "Turns a Kimodo layer into an editable Pose layer (keys of what the clip plays over its span); the Kimodo layer goes off, its take is kept; one undo step. Usage: BakeToPose -Layer n [-Every 1]")
            , m_KimodoTakesToLayers  (m_Undo, *this, playback_cmd::kind::kimodo_takes_to_layers,  "KimodoTakesToLayers",  "A Pose layer's stored takes (from the refine) as Kimodo layers with their requests, off (idempotent). Usage: KimodoTakesToLayers -Layer n")
            , m_PoseRotate           (m_Undo, *this, playback_cmd::kind::pose_rotate,             "PoseRotate",           "Turns one bone (what the rotate gizmo does) and writes its key on the top Pose layer, one undo step. Usage: PoseRotate -Bone name -Turn \"x y z\" [-Time s]")
            , m_NewClip              (m_Undo, *this, playback_cmd::kind::new_clip,                "NewClip",              "A new animation from nothing: the skeleton's rest pose (or the pose at the playhead) held for Seconds, with a Pose layer ready for keys; one undo step, then saved and compiled. Usage: NewClip -Name n [-Seconds 2] [-FPS 30] [-FromPose rest|playhead]")
            , m_IkKeyCmd             (m_Undo, *this, playback_cmd::kind::ik_key,                  "IkKey",                "An IK key on the top Pose layer: an effector's target (the handle: palm, sole) at a time, pinned or not; the body is solved on every frame between keys. One undo step. Usage: IkKey -Effector LeftHand [-Time s] [-Pos \"x y z\"] [-Pin 1|0] [-Rot 1] [-Delete 1]")
            , m_KeySelected          (m_Undo, *this, playback_cmd::kind::key_selected,            "KeySelected",          "K: keys the selected bone (or the last dragged handle as an IK key, or the pending pose when auto-key is off) at the playhead; -All 1 (Shift+K): every bone the layer owns. One undo step. Usage: KeySelected [-All 1] [-Time s]")
            , m_AutoKey              (m_Undo, *this, playback_cmd::kind::auto_key,                "AutoKey",              "Auto-key on (edits write keys) or off (an edit is a pending pose, shown, that K keys). Usage: AutoKey -On 1|0")
            , m_ClipEvent            (m_Undo, *this, playback_cmd::kind::clip_event,              "ClipEvent",            "The selected clip's events (T5: markers, footsteps, contacts, sync points; one undo step each). Usage: ClipEvent (list) | -Add name [-Kind k] [-Time s] [-End s] [-Bone b] | -Id id [-Time s] [-End s] [-Kind k] [-Name n] [-Bone b] | -Delete id")
            , m_RenderVideoCmd       (m_Undo, *this, playback_cmd::kind::render_video,            "RenderVideo",          "The selected clip as a video, as the preview shows it (skin, props, camera): one frame per editor frame, then ffmpeg encodes Cache\\Temp\\AnimVideo\\<clip>.mp4 (the frames stay in Cache\\Temp\\AnimVideo\\<clip>). Usage: RenderVideo [-Fps 30] [-Seconds s] [-Cancel 1]")
            , m_BallFlight           (m_Undo, *this, playback_cmd::kind::ball_flight,             "BallFlight",           "A kick's check: the ball (a preview prop) leaves with the kicking handle's speed at the contact (x1.1: a typical instep kick's transfer), its flight drawn for 2 s or to the ground (no air drag); reports speed, launch angle, height, distance. Usage: BallFlight [-Effector RightFoot] [-Time s] [-Prop Ball] [-Off 1]")
            , m_DetectFootsteps      (m_Undo, *this, playback_cmd::kind::detect_footsteps,        "DetectFootsteps",      "Finds each foot's support contacts in the clip as composed: a Footstep where it lands and a Contact bar over the support (replaces only what it detected before; one undo step). Usage: DetectFootsteps")
            , m_KimodoTakeCheck      (m_Undo, *this, playback_cmd::kind::kimodo_take_check,       "KimodoTakeCheck",      "The compiled take of the last accepted Kimodo candidate against the candidate itself (every frame, every bone). Usage: KimodoTakeCheck")
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
            if (pDevice) m_CameraToolbar.Init(m_Document.m_Guid.m_Instance.m_Value, 390.0f, 230.0f);     // (with Skin, Follow and Video)
            AddPanel("AnimPackage Viewport", dock::center, [this] { RenderViewport(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            AddPanel("Timeline",             dock::bottom_wide, [this] { RenderPlayback(); });       // under the viewport and the side columns
            m_TLView.m_HeaderWidth = 380;                                                               // name + weight + mode + mask + eye/solo/lock
            AddPanel("Description",          dock::left,   [this] { m_DescriptorInspector.Show(); });

            m_Viewport.m_pOpen = &m_bOpen;
            m_bReady = pDevice && m_Viewport.Init(*pDevice);
            m_pDevice = pDevice;
            Reload();
        }

        ~session() noexcept override
        {
            xresource::g_Mgr.ReleaseRef(m_Ref);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            xresource::g_Mgr.ReleaseRef(m_K.m_SkeletonRef);
            xresource::g_Mgr.ReleaseRef(m_SkinRef);
            xresource::g_Mgr.ReleaseRef(m_SkinSkelRef);
            m_SkinUnder.Release();
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
        void OnCompiled()           noexcept override
        {
            // the skeleton may have changed since the descriptor was read (its mask groups: shared masks, contract v3): the live preview
            // must read what the compile read
            if (auto* pDesc = Desc()) { xskeleton_desc::bone_manifest Keep = pDesc->m_ResolvedSkeletonBones; pDesc->m_ResolvedSkeletonBones = {}; if (pDesc->LoadSkeletonManifest(m_Document.m_DescriptorPath)) pDesc->m_ResolvedSkeletonBones = std::move(Keep); }
            MergeDetails(); Reload();
        }

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
                S += std::format("{}|{}|{}|{}|{}|r{}|", int(L.m_Space), int(L.m_Kind), L.m_Id, L.m_Parent, L.m_SharedMask, L.m_Row);
                for (const auto& K : L.m_Keys) S += std::format("p{},{},{},{},{},{},{},{},{},{},{},{},{};", K.m_Tick, K.m_Bone, K.m_X, K.m_Y, K.m_Z, K.m_W, K.m_bOffset, K.m_bStep, K.m_bPosition, K.m_PX, K.m_PY, K.m_PZ, K.m_bLinear);
                S += std::format("s{},{},{}|", L.m_bSmooth, L.m_Falloff, L.m_bBoneLimits);
                for (const auto& K : L.m_IkKeys) S += std::format("i{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{};", K.m_Tick, K.m_Effector, K.m_PX, K.m_PY, K.m_PZ, K.m_bRotation, K.m_X, K.m_Y, K.m_Z, K.m_W, K.m_bPinned, K.m_bStep, K.m_bLinear, K.m_AX, K.m_AY, K.m_AZ);
                S += std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|", L.m_bEnabled, int(L.m_Mode), int(L.m_Reference), L.m_ReferenceTick, xstrtool::To(L.m_SourceKey), L.m_SourceClip
                    , L.m_Start, L.m_End, L.m_TrimIn, L.m_TrimOut, L.m_Rate, L.m_bReverse, L.m_Weight, L.m_bRotation, L.m_bPosition, int(L.m_Root), L.m_Name);
                for (const auto& K : L.m_WeightKeys) S += std::format("k{},{},{};", K.m_Tick, K.m_Value, K.m_bStep);
                for (const auto& E : L.m_Mask) S += std::format("m{},{},{},{};", E.m_Bone, E.m_Value, E.m_bInherit, E.m_BlendDepth);
            }
            return S;
        }

        //----------------------------------------------------------------------------------------
        // The layer timeline (T2, xLION_Docs editor_timeline_framework_design): the selected clip's layer stack as rows of the editor timeline
        // (source/Tools/Editor/xeditor_timeline.h): the top layer on top, the base clip at the bottom - the mix order, as Photoshop shows it.
        // A view over the descriptor: every edit is a command (LayerTimelineEdit: a plan's text or row ops) and ONE undo step; scrubbing,
        // folding, solo and selection never touch the descriptor.
        //----------------------------------------------------------------------------------------
        static constexpr std::uint64_t tl_base_row_v = 0xB000000000000001ull;
        static constexpr std::uint64_t tl_events_row_v = 0xB000000000000002ull;      // the clip's events (T5): markers, footsteps, contact bars
        enum tl_type : std::uint64_t { tl_base = 1, tl_clip = 2, tl_pose = 3, tl_bone = 4, tl_folder = 5, tl_group = 6, tl_weight = 7, tl_ik = 8, tl_events = 9 };     // tl_ik: an effector's IK keys (keyframe batch)     // tl_weight: a layer's weight curve lane

        // contract v3: a layer's effective parent (its folder or group in this clip; 0 = the top level). A parent that names nothing, or
        // loops, is the top level - the same rule as the composition (xanim_package_layers::Compose)
        static std::uint64_t TLParentOf(const xanim_package_desc::clip& C, int i) noexcept
        {
            using kind = xanim_package_desc::layer_kind;
            auto Container = [&](std::uint64_t Id) -> const xanim_package_desc::clip_layer* { if (!Id) return nullptr; for (const auto& L : C.m_Layers) if (L.m_Id == Id && (L.m_Kind == kind::FOLDER || L.m_Kind == kind::GROUP)) return &L; return nullptr; };
            const auto* pP = Container(C.m_Layers[i].m_Parent);
            int Steps = 0;
            for (const auto* pA = pP; pA; pA = Container(pA->m_Parent)) if (++Steps > int(C.m_Layers.size()) || pA == &C.m_Layers[i]) return 0;
            return pP ? pP->m_Id : 0;
        }
        // Id is Of or inside it (any depth)
        static bool TLIsWithin(const xanim_package_desc::clip& C, std::uint64_t Id, std::uint64_t Of) noexcept
        {
            for (int Steps = 0; Id && Steps <= int(C.m_Layers.size()); ++Steps)
            {
                if (Id == Of) return true;
                int i = -1; for (int j = 0; j < int(C.m_Layers.size()); ++j) if (C.m_Layers[j].m_Id == Id) { i = j; break; }
                if (i < 0) return false;
                Id = TLParentOf(C, i);
            }
            return false;
        }
        struct tl_row { int m_iLayer = -1; std::string m_Bone; bool m_bMerged = false; bool m_bWeight = false; bool m_bIk = false; };    // m_bIk: m_Bone is the effector of an IK row    // merged: Clip/Kimodo layers sharing a Row (T4b); m_iLayer = the top one, the items name their layer
        static std::uint64_t TLMergedRow(std::uint64_t Parent, std::uint64_t Row) noexcept { return ((Row * 0x9E3779B97F4A7C15ull) ^ Parent) & 0x3FFFFFFFFFFFFFFFull | 0x8000000000000000ull; }
        // the layers of a timeline row: one, or every member of a merged row
        std::vector<int> TLMembers(std::uint64_t RowId) noexcept
        {
            std::vector<int> M; auto* pOv = SelectedOv(); const auto It = m_TLRows.find(RowId);
            if (!pOv || It == m_TLRows.end()) return M;
            if (!It->second.m_bMerged) { M.push_back(It->second.m_iLayer); return M; }
            const auto& H = pOv->m_Layers[It->second.m_iLayer];
            for (int i = 0; i < int(pOv->m_Layers.size()); ++i)
                if (pOv->m_Layers[i].m_Row == H.m_Row && TLParentOf(*pOv, i) == TLParentOf(*pOv, It->second.m_iLayer)
                    && (pOv->m_Layers[i].m_Kind == xanim_package_desc::layer_kind::CLIP || pOv->m_Layers[i].m_Kind == xanim_package_desc::layer_kind::KIMODO)) M.push_back(i);
            return M;
        }
        std::unordered_map<std::uint64_t, tl_row>       m_TLRows;               // rebuilt with the rows: row id -> layer (and bone)
        std::set<std::uint64_t>                         m_TLSeen;               // pose rows seen (a new one starts folded)
        xeditor::timeline::state                        m_TLView;
        xeditor::timeline::transport                    m_TLTransport;          // m_Solo: layer row ids auditioned (editor only, never in the build)
        static constexpr xeditor::timeline::tick tl_tps_v = 6000;               // the layers' ticks

        static std::uint64_t TLLayerRow(const xanim_package_desc::clip_layer& L, int i) noexcept { return L.m_Id ? L.m_Id : 0xA000000000000000ull + std::uint64_t(i); }
        static std::uint64_t TLBoneRow(std::uint64_t Layer, const std::string& Bone) noexcept { return ((std::hash<std::string>{}(Bone) ^ (Layer * 0x9E3779B97F4A7C15ull)) & 0x3FFFFFFFFFFFFFFFull) | 0x4000000000000000ull; }

        // the selected clip's source length in ticks (its composition span), else the compiled clip's
        xeditor::timeline::tick TLClipTicks() noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource();
            if (pOv && pS) if (const auto* pB = FindLayerSource(pS->key(), pOv->m_OriginalName); pB && pB->m_FPS > 0) return xeditor::timeline::tick(std::llround(double(std::max(1, pB->m_nFrames - 1)) / pB->m_FPS * tl_tps_v));
            if (auto* pC = SelectedClip()) return xeditor::timeline::tick(std::llround(double(ClipLength(*pC)) * tl_tps_v));
            return 4 * tl_tps_v;
        }
        // names the content the plans are made against (an undo restores the old number)
        std::uint64_t TLRevision() noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return 0;
            std::string S = std::format("{}:{}:{}", m_iSelectedSource, m_iSelectedDescriptorClip, LayerSignature(*pOv));
            for (const auto& L : pOv->m_Layers) S += L.m_bLock ? 'L' : 'u';
            for (const auto& E : pOv->m_Events) S += std::format("|{}:{}:{}", E.m_Id, E.m_Tick, E.m_EndTick);
            return std::hash<std::string>{}(S) | 1;
        }

        void TLRowsOf(std::vector<xeditor::timeline::row_desc>& Out) noexcept
        {
            m_TLRows.clear();
            auto* pOv = SelectedOv();
            if (!pOv) return;
            // the tree (contract v3): top first in mix order; a folder's or group's children indented under it, top first too
            using kind = xanim_package_desc::layer_kind;
            std::set<std::uint64_t> MergedDone;
            std::function<void(std::uint64_t, int, bool, bool)> Emit = [&](std::uint64_t Parent, int Depth, bool bMuteUp, bool bLockUp)
            {
                for (int i = int(pOv->m_Layers.size()) - 1; i >= 0; --i)
                {
                    const auto& L = pOv->m_Layers[i];
                    if (TLParentOf(*pOv, i) != Parent) continue;
                    const bool bPose = L.m_Kind == kind::POSE, bFolder = L.m_Kind == kind::FOLDER, bGroup = L.m_Kind == kind::GROUP;
                    const bool bMerged = L.m_Row && (L.m_Kind == kind::CLIP || L.m_Kind == kind::KIMODO);
                    if (bMerged && !MergedDone.insert(TLMergedRow(Parent, L.m_Row)).second) continue;     // its row is drawn already (by its top member)
                    xeditor::timeline::row_desc R;
                    R.m_Id = bMerged ? TLMergedRow(Parent, L.m_Row) : TLLayerRow(L, i); R.m_Type = bPose ? tl_pose : bFolder ? tl_folder : bGroup ? tl_group : tl_clip;
                    R.m_Depth = Depth;
                    R.m_Name = L.m_Name.empty() ? "(unnamed)" : L.m_Name;     // weight, mode and mask are the row's header widgets
                    R.m_bMute = !L.m_bEnabled || bMuteUp; R.m_bLock = L.m_bLock || bLockUp;
                    R.m_Color = bPose ? IM_COL32(210, 150, 70, 255) : bFolder ? IM_COL32(150, 150, 110, 255) : bGroup ? IM_COL32(140, 110, 190, 255) : L.m_Kind == kind::KIMODO ? IM_COL32(70, 170, 160, 255) : IM_COL32(86, 132, 210, 255);
                    std::vector<std::string> Bones;
                    if (bPose) { for (const auto& K : L.m_Keys) if (std::find(Bones.begin(), Bones.end(), K.m_Bone) == Bones.end()) Bones.push_back(K.m_Bone); std::sort(Bones.begin(), Bones.end()); }
                    std::vector<std::string> Effs;
                    if (bPose) { for (const auto& K : L.m_IkKeys) if (std::find(Effs.begin(), Effs.end(), K.m_Effector) == Effs.end()) Effs.push_back(K.m_Effector); std::sort(Effs.begin(), Effs.end()); }
                    const bool bContainer = bFolder || bGroup;
                    bool bKids = !Bones.empty() || !Effs.empty();
                    if (bContainer) for (int j = 0; j < int(pOv->m_Layers.size()) && !bKids; ++j) bKids = j != i && TLParentOf(*pOv, j) == L.m_Id && L.m_Id;
                    R.m_bHasChildren = bKids;
                    const bool bLane = !bContainer && !bMerged;          // a layer's weight curve lane (its first child row)
                    if (bLane) { R.m_bHasChildren = true; if (m_TLSeen.insert(R.m_Id).second) m_TLView.m_Folded.insert(R.m_Id); }     // folded at first: the summary row
                    if (bMerged) R.m_Name = std::format("{} (row)", R.m_Name);
                    Out.push_back(R); m_TLRows[R.m_Id] = { i, {}, bMerged };
                    if (bContainer) { if (L.m_Id) Emit(L.m_Id, Depth + 1, R.m_bMute, R.m_bLock); continue; }
                    if (bLane)
                    {
                        xeditor::timeline::row_desc Wl; Wl.m_Id = TLBoneRow(R.m_Id, "#weight"); Wl.m_Type = tl_weight; Wl.m_Depth = Depth + 1; Wl.m_Name = "Weight";
                        Wl.m_bMute = R.m_bMute; Wl.m_bLock = R.m_bLock; Wl.m_Color = IM_COL32(120, 200, 150, 255); Wl.m_bToggles = false;
                        Out.push_back(Wl); m_TLRows[Wl.m_Id] = { i, "#weight", false, true };
                    }
                    for (const auto& B : Bones)
                    {
                        xeditor::timeline::row_desc C; C.m_Id = TLBoneRow(R.m_Id, B); C.m_Type = tl_bone; C.m_Depth = Depth + 1; C.m_Name = B;
                        C.m_bMute = R.m_bMute; C.m_bLock = R.m_bLock; C.m_Color = IM_COL32(190, 140, 80, 255); C.m_bToggles = false;    // its layer's
                        Out.push_back(C); m_TLRows[C.m_Id] = { i, B };
                    }
                    for (const auto& E : Effs)      // IK keys: one row per effector, its pinned spans drawn as bars
                    {
                        xeditor::timeline::row_desc C; C.m_Id = TLBoneRow(R.m_Id, "#ik:" + E); C.m_Type = tl_ik; C.m_Depth = Depth + 1; C.m_Name = "IK " + E;
                        C.m_bMute = R.m_bMute; C.m_bLock = R.m_bLock; C.m_Color = IM_COL32(110, 190, 230, 255); C.m_bToggles = false;
                        Out.push_back(C); m_TLRows[C.m_Id] = { i, E, false, false, true };
                    }
                }
            };
            Emit(0, 0, false, false);
            xeditor::timeline::row_desc Ev; Ev.m_Id = tl_events_row_v; Ev.m_Type = tl_events; Ev.m_Name = std::format("Events ({})", pOv->m_Events.size()); Ev.m_Color = IM_COL32(220, 200, 90, 255); Ev.m_bToggles = false;
            Out.push_back(Ev);
            xeditor::timeline::row_desc Base; Base.m_Id = tl_base_row_v; Base.m_Type = tl_base; Base.m_Name = std::format("Clip: {}", pOv->m_Name); Base.m_Color = IM_COL32(150, 150, 160, 255); Base.m_bToggles = false;
            Out.push_back(Base);
        }

        // ops: a timeline plan ("rev N; seg row item start end offset ease_in ease_out; key row item tick") or row ops separated by ';':
        // "mute row 0|1", "lock row 0|1", "move row above_row" (0: the bottom), "delete row item" (pose keys), "weight row 0..1",
        // "mode row 0|1" (0 override, 1 additive)
        std::string TLApply(const std::string& Ops) noexcept
        {
            namespace tl = xeditor::timeline;
            auto* pOv = SelectedOv();
            if (!pOv) return "select a clip first";
            std::vector<tl::row_desc> Rows; TLRowsOf(Rows);
            auto RowOf = [&](std::uint64_t Id) -> const tl_row* { const auto It = m_TLRows.find(Id); return It == m_TLRows.end() ? nullptr : &It->second; };
            auto Set = [&](int iL, const std::string& Field, const std::string& V, const std::string& B) -> std::string
            { return V == B ? std::string{} : m_Undo.Execute(std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(LayerPath(iL, Field.c_str())), xeditor::Quote(V), xeditor::Quote(B))); };
            if (Ops.rfind("rev", 0) == 0)
            {
                std::uint64_t Rev = 0; std::vector<tl::text_op> P;
                if (!tl::FromText(Ops, Rev, P)) return std::format("bad plan '{}'", Ops);
                if (auto E = TLValidateOps(Rev, P); !E.empty()) return E;
                return AsOneStep("Timeline edit", [&]() -> std::string
                {
                    for (const auto& O : P)
                    {
                        if (O.m_Ref.m_Row == tl_events_row_v)
                        {
                            const int k = int(O.m_Ref.m_Item) - 1;
                            const auto E = SelectedOv()->m_Events[k];
                            const auto Pth = [&](const char* pF) { return std::format("{}[G:{}]/{}", EventsPath(), k, pF); };
                            auto SetE = [&](const char* pF, std::int64_t V, std::int64_t B) { return V == B ? std::string{} : m_Undo.Execute(std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(Pth(pF)), V, B)); };
                            if (auto Err = SetE("Tick", O.m_Seg.m_Start, E.m_Tick); !Err.empty()) return Err;
                            if (E.m_EndTick >= 0) if (auto Err = SetE("EndTick", O.m_Kind == 's' ? O.m_Seg.m_End : O.m_Seg.m_Start + (E.m_EndTick - E.m_Tick), E.m_EndTick); !Err.empty()) return Err;
                            continue;
                        }
                        const auto* pR = RowOf(O.m_Ref.m_Row);
                        const int iL = pR->m_bMerged ? int(O.m_Ref.m_Item) - 1 : pR->m_iLayer;     // a merged row's segment names its layer
                        const auto L = pOv->m_Layers[iL];                   // a copy: the writes below change the descriptor
                        if (O.m_Kind == 's')
                        {
                            const auto S = std::to_string(O.m_Seg.m_Start), E = std::to_string(O.m_Seg.m_End), Ti = std::to_string(O.m_Seg.m_Offset);
                            if (auto Err = Set(iL, "Start", S, std::to_string(L.m_Start)); !Err.empty()) return Err;
                            const bool bKeepOpen = L.m_End < 0 && O.m_Seg.m_End == TLClipTicks();      // "to the clip's end" stays that
                            if (!bKeepOpen) if (auto Err = Set(iL, "End", E, std::to_string(L.m_End)); !Err.empty()) return Err;
                            if (auto Err = Set(iL, "TrimIn", Ti, std::to_string(L.m_TrimIn)); !Err.empty()) return Err;
                            if (L.m_TrimOut >= 0)
                                if (auto Err = Set(iL, "TrimOut", std::to_string(O.m_Seg.m_Offset + (O.m_Seg.m_End - O.m_Seg.m_Start)), std::to_string(L.m_TrimOut)); !Err.empty()) return Err;
                        }
                        else if (pR->m_bIk)               // an IK key: its time (its target is kept)
                        {
                            const int k = int(O.m_Ref.m_Item) - 1;
                            if (auto Err = Set(iL, std::format("IkKeys[G:{}]/Tick", k).c_str(), std::to_string(O.m_Seg.m_Start), std::to_string(L.m_IkKeys[k].m_Tick)); !Err.empty()) return Err;
                        }
                        else if (pR->m_bWeight)           // a weight key: its time and its value (0..1)
                        {
                            const int k = int(O.m_Ref.m_Item) - 1;
                            if (auto Err = Set(iL, std::format("WeightKeys[G:{}]/Tick", k).c_str(), std::to_string(O.m_Seg.m_Start), std::to_string(L.m_WeightKeys[k].m_Tick)); !Err.empty()) return Err;
                            if (O.m_Value == O.m_Value)
                                if (auto Err = Set(iL, std::format("WeightKeys[G:{}]/Value", k).c_str(), std::format("{}", std::clamp(O.m_Value, 0.0f, 1.0f)), std::format("{}", L.m_WeightKeys[k].m_Value)); !Err.empty()) return Err;
                        }
                        else
                        {
                            const int k = int(O.m_Ref.m_Item) - 1;
                            if (auto Err = Set(iL, std::format("Keys[G:{}]/Tick", k).c_str(), std::to_string(O.m_Seg.m_Start), std::to_string(L.m_Keys[k].m_Tick)); !Err.empty()) return Err;
                        }
                    }
                    return {};
                });
            }
            // row ops
            std::vector<std::string> Parts; { std::size_t a = 0; while (a <= Ops.size()) { auto b = Ops.find(';', a); if (b == std::string::npos) b = Ops.size(); Parts.push_back(Ops.substr(a, b - a)); a = b + 1; } }
            std::map<int, std::vector<int>> Deletes, WDeletes, IDeletes;  // layer -> key indices (pose keys, weight keys, IK keys)
            std::vector<int> EDeletes;                                    // events
            std::vector<std::function<std::string()>> Steps;
            for (const auto& Part : Parts)
            {
                char W[16] = {}, A[40] = {}; unsigned long long R = 0;
                const int n = std::sscanf(Part.c_str(), " %15s %llu %39s", W, &R, A);
                const unsigned long long X = std::strtoull(A, nullptr, 10);    // a flag, or a row id (64 bits: not through a double)
                const double             F = std::strtod(A, nullptr);         // a weight
                if (n <= 0) continue;
                const std::string_view Op(W);
                if (Op == "edelete")            // "edelete row item": an event (the events row)
                {
                    if (R < 1 || R > pOv->m_Events.size()) return std::format("no event {}", R);
                    EDeletes.push_back(int(R) - 1);
                    continue;
                }
                const auto* pR = RowOf(R);
                if (!pR) return std::format("no layer row {}", R);
                const int iL = pR->m_iLayer;
                const auto& L = pOv->m_Layers[iL];
                const std::vector<int> Mem = pR->m_bMerged ? TLMembers(R) : std::vector<int>{ iL };     // a merged row's header acts on all its segments
                if (Op == "mute" && n == 3) { for (int m : Mem) Steps.push_back([=, this] { return Set(m, "Enabled", X ? "false" : "true", SelectedOv()->m_Layers[m].m_bEnabled ? "true" : "false"); }); }
                else if (Op == "lock" && n == 3) { for (int m : Mem) Steps.push_back([=, this] { return Set(m, "Lock", X ? "true" : "false", SelectedOv()->m_Layers[m].m_bLock ? "true" : "false"); }); }
                else if (Op == "weight" && n == 3)
                {
                    if (F < 0 || F > 1) return "a weight is 0..1";
                    for (int m : Mem)
                    {
                        const auto& Lm = pOv->m_Layers[m];
                        if (Lm.m_bLock) return std::format("layer '{}' is locked", Lm.m_Name);
                        if (!Lm.m_WeightKeys.empty()) return std::format("layer '{}' has a weight curve: edit its keys", Lm.m_Name);
                        Steps.push_back([=, this] { return Set(m, "Weight", std::format("{}", float(F)), std::format("{}", SelectedOv()->m_Layers[m].m_Weight)); });
                    }
                }
                else if (Op == "mode" && n == 3)
                {
                    for (int m : Mem)
                    {
                        if (pOv->m_Layers[m].m_bLock) return std::format("layer '{}' is locked", pOv->m_Layers[m].m_Name);
                        Steps.push_back([=, this] { return Set(m, "Mode", X ? "Additive" : "Override", SelectedOv()->m_Layers[m].m_Mode == xanim_package_desc::layer_mode::ADDITIVE ? "Additive" : "Override"); });
                    }
                }
                else if (Op == "join" && n == 3)        // "join row target_row": a Clip/Kimodo row becomes segments of the target's row (they crossfade where they overlap)
                {
                    const auto* pT = RowOf(X);
                    if (!pT || !pT->m_Bone.empty() || X == R) return std::format("no other clip row {}", X);
                    auto IsSeg = [&](int i) { const auto K = pOv->m_Layers[i].m_Kind; return K == xanim_package_desc::layer_kind::CLIP || K == xanim_package_desc::layer_kind::KIMODO; };
                    const auto TMem = pT->m_bMerged ? TLMembers(X) : std::vector<int>{ pT->m_iLayer };
                    for (int m : Mem)  if (!IsSeg(m)) return "only clip and Kimodo rows have segments";
                    for (int m : TMem) if (!IsSeg(m)) return "only clip and Kimodo rows have segments";
                    if (TLParentOf(*pOv, Mem[0]) != TLParentOf(*pOv, TMem[0])) return "the two rows are in different folders or groups (move one first)";
                    const auto& TL0 = pOv->m_Layers[TMem[0]];
                    const std::uint64_t RowId = TL0.m_Row ? TL0.m_Row : (TL0.m_Id ? TL0.m_Id : 0x5EED0000ull + std::uint64_t(TMem[0]));
                    auto After = pOv->m_Layers;
                    for (int m : TMem) After[m].m_Row = RowId;
                    for (int m : Mem)  After[m].m_Row = RowId;
                    if (auto E = xanim_package_desc::RowOverlapError(After, TLClipTicks()); !E.empty()) return E;
                    for (int m : TMem) if (pOv->m_Layers[m].m_Row != RowId) Steps.push_back([=, this] { return Set(m, "Row", std::to_string(RowId), std::to_string(SelectedOv()->m_Layers[m].m_Row)); });
                    for (int m : Mem)  Steps.push_back([=, this] { return Set(m, "Row", std::to_string(RowId), std::to_string(SelectedOv()->m_Layers[m].m_Row)); });
                }
                else if (Op == "split" && n == 3)       // "split row layer_index": that segment leaves the row (a row of its own)
                {
                    if (std::find(Mem.begin(), Mem.end(), int(X)) == Mem.end() || !pR->m_bMerged) return std::format("layer {} is not a segment of row {}", X, R);
                    const int m = int(X);
                    Steps.push_back([=, this] { return Set(m, "Row", "0", std::to_string(SelectedOv()->m_Layers[m].m_Row)); });
                }
                else if (Op == "joinfolder" && n >= 2)  // "joinfolder folder_row": the clip/Kimodo layers directly in that folder become ONE row (the old "several takes = a folder of Kimodo layers")
                {
                    const auto& FL = pOv->m_Layers[iL];
                    if (FL.m_Kind != xanim_package_desc::layer_kind::FOLDER || !FL.m_Id) return std::format("'{}' is not a folder", FL.m_Name);
                    auto After = pOv->m_Layers; std::vector<int> Kids;
                    for (int i = 0; i < int(After.size()); ++i)
                        if (TLParentOf(*pOv, i) == FL.m_Id && (After[i].m_Kind == xanim_package_desc::layer_kind::CLIP || After[i].m_Kind == xanim_package_desc::layer_kind::KIMODO)) { After[i].m_Row = FL.m_Id; Kids.push_back(i); }
                    if (Kids.size() < 2) return "the folder needs two clip or Kimodo layers";
                    if (auto E = xanim_package_desc::RowOverlapError(After, TLClipTicks()); !E.empty()) return E;
                    for (int m : Kids) if (pOv->m_Layers[m].m_Row != FL.m_Id) Steps.push_back([=, this] { return Set(m, "Row", std::to_string(FL.m_Id), std::to_string(SelectedOv()->m_Layers[m].m_Row)); });
                }
                else if (Op == "delete" && n == 3 && pR->m_bIk)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    if (X < 1 || X > L.m_IkKeys.size()) return std::format("no IK key {} on layer '{}'", X, L.m_Name);
                    IDeletes[iL].push_back(int(X) - 1);
                }
                else if (Op == "interp" && n >= 3)    // "interp row item smooth|linear|step": the interval from that key to the next (pose or IK key)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    char M[16] = {};
                    if (std::sscanf(Part.c_str(), " %*s %*llu %*llu %15s", M) != 1) return "interp row item smooth|linear|step";
                    const std::string Mode(M);
                    if (Mode != "smooth" && Mode != "linear" && Mode != "step") return "interp row item smooth|linear|step";
                    const bool bStep = Mode == "step", bLin = Mode == "linear";
                    const char* pList = pR->m_bIk ? "IkKeys" : "Keys";
                    const std::size_t nK = pR->m_bIk ? L.m_IkKeys.size() : L.m_Keys.size();
                    if (pR->m_bWeight || (pR->m_Bone.empty() && !pR->m_bIk) || X < 1 || X > nK) return std::format("no key {} on row {}", X, R);
                    const int k = int(X) - 1;
                    const bool bS0 = pR->m_bIk ? L.m_IkKeys[k].m_bStep : L.m_Keys[k].m_bStep, bL0 = pR->m_bIk ? L.m_IkKeys[k].m_bLinear : L.m_Keys[k].m_bLinear;
                    Steps.push_back([=, this] { return Set(iL, std::format("{}[G:{}]/Step", pList, k), bStep ? "true" : "false", bS0 ? "true" : "false"); });
                    Steps.push_back([=, this] { return Set(iL, std::format("{}[G:{}]/Linear", pList, k), bLin ? "true" : "false", bL0 ? "true" : "false"); });
                }
                else if (Op == "delete" && n == 3 && pR->m_bWeight)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    if (X < 1 || X > L.m_WeightKeys.size()) return std::format("no weight key {} on layer '{}'", X, L.m_Name);
                    WDeletes[iL].push_back(int(X) - 1);
                }
                else if (Op == "delete" && n == 3)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    if (X < 1 || X > L.m_Keys.size()) return std::format("no key {} on layer '{}'", X, L.m_Name);
                    Deletes[iL].push_back(int(X) - 1);
                }
                else if (Op == "addw" && pR->m_bWeight)        // "addw weight_row tick value": a weight key (0..1); one at that time already: refused
                {
                    long long Tk = 0; float Val = 0;
                    if (std::sscanf(Part.c_str(), " %*s %*llu %lld %f", &Tk, &Val) != 2 || Tk < 0 || !(Val >= 0 && Val <= 1)) return "addw row tick value(0..1)";
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    for (const auto& K : L.m_WeightKeys) if (K.m_Tick == Tk) return "a weight key is already at that time";
                    const int nW = int(L.m_WeightKeys.size());
                    Steps.push_back([=, this] { return Set(iL, "WeightKeys[]", std::to_string(nW + 1), std::to_string(nW)); });
                    Steps.push_back([=, this] { return Set(iL, std::format("WeightKeys[G:{}]/Tick", nW), std::to_string(Tk), "0"); });
                    Steps.push_back([=, this] { return Set(iL, std::format("WeightKeys[G:{}]/Value", nW), std::format("{}", Val), std::format("{}", SelectedOv()->m_Layers[iL].m_WeightKeys[nW].m_Value)); });
                }
                else if (Op == "wstep" && n == 3 && pR->m_bWeight)  // "wstep weight_row item": that key holds until the next (or blends again)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    if (X < 1 || X > L.m_WeightKeys.size()) return std::format("no weight key {} on layer '{}'", X, L.m_Name);
                    const int k = int(X) - 1; const bool bNow = L.m_WeightKeys[k].m_bStep;
                    Steps.push_back([=, this] { return Set(iL, std::format("WeightKeys[G:{}]/Step", k), bNow ? "false" : "true", bNow ? "true" : "false"); });
                }
                else if (Op == "move" && n == 3)
                {
                    // the row's layers (every segment of a merged row) leave the list and land together, in their order, right above row X
                    // (after its top layer in the mix order; the bottom / the clip row: first)
                    for (int m : Mem) if (pOv->m_Layers[m].m_bLock) return std::format("layer '{}' is locked", pOv->m_Layers[m].m_Name);
                    std::vector<int> M = Mem; std::sort(M.begin(), M.end());
                    const int N = int(pOv->m_Layers.size());
                    std::vector<int> Rest; for (int i = 0; i < N; ++i) if (!std::binary_search(M.begin(), M.end(), i)) Rest.push_back(i);
                    int p = 0;
                    if (X && X != tl_base_row_v)
                    {
                        const auto* pA = RowOf(X); if (!pA) return std::format("no row {}", X);
                        const auto XM = pA->m_bMerged ? TLMembers(X) : std::vector<int>{ pA->m_iLayer };
                        const int Top = *std::max_element(XM.begin(), XM.end());
                        if (std::binary_search(M.begin(), M.end(), Top)) return "no change";
                        p = int(std::find(Rest.begin(), Rest.end(), Top) - Rest.begin()) + 1;
                    }
                    std::vector<int> Final(Rest.begin(), Rest.begin() + p);
                    Final.insert(Final.end(), M.begin(), M.end()); Final.insert(Final.end(), Rest.begin() + p, Rest.end());
                    // contract v3: the row joins the folder or group of the row it lands above (the bottom / the clip row: the top level)
                    const std::uint64_t NewParent = (X && X != tl_base_row_v) ? TLParentOf(*pOv, RowOf(X)->m_iLayer) : 0;
                    for (int m : M) if (NewParent && pOv->m_Layers[m].m_Id && TLIsWithin(*pOv, NewParent, pOv->m_Layers[m].m_Id)) return std::format("'{}' cannot go inside itself", pOv->m_Layers[m].m_Name);
                    bool bSameOrder = true; for (int k = 0; k < N; ++k) bSameOrder = bSameOrder && Final[k] == k;
                    if (bSameOrder && NewParent == TLParentOf(*pOv, M[0])) return "no change";
                    // the moves, one by one on a simulated list (each ListOp Move takes the element out and puts it back at ToIndex)
                    std::vector<int> Sim(N); for (int k = 0; k < N; ++k) Sim[k] = k;
                    for (int k = 0; k < N; ++k)
                    {
                        const int j = int(std::find(Sim.begin(), Sim.end(), Final[k]) - Sim.begin());
                        if (j == k) continue;
                        Steps.push_back([=, this] { return m_Undo.Execute(std::format("ListOp -Path {} -Op Move -Index {} -ToIndex {}", xeditor::Quote(LayersPath()), j, k)); });
                        const int v = Sim[j]; Sim.erase(Sim.begin() + j); Sim.insert(Sim.begin() + k, v);
                    }
                    for (int m : M)
                        if (NewParent != TLParentOf(*pOv, m))
                        {
                            const int At = int(std::find(Final.begin(), Final.end(), m) - Final.begin());
                            const std::uint64_t Old = pOv->m_Layers[m].m_Parent;
                            Steps.push_back([=, this] { return Set(At, "Parent", std::to_string(NewParent), std::to_string(Old)); });
                        }
                }
                else if (Op == "into" && n == 3)        // "into row container_row": the row goes inside a folder or group (on top of its children)
                {
                    if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                    const auto* pC = RowOf(X);
                    if (!pC || !pC->m_Bone.empty()) return std::format("no folder or group row {}", X);
                    const auto& CL = pOv->m_Layers[pC->m_iLayer];
                    if (CL.m_Kind != xanim_package_desc::layer_kind::FOLDER && CL.m_Kind != xanim_package_desc::layer_kind::GROUP) return std::format("'{}' is not a folder or group", CL.m_Name);
                    if (!CL.m_Id || (L.m_Id && TLIsWithin(*pOv, CL.m_Id, L.m_Id))) return std::format("'{}' cannot go inside itself", L.m_Name);
                    if (TLParentOf(*pOv, iL) == CL.m_Id) return "no change";
                    // the list index is kept: children compose at their container's place, in list order among themselves
                    const std::uint64_t OldParent = L.m_Parent, NewParent = CL.m_Id;
                    Steps.push_back([=, this] { return Set(iL, "Parent", std::to_string(NewParent), std::to_string(OldParent)); });
                }
                else return std::format("unknown op '{}'", Part);
            }
            for (auto& [iL, Ks] : Deletes)
            {
                std::sort(Ks.rbegin(), Ks.rend()); Ks.erase(std::unique(Ks.begin(), Ks.end()), Ks.end());
                for (int k : Ks) Steps.push_back([=, this] { return m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(iL, "Keys")), k)); });
            }
            for (auto& [iL, Ks] : IDeletes)
            {
                std::sort(Ks.rbegin(), Ks.rend()); Ks.erase(std::unique(Ks.begin(), Ks.end()), Ks.end());
                for (int k : Ks) Steps.push_back([=, this] { return m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(iL, "IkKeys")), k)); });
            }
            for (auto& [iL, Ks] : WDeletes)
            {
                std::sort(Ks.rbegin(), Ks.rend()); Ks.erase(std::unique(Ks.begin(), Ks.end()), Ks.end());
                for (int k : Ks) Steps.push_back([=, this] { return m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(iL, "WeightKeys")), k)); });
            }
            std::sort(EDeletes.rbegin(), EDeletes.rend()); EDeletes.erase(std::unique(EDeletes.begin(), EDeletes.end()), EDeletes.end());
            for (int k : EDeletes) Steps.push_back([=, this] { return m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(EventsPath()), k)); });
            if (Steps.empty()) return "no change";
            return AsOneStep("Timeline edit", [&]() -> std::string { for (auto& S : Steps) if (auto E = S(); !E.empty()) return E; return {}; });
        }

        // the document's own rules for a plan (the core already checked capabilities, locks and key collisions)
        std::string TLValidateOps(std::uint64_t Rev, std::span<const xeditor::timeline::text_op> Ops) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "no clip";
            if (Rev != TLRevision()) return "stale plan: the layers changed since it was made";
            for (const auto& O : Ops)
            {
                if (O.m_Ref.m_Row == tl_events_row_v)
                {
                    const int k = int(O.m_Ref.m_Item) - 1;
                    if (k < 0 || k >= int(pOv->m_Events.size())) return std::format("no event {}", O.m_Ref.m_Item);
                    if (O.m_Seg.m_Start < 0) return "an event cannot be before the clip";
                    if (O.m_Kind == 's' && O.m_Seg.m_End < O.m_Seg.m_Start) return "an interval needs a length";
                    continue;
                }
                const auto It = m_TLRows.find(O.m_Ref.m_Row);
                if (It == m_TLRows.end()) return std::format("row {} is not a layer (the clip row is read only)", O.m_Ref.m_Row);
                const int iLv = It->second.m_bMerged ? int(O.m_Ref.m_Item) - 1 : It->second.m_iLayer;
                if (iLv < 0 || iLv >= int(pOv->m_Layers.size())) return std::format("row {} has no segment {}", O.m_Ref.m_Row, O.m_Ref.m_Item);
                const auto& L = pOv->m_Layers[iLv];
                if (L.m_bLock) return std::format("layer '{}' is locked", L.m_Name);
                if (O.m_Kind == 's')
                {
                    if (L.m_Kind != xanim_package_desc::layer_kind::CLIP && L.m_Kind != xanim_package_desc::layer_kind::KIMODO) return "only clip and Kimodo layers have segments";
                    if (O.m_Seg.m_Start < 0) return "a layer cannot start before the clip";
                    if (O.m_Seg.m_End <= O.m_Seg.m_Start) return "a layer needs a length";
                    if (O.m_Seg.m_Offset < 0) return "the layer's source starts at 0";
                }
                else if (It->second.m_bIk)
                {
                    const int k = int(O.m_Ref.m_Item) - 1;
                    if (k < 0 || k >= int(L.m_IkKeys.size())) return std::format("no IK key {} on layer '{}'", O.m_Ref.m_Item, L.m_Name);
                    if (O.m_Seg.m_Start < 0) return "a key cannot be before the clip";
                    for (int j = 0; j < int(L.m_IkKeys.size()); ++j)
                        if (j != k && L.m_IkKeys[j].m_Effector == L.m_IkKeys[k].m_Effector && L.m_IkKeys[j].m_Tick == O.m_Seg.m_Start
                            && std::none_of(Ops.begin(), Ops.end(), [&](const xeditor::timeline::text_op& o) { return o.m_Ref.m_Row == O.m_Ref.m_Row && int(o.m_Ref.m_Item) - 1 == j; }))
                            return std::format("the IK key of {} would land on another at the same time", L.m_IkKeys[k].m_Effector);
                }
                else if (It->second.m_bWeight)
                {
                    const int k = int(O.m_Ref.m_Item) - 1;
                    if (k < 0 || k >= int(L.m_WeightKeys.size())) return std::format("no weight key {} on layer '{}'", O.m_Ref.m_Item, L.m_Name);
                    if (O.m_Seg.m_Start < 0) return "a key cannot be before the clip";
                    if (O.m_Value == O.m_Value && !(O.m_Value >= 0 && O.m_Value <= 1)) return "a weight is 0..1";
                    for (int j = 0; j < int(L.m_WeightKeys.size()); ++j)
                        if (j != k && L.m_WeightKeys[j].m_Tick == O.m_Seg.m_Start && std::none_of(Ops.begin(), Ops.end(), [&](const xeditor::timeline::text_op& o) { return o.m_Ref.m_Row == O.m_Ref.m_Row && int(o.m_Ref.m_Item) - 1 == j; }))
                            return "the weight key would land on another weight key at the same time";
                }
                else
                {
                    const int k = int(O.m_Ref.m_Item) - 1;
                    if (k < 0 || k >= int(L.m_Keys.size())) return std::format("no key {} on layer '{}'", O.m_Ref.m_Item, L.m_Name);
                    if (O.m_Seg.m_Start < 0) return "a key cannot be before the clip";
                    for (int j = 0; j < int(L.m_Keys.size()); ++j)
                        if (j != k && L.m_Keys[j].m_Bone == L.m_Keys[k].m_Bone && L.m_Keys[j].m_Tick == O.m_Seg.m_Start
                            && std::none_of(Ops.begin(), Ops.end(), [&](const xeditor::timeline::text_op& o) { return o.m_Ref.m_Row == O.m_Ref.m_Row && int(o.m_Ref.m_Item) - 1 == j; })
                            && std::none_of(Ops.begin(), Ops.end(), [&](const xeditor::timeline::text_op& o) { return o.m_Kind == 'k' && m_TLRows.count(o.m_Ref.m_Row) && m_TLRows[o.m_Ref.m_Row].m_iLayer == It->second.m_iLayer && int(o.m_Ref.m_Item) - 1 == j; }))
                            return std::format("the {} key would land on another key at the same time (replace or merge it explicitly)", L.m_Keys[k].m_Bone);
                }
            }
            // a row crossfades two segments at most: the result of the plan is checked whole
            auto After = pOv->m_Layers;
            for (const auto& O : Ops)
            {
                if (O.m_Kind != 's' || O.m_Ref.m_Row == tl_events_row_v) continue;
                const auto& R = m_TLRows[O.m_Ref.m_Row];
                auto& L = After[R.m_bMerged ? int(O.m_Ref.m_Item) - 1 : R.m_iLayer];
                L.m_Start = O.m_Seg.m_Start; L.m_End = O.m_Seg.m_End;
            }
            if (auto E = xanim_package_desc::RowOverlapError(After, TLClipTicks()); !E.empty()) return E;
            return {};
        }

        // runs Fn (several commands) as ONE undo step: the steps it made are rolled back and replayed as one snapshot edit
        std::string AsOneStep(const std::string& Label, const std::function<std::string()>& Fn) noexcept
        {
            const int UndoAt = m_Undo.GetUndoIndex();
            xproperty::settings::context C;
            const auto Before = xproperty::ui::undo::SnapshotToString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), C);
            if (auto Err = Fn(); !Err.empty()) { while (m_Undo.GetUndoIndex() > UndoAt) m_Undo.Undo(); return Err; }
            if (m_Undo.GetUndoIndex() <= UndoAt + 1) return {};
            const auto After = xproperty::ui::undo::SnapshotToString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), C);
            while (m_Undo.GetUndoIndex() > UndoAt) m_Undo.Undo();
            return m_Undo.Execute(std::format("SnapshotEdit -Label {} -Before {} -After {}", xeditor::Quote(Label), xeditor::Quote(Before), xeditor::Quote(After)));
        }

        struct tl_document final : xeditor::timeline::document
        {
            session& m_S; explicit tl_document(session& S) noexcept : m_S(S) {}
            // the same command the console runs (a query command: its own steps are the undo steps); the reply says what happened
            std::string Run(const std::string& Ops) { return m_S.m_Undo.Query(std::format("LayerTimelineEdit -Ops {}", xeditor::Quote(Ops))); }
            void          Rows(std::vector<xeditor::timeline::row_desc>& Out) override { m_S.TLRowsOf(Out); }
            void          PlaybackRange(xeditor::timeline::tick& A, xeditor::timeline::tick& B) override { A = 0; B = m_S.TLClipTicks(); }
            std::string   SetPlaybackRange(xeditor::timeline::tick, xeditor::timeline::tick) override { return "the playback range is the clip's length"; }
            std::uint64_t Revision() override { return m_S.TLRevision(); }
            std::string   Validate(const xeditor::timeline::plan& P) override
            {
                std::uint64_t Rev = 0; std::vector<xeditor::timeline::text_op> Ops;
                if (!xeditor::timeline::FromText(xeditor::timeline::ToText(P), Rev, Ops)) return "the plan could not be read";
                return m_S.TLValidateOps(Rev, Ops);
            }
            std::string   Commit(const xeditor::timeline::plan& P) override { return Run(xeditor::timeline::ToText(P)); }
            std::string   Delete(std::span<const xeditor::timeline::item_ref> R) override
            {
                std::string Ops;
                for (auto& r : R)
                    if (r.m_Row == tl_events_row_v) Ops += std::format("edelete {}; ", r.m_Item);
                    else if (m_S.m_TLRows.count(r.m_Row) && !m_S.m_TLRows[r.m_Row].m_Bone.empty()) Ops += std::format("delete {} {}; ", r.m_Row, r.m_Item);
                return Ops.empty() ? std::string("only keys and events can be deleted here") : Run(Ops);
            }
            std::string   AddMarker(xeditor::timeline::tick T) override { return m_S.m_Undo.Query(std::format("ClipEvent -Add Marker -Time {}", double(T) / double(tl_tps_v))); }
            std::string   SetRowFlag(std::uint64_t Row, xeditor::timeline::row_flag F, bool bOn) override { return m_S.m_TLRows.count(Row) ? Run(std::format("{} {} {}", F == xeditor::timeline::row_flag::mute ? "mute" : "lock", Row, int(bOn))) : std::string("the clip row has no flags"); }
            bool          CanMoveRow(std::uint64_t Row) override { return m_S.m_TLRows.count(Row) && m_S.m_TLRows[Row].m_Bone.empty() && !m_S.m_TLRows[Row].m_bWeight; }   // a row of segments moves as one unit
            std::string   MoveRow(std::uint64_t Row, std::uint64_t Above) override { return Run(std::format("move {} {}", Row, Above)); }
            bool          CanDropInto(std::uint64_t Row) override
            {
                const auto It = m_S.m_TLRows.find(Row); auto* pOv = m_S.SelectedOv();
                if (!pOv || It == m_S.m_TLRows.end() || !It->second.m_Bone.empty()) return false;
                const auto K = pOv->m_Layers[It->second.m_iLayer].m_Kind;
                return K == xanim_package_desc::layer_kind::FOLDER || K == xanim_package_desc::layer_kind::GROUP
                    || K == xanim_package_desc::layer_kind::CLIP || K == xanim_package_desc::layer_kind::KIMODO;       // onto a clip row: its segments join that row
            }
            std::string   DropInto(std::uint64_t Row, std::uint64_t Into) override
            {
                const auto It = m_S.m_TLRows.find(Into); auto* pOv = m_S.SelectedOv();
                const bool bSeg = pOv && It != m_S.m_TLRows.end() && (pOv->m_Layers[It->second.m_iLayer].m_Kind == xanim_package_desc::layer_kind::CLIP || pOv->m_Layers[It->second.m_iLayer].m_Kind == xanim_package_desc::layer_kind::KIMODO);
                return Run(std::format("{} {} {}", bSeg ? "join" : "into", Row, Into));
            }
            void          OnSelection(const std::set<xeditor::timeline::item_ref>& Sel) override { for (const auto& r : Sel) if (const auto It = m_S.m_TLRows.find(r.m_Row); It != m_S.m_TLRows.end()) { m_S.m_iLayer = It->second.m_iLayer; break; } }
            void          Seek(xeditor::timeline::tick T) override { m_S.m_TimeSeconds = float(double(T) / double(tl_tps_v)); m_S.m_LoopsElapsed = 0; }
        };

        // the rows' items: declared; the core draws, picks, selects, snaps and drags them
        struct tl_rows final : xeditor::timeline::row_type
        {
            session& m_S; explicit tl_rows(session& S) noexcept : m_S(S) {}
            void Query(const xeditor::timeline::row_desc& R, const xeditor::timeline::query& Q, std::vector<xeditor::timeline::item>& Out) override
            {
                namespace tl = xeditor::timeline;
                auto* pOv = m_S.SelectedOv();
                if (!pOv) return;
                if (R.m_Type == tl_base)
                {
                    tl::item I; I.m_Ref = { R.m_Id, 1 }; I.m_Kind = tl::item_kind::segment; I.m_Seg = { 0, m_S.TLClipTicks() }; I.m_Caps = tl::caps::none; I.m_Label = pOv->m_OriginalName;
                    Out.push_back(std::move(I)); return;
                }
                if (R.m_Type == tl_events)
                {
                    for (int k = 0; k < int(pOv->m_Events.size()); ++k)
                    {
                        const auto& E = pOv->m_Events[k];
                        const bool bSpan = E.m_EndTick >= E.m_Tick;
                        tl::item I; I.m_Ref = { R.m_Id, std::uint64_t(k + 1) };
                        I.m_Kind = bSpan ? tl::item_kind::segment : tl::item_kind::key;
                        I.m_Seg = { E.m_Tick, bSpan ? std::max(E.m_EndTick, E.m_Tick + 1) : E.m_Tick };
                        I.m_Caps = tl::caps::move | tl::caps::del | (bSpan ? tl::caps::trim : 0u);
                        I.m_Label = std::format("{} ({}{})", E.m_Name, EventKindName(E.m_Kind), E.m_Bone.empty() ? "" : ", " + E.m_Bone);
                        if (Q.m_bAggregate && (I.m_Seg.m_End < Q.m_T0 || I.m_Seg.m_Start > Q.m_T1)) continue;
                        Out.push_back(std::move(I));
                    }
                    return;
                }
                const auto It = m_S.m_TLRows.find(R.m_Id);
                if (It == m_S.m_TLRows.end()) return;
                const auto& L = pOv->m_Layers[It->second.m_iLayer];
                if (R.m_Type == tl_folder || R.m_Type == tl_group) return;     // a container has no items of its own: its children are its rows
                if (R.m_Type == tl_clip)
                {
                    for (int m : (It->second.m_bMerged ? m_S.TLMembers(R.m_Id) : std::vector<int>{ It->second.m_iLayer }))
                    {
                    const auto& L = pOv->m_Layers[m];
                    tl::item I; I.m_Ref = { R.m_Id, It->second.m_bMerged ? std::uint64_t(m + 1) : 1 }; I.m_Kind = tl::item_kind::segment;
                    const tl::tick End = L.m_End >= 0 ? L.m_End : m_S.TLClipTicks();
                    tl::tick SrcLen = -1;
                    if (const auto* pSrc = m_S.FindLayerSource(L.m_SourceKey, L.m_SourceClip); pSrc && pSrc->m_FPS > 0) SrcLen = tl::tick(std::llround(double(std::max(1, pSrc->m_nFrames - 1)) / pSrc->m_FPS * tl_tps_v));
                    I.m_Seg = { L.m_Start, std::max(End, L.m_Start + 1), L.m_TrimIn, 0, 0, SrcLen };
                    // trim and slip map timeline ticks onto source ticks one to one: only at rate 1, forward (else the layer moves only)
                    const bool bPlain = L.m_Rate == 1.0 && !L.m_bReverse;
                    I.m_Caps = tl::caps::move | (bPlain ? (tl::caps::trim | tl::caps::slip) : 0u);
                    I.m_Label = L.m_Kind == xanim_package_desc::layer_kind::KIMODO ? m_S.KimodoLabel(L) : (L.m_SourceClip.empty() ? L.m_Name : L.m_SourceClip);
                    if (!Q.m_bAggregate || (I.m_Seg.m_End >= Q.m_T0 && I.m_Seg.m_Start <= Q.m_T1)) Out.push_back(std::move(I));
                    }
                    return;
                }
                if (R.m_Type == tl_pose)
                {
                    if (!Q.m_bAggregate) return;                // a summary has no items of its own
                    std::map<tl::tick, std::vector<tl::item_ref>> ByTick;
                    for (int k = 0; k < int(L.m_Keys.size()); ++k) ByTick[L.m_Keys[k].m_Tick].push_back({ TLBoneRow(R.m_Id, L.m_Keys[k].m_Bone), std::uint64_t(k + 1) });
                    for (auto& [T, Refs] : ByTick)
                    {
                        if (T < Q.m_T0 || T > Q.m_T1) continue;
                        tl::item I; I.m_Ref = { R.m_Id, std::uint64_t(T) + 1 }; I.m_Kind = tl::item_kind::key; I.m_Seg = { T, T }; I.m_Caps = tl::caps::move | tl::caps::del;
                        I.m_Count = std::uint32_t(Refs.size()); I.m_Represents = std::move(Refs); I.m_Label = std::format("{} key(s)", I.m_Count);
                        Out.push_back(std::move(I));
                    }
                    return;
                }
                if (R.m_Type == tl_weight)             // the weight curve: its keys as valued points (0..1), dragged in time and value
                {
                    for (int k = 0; k < int(L.m_WeightKeys.size()); ++k)
                    {
                        const auto& K = L.m_WeightKeys[k];
                        tl::item I; I.m_Ref = { R.m_Id, std::uint64_t(k + 1) }; I.m_Kind = tl::item_kind::key; I.m_Seg = { K.m_Tick, K.m_Tick }; I.m_Caps = tl::caps::move | tl::caps::del;
                        I.m_Value = K.m_Value; I.m_ValueMin = 0; I.m_ValueMax = 1; I.m_bStepValue = K.m_bStep; I.m_Label = std::format("weight {:.2f}{}", K.m_Value, K.m_bStep ? " (step)" : "");
                        Out.push_back(std::move(I));
                    }
                    return;
                }
                if (R.m_Type == tl_ik)                 // an effector's IK keys (P = pinned: held until the next key)
                {
                    for (int k = 0; k < int(L.m_IkKeys.size()); ++k)
                    {
                        const auto& K = L.m_IkKeys[k];
                        if (K.m_Effector != It->second.m_Bone || (Q.m_bAggregate && (K.m_Tick < Q.m_T0 || K.m_Tick > Q.m_T1))) continue;
                        tl::item I; I.m_Ref = { R.m_Id, std::uint64_t(k + 1) }; I.m_Kind = tl::item_kind::key; I.m_Seg = { K.m_Tick, K.m_Tick }; I.m_Caps = tl::caps::move | tl::caps::del;
                        I.m_Label = std::format("IK {}{}{}{}", K.m_Effector, K.m_bPinned ? " (pinned)" : "", K.m_bStep ? " (step)" : "", K.m_bLinear ? " (linear)" : "");
                        Out.push_back(std::move(I));
                    }
                    return;
                }
                // a bone of a pose layer: its keys
                for (int k = 0; k < int(L.m_Keys.size()); ++k)
                {
                    const auto& K = L.m_Keys[k];
                    if (K.m_Bone != It->second.m_Bone || (Q.m_bAggregate && (K.m_Tick < Q.m_T0 || K.m_Tick > Q.m_T1))) continue;
                    tl::item I; I.m_Ref = { R.m_Id, std::uint64_t(k + 1) }; I.m_Kind = tl::item_kind::key; I.m_Seg = { K.m_Tick, K.m_Tick }; I.m_Caps = tl::caps::move | tl::caps::del;
                    I.m_Label = std::format("{}{}{}", K.m_Bone, K.m_bOffset ? " (offset)" : "", K.m_bStep ? " (step)" : "");
                    Out.push_back(std::move(I));
                }
            }
            // a pose layer's falloff: shaded ramps before its first key and after its last
            void Overlay(const xeditor::timeline::row_desc& R, ImDrawList& D, const xeditor::timeline::draw_args& A) override
            {
                if (R.m_Type == tl_clip)
                {
                    m_S.KimodoOverlay(R, D, A);
                    // a row's crossfades: an X across each overlap of two segments (the policy: "Overlap: Crossfade", two at most)
                    if (auto* pOv = m_S.SelectedOv(); pOv && m_S.m_TLRows.count(R.m_Id) && m_S.m_TLRows[R.m_Id].m_bMerged)
                    {
                        const auto Mem = m_S.TLMembers(R.m_Id);
                        const auto ClipEnd = m_S.TLClipTicks();
                        auto Span = [&](int m) { const auto& L = pOv->m_Layers[m]; return std::pair<xeditor::timeline::tick, xeditor::timeline::tick>{ L.m_Start, L.m_End >= 0 ? L.m_End : ClipEnd }; };
                        for (std::size_t a = 0; a < Mem.size(); ++a)
                            for (std::size_t b = a + 1; b < Mem.size(); ++b)
                            {
                                const auto [s0, e0] = Span(Mem[a]); const auto [s1, e1] = Span(Mem[b]);
                                const auto t0 = std::max(s0, s1), t1 = std::min(e0, e1);
                                if (t1 <= t0) continue;
                                const float x0 = A.X(t0), x1 = A.X(t1), y0 = A.m_Min.y + 3, y1 = A.m_Max.y - 3;
                                D.AddRectFilled({ x0, y0 }, { x1, y1 }, IM_COL32(255, 255, 255, 30));
                                D.AddLine({ x0, y0 }, { x1, y1 }, IM_COL32(255, 255, 255, 170), 1.5f);
                                D.AddLine({ x0, y1 }, { x1, y0 }, IM_COL32(255, 255, 255, 170), 1.5f);
                            }
                    }
                    return;
                }
                if (R.m_Type == tl_weight)             // no keys: the constant weight as a dashed line (+ Key starts the curve)
                {
                    auto* pOw = m_S.SelectedOv(); const auto Iw = m_S.m_TLRows.find(R.m_Id);
                    if (!pOw || Iw == m_S.m_TLRows.end()) return;
                    const auto& L = pOw->m_Layers[Iw->second.m_iLayer];
                    if (!L.m_WeightKeys.empty()) return;
                    const float y0 = A.m_Min.y + 3, y1 = A.m_Max.y - 3, y = y1 - std::clamp(L.m_Weight, 0.0f, 1.0f) * (y1 - y0);
                    for (float x = A.m_Min.x; x < A.m_Max.x; x += 10) D.AddLine({ x, y }, { std::min(x + 5, A.m_Max.x), y }, IM_COL32(120, 200, 150, 160), 1.5f);
                    return;
                }
                if (R.m_Type == tl_ik)                 // pinned spans: a bar from a pinned key to the next key (or the clip's end)
                {
                    auto* pOi = m_S.SelectedOv(); const auto Ii = m_S.m_TLRows.find(R.m_Id);
                    if (!pOi || Ii == m_S.m_TLRows.end()) return;
                    std::vector<const xanim_package_desc::layer_ik_key*> Ks;
                    for (const auto& K : pOi->m_Layers[Ii->second.m_iLayer].m_IkKeys) if (K.m_Effector == Ii->second.m_Bone) Ks.push_back(&K);
                    std::sort(Ks.begin(), Ks.end(), [](auto* a, auto* b) { return a->m_Tick < b->m_Tick; });
                    const float y0 = A.m_Min.y + 6, y1 = A.m_Max.y - 6;
                    for (std::size_t k = 0; k < Ks.size(); ++k)
                        if (Ks[k]->m_bPinned)
                        {
                            const auto t1 = k + 1 < Ks.size() ? Ks[k + 1]->m_Tick : m_S.TLClipTicks();
                            D.AddRectFilled({ A.X(Ks[k]->m_Tick), y0 }, { A.X(t1), y1 }, IM_COL32(110, 190, 230, 90), 3.0f);
                        }
                    return;
                }
                if (R.m_Type != tl_pose && R.m_Type != tl_bone) return;
                auto* pOv = m_S.SelectedOv(); const auto It = m_S.m_TLRows.find(R.m_Id);
                if (!pOv || It == m_S.m_TLRows.end()) return;
                const auto& L = pOv->m_Layers[It->second.m_iLayer];
                if (L.m_Keys.empty() || L.m_Falloff <= 0) return;
                xeditor::timeline::tick T0 = L.m_Keys[0].m_Tick, T1 = T0;
                for (const auto& K : L.m_Keys) { T0 = std::min<xeditor::timeline::tick>(T0, K.m_Tick); T1 = std::max<xeditor::timeline::tick>(T1, K.m_Tick); }
                const auto F = xeditor::timeline::tick(std::llround(double(L.m_Falloff) * tl_tps_v));
                const float y0 = A.m_Min.y + 4, y1 = A.m_Max.y - 4;
                D.AddTriangleFilled({ A.X(T0 - F), y1 }, { A.X(T0), y0 }, { A.X(T0), y1 }, IM_COL32(210, 150, 70, 55));
                D.AddTriangleFilled({ A.X(T1), y0 }, { A.X(T1 + F), y1 }, { A.X(T1), y1 }, IM_COL32(210, 150, 70, 55));
                if (T1 > T0) D.AddRectFilled({ A.X(T0), y0 }, { A.X(T1), y1 }, IM_COL32(210, 150, 70, 25));
            }

            // a layer row's header widgets: its weight (a slider: dragging previews, the release is ONE command), its mode, its mask chip
            static constexpr float weight_w_v = 70, mode_w_v = 40, mask_w_v = 58;
            // a group: weight and mask, no mode (over its own base override and additive are the same); a folder: only eye / solo / lock
            float HeaderWidth(const xeditor::timeline::row_desc& R) override { return (R.m_Type == tl_clip || R.m_Type == tl_pose || R.m_Type == tl_group) ? weight_w_v + mode_w_v + mask_w_v + 8 : R.m_Type == tl_weight ? 110 : R.m_Type == tl_events ? 150 : 0; }
            void  Header(const xeditor::timeline::row_desc& R, xeditor::timeline::document&, const ImRect& Box) override
            {
                auto* pOv = m_S.SelectedOv(); auto* pDesc = m_S.Desc(); const auto It = m_S.m_TLRows.find(R.m_Id);
                if (pOv && R.m_Type == tl_events)       // + Marker at the playhead (M); Footsteps: detected from the feet's contacts
                {
                    ImGui::SetCursorScreenPos(Box.Min);
                    ImGui::PushID("events_header");
                    if (ImGui::SmallButton("+ Marker")) xeditor::LogConsole(m_S.m_Undo.Query("ClipEvent -Add Marker").substr(0, 300), xeditor::log_source::User);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A marker at the playhead (M). Its kind, name and bone: the ClipEvent command or the descriptor");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Footsteps")) xeditor::LogConsole(m_S.m_Undo.Query("DetectFootsteps").substr(0, 300), xeditor::log_source::User);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Finds where each foot lands and how long it supports: Footstep events and Contact bars (replaces only the detected ones)");
                    ImGui::PopID();
                    return;
                }
                if (!pOv || !pDesc || It == m_S.m_TLRows.end()) return;
                const int iL = It->second.m_iLayer;
                auto& L = pOv->m_Layers[iL];
                const float h = Box.GetHeight();
                if (R.m_Type == tl_weight)             // + Key: the weight at the playhead becomes a key; Step: the selected keys hold
                {
                    ImGui::SetCursorScreenPos(Box.Min);
                    ImGui::BeginDisabled(L.m_bLock);
                    if (ImGui::Button("+ Key", { 52, h }))
                    {
                        const auto T = std::int64_t(std::llround(double(m_S.m_TimeSeconds) * 6000.0));
                        std::vector<xanimation::weight_key> Ks; for (const auto& K : L.m_WeightKeys) Ks.push_back({ .m_Tick = K.m_Tick, .m_Value = K.m_Value, .m_Interp = K.m_bStep ? xanimation::weight_key::interp::STEP : xanimation::weight_key::interp::LINEAR });
                        std::sort(Ks.begin(), Ks.end(), [](const auto& a, const auto& b) { return a.m_Tick < b.m_Tick; });
                        m_S.m_TLView.m_Reply = m_S.TLRun(std::format("addw {} {} {}", R.m_Id, T, xanimation::EvaluateWeight(Ks, T, L.m_Weight)));
                    }
                    ImGui::SameLine(0, 4);
                    if (ImGui::Button("Step", { 50, h }))
                    {
                        std::string Ops; for (const auto& r : m_S.m_TLView.m_Selection) if (r.m_Row == R.m_Id) Ops += std::format("wstep {} {}; ", R.m_Id, r.m_Item);
                        m_S.m_TLView.m_Reply = Ops.empty() ? std::string("select weight keys first") : m_S.TLRun(Ops);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Step / smooth: the selected weight keys hold until the next (or blend again)");
                    return;
                }
                m_S.m_TLHeaderBoxes[R.m_Id] = Box;                          // tests drive the widgets with the virtual mouse
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 4, std::max(0.0f, (h - ImGui::GetTextLineHeight()) * 0.5f - 1) });

                // the weight
                ImGui::SetCursorScreenPos(Box.Min);
                const bool bCurve = !L.m_WeightKeys.empty();
                ImGui::BeginDisabled(bCurve || L.m_bLock);
                float W = L.m_Weight;
                ImGui::SetNextItemWidth(weight_w_v);
                ImGui::SliderFloat("##weight", &W, 0.0f, 1.0f, bCurve ? "curve" : "%.2f");
                if (ImGui::IsItemActivated()) { m_S.m_TLWeightRow = R.m_Id; m_S.m_TLWeightOrig = L.m_Weight; }
                if (ImGui::IsItemActive() && m_S.m_TLWeightRow == R.m_Id)
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { L.m_Weight = m_S.m_TLWeightOrig; m_S.m_TLWeightRow = 0; ImGui::ClearActiveID(); }
                    else L.m_Weight = W;                                    // the preview: the live composition follows; restored before the command
                }
                if (ImGui::IsItemDeactivated() && m_S.m_TLWeightRow == R.m_Id)
                {
                    const float New = L.m_Weight; L.m_Weight = m_S.m_TLWeightOrig; m_S.m_TLWeightRow = 0;
                    if (New != m_S.m_TLWeightOrig) m_S.m_TLView.m_Reply = m_S.TLRun(std::format("weight {} {}", R.m_Id, New));
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(bCurve ? "Weight: animated by a curve (edit its keys)" : L.m_bLock ? "Weight (the layer is locked)" : "Weight: how much of this layer is mixed in (drag: preview; release: one undo step)");

                // the mode (a group has none: it blends its children's result over its own base)
                ImGui::SetCursorScreenPos({ Box.Min.x + weight_w_v + 4, Box.Min.y });
                if (R.m_Type == tl_group)
                {
                    ImGui::BeginDisabled(true); ImGui::Button("Grp", { mode_w_v, h }); ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Group: its layers mix together first, then the result blends over the layers below with this weight and mask");
                }
                else
                {
                const bool bAdd = L.m_Mode == xanim_package_desc::layer_mode::ADDITIVE;
                ImGui::BeginDisabled(L.m_bLock);
                ImGui::PushStyleColor(ImGuiCol_Button, bAdd ? IM_COL32(70, 120, 90, 255) : IM_COL32(60, 75, 110, 255));
                if (ImGui::Button(bAdd ? "Add" : "Over", { mode_w_v, h })) m_S.m_TLView.m_Reply = m_S.TLRun(std::format("mode {} {}", R.m_Id, bAdd ? 0 : 1));
                ImGui::PopStyleColor();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(bAdd ? "Additive: adds its motion on top of the layers below (click: Override)" : "Override: replaces the layers below where its mask covers (click: Additive)");
                }

                // the mask chip: how many bones it covers; hover shows them in the viewport, a click edits the mask there
                const auto M = xanim_package_layers::LayerMask(L, pDesc->m_ResolvedSkeletonBones);
                int nOn = 0; for (float v : M) nOn += v > 0.0f;
                const bool bAll = L.m_Mask.empty() && L.m_SharedMask.empty();
                const std::string Chip = !L.m_SharedMask.empty() ? L.m_SharedMask : bAll ? (R.m_Type == tl_pose ? "keyed" : "all") : std::format("{} b", nOn);
                ImGui::SetCursorScreenPos({ Box.Min.x + weight_w_v + mode_w_v + 8, Box.Min.y });
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 62, 90, 255));
                if (ImGui::Button(std::format("{}##mask", Chip).c_str(), { mask_w_v, h })) { m_S.m_iLayer = iL; m_S.m_bMaskEdit = true; }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    m_S.m_iMaskHoverNext = iL;
                    ImGui::SetTooltip("Mask: %s\nShown in the viewport while hovered. Click: edit it there (click bones; Shift: one bone).",
                        bAll ? (R.m_Type == tl_pose ? "the keyed bones" : "the whole body") : std::format("{} of {} bones", nOn, M.size()).c_str());
                }
                ImGui::PopStyleVar();
            }
        };
        tl_rows                                         m_TLRowType{ *this };
        std::uint64_t                                   m_TLWeightRow = 0;      // the weight slider being dragged (its row) and the value before
        float                                           m_TLWeightOrig = 0;
        int                                             m_iMaskHover = -1, m_iMaskHoverNext = -1;   // a mask chip hovered: its layer's bones shown
        std::map<std::uint64_t, ImRect>                 m_TLHeaderBoxes;        // the last frame's header widget box per layer row

        std::string TLRun(const std::string& Ops) { return m_Undo.Query(std::format("LayerTimelineEdit -Ops {}", xeditor::Quote(Ops))); }
        int LayoutVersion() const noexcept override { return 1; }    // 1: the Timeline along the whole bottom

        // The timeline under the viewport: the transport is the editor's playback (the playhead = m_TimeSeconds)
        void RenderLayerTimeline(float Height) noexcept
        {
            namespace tl = xeditor::timeline;
            static_assert(tl_tps_v == tl::animation_ticks_per_second_v);
            const tl::registry Types = { { tl_base, &m_TLRowType }, { tl_clip, &m_TLRowType }, { tl_pose, &m_TLRowType }, { tl_bone, &m_TLRowType }, { tl_folder, &m_TLRowType }, { tl_group, &m_TLRowType }, { tl_weight, &m_TLRowType }, { tl_ik, &m_TLRowType }, { tl_events, &m_TLRowType } };
            tl_document Doc(*this);
            m_iMaskHover = m_iMaskHoverNext; m_iMaskHoverNext = -1;          // last frame's hovered mask chip (the viewport draws before this panel)
            m_TLHeaderBoxes.clear();
            m_TLView.m_View.m_TPS = tl_tps_v;
            if (auto* pC = SelectedClip(); pC && !m_TLView.m_bFramed) { m_TLView.m_View.m_RateNum = std::max(1, int(pC->m_FPS)); m_TLView.m_View.m_RateDen = 1; }
            m_TLTransport.m_Time = tl::tick(std::llround(double(m_TimeSeconds) * tl_tps_v));
            m_TLTransport.m_bPlaying = m_bPlaying;
            // the interval from each selected key (pose or IK) to the next: smooth (the layer's curve), linear or step
            {
                std::string Sel; int nSel = 0;
                for (const auto& r : m_TLView.m_Selection) if (const auto It = m_TLRows.find(r.m_Row); It != m_TLRows.end() && (It->second.m_bIk || (!It->second.m_Bone.empty() && !It->second.m_bWeight))) { Sel += std::format("{} {};", r.m_Row, r.m_Item); ++nSel; }
                ImGui::BeginDisabled(nSel == 0);
                ImGui::TextUnformatted("Interval:");
                for (const char* M : { "smooth", "linear", "step" })
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(M))
                    {
                        std::string Ops; std::size_t a = 0;
                        while (a < Sel.size()) { const auto b = Sel.find(';', a); Ops += std::format("interp {} {}; ", Sel.substr(a, b - a), M); a = b + 1; }
                        m_TLView.m_Reply = TLRun(Ops);
                    }
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The selected keys' interval to the next key: smooth (the layer's curve), linear or step (hold)");
            }
            tl::Draw(m_TLView, m_TLTransport, Doc, Types, { 0, Height - ImGui::GetFrameHeightWithSpacing() });
        }

        // LayerTimeline: view state changes (never the descriptor) and the timeline as text (tests read it)
        std::string TLInfo(const std::string& Time, const std::string& SoloArg, const std::string& Fold, const std::string& Unfold, const std::string& FrameArg) noexcept
        {
            namespace tl = xeditor::timeline;
            std::string A;
            if (!(A = Time).empty()) { double s = 0; if (std::from_chars(A.data(), A.data() + A.size(), s).ec == std::errc()) { m_TimeSeconds = float(std::max(0.0, s)); m_LoopsElapsed = 0; } }
            if (!(A = SoloArg).empty()) { unsigned long long R = 0; int On = 0; if (std::sscanf(A.c_str(), "%llu %d", &R, &On) == 2) { if (On) m_TLTransport.m_Solo.insert(R); else m_TLTransport.m_Solo.erase(R); } }
            if (!Fold.empty()) m_TLView.m_Folded.insert(std::strtoull(Fold.c_str(), nullptr, 10));
            if (!Unfold.empty()) m_TLView.m_Folded.erase(std::strtoull(Unfold.c_str(), nullptr, 10));
            if (!(A = FrameArg).empty()) { double a = 0, b = 0; if (std::sscanf(A.c_str(), "%lf %lf", &a, &b) == 2) { m_TLView.m_View.Frame(tl::tick(a * tl_tps_v), tl::tick(b * tl_tps_v)); m_TLView.m_bFramed = true; } }
            std::vector<tl::row_desc> Rows; TLRowsOf(Rows);
            const auto& V = m_TLView.m_View;
            std::string Sel; for (const auto& r : m_TLView.m_Selection) Sel += std::format("{}:{} ", r.m_Row, r.m_Item);
            std::string Solo; for (auto r : m_TLTransport.m_Solo) Solo += std::format("{} ", r);
            std::string T = std::format("revision {} dirty {}; time {} ticks; solo [{}]; selection [{}]; layer {}; view left {:.3f} px/tick {:.6f}\n", TLRevision(), int(m_Document.m_bDirty)
                , tl::tick(std::llround(double(m_TimeSeconds) * tl_tps_v)), Solo, Sel, m_iLayer, V.m_Left, V.m_PxPerTick);
            const auto& S = m_TLView;
            T += std::format("body {:.1f},{:.1f}..{:.1f},{:.1f} ruler {:.1f},{:.1f}..{:.1f},{:.1f}; reply {}\n", S.m_Body.Min.x, S.m_Body.Min.y, S.m_Body.Max.x, S.m_Body.Max.y, S.m_Ruler.Min.x, S.m_Ruler.Min.y, S.m_Ruler.Max.x, S.m_Ruler.Max.y, S.m_Reply);
            for (const auto& [Id, R] : S.m_RowRects) T += std::format("rowrect {} at y {:.1f}..{:.1f}\n", Id, R.Min.y, R.Max.y);
            for (const auto& [Id, R] : m_TLHeaderBoxes) T += std::format("hdr {} {:.1f} {:.1f} {:.1f} {:.1f}\n", Id, R.Min.x, R.Min.y, R.Max.x, R.Max.y);
            T += std::format("header width {:.1f}\n", S.m_HeaderWidth);
            if (auto* pOv = SelectedOv()) for (int i = 0; i < int(pOv->m_Layers.size()); ++i)
                T += std::format("layer {} weight {} mode {}\n", TLLayerRow(pOv->m_Layers[i], i), pOv->m_Layers[i].m_Weight, pOv->m_Layers[i].m_Mode == xanim_package_desc::layer_mode::ADDITIVE ? "Additive" : "Override");
            for (const auto& R : Rows)
            {
                T += std::format("row {} {} {} {} {} {}{}\n", R.m_Id, R.m_Type, R.m_Depth, int(R.m_bMute), int(R.m_bLock), R.m_Name, m_TLView.m_Folded.count(R.m_Id) ? " (folded)" : "");
                std::vector<tl::item> Items; m_TLRowType.Query(R, { &V, -(tl::tick(1) << 40), tl::tick(1) << 40, R.m_Type == tl_pose }, Items);
                for (const auto& I : Items)
                {
                    if (I.m_Kind == tl::item_kind::segment) T += std::format("seg {} {} {} {} caps {}\n", I.m_Ref.m_Item, I.m_Seg.m_Start, I.m_Seg.m_End, I.m_Seg.m_Offset, I.m_Caps);
                    else T += std::format("key {} {}{}{}\n", I.m_Ref.m_Item, I.m_Seg.m_Start, I.m_Count > 1 ? std::format(" x{}", I.m_Count) : std::string{}, I.HasValue() ? std::format(" value {}", I.m_Value) : std::string{});
                }
            }
            return T;
        }

        // The selected clip's live composition, remade when its layers changed (or the sources were reloaded)
        void UpdateLiveLayers() noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource(); auto* pDesc = Desc();
            const bool bAny = pOv && std::any_of(pOv->m_Layers.begin(), pOv->m_Layers.end(), [](const auto& L) { return L.m_bEnabled; });
            if (!bAny || !pDesc) { m_LayerComposed.clear(); m_LayerSig.clear(); m_LayerError.clear(); m_LayerReport = {}; return; }
            if (m_iSolo >= int(pOv->m_Layers.size())) m_iSolo = -1;
            std::string SoloIds; for (auto Id : m_TLTransport.m_Solo) SoloIds += std::format("{},", Id);
            const std::string Sig = std::format("{}:{}:{}:{}:{}:{}", m_iSelectedSource, m_iSelectedDescriptorClip, m_LayerSources.size(), m_iSolo, SoloIds, LayerSignature(*pOv));
            if (Sig == m_LayerSig) return;
            m_LayerSig = Sig; m_LayerComposed.clear(); m_LayerError.clear();
            const auto* pBase = FindLayerSource(pS->key(), pOv->m_OriginalName);
            if (!pBase) { m_LayerError = m_LayerSources.empty() ? "no layer sources yet: compile the package once" : "this clip is not in the layer sources: compile the package"; return; }
            xanim_package_desc::clip Audition;                              // solo: a copy with only that layer on (the descriptor is untouched)
            const xanim_package_desc::clip* pUse = pOv;
            if (m_iSolo >= 0) { Audition = *pOv; for (int i = 0; i < int(Audition.m_Layers.size()); ++i) Audition.m_Layers[i].m_bEnabled = i == m_iSolo; pUse = &Audition; }
            else if (!m_TLTransport.m_Solo.empty())     // the timeline's solo (a layer row, or a bone row of it): only those layers on, over the clip
            {
                Audition = *pOv;
                const int nL = int(Audition.m_Layers.size());
                std::vector<char> Solo(nL, 0);
                for (int i = 0; i < nL; ++i)
                {
                    const auto Row = TLLayerRow(Audition.m_Layers[i], i);
                    bool bSolo = m_TLTransport.m_Solo.count(Row) != 0;
                    for (const auto& K : Audition.m_Layers[i].m_Keys) bSolo = bSolo || m_TLTransport.m_Solo.count(TLBoneRow(Row, K.m_Bone));
                    Solo[i] = bSolo;
                }
                // contract v3: a soloed layer keeps its folders/groups on (its context); a soloed folder/group keeps everything inside it
                std::vector<char> Keep = Solo;
                for (int i = 0; i < nL; ++i)
                    for (int j = 0; j < nL; ++j)
                        if (Solo[j] && Audition.m_Layers[i].m_Id && i != j)
                        {
                            if (TLIsWithin(Audition, Audition.m_Layers[j].m_Id, Audition.m_Layers[i].m_Id)) Keep[i] = 1;      // i contains a soloed j
                            if (Audition.m_Layers[j].m_Id && TLIsWithin(Audition, Audition.m_Layers[i].m_Id, Audition.m_Layers[j].m_Id)) Keep[i] = 1;   // i is inside a soloed j
                        }
                for (int i = 0; i < nL; ++i) Audition.m_Layers[i].m_bEnabled = Audition.m_Layers[i].m_bEnabled && Keep[i];
                pUse = &Audition;
            }
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

        // Contract v3: a folder (pass-through organization) or a group (isolated: its children mix over the stack below it, then the group blends
        // that result with its weight and mask; its mask empty = the whole body)
        std::string LayerAddContainer(std::string Name, bool bGroup) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv) return "LayerAdd: select a clip first (SelectClip)";
            const int n = int(pOv->m_Layers.size());
            std::uint64_t Id = 1;
            for (const auto& S : pDesc->m_ImportSources) for (const auto& C : S.m_Clips) for (const auto& L : C.m_Layers) Id = std::max(Id, L.m_Id + 1);
            if (Name.empty()) Name = std::format("{} {}", bGroup ? "Group" : "Folder", n);
            if (auto E = AsOneStep(std::format("Add {} '{}'", bGroup ? "group" : "folder", Name), [&]() -> std::string
            {
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(LayersPath() + "[]"), xeditor::Quote(std::to_string(n + 1)), xeditor::Quote(std::to_string(n))));
                SetLayer(n, "Id", std::to_string(Id), "0");
                SetLayer(n, "Name", Name, "");
                SetLayer(n, "Kind", bGroup ? "Group" : "Folder", "Clip");
                return {};
            }); !E.empty()) return "LayerAdd: " + E;
            m_iLayer = n;
            return std::format("LayerAdd: {} {} '{}' (id {}); put layers in it with LayerParent", bGroup ? "group" : "folder", n, Name, Id);
        }

        // A layer into a folder or group (iParent its index) or back to the top level (-1): one undo step. Refuses a parent that is not a
        // folder/group, the layer itself, or one of its own descendants (a loop).
        std::string LayerParent(int iLayer, int iParent) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size())) return "LayerParent: no such layer in the selected clip";
            auto& Ls = pOv->m_Layers;
            std::uint64_t NewId = 0;
            if (iParent >= 0)
            {
                if (iParent >= int(Ls.size())) return "LayerParent: no such parent layer";
                const auto& P = Ls[iParent];
                if (P.m_Kind != xanim_package_desc::layer_kind::FOLDER && P.m_Kind != xanim_package_desc::layer_kind::GROUP) return "LayerParent: the parent must be a folder or a group";
                if (iParent == iLayer) return "LayerParent: a layer cannot be its own parent";
                for (std::uint64_t A = P.m_Id; A; )          // the parent's ancestors must not include the layer
                {
                    if (A == Ls[iLayer].m_Id) return "LayerParent: that would put a folder or group inside itself";
                    const auto It = std::find_if(Ls.begin(), Ls.end(), [A](const auto& L) { return L.m_Id == A; });
                    A = It == Ls.end() ? 0 : It->m_Parent;
                }
                if (!P.m_Id) return "LayerParent: the parent has no id yet";
                NewId = P.m_Id;
            }
            const auto Old = Ls[iLayer].m_Parent;
            if (Old == NewId) return "LayerParent: no change";
            SetLayer(iLayer, "Parent", std::to_string(NewId), std::to_string(Old));
            m_iLayer = iLayer;
            return std::format("LayerParent: layer {} '{}' -> {}", iLayer, Ls[iLayer].m_Name, iParent < 0 ? std::string("top level") : std::format("{} '{}'", iParent, Ls[iParent].m_Name));
        }

        // A layer's mask from a skeleton mask group (by name), or back to its own entries ("")
        std::string LayerMaskShared(int iLayer, const std::string& Name) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size())) return "LayerMask: no such layer in the selected clip";
            const auto Old = pOv->m_Layers[iLayer].m_SharedMask;
            if (Old == Name) return "LayerMask: no change";
            SetLayer(iLayer, "SharedMask", Name, Old);
            const auto& Ms = pDesc->m_ResolvedSkeletonBones.m_Masks;
            const bool bKnown = Name.empty() || std::any_of(Ms.begin(), Ms.end(), [&](const auto& M) { return M.m_Name == Name; });
            return Name.empty() ? std::format("LayerMask: layer {} uses its own mask entries again", iLayer)
                 : bKnown ? std::format("LayerMask: layer {} uses the skeleton mask group '{}'", iLayer, Name)
                 : std::format("LayerMask: layer {} names '{}', which the skeleton does not have: the layer contributes nothing until it is fixed", iLayer, Name);
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
            // the stack exactly as the layer sees it when the clip composes (inside a group: the group's own sub-stack); a disabled layer
            // does not run, so for it the layers before it in the list
            auto FindSrc = [this](const std::wstring& K, const std::string& C) { return FindLayerSource(K, C); };
            std::vector<xmath::transform3> Out, Full; xanim_package_layers::report R;
            if (iLayer < int(pOv->m_Layers.size()))
            {
                if (!xanim_package_layers::Compose(*pBase, *pOv, pDesc->m_ResolvedSkeletonBones, FindSrc, Full, R, Error, &pOv->m_Layers[iLayer], &Out)) return false;
            }
            if (Out.empty())
            {
                xanim_package_desc::clip Below = *pOv;
                Below.m_Layers.resize(std::min<std::size_t>(iLayer, Below.m_Layers.size()));
                if (!xanim_package_layers::Compose(*pBase, Below, pDesc->m_ResolvedSkeletonBones, FindSrc, Out, R, Error)) return false;
            }
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
                T += " ]";
                if (L.m_Kind == xanim_package_desc::layer_kind::FOLDER) T += " folder";
                if (L.m_Kind == xanim_package_desc::layer_kind::GROUP)  T += " group";
                if (const auto P = TLParentOf(*pOv, i)) T += std::format(" parent {}", P);
                if (!L.m_SharedMask.empty()) T += std::format(" shared-mask '{}'", L.m_SharedMask);
                T += "\n";
                if (L.m_Kind == xanim_package_desc::layer_kind::POSE) T += std::format("    {} curve, falloff {:.2f} s\n", L.m_bSmooth ? "smooth" : "linear", L.m_Falloff);
                if (L.m_Kind == xanim_package_desc::layer_kind::KIMODO) T += "    kimodo: " + KimodoLabel(L) + "\n";
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
                            T += std::format(" live hit {:.4f} deg", AngleDeg(Q, xmath::fquat(K.m_X, K.m_Y, K.m_Z, K.m_W)));
                        }
                        T += "\n";
                    }
            }
            for (const auto& R : m_LayerReport.m_Layers) T += std::format("  live '{}': {} bones in its mask, bone-frames written {}, outside its mask {}\n", R.m_Name, R.m_nMaskBones, R.m_Written, R.m_WrittenOutside);
            T += std::format("  live recompose {:.3f} ms{}\n", m_LayerComposeMs, m_iSolo >= 0 ? std::format(" (solo layer {}: editor only)", m_iSolo) : std::string{});
            if (!m_LayerComposed.empty())
                T += std::format("  live isolation: bone-frames changed outside every mask {}{}; root travel change {:.6f} m\n  {}\n", m_LayerReport.m_ChangedOutside
                    , m_LayerReport.m_ChangedOutside ? " (first: " + m_LayerReport.m_FirstOutside + ")" : std::string{}, m_LayerReport.m_MaxRootMove, LayersParity());
            for (const auto& W : m_LayerReport.m_Warnings) T += "  " + W + "\n";      // a missing shared mask, a parent that names nothing: shown, never silent
            return T;
        }

        // The selected layer's mask, as the bones it touches (drawn selected in the viewport)
        void UpdateMaskShown() noexcept
        {
            m_MaskShown.clear();
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            const int iL = m_iMaskHover >= 0 ? m_iMaskHover : m_iLayer;      // a hovered mask chip shows its layer's bones
            if (!pOv || !pDesc || iL < 0 || iL >= int(pOv->m_Layers.size())) return;
            const auto M = xanim_package_layers::LayerMask(pOv->m_Layers[iL], pDesc->m_ResolvedSkeletonBones);
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
            ImGui::SameLine();
            if (ImGui::Button("+ Kimodo layer")) xeditor::LogConsole(KimodoLayerAdd(m_K.m_Prompt, 2.0, m_TimeSeconds, 1, {}), xeditor::log_source::User);
            if (ImGui::IsItemHovered()) xeditor::hint::Text("A generated take as a layer: 2 s at the playhead, the prompt of the Kimodo tab.\nRegenerate makes candidates from the layers below; Accept makes one its take.");
            // the selected Kimodo layer: its states and its actions
            if (m_iLayer >= 0 && m_iLayer < int(pOv->m_Layers.size()) && pOv->m_Layers[m_iLayer].m_Kind == xanim_package_desc::layer_kind::KIMODO)
            {
                const auto& KL = pOv->m_Layers[m_iLayer];
                ImGui::TextWrapped("Kimodo layer '%s': %s", KL.m_Name.c_str(), KimodoLabel(KL).c_str());
                if (ImGui::Button("Regenerate")) xeditor::LogConsole(KimodoRegenerate(m_iLayer, {}).substr(0, 400), xeditor::log_source::User);
                ImGui::SameLine();
                if (ImGui::Button("Alternatives")) xeditor::LogConsole(KimodoRegenerate(m_iLayer, std::format("{} {} {}", KL.m_Seed, KL.m_Seed + 1, KL.m_Seed + 2)).substr(0, 400), xeditor::log_source::User);
                if (m_KL.m_LayerId == KL.m_Id && (m_KL.m_Job == "candidate" || m_KL.m_Job == "running"))      // each candidate as soon as its seed is done
                    for (int c = 0; c < int(m_KL.m_Cands.size()); ++c)
                    {
                        if (!KimodoLayerDone(c)) continue;
                        ImGui::PushID(1000 + c);
                        if (ImGui::RadioButton(std::format("seed {} (seam {:.1f} cm)", m_KL.m_Cands[c].m_Seed, m_KL.m_Cands[c].m_SeamCm).c_str(), m_KL.m_iShow == c)) m_KL.m_iShow = c;
                        ImGui::SameLine(); if (ImGui::SmallButton("Accept")) xeditor::LogConsole(KimodoAcceptCandidate(c), xeditor::log_source::User);
                        ImGui::PopID();
                    }
                if (!KL.m_SourceKey.empty()) { if (ImGui::Button("Bake to Pose layer")) xeditor::LogConsole(BakeToPose(m_iLayer, 1), xeditor::log_source::User); }
            }

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

        // pVx (set): the root goes straight at (Vx, Vz) m/s facing ahead the whole take (Kimodo's root2d path + global heading on every
        // frame): a text-only "side-step" turned its body ~40 deg over 4 s (plan C72) - a strafe needs the path and the facing stated
        std::string KimodoGenerate(const std::string& Prompt, float Seconds, int Seed, const float* pVx = nullptr, float Vz = 0) noexcept
        {
            if (auto* pWhy = KimodoWhyNot()) return std::string("KimodoGenerate: ") + pWhy;
            if (m_K.m_Job.m_bRunning) return "KimodoGenerate: a generation is already running";
            const auto Exe = kimodo::FindExe();
            if (Exe.empty()) return "KimodoGenerate: xkimodo.exe not found (build dependencies/xKimodo, or set XKIMODO_EXE)";
            std::string Err;
            if (!KimodoResolveSkeleton(Err)) return "KimodoGenerate: " + Err;
            Seconds = std::clamp(Seconds, 0.5f, 60.0f);
            if (pVx)
            {
                static int s_Count = 0;
                std::filesystem::create_directories(m_Document.m_TempDir);
                const auto Stem = std::format(L"{}\\kimodo_path_{}_{}", m_Document.m_TempDir, GetCurrentProcessId(), ++s_Count);
                const int N = std::max(2, int(std::lround(Seconds * 30.0f)));
                kimodo::constraint_set Path{ "root2d", {}, {}, {} };
                for (int f = 0; f < N; ++f)
                {
                    const float t = float(f) / 30.0f;
                    Path.m_Frames.push_back(f); Path.m_Smooth.push_back({ *pVx * t, Vz * t }); Path.m_Heading.push_back({ 1.0f, 0.0f });      // (heading 0: facing +Z, Kimodo's atan2(dz, -dx) of right - left hip)
                }
                if (!kimodo::WriteConstraints(Stem + L".json", { Path }, Err)) return "KimodoGenerate: " + Err;
                std::wstring P = xstrtool::To(Prompt);
                for (auto& c : P) if (c == L'"' || c == L'\n' || c == L'\r') c = L' ';
                const auto Args = std::format(L"gen --prompt \"{}\" --frames {} --seed {} --constraints \"{}.json\" --bvh \"{}.bvh\"", P, N, Seed, Stem, Stem);
                if (!m_K.m_Job.StartArgs(Exe, Args, Stem + L".bvh", Stem + L".log", Err)) return "KimodoGenerate: " + Err;
            }
            else if (!m_K.m_Job.Start(Exe, m_Document.m_TempDir, Prompt, Seconds, Seed, Err)) return "KimodoGenerate: " + Err;
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
        // the angle between two rotations: 2 atan2(|v|, |w|) of A * inverse(B), in double (2 acos(|dot|) in float cannot resolve angles under
        // ~0.07 deg: a dot one or two float steps below 1 already reads 0.04-0.07 deg - it made a bit-exact bake look 0.0685 deg off)
        static float AngleDeg(const xmath::fquat& A, const xmath::fquat& B) noexcept
        {
            const double ax = A.m_X, ay = A.m_Y, az = A.m_Z, aw = A.m_W, bx = -B.m_X, by = -B.m_Y, bz = -B.m_Z, bw = B.m_W;
            const double w = aw * bw - ax * bx - ay * by - az * bz;
            const double x = aw * bx + ax * bw + ay * bz - az * by, y = aw * by - ax * bz + ay * bw + az * bx, z = aw * bz + ax * by - ay * bx + az * bw;
            return float(2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::abs(w)) * 57.29577951308232);
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

        //----------------------------------------------------------------------------------------
        // Kimodo layers (timeline T4): a layer whose motion is a generated take. Its accepted take is an import source (as KimodoAccept writes:
        // the BVH in the Kimodo folder, retargeted by the compile; its clip entry Delete: a layer source, not an output clip) sampled exactly
        // like a Clip layer; with none yet it contributes nothing. Its request (prompt, seed, steps, its span) makes the take; AcceptedSig =
        // the request when the take was accepted (request relation: matches or changed). Generation makes CANDIDATES (session only) until
        // KimodoAcceptCandidate. Three independent states: the accepted take, the request relation, the job.
        // Constraints rule (stated in every request): full-body poses of the stack below the layer at the segment's first and last frame and
        // at every Pose-layer key time inside it (at most 16), and the root path of the stack below over the whole segment.
        //----------------------------------------------------------------------------------------
        struct kimodo_layer_cand { int m_Seed = 1; std::wstring m_Bvh; std::vector<xmath::transform3> m_Frames; float m_SeamCm = 0; double m_Ms = 0; int m_iJob = -1; bool m_bDone = false; };
        struct kimodo_layer_state
        {
            std::uint64_t                   m_LayerId = 0;
            std::string                     m_Sig;              // the clip's layers when the candidates were made (stale after any layer edit)
            std::string                     m_ReqSig;           // the layer's request then
            kimodo_space                    m_KS;
            std::int64_t                    m_Start = 0;        // the candidates' frame 0 (clip ticks)
            int                             m_W = 0;            // frames (30 fps)
            std::vector<kimodo_layer_cand>  m_Cands;
            int                             m_iShow = -1;
            std::string                     m_Job = "idle";     // idle, running, candidate, failed, stale (a late result whose segment changed)
            std::string                     m_LastError;
            // the background run (T4b): seeds still to make, what each run needs, and the request it was made for (a late result binds only
            // to the same layer with the same request and span)
            std::vector<int>                m_Pending;
            std::wstring                    m_Exe, m_Stem, m_Prompt;
            int                             m_Steps = 25;
            std::vector<xmath::fvec3>       m_B0, m_B1;         // the stack below's first/last frame (the seam measure)
            std::string                     m_Text;             // the report so far
        } m_KL;
        // the seeds run side by side, at most this many clients at once (ponytail: 3, the warm server queues what it cannot run together),
        // each polled once a frame; a candidate appears as soon as its seed finishes
        static constexpr int                kimodo_jobs_v = 3;
        std::array<kimodo::job, kimodo_jobs_v> m_KLJobs;
        bool KimodoLayerDone(int c) const noexcept { return c >= 0 && c < int(m_KL.m_Cands.size()) && m_KL.m_Cands[c].m_bDone; }

        static std::string KimodoReqSig(const xanim_package_desc::clip_layer& L)
        {
            return std::format("{}|{}|{}|{:.3f}|{}|{}", L.m_Prompt, L.m_Seed, L.m_Steps, L.m_Seconds, L.m_Start, L.m_End);
        }
        std::string KimodoStatus(const xanim_package_desc::clip_layer& L) noexcept
        {
            std::string S = L.m_SourceKey.empty() ? "no take" : FindLayerSource(L.m_SourceKey, L.m_SourceClip) ? "take " + std::filesystem::path(L.m_SourceKey).stem().string() : "take not compiled yet";
            if (!L.m_SourceKey.empty()) S += KimodoReqSig(L) == L.m_AcceptedSig ? ", matches its request" : ", request changed";
            if (L.m_Id && m_KL.m_LayerId == L.m_Id)
            {
                if (m_KL.m_Job == "candidate") S += std::format(", {} candidate(s){}", std::count_if(m_KL.m_Cands.begin(), m_KL.m_Cands.end(), [](const auto& C) { return C.m_bDone; }), m_KL.m_Sig == LayersSigNow() ? "" : " (stale)");
                if (m_KL.m_Job == "failed") S += ", generation failed: " + m_KL.m_LastError;
                if (m_KL.m_Job == "running") S += std::format(", generating ({} done)...", std::count_if(m_KL.m_Cands.begin(), m_KL.m_Cands.end(), [](const auto& C) { return C.m_bDone; }));
                if (m_KL.m_Job == "stale") S += ", " + m_KL.m_LastError;
            }
            return S;
        }
        std::string KimodoLabel(const xanim_package_desc::clip_layer& L) noexcept
        {
            return std::format("\"{}\" - {}", L.m_Prompt.empty() ? "(no prompt)" : L.m_Prompt, KimodoStatus(L));
        }
        std::string LayersSigNow() noexcept { auto* pOv = SelectedOv(); return pOv ? std::format("{}:{}:{}", m_iSelectedSource, m_iSelectedDescriptorClip, LayerSignature(*pOv)) : std::string{}; }

        // a Kimodo row: hatched while it has no take or its request changed since the take; the candidates' span outlined
        void KimodoOverlay(const xeditor::timeline::row_desc& R, ImDrawList& D, const xeditor::timeline::draw_args& A) noexcept
        {
            auto* pOv = SelectedOv(); const auto It = m_TLRows.find(R.m_Id);
            if (!pOv || It == m_TLRows.end()) return;
            for (int m : TLMembers(R.m_Id))
            {
            const auto& L = pOv->m_Layers[m];
            if (L.m_Kind != xanim_package_desc::layer_kind::KIMODO) continue;
            const auto T0 = L.m_Start, T1 = L.m_End >= 0 ? L.m_End : TLClipTicks();
            const float y0 = A.m_Min.y + 3, y1 = A.m_Max.y - 3, x0 = A.X(T0), x1 = A.X(T1);
            if (L.m_SourceKey.empty() || KimodoReqSig(L) != L.m_AcceptedSig)
            {
                D.PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
                for (float x = x0 - (y1 - y0); x < x1; x += 7) D.AddLine(ImVec2(x, y1), ImVec2(x + (y1 - y0), y0), IM_COL32(255, 255, 255, 60));
                D.PopClipRect();
            }
            if (L.m_Id && m_KL.m_LayerId == L.m_Id && m_KL.m_Job == "candidate" && m_KL.m_W > 1)
            {
                const auto C1 = m_KL.m_Start + std::int64_t(std::llround(double(m_KL.m_W - 1) / 30.0 * 6000.0));
                D.AddRect(ImVec2(A.X(m_KL.m_Start), y0 - 2), ImVec2(A.X(C1), y1 + 2), IM_COL32(120, 220, 255, 220), 2.0f, 0, 2.0f);
            }
            if (L.m_Id && m_KL.m_LayerId == L.m_Id && m_KL.m_Job == "running")     // generating in the background: a moving dash along the span
            {
                const float Ph = float(std::fmod(ImGui::GetTime() * 60.0, 12.0));
                for (float x = x0 - 12 + Ph; x < x1; x += 12) D.AddLine(ImVec2(std::max(x, x0), y1 + 1), ImVec2(std::min(x + 6, x1), y1 + 1), IM_COL32(120, 220, 255, 230), 2.0f);
            }
            }
        }

        // the stack as layer iLayer sees it (inside a group: the group's own sub-stack), every frame of the clip
        bool StackFramesFor(int iLayer, std::vector<xmath::transform3>& Out, int& FPS, int& nF, std::string& Error) noexcept
        {
            auto* pOv = SelectedOv(); auto* pS = SelectedSource(); auto* pDesc = Desc();
            const auto* pBase = (pOv && pS) ? FindLayerSource(pS->key(), pOv->m_OriginalName) : nullptr;
            if (!pBase) { Error = "this clip is not in the layer sources: compile the package"; return false; }
            auto FindSrc = [this](const std::wstring& K, const std::string& C) { return FindLayerSource(K, C); };
            std::vector<xmath::transform3> Full; xanim_package_layers::report R;
            Out.clear();
            // the layer itself must not count: compose with it disabled, ask for what it would see
            xanim_package_desc::clip Tmp = *pOv;
            Tmp.m_Layers[iLayer].m_SourceKey.clear();
            if (!xanim_package_layers::Compose(*pBase, Tmp, pDesc->m_ResolvedSkeletonBones, FindSrc, Full, R, Error, &Tmp.m_Layers[iLayer], &Out)) return false;
            if (Out.empty()) { xanim_package_desc::clip Below = *pOv; Below.m_Layers.resize(iLayer); if (!xanim_package_layers::Compose(*pBase, Below, pDesc->m_ResolvedSkeletonBones, FindSrc, Out, R, Error)) return false; }
            FPS = std::max(1, pBase->m_FPS); nF = pBase->m_nFrames;
            return true;
        }

        // an edit made straight on the descriptor, then recorded as ONE undo step (a snapshot edit); nothing changed: no step
        std::string DirectOneStep(const std::string& Label, const std::function<std::string(desc&)>& Fn) noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) return "nothing loaded";
            xproperty::settings::context C;
            auto Snap = [&] { return xproperty::ui::undo::SnapshotToString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), C); };
            const auto Before = Snap();
            if (auto Err = Fn(*pDesc); !Err.empty()) { xproperty::ui::undo::ApplySnapshotFromString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), Before, C); return Err; }
            const auto After = Snap();
            if (After == Before) return "no change";
            xproperty::ui::undo::ApplySnapshotFromString(*m_Document.m_pDescriptor->getProperties(), m_Document.m_pDescriptor.get(), Before, C);
            return m_Undo.Execute(std::format("SnapshotEdit -Label {} -Before {} -After {}", xeditor::Quote(Label), xeditor::Quote(Before), xeditor::Quote(After)));
        }
        static std::uint64_t NextLayerId(const desc& D) noexcept
        {
            std::uint64_t Id = 1;
            for (const auto& S : D.m_ImportSources) for (const auto& C : S.m_Clips) for (const auto& L : C.m_Layers) Id = std::max(Id, L.m_Id + 1);
            return Id;
        }

        std::string KimodoLayerAdd(const std::string& Prompt, double Seconds, double StartS, int Seed, std::string Name) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "KimodoLayerAdd: select a clip first (SelectClip)";
            if (Seconds < 0.5 || Seconds > 10.0) return "KimodoLayerAdd: -Seconds is 0.5 .. 10 (one Kimodo take; a longer motion is several Kimodo layers, e.g. in a folder)";
            const int n = int(pOv->m_Layers.size());
            if (Name.empty()) Name = std::format("Kimodo {}", n);
            const auto T0 = std::int64_t(std::llround(std::max(0.0, StartS) * 6000.0)), T1 = T0 + std::int64_t(std::llround(Seconds * 6000.0));
            std::uint64_t Id = 0;
            const auto E = DirectOneStep(std::format("Add Kimodo layer '{}'", Name), [&](desc& D) -> std::string
            {
                auto* pO = SelectedOv();
                Id = NextLayerId(D);
                auto& L = pO->m_Layers.emplace_back();
                L.m_Id = Id; L.m_Name = Name; L.m_Kind = xanim_package_desc::layer_kind::KIMODO; L.m_Mode = xanim_package_desc::layer_mode::OVERRIDE;
                L.m_Start = T0; L.m_End = T1; L.m_Prompt = Prompt; L.m_Seed = Seed; L.m_Seconds = float(Seconds); L.m_Steps = 25;
                L.m_bRotation = true; L.m_bPosition = true;             // the take's hips height comes with it (the root still follows the stack below)
                return {};
            });
            if (!E.empty()) return "KimodoLayerAdd: " + E;
            m_iLayer = n;
            return std::format("KimodoLayerAdd: layer {} '{}' (id {}), {:.2f} .. {:.2f} s, \"{}\"; no take yet: KimodoRegenerate -Layer {}", n, Name, Id, T0 / 6000.0, T1 / 6000.0, Prompt, n);
        }

        std::string KimodoRegenerate(int iLayer, const std::string& Seeds) noexcept
        {
            using clock = std::chrono::steady_clock;
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size()) || pOv->m_Layers[iLayer].m_Kind != xanim_package_desc::layer_kind::KIMODO) return "KimodoRegenerate: -Layer is not a Kimodo layer of the selected clip";
            const auto L = pOv->m_Layers[iLayer];
            if (auto* pWhy = KimodoWhyNot()) return std::string("KimodoRegenerate: ") + pWhy;
            const auto Exe = kimodo::FindExe();
            if (Exe.empty()) return "KimodoRegenerate: xkimodo.exe not found (build dependencies/xKimodo, or set XKIMODO_EXE)";
            std::string Err;
            auto* pCtx = KimodoRetargetCtx(Err);
            if (!pCtx) return "KimodoRegenerate: " + Err;
            std::vector<xmath::transform3> Below; int FPS = 30, nF = 0;
            if (!StackFramesFor(iLayer, Below, FPS, nF, Err)) return "KimodoRegenerate: " + Err;
            const int nB = int(pDesc->m_ResolvedSkeletonBones.m_Bones.size());
            if (nB == 0 || int(Below.size()) != nF * nB) return "KimodoRegenerate: the clip and the skeleton disagree (compile the package)";
            const double T0 = L.m_Start / 6000.0, T1 = (L.m_End >= 0 ? L.m_End : TLClipTicks()) / 6000.0;
            const int W = std::clamp(int(std::lround((T1 - T0) * 30.0)) + 1, 2, 301);
            std::vector<xmath::transform3> Win(std::size_t(W) * nB);
            for (int i = 0; i < W; ++i) { const auto P = SamplePose(Below, FPS, nF, nB, T0 + i / 30.0); std::copy(P.begin(), P.end(), Win.begin() + std::size_t(i) * nB); }
            kimodo_space KS; std::vector<kimodo::soma_pose> SP;
            if (!ToKimodo(*pCtx, Win, W, nB, KS, SP, Err)) return "KimodoRegenerate: " + Err;

            // the constraints rule
            std::vector<int> Full{ 0, W - 1 };
            for (const auto& O : pOv->m_Layers)
                if (O.m_Kind == xanim_package_desc::layer_kind::POSE && O.m_bEnabled)
                    for (const auto& K : O.m_Keys)
                    {
                        const int f = int(std::lround((K.m_Tick / 6000.0 - T0) * 30.0));
                        if (f > 0 && f < W - 1 && std::find(Full.begin(), Full.end(), f) == Full.end()) Full.push_back(f);
                    }
            std::sort(Full.begin(), Full.end());
            if (Full.size() > 16) { std::vector<int> F; for (int i = 0; i < 16; ++i) F.push_back(Full[std::size_t(std::lround(i * (Full.size() - 1) / 15.0))]); Full = F; }
            kimodo::constraint_set FB{ "fullbody", {}, {}, {} };
            for (int f : Full) { FB.m_Frames.push_back(f); FB.m_Poses.push_back(SP[f]); FB.m_Smooth.push_back({ SP[f].m_Root[0], SP[f].m_Root[2] }); }
            kimodo::constraint_set Path{ "root2d", {}, {}, {} };
            for (int i = 0; i < W; ++i) { Path.m_Frames.push_back(i); Path.m_Smooth.push_back({ SP[i].m_Root[0], SP[i].m_Root[2] }); }
            const std::vector<kimodo::constraint_set> Sets{ FB, Path };
            static int s_Count = 0;
            const auto Stem = std::format(L"{}\\kimlayer_{}_{}", m_Document.m_TempDir, GetCurrentProcessId(), ++s_Count);
            std::filesystem::create_directories(m_Document.m_TempDir);
            if (!kimodo::WriteConstraints(Stem + L".json", Sets, Err)) return "KimodoRegenerate: " + Err;

            std::string Text = std::format("KimodoRegenerate: layer {} '{}', {:.2f} .. {:.2f} s ({} frames at 30 fps), \"{}\", {} steps\n", iLayer, L.m_Name, T0, T1, W, L.m_Prompt, L.m_Steps);
            Text += std::format("  constraints: full-body poses of the stack below at frames {}; the root path of the stack below at all {} frames\n", [&] { std::string S; for (int f : Full) S += (S.empty() ? "" : " ") + std::to_string(f); return S; }(), W);
            // the seam: the candidate's first and last frame against the stack below (positions, the editor's FK)
            std::vector<std::int32_t> Parents(nB);
            for (int b = 0; b < nB; ++b) Parents[b] = pDesc->m_ResolvedSkeletonBones.m_Bones[b].m_iParent;
            xanimation::body_ik FK; FK.Setup(Parents);
            auto Pos = [&](const std::vector<xmath::transform3>& F, int f) { xanimation::pose P; P.resize(nB); for (int b = 0; b < nB; ++b) P.set(b, F[std::size_t(f) * nB + b]); FK.ComputeWorld(P); std::vector<xmath::fvec3> V(nB); for (int b = 0; b < nB; ++b) V[b] = FK.WorldPosition(b); return V; };
            const auto B0 = Pos(Win, 0), B1 = Pos(Win, W - 1);

            // in the background (T4b): the seeds run side by side (bounded) through the xkimodo client, polled once a frame; this returns at once
            for (auto& J : m_KLJobs) J.Cancel();
            m_KL = {};
            m_KL.m_LayerId = L.m_Id; m_KL.m_Sig = LayersSigNow(); m_KL.m_ReqSig = KimodoReqSig(L); m_KL.m_KS = KS; m_KL.m_Start = L.m_Start; m_KL.m_W = W;
            m_KL.m_Exe = Exe; m_KL.m_Stem = Stem; m_KL.m_Steps = std::clamp(L.m_Steps, 1, 200); m_KL.m_B0 = B0; m_KL.m_B1 = B1;
            m_KL.m_Prompt = xstrtool::To(L.m_Prompt.empty() ? std::string("A person.") : L.m_Prompt);
            for (auto& c : m_KL.m_Prompt) if (c == L'"' || c == L'\n' || c == L'\r') c = L' ';
            std::istringstream In(Seeds.empty() ? std::to_string(L.m_Seed) : Seeds); int Seed = 0;
            while (In >> Seed) m_KL.m_Pending.push_back(Seed);
            if (m_KL.m_Pending.empty()) return Text + "KimodoRegenerate: no seed";
            m_KL.m_Text = Text;
            const int nSeeds = int(m_KL.m_Pending.size());
            if (!KimodoLayerStartNext(Err)) return Text + "KimodoRegenerate: " + Err;
            return Text + std::format("  running in the background ({} seed(s), up to {} at once); the row shows it; KimodoRegenerate -Wait 1 waits", nSeeds, kimodo_jobs_v);
        }

        // starts pending seeds on every free client slot
        bool KimodoLayerStartNext(std::string& Err) noexcept
        {
            for (int j = 0; j < kimodo_jobs_v && !m_KL.m_Pending.empty(); ++j)
            {
                if (m_KLJobs[j].m_bRunning) continue;
                const int Seed = m_KL.m_Pending.front(); m_KL.m_Pending.erase(m_KL.m_Pending.begin());
                const auto Bvh = std::format(L"{}_s{}.bvh", m_KL.m_Stem, Seed);
                const auto Args = std::format(L"gen --prompt \"{}\" --frames {} --steps {} --seed {} --constraints \"{}.json\" --bvh \"{}\"", m_KL.m_Prompt, m_KL.m_W, m_KL.m_Steps, Seed, m_KL.m_Stem, Bvh);
                if (!m_KLJobs[j].StartArgs(m_KL.m_Exe, Args, Bvh, std::format(L"{}_s{}.log", m_KL.m_Stem, Seed), Err)) { m_KL.m_Text += std::format("  seed {}: {}\n", Seed, Err); continue; }
                m_KL.m_Job = "running";
                m_KL.m_Cands.push_back({ .m_Seed = Seed, .m_Bvh = Bvh, .m_iJob = j });     // its frames when it ends
            }
            if (m_KL.m_Job != "running") { m_KL.m_Job = "failed"; m_KL.m_LastError = Err.empty() ? "no seed could start" : Err; return false; }
            return true;
        }

        // Once a frame: a finished seed becomes a candidate - only for the same layer with the same request and span (a late result never
        // binds to a changed, moved or deleted segment: it is dropped as stale)
        void KimodoLayerPoll() noexcept
        {
            for (int j = 0; j < kimodo_jobs_v; ++j)
            {
                bool Ok = false; std::string Msg;
                if (!m_KLJobs[j].Poll(Ok, Msg) || m_KL.m_Job != "running") continue;
                int c = -1; for (int i = 0; i < int(m_KL.m_Cands.size()); ++i) if (m_KL.m_Cands[i].m_iJob == j && !m_KL.m_Cands[i].m_bDone) c = i;
                if (c >= 0) KimodoLayerFinish(c, Ok, Msg);
                if (m_KL.m_Job != "running") { for (auto& J : m_KLJobs) J.Cancel(); return; }      // stale: the other seeds are dropped too
            }
            if (m_KL.m_Job != "running") return;
            std::string Err;
            if (!m_KL.m_Pending.empty()) { KimodoLayerStartNext(Err); return; }
            bool bBusy = false; for (const auto& J : m_KLJobs) bBusy = bBusy || J.m_bRunning;
            if (bBusy) return;
            // all seeds ended: the candidates that made it (a failed seed stays listed, not done, never shown or accepted)
            if (std::none_of(m_KL.m_Cands.begin(), m_KL.m_Cands.end(), [](const auto& C) { return C.m_bDone; })) { m_KL.m_Job = "failed"; if (m_KL.m_LastError.empty()) m_KL.m_LastError = "no seed made a candidate"; return; }
            m_KL.m_Job = "candidate";
        }

        // one seed ended: its candidate (or why not); a late result for a changed segment makes the whole run stale
        void KimodoLayerFinish(int c, bool Ok, const std::string& Msg) noexcept
        {
            auto& C = m_KL.m_Cands[c];
            const int Slot = C.m_iJob; C.m_iJob = -1;
            const xanim_package_desc::clip_layer* pL = nullptr;
            if (auto* pOv = SelectedOv()) for (const auto& L : pOv->m_Layers) if (L.m_Id == m_KL.m_LayerId) pL = &L;
            if (!pL || KimodoReqSig(*pL) != m_KL.m_ReqSig) { m_KL.m_Job = "stale"; m_KL.m_LastError = "the segment changed while it generated (moved, edited or deleted): regenerate"; m_KL.m_Cands.clear(); m_KL.m_Pending.clear(); return; }
            std::string Err;
            auto* pCtx = KimodoRetargetCtx(Err); auto* pDesc = Desc();
            kimodo::bvh B;
            if (!Ok || !pCtx || !pDesc || !B.Load(C.m_Bvh, Err) || B.m_nFrames < m_KL.m_W)
            { m_KL.m_LastError = !Ok ? (Msg.empty() ? "xkimodo failed" : Msg) : Err.empty() ? "too few frames" : Err; m_KL.m_Text += std::format("  seed {}: {}\n", C.m_Seed, m_KL.m_LastError); return; }
            const int nB = int(pDesc->m_ResolvedSkeletonBones.m_Bones.size()), W = m_KL.m_W;
            C.m_Frames = FromKimodo(*pCtx, B, m_KL.m_KS, nB); C.m_Frames.resize(std::size_t(W) * nB);
            std::vector<std::int32_t> Parents(nB);
            for (int b = 0; b < nB; ++b) Parents[b] = pDesc->m_ResolvedSkeletonBones.m_Bones[b].m_iParent;
            xanimation::body_ik FK; FK.Setup(Parents);
            auto Pos = [&](int f) { xanimation::pose P; P.resize(nB); for (int b = 0; b < nB; ++b) P.set(b, C.m_Frames[std::size_t(f) * nB + b]); FK.ComputeWorld(P); std::vector<xmath::fvec3> V(nB); for (int b = 0; b < nB; ++b) V[b] = FK.WorldPosition(b); return V; };
            const auto C0 = Pos(0), C1 = Pos(W - 1);
            if (int(m_KL.m_B0.size()) == nB) for (int b = 0; b < nB; ++b) C.m_SeamCm = std::max({ C.m_SeamCm, (C0[b] - m_KL.m_B0[b]).Length() * 100.0f, (C1[b] - m_KL.m_B1[b]).Length() * 100.0f });
            C.m_Ms = (kimodo::job::Now() - m_KLJobs[Slot].m_StartTime) * 1000.0;
            C.m_bDone = true;
            if (!KimodoLayerDone(m_KL.m_iShow)) m_KL.m_iShow = c;          // the first one ready is shown
            m_KL.m_Text += std::format("  candidate {} (seed {}): {:.0f} ms; the seam (first/last frame vs the stack below) max {:.2f} cm\n", c, C.m_Seed, C.m_Ms, C.m_SeamCm);
        }

        // a generated window into the clip's space: the root's channels un-normalized (Kimodo starts at the origin facing +Z), the rest kept
        static bool DenormalizeBvh(const std::wstring& Src, const std::wstring& Dst, const kimodo_space& KS, std::string& Err) noexcept
        {
            std::ifstream In{ std::filesystem::path(Src) };
            if (!In) { Err = "cannot open the generated BVH"; return false; }
            std::stringstream Head; std::string Line, RootChannels;
            bool bRoot = false;
            while (std::getline(In, Line))
            {
                Head << Line << '\n';
                if (!bRoot && Line.find("CHANNELS") != std::string::npos) { RootChannels = Line; bRoot = true; }
                if (Line.find("Frame Time") != std::string::npos) break;
            }
            std::istringstream CS(RootChannels); std::string Tok; int n = 0; CS >> Tok >> n;
            std::string Order; int nPos = 0;
            for (int i = 0; i < n; ++i) { CS >> Tok; if (Tok.find("position") != std::string::npos) ++nPos; else Order += char(std::toupper(Tok[0])); }
            if (Order.size() != 3 || nPos != 3) { Err = "the root's channels are not 3 positions + 3 rotations"; return false; }
            auto Ax = [](char A) { return A == 'X' ? 0 : A == 'Y' ? 1 : 2; };
            const int i0 = Ax(Order[0]), i1 = Ax(Order[1]), i2 = Ax(Order[2]);
            const float s = ((i1 - i0 + 3) % 3 == 1) ? 1.0f : -1.0f;                // cyclic order (XYZ, YZX, ZXY): +1
            // the denormalizing turn as a matrix (about Y)
            float Dm[9]; { const auto& q = KS.m_Denorm; const float x = q.m_X, y = q.m_Y, z = q.m_Z, w = q.m_W;
                Dm[0] = 1 - 2 * (y * y + z * z); Dm[1] = 2 * (x * y - z * w); Dm[2] = 2 * (x * z + y * w);
                Dm[3] = 2 * (x * y + z * w); Dm[4] = 1 - 2 * (x * x + z * z); Dm[5] = 2 * (y * z - x * w);
                Dm[6] = 2 * (x * z - y * w); Dm[7] = 2 * (y * z + x * w); Dm[8] = 1 - 2 * (x * x + y * y); }
            std::ofstream Out{ std::filesystem::path(Dst) };
            if (!Out) { Err = "cannot write the take"; return false; }
            Out << Head.str();
            Out.setf(std::ios::fixed); Out.precision(6);
            while (std::getline(In, Line))
            {
                if (Line.find_first_not_of(" \t\r") == std::string::npos) continue;
                std::istringstream LS(Line); std::vector<std::string> V; while (LS >> Tok) V.push_back(Tok);
                if (V.size() < 6) { Out << Line << '\n'; continue; }
                const float p[3] = { std::stof(V[0]), std::stof(V[1]), std::stof(V[2]) };
                float R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                for (int k = 0; k < 3; ++k) { float M[9]; kimodo::bvh::Axis(Order[k], std::stof(V[3 + k]), M); kimodo::bvh::Mul(R, M, R); }
                float R2[9]; kimodo::bvh::Mul(Dm, R, R2);
                const xmath::fvec3 P2 = KS.m_Denorm * xmath::fvec3(p[0], p[1], p[2]) + KS.m_Origin;
                // R2 = R_a(A) R_b(B) R_c(C) in the root's channel order
                auto M = [&](int r, int c) { return R2[r * 3 + c]; };
                const float B = std::asin(std::clamp(s * M(i0, i2), -1.0f, 1.0f));
                const float A = std::atan2(-s * M(i1, i2), M(i2, i2));
                const float C = std::atan2(-s * M(i0, i1), M(i0, i0));
                constexpr float D = 57.29577951f;
                Out << P2.m_X << ' ' << P2.m_Y << ' ' << P2.m_Z << ' ' << A * D << ' ' << B * D << ' ' << C * D;
                for (std::size_t k = 6; k < V.size(); ++k) Out << ' ' << V[k];
                Out << '\n';
            }
            return bool(Out);
        }

        std::string KimodoAcceptCandidate(int iCand) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || !pDesc) return "KimodoAcceptCandidate: select a clip first";
            if ((m_KL.m_Job != "candidate" && m_KL.m_Job != "running") || !KimodoLayerDone(iCand)) return "KimodoAcceptCandidate: no such candidate ready (KimodoRegenerate first, or wait for that seed)";
            for (auto& J : m_KLJobs) J.Cancel();                    // accepting ends the run: the seeds still generating are dropped
            int iL = -1;
            for (int i = 0; i < int(pOv->m_Layers.size()); ++i) if (pOv->m_Layers[i].m_Id == m_KL.m_LayerId) iL = i;
            if (iL < 0) return "KimodoAcceptCandidate: the layer is gone";
            if (m_KL.m_Sig != LayersSigNow()) return "KimodoAcceptCandidate: the candidate is stale (the clip's layers changed since): KimodoRegenerate again";
            const auto& C = m_KL.m_Cands[iCand];
            const auto& L = pOv->m_Layers[iL];
            const std::wstring Folder = pDesc->m_Kimodo.m_Folder;
            const auto Dir = ProjectPath() / Folder;
            std::error_code Ec; std::filesystem::create_directories(Dir, Ec);
            const auto Name = SafeFileName(kimodo::NameFromPrompt(L.m_Prompt.empty() ? std::string("take") : L.m_Prompt)) + "_take";
            std::wstring File = xstrtool::To(Name) + L".bvh";
            for (int i = 2; std::filesystem::exists(Dir / File); ++i) File = std::format(L"{}_{}.bvh", xstrtool::To(Name), i);
            std::string Err;
            if (!DenormalizeBvh(C.m_Bvh, (Dir / File).wstring(), m_KL.m_KS, Err)) return "KimodoAcceptCandidate: " + Err;
            {
                std::ofstream Info(Dir / (std::filesystem::path(File).stem().wstring() + L".kimodo.txt"));
                Info << "generator: NVIDIA Kimodo (Kimodo-SOMA-RP-v1.2) through xkimodo\nfor: Kimodo layer '" << L.m_Name << "' of clip '" << pOv->m_Name << "'\nprompt: " << L.m_Prompt
                     << "\nseed: " << C.m_Seed << "\nsteps: " << L.m_Steps << "\nframes: " << m_KL.m_W << " at 30 fps from clip tick " << m_KL.m_Start
                     << "\nconstraints: full-body poses of the stack below at the segment's ends and Pose-layer key times inside, the root path of the stack below\n";
            }
            const auto Rel = Folder + L"\\" + File;
            const auto Stem = xstrtool::To(std::filesystem::path(File).stem().wstring());
            const auto ReqSig = m_KL.m_ReqSig;
            const int Seed = C.m_Seed;
            const auto E = DirectOneStep(std::format("Accept Kimodo take for '{}'", L.m_Name), [&](desc& D) -> std::string
            {
                auto& S = D.m_ImportSources.emplace_back();
                S.m_Kind = xanim_package_desc::import_kind::DIFFERENT_SKELETON_ASSET; S.m_Path = Rel; S.m_Retarget = D.m_Kimodo.m_Retarget;
                auto& Cl = S.m_Clips.emplace_back(); Cl.m_OriginalName = "Motion"; Cl.m_Name = Stem; Cl.m_bDelete = true;       // a layer's source, not an output clip
                auto& LL = SelectedOv()->m_Layers[iL];
                LL.m_SourceKey = Rel; LL.m_SourceClip = "Motion"; LL.m_TrimIn = 0; LL.m_TrimOut = -1; LL.m_Rate = 1.0; LL.m_bReverse = false;
                LL.m_Seed = Seed; LL.m_AcceptedSig = KimodoReqSig(LL);
                (void)ReqSig;
                return {};
            });
            if (!E.empty()) return "KimodoAcceptCandidate: " + E;
            m_KAcceptedLayer = m_KL.m_LayerId; m_KAcceptedFrames = m_KL.m_Cands[iCand].m_Frames; m_KAcceptedW = m_KL.m_W;
            m_KL = {};
            (void)m_Undo.Query("Save");
            (void)m_Undo.Query("Compile");
            return std::format("KimodoAcceptCandidate: candidate {} (seed {}) is the take of layer {} ({}); one undo step; the package is compiling (the earlier take stays on disk)", iCand, Seed, iL, xstrtool::To(Rel));
        }

        // A Kimodo layer as an editable Pose layer: absolute keys (every Every frames) of what the clip plays over the segment, placed right
        // above it; the Kimodo layer is turned off (only one of the two plays) and the take is kept, with the provenance on the Pose layer
        std::string BakeToPose(int iLayer, int Every) noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size()) || pOv->m_Layers[iLayer].m_Kind != xanim_package_desc::layer_kind::KIMODO) return "BakeToPose: -Layer is not a Kimodo layer of the selected clip";
            const auto L = pOv->m_Layers[iLayer];
            if (L.m_SourceKey.empty() || !L.m_bEnabled) return "BakeToPose: the layer has no take playing (accept a candidate, turn it on)";
            const std::vector<xmath::transform3>* pF = nullptr; int FPS = 30, nF = 0; std::string Err;
            if (!SuggestClipFrames(pF, FPS, nF, Err)) return "BakeToPose: " + Err;
            const auto Played = *pF;
            const auto& Man = pDesc->m_ResolvedSkeletonBones.m_Bones;
            const int nB = int(Man.size());
            std::vector<xmath::transform3> Below; int FPSb = 30, nFb = 0;
            if (!StackFramesFor(iLayer, Below, FPSb, nFb, Err)) return "BakeToPose: " + Err;
            const double T0 = L.m_Start / 6000.0, T1 = (L.m_End >= 0 ? L.m_End : TLClipTicks()) / 6000.0;
            Every = std::max(1, Every);
            // the keys on the clip's own frames (the composition samples there): keys between frames would be interpolated twice
            std::vector<double> Times;
            for (int f = int(std::ceil(T0 * FPS - 1e-6)); f / double(FPS) < T1 - 1e-6; f += Every) Times.push_back(f / double(FPS));
            if (Times.empty() || Times.front() > T0 + 1e-6) Times.insert(Times.begin(), T0);
            Times.push_back(T1);
            // the bones the layer changes (anywhere in the span), the root bone excluded (it carries the clip's motion)
            std::vector<char> Use(nB, 0);
            int iHips = -1;
            for (double t : Times)
            {
                const auto A = SamplePose(Played, FPS, nF, nB, t), B = SamplePose(Below, FPSb, nFb, nB, t);
                for (int b = 0; b < nB; ++b) if (Man[b].m_iParent >= 0 && AngleDeg(A[b].m_Rotation, B[b].m_Rotation) > 0.05f) Use[b] = 1;
            }
            for (int b = 0; b < nB && iHips < 0; ++b) if (Man[b].m_iParent >= 0 && Man[Man[b].m_iParent].m_iParent < 0) iHips = b;
            const auto Name = L.m_Name + " (baked)";
            int nKeys = 0;
            const auto E = DirectOneStep(std::format("Bake '{}' to a Pose layer", L.m_Name), [&](desc& D) -> std::string
            {
                auto* pO = SelectedOv();
                xanim_package_desc::clip_layer P;
                P.m_Id = NextLayerId(D); P.m_Name = Name; P.m_Kind = xanim_package_desc::layer_kind::POSE; P.m_Mode = xanim_package_desc::layer_mode::OVERRIDE;
                P.m_bSmooth = true; P.m_Falloff = 0.0f; P.m_Parent = L.m_Parent; P.m_Start = L.m_Start; P.m_End = L.m_End;
                P.m_Mask = L.m_Mask; P.m_SharedMask = L.m_SharedMask;
                for (double t : Times)
                {
                    const auto A = SamplePose(Played, FPS, nF, nB, t);
                    const auto Tick = std::int64_t(std::llround(t * 6000.0));
                    for (int b = 0; b < nB; ++b)
                    {
                        if (!Use[b] && b != iHips) continue;
                        xanim_package_desc::layer_pose_key K; K.m_Tick = Tick; K.m_Bone = Man[b].m_Name;
                        const auto Q = A[b].m_Rotation.NormalizeCopy(); K.m_X = Q.m_X; K.m_Y = Q.m_Y; K.m_Z = Q.m_Z; K.m_W = Q.m_W;
                        if (b == iHips) { K.m_bPosition = true; K.m_PX = A[b].m_Position.m_X; K.m_PY = A[b].m_Position.m_Y; K.m_PZ = A[b].m_Position.m_Z; }
                        P.m_Keys.push_back(K); ++nKeys;
                    }
                }
                xanim_package_desc::layer_take Tk; Tk.m_File = std::filesystem::path(L.m_SourceKey).stem().string(); Tk.m_Prompt = L.m_Prompt; Tk.m_Seed = L.m_Seed; Tk.m_Steps = L.m_Steps;
                Tk.m_Start = L.m_Start; Tk.m_End = L.m_End; Tk.m_Offset = L.m_Start; Tk.m_Why = std::format("baked from Kimodo layer '{}' ({})", L.m_Name, xstrtool::To(L.m_SourceKey));
                P.m_Takes.push_back(Tk);
                pO->m_Layers[iLayer].m_bEnabled = false;
                pO->m_Layers.insert(pO->m_Layers.begin() + iLayer + 1, std::move(P));
                return {};
            });
            if (!E.empty()) return "BakeToPose: " + E;
            // what the clip plays now against before (the keys reproduce it)
            std::string Note;
            if (SuggestClipFrames(pF, FPS, nF, Err))
            {
                float Max = 0, MaxAll = 0; std::string MB; double MT = 0;
                for (int f = int(std::ceil(T0 * FPS - 1e-6)); f / double(FPS) <= T1 + 1e-9; ++f)
                {
                    const double t = f / double(FPS);
                    const auto A = SamplePose(Played, FPS, nF, nB, t), B = SamplePose(*pF, FPS, nF, nB, t);
                    for (int b = 0; b < nB; ++b)
                    {
                        const float a = AngleDeg(A[b].m_Rotation, B[b].m_Rotation);
                        if (Use[b] && a > Max) { Max = a; MB = Man[b].m_Name; MT = t; }
                        MaxAll = std::max(MaxAll, a);
                    }
                }
                Note = std::format("; the clip now against before, every frame of the span ({} fps): max {:.4f} deg ({} at {:.4f} s), all bones {:.4f} deg", FPS, Max, MB, MT, MaxAll);
            }
            return std::format("BakeToPose: {} keys on new Pose layer '{}' (layer {}), the Kimodo layer off, its take kept; one undo step{}", nKeys, Name, iLayer + 1, Note);
        }

        // The refine's stored takes (a Pose layer's Takes) as Kimodo layers: their request (prompt, seed, steps, span), off and with no take
        // (their windows were made in Kimodo's own frame: KimodoRegenerate makes a take for the layer). Idempotent: a take already turned
        // into a layer (by its name) is skipped. Never both playing: the new layers are off.
        std::string KimodoTakesToLayers(int iLayer) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv || iLayer < 0 || iLayer >= int(pOv->m_Layers.size())) return "KimodoTakesToLayers: -Layer is not a layer of the selected clip";
            const auto Takes = pOv->m_Layers[iLayer].m_Takes;
            if (Takes.empty()) return "KimodoTakesToLayers: that layer has no stored takes";
            // re-attach (T4b): each stored window (Kimodo's own frame) is re-oriented into the clip's space - the same turn and origin the refine
            // used, from the clip's root at the window's first frame - and becomes the accepted take of a segment; all the takes of this
            // layer are ONE row (their overlaps crossfade). The segments are off: the Pose layer's keys already play this motion (never both).
            auto* pDesc = Desc();
            std::string Err;
            auto* pCtx = KimodoRetargetCtx(Err);
            const std::vector<xmath::transform3>* pF = nullptr; int FPS = 30, nF = 0;
            const bool bFrames = pCtx && SuggestClipFrames(pF, FPS, nF, Err);
            const auto TakeDir = ProjectPath() / L"Cache" / L"KimodoTakes" / std::format(L"{:016X}", m_Document.m_Guid.m_Instance.m_Value);
            const std::wstring Folder = pDesc ? pDesc->m_Kimodo.m_Folder : std::wstring(L"Assets\\Kimodo");
            const auto Dir = ProjectPath() / Folder;
            std::error_code Ec; std::filesystem::create_directories(Dir, Ec);
            struct attached { std::wstring m_Rel; std::string m_Stem; };
            std::vector<attached> Att(Takes.size());
            int nAtt = 0;
            for (std::size_t i = 0; i < Takes.size() && bFrames; ++i)
            {
                const auto& T = Takes[i];
                const auto Src = TakeDir / (xstrtool::To(T.m_File) + L".bvh");
                kimodo::bvh B;
                if (!std::filesystem::exists(Src) || !B.Load(Src.wstring(), Err)) continue;
                const int nB = int(pDesc->m_ResolvedSkeletonBones.m_Bones.size()), W = B.m_nFrames;
                std::vector<xmath::transform3> Win(std::size_t(W) * nB);
                for (int f = 0; f < W; ++f) { const auto P = SamplePose(*pF, FPS, nF, nB, T.m_Offset / 6000.0 + f / 30.0); std::copy(P.begin(), P.end(), Win.begin() + std::size_t(f) * nB); }
                kimodo_space KS; std::vector<kimodo::soma_pose> SP;
                if (!ToKimodo(*pCtx, Win, W, nB, KS, SP, Err)) continue;
                std::wstring File = xstrtool::To(T.m_File) + L".bvh";
                if (!std::filesystem::exists(Dir / File) && !DenormalizeBvh(Src.wstring(), (Dir / File).wstring(), KS, Err)) continue;
                Att[i] = { Folder + L"\\" + File, T.m_File }; ++nAtt;
            }
            int nNew = 0;
            const std::uint64_t RowId = pOv->m_Layers[iLayer].m_Id ? pOv->m_Layers[iLayer].m_Id : 0x7A4E0000ull + std::uint64_t(iLayer);
            const auto E = DirectOneStep("Takes to Kimodo segments", [&](desc& D) -> std::string
            {
                auto* pO = SelectedOv();
                for (std::size_t i = 0; i < Takes.size(); ++i)
                {
                    const auto& T = Takes[i];
                    const auto Name = "Take " + T.m_File;
                    if (std::any_of(pO->m_Layers.begin(), pO->m_Layers.end(), [&](const auto& X) { return X.m_Name == Name; })) continue;
                    if (!Att[i].m_Rel.empty() && D.findImportSource(Att[i].m_Rel) == -1)
                    {
                        auto& S = D.m_ImportSources.emplace_back();
                        S.m_Kind = xanim_package_desc::import_kind::DIFFERENT_SKELETON_ASSET; S.m_Path = Att[i].m_Rel; S.m_Retarget = D.m_Kimodo.m_Retarget;
                        auto& Cl = S.m_Clips.emplace_back(); Cl.m_OriginalName = "Motion"; Cl.m_Name = Att[i].m_Stem; Cl.m_bDelete = true;    // a layer's source, not an output clip
                    }
                    pO = SelectedOv();
                    auto& L = pO->m_Layers.emplace_back();
                    L.m_Id = NextLayerId(D); L.m_Name = Name; L.m_Kind = xanim_package_desc::layer_kind::KIMODO; L.m_bEnabled = false;
                    L.m_Prompt = T.m_Prompt; L.m_Seed = T.m_Seed; L.m_Steps = T.m_Steps; L.m_Start = T.m_Start; L.m_End = std::max(T.m_End, T.m_Start + 1);
                    L.m_Seconds = float((L.m_End - L.m_Start) / 6000.0); L.m_bPosition = true; L.m_Row = RowId;
                    if (!Att[i].m_Rel.empty())
                    {
                        L.m_SourceKey = Att[i].m_Rel; L.m_SourceClip = "Motion";
                        L.m_TrimIn = std::max<std::int64_t>(0, T.m_Start - T.m_Offset);     // the take's frame 0 is clip time Offset
                        L.m_AcceptedSig = KimodoReqSig(L);
                    }
                    ++nNew;
                }
                return {};
            });
            if (!E.empty() && E != "no change") return "KimodoTakesToLayers: " + E;
            if (nAtt) { (void)m_Undo.Query("Save"); (void)m_Undo.Query("Compile"); }
            return std::format("KimodoTakesToLayers: {} new Kimodo segment(s) in one row, {} with their stored take re-attached (off: the Pose layer's keys play this motion; never both){}; {} already there"
                , nNew, nAtt, nAtt ? "; the package is compiling" : "", int(Takes.size()) - nNew);
        }

        std::string KimodoTakeCheck() noexcept
        {
            auto* pOv = SelectedOv(); auto* pDesc = Desc();
            if (!pOv || !pDesc || m_KAcceptedFrames.empty()) return "KimodoTakeCheck: no accepted candidate this session";
            const xanim_package_desc::clip_layer* pL = nullptr;
            for (const auto& L : pOv->m_Layers) if (L.m_Id == m_KAcceptedLayer) pL = &L;
            if (!pL || pL->m_SourceKey.empty()) return "KimodoTakeCheck: the layer or its take is gone";
            const auto* pSrc = FindLayerSource(pL->m_SourceKey, pL->m_SourceClip);
            if (!pSrc) return "KimodoTakeCheck: the take is not in the layer sources yet (compile)";
            const int nB = int(pDesc->m_ResolvedSkeletonBones.m_Bones.size());
            const int n = std::min(m_KAcceptedW, pSrc->m_nFrames);
            float Max = 0, Pos = 0; std::string MB;
            for (int f = 0; f < n; ++f)
                for (int b = 0; b < nB; ++b)
                {
                    const auto& A = m_KAcceptedFrames[std::size_t(f) * nB + b]; const auto& B = pSrc->m_Frames[std::size_t(f) * nB + b];
                    const float a = AngleDeg(A.m_Rotation, B.m_Rotation);
                    if (a > Max) { Max = a; MB = pDesc->m_ResolvedSkeletonBones.m_Bones[b].m_Name; }
                    if (pDesc->m_ResolvedSkeletonBones.m_Bones[b].m_iParent >= 0) Pos = std::max(Pos, (A.m_Position - B.m_Position).Length());
                }
            return std::format("KimodoTakeCheck: {} frames (take {}, candidate {}): rotations max {:.4f} deg ({}), positions max {:.5f}", n, pSrc->m_nFrames, m_KAcceptedW, Max, MB, Pos);
        }

        // the shown candidate, drawn as the pose at the playhead when it is inside the candidate's span
        void KimodoPreviewBones() noexcept
        {
            if ((m_KL.m_Job != "candidate" && m_KL.m_Job != "running") || !KimodoLayerDone(m_KL.m_iShow)) return;
            const int f = int(std::lround((m_TimeSeconds - m_KL.m_Start / 6000.0) * 30.0));
            if (f < 0 || f >= m_KL.m_W) return;
            std::string Err;
            if (!PoseRig(Err) || int(m_PoseBones.size()) != m_PT.m_Rig.count()) return;
            const int nB = m_PT.m_Rig.count();
            const auto& Fr = m_KL.m_Cands[m_KL.m_iShow].m_Frames;
            if (int(Fr.size()) < (f + 1) * nB) return;
            xanimation::pose C; C.resize(nB);
            for (int b = 0; b < nB; ++b) C.set(b, Fr[std::size_t(f) * nB + b]);
            xanimation::pose Now; xmath::fvec3 Off(0, 0, 0);
            if (PoseAt(m_TimeSeconds, Now, Err)) { xanimation::body_ik FK; FK.Setup(m_PT.m_Rig.m_Par); FK.ComputeWorld(Now); Off = m_PoseBones[0].m_Position - FK.WorldPosition(0); }
            PoseToBones(C, Off);
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
                    // the hips much smoother (~0.13 s): Kimodo's hip path jitters more than the clip's momentum allows; the limbs keep its detail
                    const int nPass = b == iHips ? 40 : 3;
                    for (int pass = 0; pass < nPass; ++pass)
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
            // Balance (big edits): on quasi-static frames whose centre of mass falls outside the planted feet, the hips move horizontally to
            // bring it over them (aiming 15 % past the nearest edge); the shift smoothed in time and zero at every anchor - the key (its lean
            // is the user's intent), the user's keys, the clip's anchors and the edges. A few passes: the legs' IK takes part of the shift.
            int nBalanced = 0; float MaxShift = 0;
            if (iHips >= 0 && Parents[iHips] >= 0)
            {
                const auto Rig = xanimation::MakeHumanoidRealismRig(xanimation::skeleton_view{ Parents }, Views, Rest);
                std::vector<std::uint8_t> IsAnchor(W, 0); for (int f : Anchors) IsAnchor[f] = 1;
                // the centre of mass of a pose (model space) and how far it falls outside the planted feet at frame f (0 inside)
                auto ComOf = [&](const std::vector<xmath::fvec3>& Pp) { xmath::fvec3 C(0, 0, 0); float M = 0; for (int b = 0; b < nB; ++b) if (Rig.m_Mass[b] > 0) { C = C + (Rig.m_MassTip[b] >= 0 ? (Pp[b] + Pp[Rig.m_MassTip[b]]) * 0.5f : Pp[b]) * Rig.m_Mass[b]; M += Rig.m_Mass[b]; } return M > 0 ? C * (1.0f / M) : C; };
                auto Outside = [&](int f, const std::vector<xmath::fvec3>& Pp, const xmath::fvec3& C, std::array<float, 2>* pNr)
                {
                    std::vector<std::array<float, 2>> Pts;
                    for (int s = 0; s < 2; ++s) if (InContact[s][f]) { Pts.push_back({ FootPos[s][f].m_X, FootPos[s][f].m_Z }); if (Rig.m_Toes[s] >= 0) Pts.push_back({ Pp[Rig.m_Toes[s]].m_X, Pp[Rig.m_Toes[s]].m_Z }); }
                    return Pts.empty() ? 0.0f : xanimation::details::OutsideHull(Pts, { C.m_X, C.m_Z }, pNr);
                };
                // the clip's own imbalance at each frame (a walk leans out of its support by itself: only what the edit adds is corrected)
                std::vector<float> ClipOut(W, 0.0f);
                if (Own.size() == Accepted.size())
                    for (int f = A; f <= B; ++f) { const auto Po = SamplePose(Own, FPS, nF, nB, Key + double(f - K) / 30.0); std::vector<xmath::fvec3> Pp; World(Po.data(), Pp); ClipOut[f] = Outside(f, Pp, ComOf(Pp), nullptr); }
                if (int(Rig.m_Mass.size()) == nB)
                    for (int it = 0; it < 3; ++it)
                    {
                        std::vector<std::vector<xmath::fvec3>> Pw(W); std::vector<std::vector<xmath::fquat>> Rw(W); std::vector<xmath::fvec3> Com(W, xmath::fvec3(0, 0, 0));
                        for (int f = 0; f < W; ++f) { World(&Ref[std::size_t(f) * nB], Pw[f], &Rw[f]); Com[f] = ComOf(Pw[f]); }
                        std::vector<xmath::fvec3> S(W, xmath::fvec3(0, 0, 0));
                        int nOut = 0;
                        for (int f = std::max(A, 1); f <= std::min(B, W - 2); ++f)
                        {
                            if (IsAnchor[f]) continue;
                            const auto Ac = (Com[f + 1] - Com[f] * 2.0f + Com[f - 1]) * (900.0f / HipH);
                            if (std::hypot(Ac.m_X, Ac.m_Z) > 0.5f) continue;                       // not quasi-static (the realism measure's rule)
                            std::array<float, 2> Nr{};
                            const float Out = Outside(f, Pw[f], Com[f], &Nr), Excess = Out - ClipOut[f] - 0.02f * HipH;
                            if (Excess <= 0) continue;                                           // no worse than the clip there
                            const auto Dir = xmath::fvec3(Nr[0] - Com[f].m_X, 0, Nr[1] - Com[f].m_Z) * (1.0f / std::max(1e-6f, Out));
                            S[f] = Dir * (Excess * 1.15f);
                            ++nOut;
                        }
                        if (!nOut) break;
                        nBalanced = std::max(nBalanced, nOut);
                        for (int pass = 0; pass < 40; ++pass)               // smooth (~0.13 s, like the hips' path), the anchors held at zero
                        {
                            auto S2 = S;
                            for (int f = A + 1; f < B; ++f) if (!IsAnchor[f]) S2[f] = (S[f - 1] + S[f] * 2.0f + S[f + 1]) * 0.25f;
                            S = S2;
                        }
                        for (int f = A + 1; f < B; ++f)
                        {
                            if (IsAnchor[f] || S[f].LengthSq() < 1e-12f) continue;
                            MaxShift = std::max(MaxShift, S[f].Length());
                            // world -> the hips' local units: its parent's rotation, and the scale between its local offset and its world offset
                            auto& H = Ref[std::size_t(f) * nB + iHips];
                            const int p = Parents[iHips];
                            const float Lw = (Pw[f][iHips] - Pw[f][p]).Length(), Ll = H.m_Position.Length();
                            const float Sc = (Lw > 1e-6f && Ll > 1e-6f) ? Lw / Ll : 1.0f;
                            H.m_Position = H.m_Position + Rw[f][p].ConjugateCopy() * (S[f] * (1.0f / Sc));
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
                                 "skate in contact {:.4f} -> {:.4f} hip heights/s; the key kept within {:.4f} deg; balance: the hips shifted on {} still frames (max {:.1f} cm); time: request {:.0f} ms, Kimodo {:.0f} ms, back + keys {:.0f} ms"
                , double(K - A) / 30.0, double(B - K) / 30.0, W, Chunks.size(), MaxW, Chunks.size() > 1 ? ", stitched" : "", Anchors.size(), nFrames, nKeys, nFootFrames, PinMax * 100.0f, SkateBefore, SkateAfter, KeyMiss, nBalanced, MaxShift * 100.0f, ms(t0, t1), ms(t1, t2), ms(t2, t3));
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

        //----------------------------------------------------------------------------------------
        // Posing (posing v2 P1/P2): handles in the viewport at hands, feet, head and hips; a drag is full-body IK from the pose at the
        // playhead (feet in contact pinned, the pins held, the skeleton's limits when it has them), released = keys on the top Pose layer
        // (made when missing), ONE undo step (SuggestPoseAccept's key writer, no refine: the user's pose exactly; the layer's smooth curve and
        // falloff blend it in). A bone's tip handle turns that bone alone (FK). S = Suggest Pose for the same handles; Tab / Enter / Esc.
        //----------------------------------------------------------------------------------------
        struct pose_rig
        {
            std::vector<std::int32_t>               m_Par;
            std::vector<std::string>                m_Names;
            xanimation::pose                        m_Rest;
            int                                     m_LH = -1, m_RH = -1, m_LF = -1, m_RF = -1, m_Head = -1, m_Hips = -1;
            std::vector<std::uint8_t>               m_Owned;        // the bones a drag may turn: all but the roots
            std::vector<xanimation::joint_limit>    m_Lim;          // the skeleton's compiled limits (JointLimits On), else empty: free
            std::vector<xanimation::two_bone_chain> m_Limbs;
            std::vector<xmath::fvec3>               m_AttachM;      // a handle's point in its bone's frame, model units (palm, sole; 0: the joint)
            std::vector<xmath::fvec3>               m_AttachB;      // the same in the bone's own units (body_ik's ik_task::m_Attach: scaled like the bone)
            int count() const noexcept { return int(m_Par.size()); }
            int Find(std::string_view N) const noexcept
            {
                if (N == "LeftHand") return m_LH; if (N == "RightHand") return m_RH; if (N == "LeftFoot") return m_LF; if (N == "RightFoot") return m_RF;
                if (N == "Head") return m_Head; if (N == "Hips") return m_Hips;
                for (int b = 0; b < count(); ++b) if (m_Names[b] == N) return b;
                return -1;
            }
            const char* Alias(int b) const noexcept
            {
                return b == m_LH ? "LeftHand" : b == m_RH ? "RightHand" : b == m_LF ? "LeftFoot" : b == m_RF ? "RightFoot" : b == m_Head ? "Head" : b == m_Hips ? "Hips" : nullptr;
            }
            std::string Name(int b) const { const char* a = Alias(b); return a ? a : (b >= 0 && b < count() ? m_Names[b] : std::string("?")); }
            std::vector<int> Effectors() const { std::vector<int> E; for (int b : { m_LH, m_RH, m_LF, m_RF, m_Head, m_Hips }) if (b >= 0) E.push_back(b); return E; }
        };
        struct pose_tool
        {
            bool                                m_bOn = false;
            pose_rig                            m_Rig; std::size_t m_RigKey = 0;
            std::set<int>                       m_Pins;                 // handles pinned by the user
            bool                                m_bAutoPins = true;     // the feet in contact at the playhead pinned too
            int                                 m_Drag = -1;            // the handle being dragged
            bool                                m_bFK = false;          // the drag turns that one bone (its tip handle)
            int                                 m_FKBone = -1;          // the bone selected for FK
            xanimation::pose                    m_Base;                 // the pose at the playhead when the drag began
            std::set<int>                       m_DragPins;
            double                              m_Time = 0;
            xmath::fvec3                        m_Offset{ 0, 0, 0 };    // drawn - model (root motion shown)
            xmath::fvec3                        m_Plane{ 0, 0, 0 }, m_Normal{ 0, 0, 1 }, m_Start{ 0, 0, 0 }, m_Target{ 0, 0, 0 };    // drawn space
            xanimation::pose                    m_Preview; bool m_bPreview = false;
            std::string                         m_Note;                 // the last drag: reached / missed by x cm: why
            int                                 m_LastEff = -1;
            std::vector<xmath::transform3>      m_Copy;
            int                                 m_Cand = -1;            // the Suggest candidate previewed
            char                                m_Prompt[128] = "";
            int                                 m_Hover = -1; bool m_bHoverTip = false;
            std::vector<std::pair<int, ImVec2>> m_Screen;               // the handles as drawn last frame (bone, screen)
            ImVec2                              m_TipScreen{ -1, -1 };
            std::string                         m_ContactText;          // the contacts found (feet), for the info
            std::set<int>                       m_Unpinned;             // feet in contact the user unpinned (auto pins they override)
            std::vector<std::pair<int, std::pair<ImVec2, ImVec2>>> m_PinBoxes;   // the pin icons as drawn last frame (handle, box)
            int                                 m_PinHover = -1;
            std::map<int, xmath::fvec3>         m_HandleAt;             // the handles as drawn (drawn space: palm, sole, else the joint)
            bool                                m_bAutoKey = true;      // edits write keys; off: an edit is a pending pose (shown) that K keys
            xanimation::pose                    m_PendingPose; bool m_bPending = false; double m_PendingTime = 0;
            bool                                m_bGizmo = true;        // the selected bone's rotate gizmo
            bool                                m_bGizmoUsing = false, m_bGizmoCancelled = false;
            xanimation::pose                    m_GizmoBase; double m_GizmoTime = 0;
        } m_PT;

        bool PoseRig(std::string& Err) noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) { Err = "nothing loaded"; return false; }
            const auto& Man = pDesc->m_ResolvedSkeletonBones.m_Bones;
            const int nB = int(Man.size());
            if (nB == 0) { Err = "the skeleton's bones are not known yet (compile the package)"; return false; }
            auto* pSk = m_SkeletonRef.empty() ? nullptr : xresource::g_Mgr.getResource(m_SkeletonRef);
            const std::size_t Key = std::size_t(nB) * 1000003u + std::size_t(pSk ? pSk->m_nJoints : 0) + std::hash<std::string>{}(Man[0].m_Name);
            if (m_PT.m_RigKey == Key && m_PT.m_Rig.count() == nB) return true;
            pose_rig R;
            R.m_Par.resize(nB); R.m_Names.resize(nB); R.m_Rest.resize(nB);
            for (int b = 0; b < nB; ++b)
            {
                R.m_Par[b] = Man[b].m_iParent; R.m_Names[b] = Man[b].m_Name;
                R.m_Rest.m_Position[b] = Man[b].m_RestPosition; R.m_Rest.m_Scale[b] = Man[b].m_RestScale;
                R.m_Rest.m_Rotation[b] = xmath::fquat(Man[b].m_RestRotX, Man[b].m_RestRotY, Man[b].m_RestRotZ, Man[b].m_RestRotW).NormalizeSafeCopy();
            }
            std::vector<std::string_view> Views(R.m_Names.begin(), R.m_Names.end());
            const auto Slots = xanimation::details::BuildSlots(xanimation::skeleton_view{ R.m_Par }, Views, R.m_Rest);
            auto Slot = [&](xanimation::bone_slot S, xanimation::bone_side D) { const auto It = Slots.m_Bones.find({ S, D }); return It == Slots.m_Bones.end() ? -1 : It->second[0]; };
            using xanimation::bone_slot; using xanimation::bone_side;
            R.m_LH = Slot(bone_slot::HAND, bone_side::LEFT); R.m_RH = Slot(bone_slot::HAND, bone_side::RIGHT);
            R.m_LF = Slot(bone_slot::FOOT, bone_side::LEFT); R.m_RF = Slot(bone_slot::FOOT, bone_side::RIGHT);
            R.m_Head = Slot(bone_slot::HEAD, bone_side::NONE); R.m_Hips = Slot(bone_slot::HIPS, bone_side::NONE);
            R.m_Owned.assign(nB, 0);
            for (int b = 0; b < nB; ++b) R.m_Owned[b] = R.m_Par[b] >= 0;
            if (pSk && pSk->m_nJoints == nB && pSk->m_nBones == nB)
            {
                R.m_Lim.resize(nB);
                for (int b = 0; b < nB; ++b) R.m_Lim[b] = xskeleton_joints::FromRuntime(pSk->m_pJoints[b], pSk->m_pBoneRests[b].m_RestPose.m_Rotation.NormalizeSafeCopy());
            }
            for (int E : { R.m_LH, R.m_RH, R.m_LF, R.m_RF }) if (E >= 0) if (const int p = R.m_Par[E]; p >= 0 && R.m_Par[p] >= 0) R.m_Limbs.push_back({ R.m_Par[p], p, E });
            // the handles where a hand or a foot is held: the palm (halfway to the middle finger) and the sole (between the foot and its toe,
            // at the toe's height: the ground), when the skeleton has those bones; else the joint itself
            {
                R.m_AttachM.assign(nB, xmath::fvec3(0, 0, 0)); R.m_AttachB.assign(nB, xmath::fvec3(0, 0, 0));
                std::vector<xmath::fquat> Rw; std::vector<xmath::fvec3> Pw, Sc;
                xanimation::details::WorldRPS(xanimation::skeleton_view{ R.m_Par }, R.m_Rest, Rw, Pw, Sc);
                auto Child = [&](int b, const char* pPrefer) { int First = -1; for (int c = b + 1; c < nB; ++c) if (R.m_Par[c] == b) { if (First < 0) First = c; if (pPrefer && xstrtool::findI(R.m_Names[c], pPrefer) != std::string::npos) return c; } return First; };
                auto Set = [&](int b, const xmath::fvec3& Point)
                {
                    const auto A = Rw[b].ConjugateCopy() * (Point - Pw[b]);
                    R.m_AttachM[b] = A;
                    R.m_AttachB[b] = xmath::fvec3(A.m_X / std::max(1e-9f, Sc[b].m_X), A.m_Y / std::max(1e-9f, Sc[b].m_Y), A.m_Z / std::max(1e-9f, Sc[b].m_Z));
                };
                for (int F : { R.m_LF, R.m_RF }) if (F >= 0) if (const int T = Child(F, "toe"); T >= 0) { auto P = (Pw[F] + Pw[T]) * 0.5f; P.m_Y = Pw[T].m_Y; Set(F, P); }
                for (int H : { R.m_LH, R.m_RH })                // the middle finger's base, else the fingers' bases' centre: the palm halfway there
                {
                    if (H < 0) continue;
                    xmath::fvec3 Sum(0, 0, 0); int n = 0, Mid = -1;
                    for (int c = H + 1; c < nB; ++c) if (R.m_Par[c] == H) { Sum = Sum + Pw[c]; ++n; if (xstrtool::findI(R.m_Names[c], "middle") != std::string::npos) Mid = c; }
                    if (n) Set(H, Pw[H] + ((Mid >= 0 ? Pw[Mid] : Sum * (1.0f / n)) - Pw[H]) * 0.5f);
                }
            }
            m_PT.m_Rig = std::move(R); m_PT.m_RigKey = Key;
            return true;
        }

        // where a handle is (its palm or sole when it has one), model space
        xmath::fvec3 HandleModel(const xanimation::body_ik& FK, int b) const noexcept
        {
            const auto& R = m_PT.m_Rig;
            return FK.WorldPosition(b) + (b >= 0 && b < int(R.m_AttachM.size()) ? FK.WorldRotation(b) * R.m_AttachM[b] : xmath::fvec3(0, 0, 0));
        }

        bool PoseAt(double Seconds, xanimation::pose& P, std::string& Err) noexcept
        {
            if (m_PT.m_bPending && std::abs(Seconds - m_PT.m_PendingTime) < 1e-4) { P = m_PT.m_PendingPose; return true; }     // auto-key off: edits build on the pending pose
            const std::vector<xmath::transform3>* pF = nullptr; int FPS = 30, nF = 0;
            if (!SuggestClipFrames(pF, FPS, nF, Err)) return false;
            const int nB = m_PT.m_Rig.count();
            if (nB == 0 || int(pF->size()) != nF * nB) { Err = "the clip and the skeleton disagree on the bones (compile the package)"; return false; }
            const auto S = SamplePose(*pF, FPS, nF, nB, Seconds);
            P.resize(nB); for (int b = 0; b < nB; ++b) P.set(b, S[b]);
            return true;
        }

        // the feet's support contacts over the clip as composed (low and slow: FindContacts on positions in hip heights); [0] left, [1] right
        bool FootContacts(std::vector<xanimation::contact_interval> (&Out)[2], int& FPS, int& nF, float& HipH) noexcept
        {
            Out[0].clear(); Out[1].clear(); HipH = 0;
            const auto& R = m_PT.m_Rig;
            const std::vector<xmath::transform3>* pF = nullptr; std::string Err; FPS = 30; nF = 0;
            if (R.m_LF < 0 || R.m_RF < 0 || R.m_Hips < 0 || !SuggestClipFrames(pF, FPS, nF, Err) || int(pF->size()) != nF * R.count()) return false;
            const int nB = R.count();
            std::vector<xmath::fvec3> Feet[2];
            xanimation::pose P; P.resize(nB); std::vector<xmath::fquat> Rw; std::vector<xmath::fvec3> Pw;
            for (int f = 0; f < nF; ++f)
            {
                for (int b = 0; b < nB; ++b) P.set(b, (*pF)[std::size_t(f) * nB + b]);
                xanimation::details::WorldRP(xanimation::skeleton_view{ R.m_Par }, P, Rw, Pw);
                Feet[0].push_back(Pw[R.m_LF]); Feet[1].push_back(Pw[R.m_RF]);
                if (f == 0) HipH = std::max(1e-3f, Pw[R.m_Hips].m_Y - std::min(Pw[R.m_LF].m_Y, Pw[R.m_RF].m_Y));
            }
            for (int s = 0; s < 2; ++s) { for (auto& v : Feet[s]) v = v * (1.0f / HipH); Out[s] = xanimation::FindContacts(Feet[s], float(FPS)); }
            return true;
        }

        // the feet in contact at a time (the clip as composed: low and slow, FindContacts in hip heights)
        std::set<int> PoseContactsAt(double Seconds) noexcept
        {
            std::set<int> Out;
            const auto& R = m_PT.m_Rig;
            std::vector<xanimation::contact_interval> C2[2]; int FPS = 30, nF = 0; float HipH = 0;
            if (!FootContacts(C2, FPS, nF, HipH)) return Out;
            const int fk = std::clamp(int(std::lround(Seconds * FPS)), 0, std::max(0, nF - 1));
            m_PT.m_ContactText.clear();
            for (int s = 0; s < 2; ++s)
                for (const auto& C : C2[s])
                {
                    m_PT.m_ContactText += std::format("{}{}:{}..{}", m_PT.m_ContactText.empty() ? "" : " ", s == 0 ? "L" : "R", C.m_First, C.m_Last);
                    // a foot just lifting or about to land is still the support: contacts widened by 0.1 s (the detector's low-and-slow
                    // thresholds miss the weight transfer between two contacts)
                    const int Wd = std::max(1, int(std::lround(0.1 * FPS)));
                    if (fk >= C.m_First - Wd && fk <= C.m_Last + Wd) Out.insert(s == 0 ? R.m_LF : R.m_RF);
                }
            m_PT.m_ContactText += std::format(" (frame {} of {}, hip height {:.3f})", fk, nF, HipH);
            return Out;
        }

        //------------------------------------------------------------------------------------------------
        // Clip events (T5): markers, footsteps, contacts, sync points on the selected clip, in composition ticks. Every change is one undo step.
        static constexpr std::uint64_t auto_event_id_v = 0xF000000000000000ull;    // events DetectFootsteps made (it replaces only these)
        std::string EventsPath() const { return std::format("AnimPackage/ImportSources[G:{}]/Clips[G:{}]/Events", m_iSelectedSource, m_iSelectedDescriptorClip); }
        static const char* EventKindName(xanim_package::event_kind K) noexcept { for (const auto& E : xanim_package_desc::event_kind_v) if (E.m_Value == static_cast<std::uint32_t>(K)) return E.m_pName; return "Marker"; }
        static bool EventKindFrom(std::string_view S, xanim_package::event_kind& K) noexcept { for (const auto& E : xanim_package_desc::event_kind_v) if (S == E.m_pName) { K = static_cast<xanim_package::event_kind>(E.m_Value); return true; } return false; }

        std::string EventsInfo() noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "ClipEvent: select a clip first";
            std::string T = std::format("{} events on '{}'", pOv->m_Events.size(), pOv->m_Name);
            for (const auto& E : pOv->m_Events)
                T += std::format("\n  id {:X} {} '{}' at {:.3f} s{}{}", E.m_Id, EventKindName(E.m_Kind), E.m_Name, double(E.m_Tick) / tl_tps_v
                    , E.m_EndTick >= E.m_Tick ? std::format(" to {:.3f} s", double(E.m_EndTick) / tl_tps_v) : std::string{}, E.m_Bone.empty() ? std::string{} : " bone " + E.m_Bone);
            return T;
        }

        // the steps that append one event (the caller runs them inside AsOneStep)
        std::string EventAppend(const xanim_package_desc::clip_event& E) noexcept
        {
            auto* pOv = SelectedOv();
            const int n = int(pOv->m_Events.size());
            const auto P = EventsPath();
            auto Set = [&](const std::string& Path, const std::string& V, const std::string& B) { return m_Undo.Execute(std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(Path), xeditor::Quote(V), xeditor::Quote(B))); };
            if (auto Err = Set(P + "[]", std::to_string(n + 1), std::to_string(n)); !Err.empty()) return Err;
            const auto F = [&](const char* pF) { return std::format("{}[G:{}]/{}", P, n, pF); };
            const auto& D = SelectedOv()->m_Events[n];
            if (auto Err = Set(F("Id"), std::to_string(E.m_Id), std::to_string(D.m_Id)); !Err.empty()) return Err;
            if (auto Err = Set(F("Name"), E.m_Name, D.m_Name); !Err.empty()) return Err;
            if (auto Err = Set(F("Kind"), EventKindName(E.m_Kind), EventKindName(D.m_Kind)); !Err.empty()) return Err;
            if (auto Err = Set(F("Tick"), std::to_string(E.m_Tick), std::to_string(D.m_Tick)); !Err.empty()) return Err;
            if (auto Err = Set(F("EndTick"), std::to_string(E.m_EndTick), std::to_string(D.m_EndTick)); !Err.empty()) return Err;
            if (!E.m_Bone.empty()) if (auto Err = Set(F("Bone"), E.m_Bone, D.m_Bone); !Err.empty()) return Err;
            return {};
        }

        std::uint64_t NewEventId() noexcept
        {
            auto* pOv = SelectedOv(); std::uint64_t Id = 1;
            for (const auto& E : pOv->m_Events) if (E.m_Id < auto_event_id_v) Id = std::max(Id, E.m_Id + 1);
            return Id;
        }

        // ClipEvent: -Add name (with -Kind, -Time, -End, -Bone), -Id id with fields to change, -Delete id, nothing: the list
        std::string ClipEvent(const std::string& Add, const std::string& IdS, const std::string& Del, const std::string& KindS, const std::string& TimeS, const std::string& EndS, const std::string& Bone, const std::string& Name) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "ClipEvent: select a clip first";
            auto Ticks = [](const std::string& S, std::int64_t& T) { double v = 0; if (std::from_chars(S.data(), S.data() + S.size(), v).ec != std::errc() || v < 0) return false; T = std::llround(v * tl_tps_v); return true; };
            auto Find = [&](const std::string& S) -> int { std::uint64_t Id = 0; if (std::from_chars(S.data(), S.data() + S.size(), Id, 16).ec != std::errc()) return -1; for (int i = 0; i < int(pOv->m_Events.size()); ++i) if (pOv->m_Events[i].m_Id == Id) return i; return -1; };
            xanim_package::event_kind K{};
            if (!KindS.empty() && !EventKindFrom(KindS, K)) return "ClipEvent: -Kind Marker|Footstep|Contact|Sync";
            if (!Bone.empty() && PoseRigReady() && m_PT.m_Rig.Find(Bone) < 0) return std::format("ClipEvent: no bone '{}'", Bone);
            if (!Del.empty())
            {
                const int k = Find(Del); if (k < 0) return std::format("ClipEvent: no event {}", Del);
                const auto Nm = pOv->m_Events[k].m_Name;
                if (auto Err = m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(EventsPath()), k)); !Err.empty()) return "ClipEvent: " + Err;
                return std::format("ClipEvent: '{}' deleted", Nm);
            }
            if (!Add.empty())
            {
                xanim_package_desc::clip_event E; E.m_Id = NewEventId(); E.m_Name = Add; E.m_Kind = K;
                E.m_Tick = std::llround(m_TimeSeconds * tl_tps_v);
                if (!TimeS.empty() && !Ticks(TimeS, E.m_Tick)) return "ClipEvent: -Time is seconds";
                if (!EndS.empty() && (!Ticks(EndS, E.m_EndTick) || E.m_EndTick < E.m_Tick)) return "ClipEvent: -End is seconds, not before -Time";
                if (EndS.empty() && K == xanim_package::event_kind::CONTACT) E.m_EndTick = E.m_Tick + tl_tps_v / 4;      // a contact is an interval: a quarter second to start with
                E.m_Bone = Bone;
                if (auto Err = AsOneStep(std::format("Add event {}", Add), [&] { return EventAppend(E); }); !Err.empty()) return "ClipEvent: " + Err;
                return std::format("ClipEvent: '{}' added (id {:X}) at {:.3f} s", Add, E.m_Id, double(E.m_Tick) / tl_tps_v);
            }
            if (!IdS.empty())
            {
                const int k = Find(IdS); if (k < 0) return std::format("ClipEvent: no event {}", IdS);
                const auto E = pOv->m_Events[k];
                const auto F = [&](const char* pF) { return std::format("{}[G:{}]/{}", EventsPath(), k, pF); };
                auto Set = [&](const char* pF, const std::string& V, const std::string& B) { return V == B ? std::string{} : m_Undo.Execute(std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote(F(pF)), xeditor::Quote(V), xeditor::Quote(B))); };
                std::int64_t T = E.m_Tick, End = E.m_EndTick;
                if (!TimeS.empty() && !Ticks(TimeS, T)) return "ClipEvent: -Time is seconds";
                if (!EndS.empty()) { if (EndS == "-1") End = -1; else if (!Ticks(EndS, End)) return "ClipEvent: -End is seconds (-1: a point)"; }
                if (End >= 0 && E.m_EndTick >= 0 && EndS.empty()) End = T + (E.m_EndTick - E.m_Tick);      // moved: an interval keeps its length
                if (End >= 0 && End < T) return "ClipEvent: the end is before the start";
                const auto R = AsOneStep(std::format("Edit event {}", E.m_Name), [&]() -> std::string
                {
                    if (auto Err = Set("Tick", std::to_string(T), std::to_string(E.m_Tick)); !Err.empty()) return Err;
                    if (auto Err = Set("EndTick", std::to_string(End), std::to_string(E.m_EndTick)); !Err.empty()) return Err;
                    if (!KindS.empty()) if (auto Err = Set("Kind", EventKindName(K), EventKindName(E.m_Kind)); !Err.empty()) return Err;
                    if (!Name.empty()) if (auto Err = Set("Name", Name, E.m_Name); !Err.empty()) return Err;
                    if (!Bone.empty()) if (auto Err = Set("Bone", Bone == "-" ? std::string{} : Bone, E.m_Bone); !Err.empty()) return Err;
                    return {};
                });
                return R.empty() ? std::format("ClipEvent: '{}' changed", E.m_Name) : "ClipEvent: " + R;
            }
            return EventsInfo();
        }

        bool PoseRigReady() noexcept { std::string E; return PoseRig(E); }

        // DetectFootsteps: each foot's support contacts (the clip as composed) become Footstep bars (from where the foot lands, as long as it
        // supports; the event fires at the landing); replaces only what it made before (user events are kept). One undo step.
        std::string DetectFootsteps() noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) return "DetectFootsteps: select a clip first";
            std::string Err; if (!PoseRig(Err)) return "DetectFootsteps: " + Err;
            std::vector<xanimation::contact_interval> C[2]; int FPS = 30, nF = 0; float HipH = 0;
            if (!FootContacts(C, FPS, nF, HipH)) return "DetectFootsteps: no feet or no frames (a humanoid foot on each side, the clip compiled)";
            const auto& R = m_PT.m_Rig;
            std::vector<xanim_package_desc::clip_event> New;
            auto MakeEv = [](std::uint64_t Id, std::string N, xanim_package::event_kind K, std::int64_t A, std::int64_t B, const std::string& Bn) { xanim_package_desc::clip_event E; E.m_Id = Id; E.m_Name = std::move(N); E.m_Kind = K; E.m_Tick = A; E.m_EndTick = B; E.m_Bone = Bn; return E; };
            std::uint64_t Id = auto_event_id_v;
            for (int s = 0; s < 2; ++s)
                for (const auto& I : C[s])
                {
                    const char* pSide = s == 0 ? "Left" : "Right";
                    const std::string Bone = R.m_Names[s == 0 ? R.m_LF : R.m_RF];
                    const std::int64_t A = std::llround(double(I.m_First) / FPS * tl_tps_v), B = std::llround(double(I.m_Last) / FPS * tl_tps_v);
                    // one bar per support: a Footstep fires where the foot lands (its start) and lasts while it supports; a foot already down at
                    // frame 0 did not land there: a Contact (no footstep sound at the clip's start)
                    New.push_back(MakeEv(++Id, std::format("{}Foot", pSide), I.m_First > 0 ? xanim_package::event_kind::FOOTSTEP : xanim_package::event_kind::CONTACT, A, B, Bone));
                }
            std::sort(New.begin(), New.end(), [](const auto& a, const auto& b) { return a.m_Tick < b.m_Tick; });
            int nOld = 0;
            const auto Res = AsOneStep("Detect footsteps", [&]() -> std::string
            {
                for (int k = int(SelectedOv()->m_Events.size()) - 1; k >= 0; --k)
                    if (SelectedOv()->m_Events[k].m_Id >= auto_event_id_v) { ++nOld; if (auto E = m_Undo.Execute(std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(EventsPath()), k)); !E.empty()) return E; }
                for (const auto& E : New) if (auto X = EventAppend(E); !X.empty()) return X;
                return {};
            });
            if (!Res.empty()) return "DetectFootsteps: " + Res;
            int nSteps = 0; for (const auto& E : New) nSteps += E.m_Kind == xanim_package::event_kind::FOOTSTEP;
            return std::format("DetectFootsteps: {} footsteps and {} contacts already down at the start (left {}, right {} supports; {} detected events replaced)", nSteps, New.size() - nSteps, C[0].size(), C[1].size(), nOld);
        }

        std::set<int> PoseEffectivePins(double Seconds) noexcept
        {
            auto P = m_PT.m_Pins;
            if (m_PT.m_bAutoPins) for (int b : PoseContactsAt(Seconds)) if (!m_PT.m_Unpinned.count(b)) P.insert(b);
            return P;
        }
        // a handle's pin, on or off: the user's pin, or a foot pinned by its contact (then the contact pin is overridden)
        void PoseTogglePin(int b) noexcept
        {
            if (PoseEffectivePins(m_TimeSeconds).count(b)) { if (m_PT.m_Pins.count(b)) m_PT.m_Pins.erase(b); else m_PT.m_Unpinned.insert(b); }
            else { m_PT.m_Unpinned.erase(b); m_PT.m_Pins.insert(b); }
        }

        static std::string PoseReason(std::uint8_t r) noexcept
        {
            using namespace xanimation;
            if (r & ik_reason::CONFLICT) return "pins in conflict";
            if (r & ik_reason::LIMIT)    return "a joint limit";
            if (r & ik_reason::REACH)    return "beyond reach";
            if (r & ik_reason::LOCKED)   return "a bone that may not move";
            if (r & ik_reason::BUDGET)   return "the solver ran out of iterations";
            return "missed";
        }

        // One solve from a baseline: the handle at TargetModel (model frame), the pins held where the baseline has them. Pure: same inputs,
        // same pose (a drag solves from its start pose each frame, so the mouse's event rate does not matter).
        bool PoseSolve(const xanimation::pose& Base, int Eff, const xmath::fvec3& TargetModel, bool bFK, const std::set<int>& Pins, xanimation::pose& Out, std::string& Note, float* pPinErrCm = nullptr) noexcept
        {
            const auto& R = m_PT.m_Rig;
            const int nB = R.count();
            xanimation::body_ik S; S.Setup(R.m_Par, PoseLimitsOn() ? std::span<const xanimation::joint_limit>(R.m_Lim) : std::span<const xanimation::joint_limit>{}, {}, R.m_Limbs);
            S.ComputeWorld(Base);
            std::vector<xanimation::ik_task> Tasks;
            std::vector<std::uint8_t> W = R.m_Owned;
            xanimation::body_ik_settings Set;
            if (bFK)
            {   // the bone alone turns; its tip (first child) goes toward the target
                int Tip = -1; for (int c = Eff + 1; c < nB; ++c) if (R.m_Par[c] == Eff) { Tip = c; break; }
                if (Tip < 0) { Note = "this bone has no tip to drag"; Out = Base; return false; }
                W.assign(nB, 0); W[Eff] = 1;
                xanimation::ik_task T; T.m_Bone = Tip; T.m_bPosition = true; T.m_Position = TargetModel; Tasks.push_back(T);
            }
            else
            {
                xanimation::ik_task T; T.m_Bone = Eff; T.m_bPosition = true; T.m_Position = TargetModel; T.m_Attach = R.m_AttachB[Eff]; Tasks.push_back(T);
                Set.m_TranslateBone = R.m_Hips;
                for (int b : Pins)
                {
                    if (b == Eff || b < 0 || b >= nB) continue;
                    xanimation::ik_task Pn; Pn.m_Bone = b; Pn.m_bPosition = true; Pn.m_Position = S.WorldPosition(b) + S.WorldRotation(b) * R.m_AttachM[b]; Pn.m_Attach = R.m_AttachB[b];
                    Pn.m_bOrientation = true; Pn.m_Orientation = S.WorldRotation(b); Pn.m_bHard = true; Tasks.push_back(Pn);
                }
            }
            std::vector<xanimation::ik_task_report> Rep(Tasks.size());
            S.Solve(Base, W, Tasks, Rep, Out, Set);
            // why: the root never moves (only the hips do), so "locked" alone means the body was held by the pins or simply ran out of reach
            std::uint8_t Why = Rep[0].m_Reasons;
            if (!bFK && (Why & ~xanimation::ik_reason::BUDGET) == xanimation::ik_reason::LOCKED) Why = 0;
            const std::string Reason = Why ? PoseReason(Why) : (!bFK && Tasks.size() > 1 ? "the pinned handles hold the body (unpin one to reach further)" : "beyond reach");
            Note = Rep[0].m_Status == xanimation::ik_status::REACHED ? "reached" : std::format("missed by {:.1f} cm: {}", Rep[0].m_PosMiss * 100.0f, Reason);
            if (pPinErrCm) { *pPinErrCm = 0; for (std::size_t t = 1; t < Rep.size(); ++t) *pPinErrCm = std::max(*pPinErrCm, Rep[t].m_PosMiss * 100.0f); }
            return true;
        }

        // the IK respects the skeleton's limits unless the layer posing writes to (the top Pose layer) has BoneLimits off (C59)
        bool PoseLimitsOn() noexcept
        {
            const auto* pOv = SelectedOv();
            if (!pOv || pOv->m_Layers.empty()) return true;
            const auto& L = pOv->m_Layers.back();
            return L.m_Kind != xanim_package_desc::layer_kind::POSE || L.m_bBoneLimits;
        }

        // the top layer of the selected clip as a Pose layer (one is added when it is not): where posing writes
        int PoseLayer(std::string& Err) noexcept
        {
            auto* pOv = SelectedOv();
            if (!pOv) { Err = "select a clip first"; return -1; }
            const int n = int(pOv->m_Layers.size());
            if (n > 0 && pOv->m_Layers[n - 1].m_Kind == xanim_package_desc::layer_kind::POSE && !pOv->m_Layers[n - 1].m_bLock) return n - 1;
            const auto R = LayerAdd({}, {}, "Pose", true);
            if (!R.starts_with("LayerAdd: pose layer")) { Err = R; return -1; }
            return int(SelectedOv()->m_Layers.size()) - 1;
        }

        // A pose (every bone's local) at a time as keys on the top Pose layer: the bones it changes, the hips' shift too. ONE undo step.
        std::string PoseCommit(const xanimation::pose& P, double Seconds, const std::string& Label) noexcept
        {
            const auto& R = m_PT.m_Rig;
            const int nB = R.count();
            if (P.count() != nB) return "the pose does not fit the skeleton";
            std::string Out;
            const auto E = AsOneStep(Label, [&]() -> std::string
            {
                std::string Err;
                const int iL = PoseLayer(Err);
                if (iL < 0) return Err;
                xanimation::pose Base; if (!PoseAt(Seconds, Base, Err)) return Err;
                m_Suggest = {};
                m_Suggest.m_Owned = R.m_Owned; m_Suggest.m_Time = Seconds; m_Suggest.m_Sig = SuggestSignature(); m_Suggest.m_iHips = R.m_Hips;
                m_Suggest.m_Original.resize(nB); for (int b = 0; b < nB; ++b) m_Suggest.m_Original[b] = Base.get(b);
                suggest_candidate C; C.m_Pose.resize(nB); for (int b = 0; b < nB; ++b) C.m_Pose[b] = P.get(b);
                m_Suggest.m_Candidates.push_back(std::move(C));
                Out = SuggestPoseAccept(iL, 0, false);
                m_Suggest = {};
                return Out.find(" keys on layer ") != std::string::npos ? std::string{} : Out;
            });
            if (!E.empty()) return E;
            return Out;
        }

        //----------------------------------------------------------------------------------------
        // Keyframe batch (2026-11-16): a new animation from nothing, IK keys, K / Shift+K, auto-key
        //----------------------------------------------------------------------------------------

        // An absolute key straight into a layer (no stack below needed: an Override layer's key IS the local rotation; the hips also positioned)
        void SetAbsoluteKey(int iL, const std::string& Bone, std::int64_t Tick, const xmath::fquat& Q, const xmath::fvec3* pPos) noexcept
        {
            const auto& L = SelectedOv()->m_Layers[iL];
            const int k = int(L.m_Keys.size());
            SetLayer(iL, "Keys[]", std::to_string(k + 1), std::to_string(k));
            auto Fd = [&](const char* p) { return std::format("Keys[G:{}]/{}", k, p); };
            SetLayer(iL, Fd("Tick").c_str(), std::to_string(Tick), "0"); SetLayer(iL, Fd("Bone").c_str(), Bone, "");
            SetLayer(iL, Fd("X").c_str(), F(Q.m_X), "0"); SetLayer(iL, Fd("Y").c_str(), F(Q.m_Y), "0"); SetLayer(iL, Fd("Z").c_str(), F(Q.m_Z), "0"); SetLayer(iL, Fd("W").c_str(), F(Q.m_W), "1");
            if (pPos) { SetLayer(iL, Fd("Positioned").c_str(), "true", "false"); SetLayer(iL, Fd("PX").c_str(), F(pPos->m_X), "0"); SetLayer(iL, Fd("PY").c_str(), F(pPos->m_Y), "0"); SetLayer(iL, Fd("PZ").c_str(), F(pPos->m_Z), "0"); }
        }

        // A clip made from nothing: an import source of the "New animation" kind (the skeleton's rest pose held for its length), its clip, and an
        // Override Pose layer (no falloff: keys hold) ready for keys; from the playhead: that pose keyed at 0 on every bone. One undo step, then
        // saved and compiled (the layers need the new clip's source frames).
        std::string NewClip(std::string Name, double Seconds, int FPS, bool bFromPlayhead) noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) return "NewClip: nothing loaded";
            Name = SafeFileName(Name);
            for (const auto& S : pDesc->m_ImportSources) for (const auto& C : S.m_Clips) if (C.m_Name == Name || C.m_OriginalName == Name) return std::format("NewClip: a clip '{}' exists", Name);
            std::string Err;
            if (!PoseRig(Err)) return "NewClip: " + Err;
            const auto& R = m_PT.m_Rig;
            xanimation::pose Start;
            if (bFromPlayhead && !PoseAt(m_TimeSeconds, Start, Err)) return "NewClip: the pose at the playhead: " + Err;
            const int OldS = m_iSelectedSource, OldC = m_iSelectedDescriptorClip;
            const int n = int(pDesc->m_ImportSources.size());
            const auto Base = std::format("AnimPackage/ImportSources[G:{}]/", n);
            auto E = AsOneStep(std::format("New animation '{}'", Name), [&]() -> std::string
            {
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Quote("AnimPackage/ImportSources[]"), xeditor::Quote(std::to_string(n + 1)), xeditor::Quote(std::to_string(n))));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Type"), xeditor::Quote("New animation (rest pose)")));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Path"), xeditor::Quote(Name)));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Seconds"), xeditor::Quote(F(Seconds))));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "FPS"), xeditor::Quote(std::to_string(FPS))));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[]"), xeditor::Quote("1")));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[G:0]/OriginalName"), xeditor::Quote(Name)));
                xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {}", xeditor::Quote(Base + "Clips[G:0]/Name"), xeditor::Quote(Name)));
                if (Desc()->m_ImportSources.size() != std::size_t(n + 1) || Desc()->m_ImportSources[n].m_Clips.empty()) return "the new source could not be added";
                m_iSelectedSource = n; m_iSelectedDescriptorClip = 0;
                const auto A = LayerAdd({}, {}, "Keys", true);
                if (!A.starts_with("LayerAdd: pose layer")) return A;
                SetLayer(0, "Mode", "Override", "Additive");          // keys are the pose itself
                SetLayer(0, "Falloff", "0", "0.5");                     // and hold before the first / after the last
                if (bFromPlayhead)
                    for (int b = 0; b < R.count(); ++b)
                    {
                        if (!R.m_Owned[b] && b != R.m_Hips) continue;
                        const auto P = Start.m_Position[b];
                        SetAbsoluteKey(0, R.m_Names[b], 0, Start.m_Rotation[b], b == R.m_Hips ? &P : nullptr);
                    }
                return {};
            });
            if (!E.empty()) { m_iSelectedSource = OldS; m_iSelectedDescriptorClip = OldC; return "NewClip: " + E; }
            (void)m_Undo.Query("Save");
            (void)m_Undo.Query("Compile");
            return std::format("NewClip: '{}' ({} s at {} fps, from the {}) is import source {} with a Pose layer (Override, keys hold); saved, compiling", Name, Seconds, FPS, bFromPlayhead ? "pose at the playhead" : "rest pose", n);
        }

        // An effector's IK key on the top Pose layer at a time: the handle's target (default: where it is now), its rotation (Rot), pinned or not
        // (default: the handle's pin state in the Pose tool). Replaces a key of that effector at that tick. One undo step.
        std::string IkKey(const std::string& Effector, double Seconds, const xmath::fvec3* pPos, int PinState, bool bRot, bool bDelete) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "IkKey: " + Err;
            const auto& R = m_PT.m_Rig;
            const int b = R.Find(Effector);
            if (b < 0) return std::format("IkKey: no handle or bone '{}'", Effector);
            xanimation::pose Cur; if (!PoseAt(Seconds, Cur, Err)) return "IkKey: " + Err;
            xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(Cur);
            const auto Pos = pPos ? *pPos : HandleModel(FK, b);
            const bool bPin = PinState < 0 ? (m_PT.m_Pins.count(b) > 0) : PinState == 1;
            const auto Rot = FK.WorldRotation(b);
            const auto Tick = xanim_package_layers::ToTick(Seconds);
            std::string Out;
            const auto E = AsOneStep(std::format("IK key {}", R.Name(b)), [&]() -> std::string
            {
                const int iL = PoseLayer(Err);
                if (iL < 0) return Err;
                const auto& L = SelectedOv()->m_Layers[iL];
                int k = -1;
                for (int i = 0; i < int(L.m_IkKeys.size()); ++i) if (L.m_IkKeys[i].m_Effector == R.m_Names[b] && L.m_IkKeys[i].m_Tick == Tick) { k = i; break; }
                if (bDelete)
                {
                    if (k < 0) return std::format("no IK key of {} at {:.3f} s", R.Name(b), Seconds);
                    xeditor::Run(m_Undo, std::format("ListOp -Path {} -Op Delete -Index {}", xeditor::Quote(LayerPath(iL, "IkKeys")), k));
                    Out = std::format("IkKey: {}'s key at {:.3f} s deleted", R.Name(b), Seconds); return {};
                }
                const bool bNew = k < 0;
                if (bNew) { k = int(L.m_IkKeys.size()); SetLayer(iL, "IkKeys[]", std::to_string(k + 1), std::to_string(k)); }
                const auto Old = bNew ? xanim_package_desc::layer_ik_key{} : SelectedOv()->m_Layers[iL].m_IkKeys[k];
                auto Fd = [&](const char* p) { return std::format("IkKeys[G:{}]/{}", k, p); };
                if (bNew) { SetLayer(iL, Fd("Tick").c_str(), std::to_string(Tick), "0"); SetLayer(iL, Fd("Effector").c_str(), R.m_Names[b], ""); }
                SetLayer(iL, Fd("PX").c_str(), F(Pos.m_X), F(Old.m_PX)); SetLayer(iL, Fd("PY").c_str(), F(Pos.m_Y), F(Old.m_PY)); SetLayer(iL, Fd("PZ").c_str(), F(Pos.m_Z), F(Old.m_PZ));
                if (bRot != Old.m_bRotation) SetLayer(iL, Fd("Rotated").c_str(), bRot ? "true" : "false", Old.m_bRotation ? "true" : "false");
                if (bRot) { SetLayer(iL, Fd("X").c_str(), F(Rot.m_X), F(Old.m_X)); SetLayer(iL, Fd("Y").c_str(), F(Rot.m_Y), F(Old.m_Y)); SetLayer(iL, Fd("Z").c_str(), F(Rot.m_Z), F(Old.m_Z)); SetLayer(iL, Fd("W").c_str(), F(Rot.m_W), F(Old.m_W)); }
                if (bPin != Old.m_bPinned) SetLayer(iL, Fd("Pinned").c_str(), bPin ? "true" : "false", Old.m_bPinned ? "true" : "false");
                const auto& At = R.m_AttachB[b];
                if (At.m_X != Old.m_AX) SetLayer(iL, Fd("AX").c_str(), F(At.m_X), F(Old.m_AX));
                if (At.m_Y != Old.m_AY) SetLayer(iL, Fd("AY").c_str(), F(At.m_Y), F(Old.m_AY));
                if (At.m_Z != Old.m_AZ) SetLayer(iL, Fd("AZ").c_str(), F(At.m_Z), F(Old.m_AZ));
                Out = std::format("IkKey: layer {} key {} {} {} at {:.3f} s: {:.4f} {:.4f} {:.4f}{}{}", iL, k, bNew ? "added" : "replaced", R.Name(b), Seconds, Pos.m_X, Pos.m_Y, Pos.m_Z, bPin ? " pinned" : "", bRot ? " +rotation" : "");
                return {};
            });
            return E.empty() ? Out : "IkKey: " + E;
        }

        // where a preview prop's centre is at a time (model space, the composed animation: a prop a bone holds moves with it); IK keys aim at it
        bool PropAt(const std::string& Name, double Seconds, xmath::fvec3& Out, std::string& Err) noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) { Err = "no description"; return false; }
            const xanim_package_desc::preview_prop* pP = nullptr;
            for (const auto& P : pDesc->m_PreviewProps) if (P.m_Name == Name) { pP = &P; break; }
            if (!pP) { Err = std::format("no preview prop '{}' (the description's PreviewProps)", Name); return false; }
            if (pP->m_Bone.empty()) { Out = pP->m_Offset; return true; }
            if (!PoseRig(Err)) return false;
            const int b = m_PT.m_Rig.Find(pP->m_Bone);
            if (b < 0) { Err = std::format("prop '{}': no bone '{}'", Name, pP->m_Bone); return false; }
            xanimation::pose Cur; if (!PoseAt(Seconds, Cur, Err)) return false;
            xanimation::body_ik FK; FK.Setup(m_PT.m_Rig.m_Par); FK.ComputeWorld(Cur);
            Out = FK.WorldPosition(b) + FK.WorldRotation(b) * pP->m_Offset;
            return true;
        }

        // A kick's check (soccer answer 1): the ball leaves with the kicking handle's velocity at the contact (central difference over one frame
        // at 60 Hz) times 1.1 (an instep kick: (1 + e) M / (M + m) with e 0.5, the leg's 1.2 kg against the ball's 0.43); a parabola under
        // gravity (model units are metres; no air drag) until it comes back to the ball's start height or 2 s. ponytail: no spin, no drag.
        std::vector<xmath::fvec3> m_Flight;
        // Render video: the selected clip, paused, stepped one video frame per editor frame (xeditor::frame_recorder); its frames and the video
        // go to <project>\Cache\Temp\AnimVideo (the clip's name)
        xeditor::frame_recorder m_Recorder;
        bool                    m_bRecorderWasRunning = false;
        std::string StartVideo(int Fps, float Seconds) noexcept
        {
            auto* pClip = SelectedClip();
            if (!pClip) return "RenderVideo: no clip is selected";
            if (m_K.m_bPreview) return "RenderVideo: a Kimodo preview is showing (accept or discard it first)";
            if (m_Recorder.Running()) return "RenderVideo: already recording (RenderVideo -Cancel 1 stops it)";
            const float Length = Seconds > 0 ? Seconds : ClipLength(*pClip);
            std::string Name = "clip";
            if (auto* pDesc = Desc(); pDesc && m_iSelectedSource >= 0 && m_iSelectedSource < int(pDesc->m_ImportSources.size()))
                if (const auto& Clips = pDesc->m_ImportSources[m_iSelectedSource].m_Clips; m_iSelectedDescriptorClip >= 0 && m_iSelectedDescriptorClip < int(Clips.size())) Name = Clips[m_iSelectedDescriptorClip].m_Name;
            for (auto& c : Name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
            const auto Dir = ProjectPath() / L"Cache" / L"Temp" / L"AnimVideo";
            const int nFrames = std::max(1, int(std::ceil(Length * float(Fps) - 1e-4f)));
            m_bPlaying = false;
            m_Recorder.Start(Dir / Name, Dir / (Name + ".mp4"), nFrames, Fps);
            m_bRecorderWasRunning = true;
            return std::format("RenderVideo: recording {} frames ({:.2f} s at {} fps) to {}", nFrames, Length, Fps, (Dir / Name).string());
        }

        std::string BallFlight(const std::string& Effector, double Seconds, const std::string& Prop) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "BallFlight: " + Err;
            const int b = m_PT.m_Rig.Find(Effector);
            if (b < 0) return std::format("BallFlight: no handle or bone '{}'", Effector);
            xmath::fvec3 Ball; if (!PropAt(Prop, Seconds, Ball, Err)) return "BallFlight: " + Err;
            auto At = [&](double s, xmath::fvec3& P) { xanimation::pose Cur; if (!PoseAt(s, Cur, Err)) return false; xanimation::body_ik FK; FK.Setup(m_PT.m_Rig.m_Par); FK.ComputeWorld(Cur); P = HandleModel(FK, b); return true; };
            constexpr double H = 1.0 / 60.0;
            xmath::fvec3 A, B, Foot; if (!At(std::max(0.0, Seconds - H), A) || !At(Seconds + H, B) || !At(Seconds, Foot)) return "BallFlight: " + Err;
            const auto Vf = (B - A) * float(1.0 / (Seconds + H - std::max(0.0, Seconds - H)));
            auto V = Vf * 1.1f;
            m_Flight.clear();
            xmath::fvec3 P = Ball; float Top = Ball.m_Y, Dist = 0;
            const bool bRoll = V.m_Y <= 0.0f;           // kicked down or level: the ground holds it - it rolls (grass: 0.5 m/s2 of rolling resistance)
            if (bRoll)
            {
                V.m_Y = 0;
                const float S0 = std::sqrt(V.m_X * V.m_X + V.m_Z * V.m_Z);
                const auto Dir = S0 > 1e-6f ? V * (1.0f / S0) : xmath::fvec3(0, 0, 0);
                for (int i = 0; i <= 200; ++i)
                {
                    const float t = std::min(0.01f * float(i), S0 / 0.5f);
                    m_Flight.push_back(Ball + Dir * (S0 * t - 0.25f * t * t));
                    if (t >= S0 / 0.5f) break;
                }
            }
            else
                for (int i = 0; i <= 200; ++i)
                {
                    const float t = 0.01f * float(i);
                    P = Ball + V * t + xmath::fvec3(0, -4.905f * t * t, 0);
                    if (i > 0 && P.m_Y < Ball.m_Y) break;
                    m_Flight.push_back(P); Top = std::max(Top, P.m_Y);
                }
            const auto D = m_Flight.back() - Ball; Dist = std::sqrt(D.m_X * D.m_X + D.m_Z * D.m_Z);
            const float Sp = std::sqrt(V.Dot(V)), Hs = std::sqrt(V.m_X * V.m_X + V.m_Z * V.m_Z);
            const auto Gap = Foot - Ball;
            if (bRoll)
                return std::format("BallFlight: {} at {:.3f} s moves at {:.2f} m/s, downward {:.1f} deg: the ball rolls at {:.2f} m/s and stops {:.2f} m away{} ({} points drawn); the handle is {:.1f} cm from the ball's centre at the contact"
                    , m_PT.m_Rig.Name(b), Seconds, std::sqrt(Vf.Dot(Vf)), std::atan2(Vf.m_Y, std::sqrt(Vf.m_X * Vf.m_X + Vf.m_Z * Vf.m_Z)) * 57.29578f, Sp, Dist, m_Flight.size() > 200 ? " (still rolling after 2 s)" : "", m_Flight.size(), std::sqrt(Gap.Dot(Gap)) * 100.0f);
            return std::format("BallFlight: {} at {:.3f} s moves at {:.2f} m/s; the ball leaves at {:.2f} m/s, {:.1f} deg up, rises {:.2f} m, lands {:.2f} m away{} ({} points drawn); the handle is {:.1f} cm from the ball's centre at the contact"
                , m_PT.m_Rig.Name(b), Seconds, std::sqrt(Vf.Dot(Vf)), Sp, std::atan2(V.m_Y, Hs) * 57.29578f, Top - Ball.m_Y, Dist, m_Flight.size() > 200 ? " (still flying after 2 s)" : "", m_Flight.size(), std::sqrt(Gap.Dot(Gap)) * 100.0f);
        }

        // where a handle is at a time in the composed animation (model space; the gates measure IK keys with it)
        std::string HandleProbe(const std::string& Effector, double Seconds) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "IkKey: " + Err;
            const int b = m_PT.m_Rig.Find(Effector);
            if (b < 0) return std::format("IkKey: no handle or bone '{}'", Effector);
            xanimation::pose Cur; if (!PoseAt(Seconds, Cur, Err)) return "IkKey: " + Err;
            xanimation::body_ik FK; FK.Setup(m_PT.m_Rig.m_Par); FK.ComputeWorld(Cur);
            const auto P = HandleModel(FK, b), H = FK.WorldPosition(m_PT.m_Rig.m_Hips >= 0 ? m_PT.m_Rig.m_Hips : 0);
            // the pose against the skeleton's limits (as the package's compile sees them): the worst joint outside, degrees (C59's gate)
            std::string Lim = "no limits (the skeleton's are off)";
            if (const auto L = xanim_package_layers::ManifestLimits(Desc()->m_ResolvedSkeletonBones); int(L.size()) == Cur.count())
            {
                float Worst = 0; int iW = -1;
                for (int j = 0; j < Cur.count(); ++j) if (const auto Ev = xanimation::Evaluate(L[j], Cur.m_Rotation[j]); Ev.m_Excess > Worst) { Worst = Ev.m_Excess; iW = j; }
                Lim = iW < 0 ? "limits: all inside" : std::format("limits: worst {:.3f} deg outside ({})", Worst * 57.29578f, m_PT.m_Rig.Name(iW));
            }
            return std::format("IkKey probe {} at {:.4f} s: {:.5f} {:.5f} {:.5f}; hips {:.5f} {:.5f} {:.5f}; {}", m_PT.m_Rig.Name(b), Seconds, P.m_X, P.m_Y, P.m_Z, H.m_X, H.m_Y, H.m_Z, Lim);
        }

        // K: the pending pose (auto-key off), else the selected bone (FK), else the last dragged handle as an IK key. Shift+K: every bone the
        // top Pose layer already keys (none yet: every bone a drag may turn). The keys hold the pose shown now. One undo step.
        // auto-key off: the edit is kept as a pending pose, shown, until K keys it (or another edit replaces it)
        std::string PosePend(const xanimation::pose& P, double Seconds) noexcept
        {
            m_TimeSeconds = float(Seconds);                                     // the playhead goes where the pending pose is (it is dropped when the playhead leaves)
            m_PT.m_PendingPose = P; m_PT.m_bPending = true; m_PT.m_PendingTime = m_TimeSeconds;
            m_PT.m_Preview = P; m_PT.m_bPreview = true;
            return "pending: not keyed (auto-key is off): K keys it";
        }

        std::string KeySelected(double Seconds, bool bAll) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "KeySelected: " + Err;
            const auto& R = m_PT.m_Rig;
            if (!bAll && m_PT.m_bPending && std::abs(m_PT.m_PendingTime - Seconds) < 1e-4)
            {
                const auto P = m_PT.m_PendingPose; m_PT.m_bPending = false; m_PT.m_bPreview = false;
                return "KeySelected: the pending pose: " + PoseCommit(P, Seconds, "Key pending pose");
            }
            if (!bAll && m_PT.m_FKBone < 0)
            {
                if (m_PT.m_LastEff >= 0) return "KeySelected: " + IkKey(R.m_Names[m_PT.m_LastEff], Seconds, nullptr, -1, false, false);
                return "KeySelected: select a bone (click it) or drag a handle first; Shift+K keys every bone";
            }
            xanimation::pose Cur; if (!PoseAt(Seconds, Cur, Err)) return "KeySelected: " + Err;
            std::vector<int> Bones;
            int nKeys = 0;
            const auto E = AsOneStep(bAll ? "Key all" : "Key selected", [&]() -> std::string
            {
                const int iL = PoseLayer(Err);
                if (iL < 0) return Err;
                if (bAll)
                {
                    for (const auto& K : SelectedOv()->m_Layers[iL].m_Keys) { const int b = R.Find(K.m_Bone); if (b >= 0 && std::find(Bones.begin(), Bones.end(), b) == Bones.end()) Bones.push_back(b); }
                    if (Bones.empty()) for (int b = 0; b < R.count(); ++b) if (R.m_Owned[b] || b == R.m_Hips) Bones.push_back(b);
                }
                else Bones.push_back(m_PT.m_FKBone);
                for (int b : Bones)
                {
                    xmath::fquat B; xmath::fvec3 BP;
                    if (!TransformBelow(iL, b, Seconds, B, BP, Err)) return Err;
                    const auto Turn = (Cur.m_Rotation[b] * B.ConjugateCopy()).NormalizeCopy();
                    const auto Move = Cur.m_Position[b] - BP;
                    const auto Rk = PoseKey(iL, R.m_Names[b], Seconds, Turn, false, b == R.m_Hips ? &Move : nullptr);
                    if (!Rk.starts_with("PoseKey: layer")) return Rk;
                    ++nKeys;
                }
                return {};
            });
            return E.empty() ? std::format("KeySelected: {} key(s) at {:.3f} s ({})", nKeys, Seconds, bAll ? "every bone the layer owns" : R.m_Names[m_PT.m_FKBone]) : "KeySelected: " + E;
        }

        std::string PoseDrag(const std::string& Effector, const xmath::fvec3& MoveCm, const std::string& PinArg, bool bFK, double Seconds, bool bCommit) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PoseDrag: " + Err;
            const auto& R = m_PT.m_Rig;
            const int Eff = R.Find(Effector);
            if (Eff < 0) return std::format("PoseDrag: no handle or bone '{}'", Effector);
            xanimation::pose Base;
            if (!PoseAt(Seconds, Base, Err)) return "PoseDrag: " + Err;
            std::set<int> Pins;
            if (PinArg.empty() || PinArg == "auto") Pins = PoseEffectivePins(Seconds);
            else if (PinArg != "none") { std::istringstream In(PinArg); std::string N; while (In >> N) { const int b = R.Find(N); if (b < 0) return std::format("PoseDrag: no handle '{}' to pin", N); Pins.insert(b); } }
            xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(Base);
            int Moved = Eff;
            if (bFK) for (int c = Eff + 1; c < R.count(); ++c) if (R.m_Par[c] == Eff) { Moved = c; break; }
            const auto Target = (bFK ? FK.WorldPosition(Moved) : HandleModel(FK, Eff)) + MoveCm * 0.01f;
            xanimation::pose Out; std::string Note; float PinErr = 0;
            if (!PoseSolve(Base, Eff, Target, bFK, Pins, Out, Note, &PinErr)) return "PoseDrag: " + Note;
            m_PT.m_Note = Note; m_PT.m_LastEff = Eff;
            std::string PinText; for (int b : Pins) if (b != Eff) PinText += (PinText.empty() ? "" : " ") + R.Name(b);
            const auto Head = std::format("PoseDrag: {} {} at {:.3f} s: {}; pins [{}] held within {:.3f} cm", bFK ? "FK" : "IK", R.Name(Eff), Seconds, Note, PinText, PinErr);
            if (!bCommit) { m_PT.m_Preview = Out; m_PT.m_bPreview = true; return Head + " (preview only)"; }
            m_PT.m_bPreview = false;
            if (!m_PT.m_bAutoKey) return Head + "\n" + PosePend(Out, Seconds);         // auto-key off: pending until K
            const auto C = PoseCommit(Out, Seconds, std::format("Pose {} {}", bFK ? "turn" : "drag", R.Name(Eff)));
            return Head + "\n" + C;
        }

        // The pose at a time mirrored across the character's middle plane: each bone takes its mirror bone's turn from rest, reflected
        std::string PoseMirror(double Seconds) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PoseMirror: " + Err;
            const auto& R = m_PT.m_Rig;
            const int nB = R.count();
            xanimation::pose P; if (!PoseAt(Seconds, P, Err)) return "PoseMirror: " + Err;
            std::vector<std::string_view> Views(R.m_Names.begin(), R.m_Names.end());
            const xanimation::skeleton_view SV{ R.m_Par };
            const float Fd = xanimation::MeasureFacingDeg(SV, Views, R.m_Rest) * 3.14159265f / 180.0f;
            const xmath::fvec3 N(std::cos(Fd), 0, -std::sin(Fd));          // the lateral axis: the plane's normal
            auto F = [&](const xmath::fvec3& v) { return v - N * (2.0f * v.Dot(N)); };
            auto FQ = [&](const xmath::fquat& q) { const auto v = F(xmath::fvec3(q.m_X, q.m_Y, q.m_Z)); return xmath::fquat(-v.m_X, -v.m_Y, -v.m_Z, q.m_W); };
            std::vector<xmath::fquat> Wr, WrRest; std::vector<xmath::fvec3> Wp, WpRest, Sc, ScRest;
            xanimation::details::WorldRPS(SV, P, Wr, Wp, Sc);
            xanimation::details::WorldRPS(SV, R.m_Rest, WrRest, WpRest, ScRest);
            std::vector<xmath::fquat> Mw(nB); std::vector<xmath::fvec3> Mp(nB);
            const xmath::fvec3 Mid = R.m_Hips >= 0 ? WpRest[R.m_Hips] : xmath::fvec3(0, 0, 0);
            for (int b = 0; b < nB; ++b)
            {
                int m = xanimation::FindMirrorBone(Views, b); if (m < 0) m = b;
                const auto D = (Wr[m] * WrRest[m].ConjugateCopy()).NormalizeCopy();         // the mirror bone's world turn from rest
                Mw[b] = (FQ(D) * WrRest[b]).NormalizeCopy();
                Mp[b] = F(Wp[m] - Mid) + Mid;
            }
            xanimation::pose O = P;
            for (int b = 0; b < nB; ++b)
            {
                const int p = R.m_Par[b];
                if (p < 0) continue;
                O.m_Rotation[b] = (Mw[p].ConjugateCopy() * Mw[b]).NormalizeCopy();
                if (b == R.m_Hips) { const auto L = Mw[p].ConjugateCopy() * (Mp[b] - Mp[p]); O.m_Position[b] = xmath::fvec3(L.m_X / Sc[p].m_X, L.m_Y / Sc[p].m_Y, L.m_Z / Sc[p].m_Z); }
            }
            return "PoseMirror: " + PoseCommit(O, Seconds, "Pose mirror");
        }

        // Suggest Pose for the current handles: the pins and the last dragged handle held where they are now
        std::string PoseSuggest(const std::string& Seeds, const std::string& Prompt) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PoseSuggest: " + Err;
            const auto& R = m_PT.m_Rig;
            const double t = m_TimeSeconds;
            auto Pins = PoseEffectivePins(t);
            if (m_PT.m_LastEff >= 0) Pins.insert(m_PT.m_LastEff);
            std::string Names; for (int b : Pins) Names += (Names.empty() ? "" : " ") + R.m_Names[b];
            const auto Text = SuggestPose(t, "", Names.empty() ? "none" : Names, Seeds.empty() ? "1 2 3" : Seeds, Prompt);
            m_PT.m_Cand = m_Suggest.m_Candidates.empty() ? -1 : 0;
            return "PoseSuggest: " + Text;
        }
        std::string PoseSuggestAccept(int i) noexcept
        {
            if (i < 0 || i >= int(m_Suggest.m_Candidates.size())) return "PoseSuggest: no such candidate (PoseSuggest first)";
            std::string Out;
            const auto E = AsOneStep("Suggest Pose", [&]() -> std::string
            {
                std::string Err;
                const bool bFresh = m_Suggest.m_Sig == SuggestSignature();
                const int iL = PoseLayer(Err);
                if (iL < 0) return Err;
                if (bFresh) m_Suggest.m_Sig = SuggestSignature();     // an empty Pose layer just added changes nothing in the pose
                Out = SuggestPoseAccept(iL, i, true);
                return Out.find(" keys on layer ") != std::string::npos ? std::string{} : Out;
            });
            m_PT.m_Cand = -1;
            return E.empty() ? "PoseSuggest: " + Out : "PoseSuggest: " + E;
        }

        // the bones as drawn, from a local pose (the editor's own FK: scales included) shifted by the drawn offset
        void PoseToBones(const xanimation::pose& P, const xmath::fvec3& Off) noexcept
        {
            const int nB = P.count();
            if (int(m_PoseBones.size()) != nB) return;
            std::vector<xmath::fmat4> W(nB);
            for (int i = 0; i < nB; ++i)
            {
                const auto L = P.get(i).toMatrix();
                W[i] = m_PT.m_Rig.m_Par[i] < 0 ? L : W[m_PT.m_Rig.m_Par[i]] * L;
                m_PoseBones[i].m_Position = W[i].ExtractPosition() + Off;
                m_PoseBones[i].m_Right = W[i].Right(); m_PoseBones[i].m_Up = W[i].Up();
            }
        }

        // before the viewport renders: the drag (target from the mouse on the plane through the handle) and the previewed pose
        void PoseBeforeRender() noexcept
        {
            m_Viewport.m_bGizmoHot = m_Viewport.m_bGizmoDrag = false;
            std::string Err;
            if (!m_PT.m_bOn || !PoseRig(Err) || int(m_PoseBones.size()) != m_PT.m_Rig.count()) return;
            const ImVec2 Mouse = ImGui::GetIO().MousePos;
            // hover: the nearest handle drawn last frame within 10 px
            m_PT.m_Hover = -1; m_PT.m_bHoverTip = false;
            float Best = 10.0f * 10.0f;
            for (const auto& [b, S] : m_PT.m_Screen) { const float d = (S.x - Mouse.x) * (S.x - Mouse.x) + (S.y - Mouse.y) * (S.y - Mouse.y); if (d < Best) { Best = d; m_PT.m_Hover = b; } }
            if (m_PT.m_TipScreen.x >= 0) { const float d = (m_PT.m_TipScreen.x - Mouse.x) * (m_PT.m_TipScreen.x - Mouse.x) + (m_PT.m_TipScreen.y - Mouse.y) * (m_PT.m_TipScreen.y - Mouse.y); if (d < Best) { m_PT.m_Hover = m_PT.m_FKBone; m_PT.m_bHoverTip = true; } }
            if (m_PT.m_Drag >= 0)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_PT.m_Drag = -1; m_PT.m_bPreview = false; m_PT.m_Note = "cancelled"; }
                else
                {
                    const auto Eye = m_Viewport.m_Scene.m_View.getPosition();
                    const auto Ray = m_Viewport.m_Scene.m_View.RayFromScreen(Mouse.x, Mouse.y);
                    const float Den = Ray.Dot(m_PT.m_Normal);
                    if (std::abs(Den) > 1e-6f) m_PT.m_Target = Eye + Ray * ((m_PT.m_Plane - Eye).Dot(m_PT.m_Normal) / Den);
                    std::string Note;
                    PoseSolve(m_PT.m_Base, m_PT.m_Drag, m_PT.m_Target - m_PT.m_Offset, m_PT.m_bFK, m_PT.m_DragPins, m_PT.m_Preview, Note);
                    m_PT.m_Note = Note; m_PT.m_bPreview = true;
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {   // released: the keys, one undo step
                        const int Eff = m_PT.m_Drag; m_PT.m_Drag = -1;
                        const auto P = m_PT.m_Preview; m_PT.m_bPreview = false;
                        if ((m_PT.m_Target - m_PT.m_Start).Length() > 1e-5f)
                        {
                            m_PT.m_LastEff = Eff;
                            const auto R = m_PT.m_bAutoKey ? PoseCommit(P, m_PT.m_Time, std::format("Pose {} {}", m_PT.m_bFK ? "turn" : "drag", m_PT.m_Rig.Name(Eff))) : PosePend(P, m_PT.m_Time);
                            if (R.find(" keys on layer ") == std::string::npos) m_PT.m_Note = R;
                            xeditor::LogConsole(std::format("Pose: {} ({})", m_PT.m_Note, R.substr(0, 160)), xeditor::log_source::User);
                        }
                    }
                }
                m_Viewport.m_bGizmoHot = m_Viewport.m_bGizmoDrag = m_PT.m_Drag >= 0;
            }
            else if (m_PT.m_Hover >= 0 || m_PT.m_PinHover >= 0) m_Viewport.m_bGizmoHot = true;
            if (m_PT.m_bGizmo && m_PT.m_FKBone >= 0 && (ImGuizmo::IsOver() || ImGuizmo::IsUsing() || m_PT.m_bGizmoUsing)) { m_Viewport.m_bGizmoHot = true; if (ImGuizmo::IsUsing()) m_Viewport.m_bGizmoDrag = true; }
            // the previewed pose: a drag, or a Suggest candidate
            if (m_PT.m_bPending && std::abs(double(m_TimeSeconds) - m_PT.m_PendingTime) > 1e-6) { m_PT.m_bPending = false; if (m_PT.m_Drag < 0 && !m_PT.m_bGizmoUsing) m_PT.m_bPreview = false; m_PT.m_Note = "the pending pose was not keyed (the playhead moved)"; }
            if (m_PT.m_bPreview) PoseToBones(m_PT.m_Preview, m_PT.m_Offset);
            else if (m_PT.m_Cand >= 0 && m_PT.m_Cand < int(m_Suggest.m_Candidates.size()))
            {
                xanimation::pose C; C.resize(m_PT.m_Rig.count());
                for (int b = 0; b < C.count(); ++b) C.set(b, m_Suggest.m_Candidates[m_PT.m_Cand].m_Pose[b]);
                xanimation::pose Now; std::string E2; xmath::fvec3 Off(0, 0, 0);
                if (PoseAt(m_TimeSeconds, Now, E2)) { xanimation::body_ik FK; FK.Setup(m_PT.m_Rig.m_Par); FK.ComputeWorld(Now); Off = m_PoseBones[0].m_Position - FK.WorldPosition(0); }
                PoseToBones(C, Off);
            }
        }

        // after the viewport renders: the handles, the ghost, the clicks and keys
        void PoseAfterRender(const xskeleton_editor::viewport_item& Item) noexcept
        {
            m_PT.m_Screen.clear(); m_PT.m_TipScreen = ImVec2(-1, -1);
            std::string Err;
            if (!m_PT.m_bOn || !PoseRig(Err) || int(m_PoseBones.size()) != m_PT.m_Rig.count()) return;
            const auto& R = m_PT.m_Rig;
            auto* DL = ImGui::GetWindowDrawList();
            const auto Pins = m_PT.m_Drag >= 0 ? m_PT.m_DragPins : PoseEffectivePins(m_TimeSeconds);
            // the handles where the shown pose has them (the palm, the sole): a drag's preview, else the clip at the playhead
            m_PT.m_HandleAt.clear(); m_PT.m_PinBoxes.clear();
            {
                xanimation::pose Shown; std::string E2;
                const bool bHave = m_PT.m_bPreview ? (Shown = m_PT.m_Preview, true) : PoseAt(m_TimeSeconds, Shown, E2);
                if (bHave && Shown.count() == R.count())
                {
                    xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(Shown);
                    const auto Off = m_PoseBones[0].m_Position - FK.WorldPosition(0);
                    for (int b : R.Effectors()) m_PT.m_HandleAt[b] = HandleModel(FK, b) + Off;
                }
            }
            for (int b : R.Effectors())
            {
                const auto At = m_PT.m_HandleAt.find(b);
                ImVec2 S; if (!m_Viewport.Project(At != m_PT.m_HandleAt.end() ? At->second : m_PoseBones[b].m_Position, S)) continue;
                m_PT.m_Screen.push_back({ b, S });
                const bool bPin = Pins.count(b) > 0, bHot = m_PT.m_Hover == b && !m_PT.m_bHoverTip, bDrag = m_PT.m_Drag == b && !m_PT.m_bFK;
                xskeleton_editor::handles::Draw(*DL, S, bPin ? xskeleton_editor::handles::kind::pinned : xskeleton_editor::handles::kind::effector, bHot, bDrag);
                const bool bPinHot = m_PT.m_PinHover == b;
                m_PT.m_PinBoxes.push_back({ b, xskeleton_editor::handles::PinIcon(*DL, S, bPin, bPinHot) });
                if (bHot) DL->AddText(ImVec2(S.x + 22, S.y - 8), IM_COL32(255, 255, 255, 255), std::format("{}{}", R.Name(b), bPin ? " (pinned)" : " (drag it)").c_str());
                if (bPinHot) DL->AddText(ImVec2(S.x + 22, S.y - 22), IM_COL32(255, 255, 255, 255), bPin ? "click: unpin" : "click: pin here");
            }
            {
                const ImVec2 M = ImGui::GetIO().MousePos;
                m_PT.m_PinHover = -1;
                if (m_PT.m_Drag < 0 && !m_PT.m_bGizmoUsing) for (const auto& [b, Box] : m_PT.m_PinBoxes) if (xskeleton_editor::handles::In(Box, M)) m_PT.m_PinHover = b;
            }
            if (m_PT.m_FKBone >= 0 && m_PT.m_FKBone < R.count())
            {   // the selected bone's tip: dragging it turns that bone alone
                int Tip = -1; for (int c = m_PT.m_FKBone + 1; c < R.count(); ++c) if (R.m_Par[c] == m_PT.m_FKBone) { Tip = c; break; }
                ImVec2 S;
                if (Tip >= 0 && m_Viewport.Project(m_PoseBones[Tip].m_Position, S))
                {
                    m_PT.m_TipScreen = S;
                    const bool bHot = m_PT.m_bHoverTip || (m_PT.m_Drag == m_PT.m_FKBone && m_PT.m_bFK);
                    DL->AddQuadFilled(ImVec2(S.x, S.y - 7), ImVec2(S.x + 7, S.y), ImVec2(S.x, S.y + 7), ImVec2(S.x - 7, S.y), bHot ? IM_COL32(120, 220, 255, 255) : IM_COL32(60, 170, 255, 255));
                    if (bHot) DL->AddText(ImVec2(S.x + 10, S.y - 8), IM_COL32(255, 255, 255, 255), std::format("{}: turn (FK)", R.m_Names[m_PT.m_FKBone]).c_str());
                }
            }
            PoseGizmo();
            // the ghost: where the handle was asked to go, the line to where it got, why it stopped
            if (m_PT.m_Drag >= 0)
            {
                ImVec2 T, A;
                const int Got = m_PT.m_bFK ? [&] { for (int c = m_PT.m_Drag + 1; c < R.count(); ++c) if (R.m_Par[c] == m_PT.m_Drag) return c; return m_PT.m_Drag; }() : m_PT.m_Drag;
                const auto GotAt = m_PT.m_bFK ? m_PoseBones[Got].m_Position : (m_PT.m_HandleAt.count(Got) ? m_PT.m_HandleAt[Got] : m_PoseBones[Got].m_Position);
                if (m_Viewport.Project(m_PT.m_Target, T) && m_Viewport.Project(GotAt, A))
                {
                    const bool bMiss = m_PT.m_Note != "reached";
                    DL->AddCircle(T, 9.0f, bMiss ? IM_COL32(255, 80, 80, 230) : IM_COL32(255, 255, 255, 160), 0, 2.0f);
                    if (bMiss) DL->AddLine(T, A, IM_COL32(255, 80, 80, 200), 2.0f);
                    DL->AddText(ImVec2(T.x + 12, T.y + 4), bMiss ? IM_COL32(255, 120, 120, 255) : IM_COL32(220, 255, 220, 255), m_PT.m_Note.c_str());
                }
            }
            if (m_PT.m_Cand >= 0 && m_PT.m_Cand < int(m_Suggest.m_Candidates.size()))
            {
                ImVec2 S; if (R.m_Head >= 0 && m_Viewport.Project(m_PoseBones[R.m_Head].m_Position, S))
                    DL->AddText(ImVec2(S.x + 14, S.y - 24), IM_COL32(120, 220, 255, 255), std::format("Suggestion {}/{} (seed {}): Tab next, Enter accept, Esc keep yours", m_PT.m_Cand + 1, m_Suggest.m_Candidates.size(), m_Suggest.m_Candidates[m_PT.m_Cand].m_Seed).c_str());
            }

            // mouse: a handle wins over a bone pick; right-click pins; a click on a bone selects it for FK
            if (m_Viewport.Over() && m_PT.m_Drag < 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            {
                const ImVec2 Mouse = ImGui::GetIO().MousePos;
                if (m_PT.m_PinHover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) PoseTogglePin(m_PT.m_PinHover);
                else if (m_PT.m_Hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    std::string E2;
                    if (PoseAt(m_TimeSeconds, m_PT.m_Base, E2))
                    {
                        m_PT.m_bFK = m_PT.m_bHoverTip; m_PT.m_Drag = m_PT.m_Hover; m_PT.m_Time = m_TimeSeconds; m_PT.m_Cand = -1;
                        m_PT.m_DragPins = PoseEffectivePins(m_TimeSeconds);
                        xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(m_PT.m_Base);
                        int Grab = m_PT.m_Drag;
                        if (m_PT.m_bFK) for (int c = Grab + 1; c < R.count(); ++c) if (R.m_Par[c] == m_PT.m_Drag) { Grab = c; break; }
                        m_PT.m_Offset = m_PoseBones[Grab].m_Position - FK.WorldPosition(Grab);
                        m_PT.m_Plane = m_PT.m_Start = m_PT.m_Target = m_PT.m_bFK ? m_PoseBones[Grab].m_Position : HandleModel(FK, Grab) + m_PT.m_Offset;
                        m_PT.m_Normal = m_Viewport.m_Scene.m_View.RayFromScreen(Mouse.x, Mouse.y).NormalizeSafeCopy();
                    }
                    else m_PT.m_Note = E2;
                }
                else if (m_PT.m_Hover >= 0 && !m_PT.m_bHoverTip && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    PoseTogglePin(m_PT.m_Hover);
                }
                else if (!m_bMaskEdit && !ImGuizmo::IsOver() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    int iBone = -1; float T = 0;
                    m_PT.m_FKBone = m_Viewport.PickBone(Item, Mouse, iBone, T) ? iBone : -1;
                }
            }
            // keys over the viewport: S suggests, Tab cycles, Enter accepts, Esc keeps the user's pose
            if (m_Viewport.Over() && !ImGui::GetIO().WantTextInput && m_PT.m_Drag < 0)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !ImGui::GetIO().KeyCtrl) xeditor::LogConsole(PoseSuggest("", m_PT.m_Prompt).substr(0, 300), xeditor::log_source::User);
                if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !ImGui::GetIO().KeyCtrl) xeditor::LogConsole(KeySelected(m_TimeSeconds, ImGui::GetIO().KeyShift).substr(0, 300), xeditor::log_source::User);
                if (m_PT.m_Cand >= 0)
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !m_Suggest.m_Candidates.empty()) m_PT.m_Cand = (m_PT.m_Cand + 1) % int(m_Suggest.m_Candidates.size());
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) xeditor::LogConsole(PoseSuggestAccept(m_PT.m_Cand).substr(0, 300), xeditor::log_source::User);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_PT.m_Cand = -1;
                }
            }
        }

        // The selected bone's rotate gizmo (ImGuizmo, the bone's own axes: twist and swing): a drag previews the turn, the release writes it as
        // the bone's key on the top Pose layer (ONE undo step), Esc cancels. Its console twin: PoseRotate.
        void PoseGizmo() noexcept
        {
            const auto& R = m_PT.m_Rig;
            const int b = m_PT.m_FKBone;
            const bool bOn = m_PT.m_bGizmo && b >= 0 && b < R.count() && R.m_Par[b] >= 0 && m_PT.m_Drag < 0 && m_Viewport.RectSize().x > 1 && m_Viewport.RectSize().y > 1;
            if (!bOn) { m_PT.m_bGizmoUsing = false; return; }
            std::string Err;
            xanimation::pose Base;
            if (m_PT.m_bGizmoUsing) Base = m_PT.m_GizmoBase;
            else if (!PoseAt(m_TimeSeconds, Base, Err)) return;
            const xanimation::pose& Cur = (m_PT.m_bGizmoUsing && m_PT.m_bPreview) ? m_PT.m_Preview : Base;
            xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(Cur);
            const auto Off = m_PoseBones[0].m_Position - FK.WorldPosition(0);
            xmath::fmat4 World(xmath::fvec3(1, 1, 1), FK.WorldRotation(b), FK.WorldPosition(b) + Off);

            ImGuizmo::BeginFrame();
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(m_Viewport.RectMin().x, m_Viewport.RectMin().y, m_Viewport.RectSize().x, m_Viewport.RectSize().y);
            const xmath::fmat4 W2V = m_Viewport.m_Scene.m_View.getW2V();
            xmath::fmat4 Proj = m_Viewport.m_Scene.m_View.getV2C();
            Proj.m_10 = -Proj.m_10; Proj.m_11 = -Proj.m_11; Proj.m_12 = -Proj.m_12; Proj.m_13 = -Proj.m_13;      // ImGuizmo is OpenGL-convention (undo the Vulkan Y flip)
            const float Snap[3] = { 15.0f, 15.0f, 15.0f };
            const bool bChanged = ImGuizmo::Manipulate(reinterpret_cast<const float*>(&W2V), reinterpret_cast<const float*>(&Proj), ImGuizmo::ROTATE, ImGuizmo::LOCAL
                                                      , reinterpret_cast<float*>(&World), nullptr, ImGui::GetIO().KeyCtrl ? Snap : nullptr);
            const bool bUsing = ImGuizmo::IsUsing();
            if (!bUsing) m_PT.m_bGizmoCancelled = false;
            if (bUsing && !m_PT.m_bGizmoUsing && !m_PT.m_bGizmoCancelled) { m_PT.m_bGizmoUsing = true; m_PT.m_GizmoBase = Base; m_PT.m_GizmoTime = m_TimeSeconds; m_PT.m_Cand = -1; }
            if (m_PT.m_bGizmoUsing && ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_PT.m_bGizmoUsing = false; m_PT.m_bGizmoCancelled = true; m_PT.m_bPreview = false; m_PT.m_Note = "cancelled"; return; }
            if (m_PT.m_bGizmoUsing && bChanged)
            {
                xanimation::body_ik FB; FB.Setup(R.m_Par); FB.ComputeWorld(m_PT.m_GizmoBase);
                const auto Rp = FB.WorldRotation(R.m_Par[b]);
                m_PT.m_Preview = m_PT.m_GizmoBase;
                m_PT.m_Preview.m_Rotation[b] = (Rp.ConjugateCopy() * World.ExtractRotation()).NormalizeCopy();
                m_PT.m_bPreview = true;
            }
            if (m_PT.m_bGizmoUsing && !bUsing)
            {   // released: the key, one undo step
                m_PT.m_bGizmoUsing = false;
                if (m_PT.m_bPreview)
                {
                    const auto P = m_PT.m_Preview; m_PT.m_bPreview = false;
                    const auto C = m_PT.m_bAutoKey ? PoseCommit(P, m_PT.m_GizmoTime, std::format("Pose rotate {}", R.m_Names[b])) : PosePend(P, m_PT.m_GizmoTime);
                    m_PT.m_Note = C.find(" keys on layer ") != std::string::npos ? std::format("{} turned", R.m_Names[b]) : C;
                    xeditor::LogConsole(std::format("Pose: {} ({})", m_PT.m_Note, C.substr(0, 160)), xeditor::log_source::User);
                }
            }
        }

        // the console's gizmo: the bone turned by Turn (degrees about the model's x, y, z, applied to its current world rotation) at a time, as keys
        std::string PoseRotate(const std::string& Bone, const xmath::fvec3& TurnDeg, double Seconds) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PoseRotate: " + Err;
            const auto& R = m_PT.m_Rig;
            const int b = R.Find(Bone);
            if (b < 0 || R.m_Par[b] < 0) return std::format("PoseRotate: no bone '{}' that can turn", Bone);
            xanimation::pose Base; if (!PoseAt(Seconds, Base, Err)) return "PoseRotate: " + Err;
            xanimation::body_ik FK; FK.Setup(R.m_Par); FK.ComputeWorld(Base);
            constexpr float D = 3.14159265f / 180.0f;
            const auto Turn = (xanimation::details::AxisAngle(xmath::fvec3(0, 0, 1), TurnDeg.m_Z * D) * xanimation::details::AxisAngle(xmath::fvec3(0, 1, 0), TurnDeg.m_Y * D) * xanimation::details::AxisAngle(xmath::fvec3(1, 0, 0), TurnDeg.m_X * D)).NormalizeCopy();
            auto P = Base;
            P.m_Rotation[b] = (FK.WorldRotation(R.m_Par[b]).ConjugateCopy() * Turn * FK.WorldRotation(b)).NormalizeCopy();
            return "PoseRotate: " + PoseCommit(P, Seconds, std::format("Pose rotate {}", R.m_Names[b]));
        }

        // the Pose tool's bar over the viewport
        void PoseToolbar() noexcept
        {
            if (ImGui::Checkbox("Pose", &m_PT.m_bOn) && m_PT.m_bOn) PoseFrame();
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Posing handles: drag a hand, foot, head or the hips (full-body IK, feet in contact held); right-click a handle pins it;\nclick a bone, then drag its tip to turn it alone. Releasing writes keys on the top Pose layer (one undo step).\nS: Suggest Pose (Tab: next, Enter: accept, Esc: keep yours).");
            if (!m_PT.m_bOn) return;
            auto Preset = [&](const char* L, std::initializer_list<int> B) { ImGui::SameLine(); if (ImGui::SmallButton(L)) { m_PT.m_Pins.clear(); for (int b : B) if (b >= 0) m_PT.m_Pins.insert(b); } };
            std::string Err; PoseRig(Err);
            const auto& R = m_PT.m_Rig;
            ImGui::SameLine(); ImGui::TextDisabled("Pins:");
            Preset("Feet", { R.m_LF, R.m_RF }); Preset("Hands", { R.m_LH, R.m_RH }); Preset("Both", { R.m_LF, R.m_RF, R.m_LH, R.m_RH }); Preset("None", {});
            {   // Bone limits: the top Pose layer's BoneLimits (posing and its IK keys respect the skeleton's joint limits); one undo step
                auto* pOv = SelectedOv();
                const int iTop = pOv ? int(pOv->m_Layers.size()) - 1 : -1;
                const bool bPoseTop = iTop >= 0 && pOv->m_Layers[iTop].m_Kind == xanim_package_desc::layer_kind::POSE;
                bool bOn = PoseLimitsOn();
                ImGui::SameLine(); ImGui::BeginDisabled(!bPoseTop || R.m_Lim.empty());
                if (ImGui::Checkbox("Bone limits", &bOn)) SetLayer(iTop, "BoneLimits", bOn ? "true" : "false", bOn ? "false" : "true");
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) xeditor::hint::Text(R.m_Lim.empty()
                    ? "The skeleton's joint limits are off (turn them on in the Skeleton Editor's Joints mode): posing is free"
                    : "Posing and IK keys keep every joint inside the skeleton's limits (this Pose layer's BoneLimits).\nTurn off only for an impossible pose, such as a broken leg.");
            }
            ImGui::SameLine(); ImGui::Checkbox("Auto feet", &m_PT.m_bAutoPins);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, m_PT.m_bAutoKey ? IM_COL32(170, 40, 40, 255) : IM_COL32(70, 70, 70, 255));
            if (ImGui::SmallButton(m_PT.m_bAutoKey ? "Auto-key" : "Auto-key off")) { m_PT.m_bAutoKey = !m_PT.m_bAutoKey; if (m_PT.m_bAutoKey) m_PT.m_bPending = false; }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) xeditor::hint::Text("Auto-key (red): every edit writes keys at the playhead.\nOff: an edit is a pending pose (shown) that K keys; moving the playhead drops it.\nK: key the selected bone (or the last dragged handle as an IK key); Shift+K: every bone the layer keys.");
            ImGui::SameLine(); if (ImGui::SmallButton("Key (K)")) xeditor::LogConsole(KeySelected(m_TimeSeconds, false).substr(0, 300), xeditor::log_source::User);
            ImGui::SameLine(); if (ImGui::SmallButton("Key IK")) { if (m_PT.m_LastEff >= 0) xeditor::LogConsole(IkKey(m_PT.m_Rig.m_Names[m_PT.m_LastEff], m_TimeSeconds, nullptr, -1, false, false).substr(0, 300), xeditor::log_source::User); else m_PT.m_Note = "drag a handle first: Key IK keys its target"; }
            if (ImGui::IsItemHovered()) xeditor::hint::Text("An IK key for the last dragged handle: its target (and pin) at the playhead; between IK keys the body is solved on every frame");
            ImGui::SameLine(); if (ImGui::SmallButton("Mirror")) xeditor::LogConsole(PoseMirror(m_TimeSeconds).substr(0, 300), xeditor::log_source::User);
            ImGui::SameLine(); if (ImGui::SmallButton("Copy")) { xanimation::pose P; if (PoseAt(m_TimeSeconds, P, Err)) { m_PT.m_Copy.resize(P.count()); for (int b = 0; b < P.count(); ++b) m_PT.m_Copy[b] = P.get(b); } }
            ImGui::SameLine(); ImGui::BeginDisabled(m_PT.m_Copy.empty()); if (ImGui::SmallButton("Paste")) xeditor::LogConsole(PosePasteAt(m_TimeSeconds).substr(0, 300), xeditor::log_source::User); ImGui::EndDisabled();
            ImGui::SameLine(); if (ImGui::SmallButton("Suggest (S)")) xeditor::LogConsole(PoseSuggest("", m_PT.m_Prompt).substr(0, 300), xeditor::log_source::User);
            ImGui::SameLine(); ImGui::SetNextItemWidth(160); ImGui::InputTextWithHint("##poseprompt", "what the pose is (optional)", m_PT.m_Prompt, sizeof(m_PT.m_Prompt));
            if (!m_PT.m_Note.empty()) { ImGui::SameLine(); ImGui::TextColored(m_PT.m_Note == "reached" ? ImVec4(0.6f, 1, 0.6f, 1) : ImVec4(1, 0.6f, 0.5f, 1), "%s", m_PT.m_Note.c_str()); }
        }

        // the camera on the character where it is now (a walk leaves the frame the clip opened with)
        void PoseFrame() noexcept
        {
            if (m_PoseBones.empty()) return;
            xmath::fvec3 Mn = m_PoseBones[0].m_Position, Mx = Mn;
            for (const auto& B : m_PoseBones) { Mn = Mn.Min(B.m_Position); Mx = Mx.Max(B.m_Position); }
            m_Viewport.m_Scene.m_Center = (Mn + Mx) * 0.5f;
            m_Viewport.m_Scene.m_Radius = std::max(0.25f, (Mx - Mn).Length() * 0.6f);
            m_Viewport.m_Scene.m_bReframe = true;
        }

        std::string PosePasteAt(double Seconds) noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PosePaste: " + Err;
            if (int(m_PT.m_Copy.size()) != m_PT.m_Rig.count()) return "PosePaste: nothing copied (PoseCopy first)";
            xanimation::pose P; P.resize(m_PT.m_Rig.count());
            for (int b = 0; b < P.count(); ++b) P.set(b, m_PT.m_Copy[b]);
            return "PosePaste: " + PoseCommit(P, Seconds, "Pose paste");
        }

        std::string PoseToolInfo() noexcept
        {
            std::string Err;
            if (!PoseRig(Err)) return "PoseTool: " + Err;
            const auto& R = m_PT.m_Rig;
            const auto Pins = PoseEffectivePins(m_TimeSeconds);
            std::string T = std::format("PoseTool: {}; time {:.3f} s; limits {}; auto feet {}\n", m_PT.m_bOn ? "on" : "off", m_TimeSeconds, R.m_Lim.empty() ? "none (the skeleton's limits are off)" : PoseLimitsOn() ? "the skeleton's" : "off for this layer (BoneLimits)", m_PT.m_bAutoPins ? "on" : "off");
            for (int b : R.Effectors())
            {
                ImVec2 S(-1, -1); for (const auto& [h, P] : m_PT.m_Screen) if (h == b) S = P;
                ImVec2 Pc(-1, -1); for (const auto& [h, Box] : m_PT.m_PinBoxes) if (h == b) Pc = ImVec2((Box.first.x + Box.second.x) * 0.5f, (Box.first.y + Box.second.y) * 0.5f);
                const auto& A = R.m_AttachM[b];
                T += std::format("handle {} ({}): screen {:.0f} {:.0f}; pin icon {:.0f} {:.0f}; attach {:.3f} {:.3f} {:.3f} ({}){}{}\n", R.Name(b), R.m_Names[b], S.x, S.y, Pc.x, Pc.y, A.m_X, A.m_Y, A.m_Z, A.Length() > 0 ? (b == R.m_LF || b == R.m_RF ? "sole" : "palm") : "the joint", Pins.count(b) ? " pinned" : "", m_PT.m_Pins.count(b) ? " (by the user)" : m_PT.m_Unpinned.count(b) ? " (contact pin off)" : "");
            }
            T += "contacts: " + m_PT.m_ContactText + "\n";
            if (m_PT.m_FKBone >= 0) T += std::format("FK bone: {}; tip screen {:.0f} {:.0f}\n", R.m_Names[m_PT.m_FKBone], m_PT.m_TipScreen.x, m_PT.m_TipScreen.y);
            T += std::format("drag: {}; last: {}; candidates {} (showing {})", m_PT.m_Drag >= 0 ? R.Name(m_PT.m_Drag) : "none", m_PT.m_Note.empty() ? "(none)" : m_PT.m_Note, m_Suggest.m_Candidates.size(), m_PT.m_Cand);
            return T;
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
                ImGui::SameLine(); ImGui::Text("generating... %.0f s", kimodo::job::Now() - m_K.m_Job.m_StartTime);
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
            KimodoLayerPoll();
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

        // the description's PreviewProps as wire shapes over the bones (the shared shape the Ragdoll editor draws its bodies with)
        void DrawPreviewProps() noexcept
        {
            auto* pDesc = Desc();
            if (!pDesc) return;
            const auto& Names = pDesc->m_ResolvedSkeletonBones.m_Bones;
            for (const auto& P : pDesc->m_PreviewProps)
            {
                xmath::fvec3 At = P.m_Offset; xmath::fquat Q = xmath::fquat::fromIdentity();
                if (!P.m_Bone.empty())
                {
                    int b = -1; for (int i = 0; i < int(Names.size()) && i < int(m_PoseWorlds.size()); ++i) if (Names[i].m_Name == P.m_Bone) { b = i; break; }
                    if (b < 0) continue;            // (a bone the skeleton lacks: nothing drawn; the Description shows the name)
                    const auto& W = m_PoseWorlds[std::size_t(b)];
                    Q = W.ExtractRotation().NormalizeSafeCopy();
                    At = W * P.m_Offset;
                }
                const int K = P.m_Shape == xanim_package_desc::prop_shape::CAPSULE ? 0 : P.m_Shape == xanim_package_desc::prop_shape::SPHERE ? 1 : 2;
                xskeleton_editor::EmitWireShape(m_Viewport.m_ExtraLines, K, P.m_Size.m_X, P.m_Size.m_Y, P.m_Size, At, Q, IM_COL32(255, 200, 60, 255));
            }
            for (std::size_t i = 1; i < m_Flight.size(); ++i) xskeleton_editor::EmitSegment(m_Viewport.m_ExtraLines, m_Flight[i - 1], m_Flight[i], IM_COL32(90, 230, 255, 255), IM_COL32(90, 230, 255, 255));     // the last BallFlight
        }

        void RenderViewport() noexcept
        {
            if (const int f = m_Recorder.Frame(); f >= 0) { m_bPlaying = false; m_TimeSeconds = float(f) / float(m_Recorder.m_Fps); m_LoopsElapsed = 0; }
            else if (m_bRecorderWasRunning && !m_Recorder.Running()) { m_bRecorderWasRunning = false; xeditor::LogConsole(m_Recorder.m_Result, xeditor::log_source::User); }
            m_Viewport.m_Scene.m_State.m_bHideOverlay = m_Recorder.Running();
            m_CameraToolbar.Render(ImGui::GetContentRegionAvail(), m_Viewport.m_Scene.m_State, [this] { RenderViewportBody(); }
                                  , [this](bool bHorizontal, float ButtonHeight)
                                    {
                                        xgeom_skin_editor::skin_under::Button(m_bShowSkin, bHorizontal, ButtonHeight, "The preview skin (the description's PreviewSkin), lit, under the bones");
                                        if (bHorizontal) ImGui::SameLine();
                                        const bool bOn = m_bFollow;
                                        if (bOn) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
                                        if (ImGui::Button(bHorizontal ? "Follow###Follow" : "F###Follow", bHorizontal ? ImVec2(60.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) { m_bFollow = !m_bFollow; m_bFollowHas = false; }
                                        if (bOn) ImGui::PopStyleColor();
                                        if (ImGui::IsItemHovered()) xeditor::hint::Text("The camera follows the character as it moves (%s)", bOn ? "on" : "off");
                                        if (bHorizontal) ImGui::SameLine();
                                        const bool bRec = m_Recorder.Running();
                                        if (bRec) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
                                        const std::string Label = bRec ? std::format("{}/{}###Video", m_Recorder.m_iShown, m_Recorder.m_nFrames) : std::string(bHorizontal ? "Video###Video" : "V###Video");
                                        if (ImGui::Button(Label.c_str(), bHorizontal ? ImVec2(60.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight)))
                                            xeditor::LogConsole(bRec ? m_Undo.Query("RenderVideo -Cancel 1") : m_Undo.Query("RenderVideo"), xeditor::log_source::User);
                                        if (bRec) ImGui::PopStyleColor();
                                        if (ImGui::IsItemHovered()) xeditor::hint::Text(bRec ? "Recording the clip to a video: click to stop" : "Render video: the selected clip as the preview shows it (skin, props, camera), 30 fps, to Cache\\Temp\\AnimVideo (ffmpeg encodes the .mp4)");
                                    });
            if (m_Recorder.Running())          // (this frame's view, read back at the flip; the next frame writes it)
                if (auto* pHost = xeditor::host::current())
                    if (auto* pWindow = pHost->find<xgpu::window>()) m_Recorder.Capture(*pWindow, m_Viewport.RectMin(), m_Viewport.RectSize());
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
            PoseToolbar();
            EvaluatePose(*pSkeleton);
            if (m_bFollow && !m_PoseBones.empty())      // the camera's target moves by the character's move (its bones' centre, on the ground plane): the user's own offset is kept
            {
                xmath::fvec3 C(0, 0, 0); for (const auto& B : m_PoseBones) C = C + B.m_Position; C = C * (1.0f / float(m_PoseBones.size())); C.m_Y = 0;
                if (m_bFollowHas) m_Viewport.m_Scene.m_Target = m_Viewport.m_Scene.m_Target + (C - m_FollowLast);
                m_FollowLast = C; m_bFollowHas = true;
            }
            UpdateMaskShown();
            PoseBeforeRender();
            KimodoPreviewBones();
            std::set<int> PoseSel; if (m_PT.m_bOn && m_PT.m_FKBone >= 0) PoseSel.insert(m_PT.m_FKBone);
            const std::set<int>* pSel = !m_MaskShown.empty() ? &m_MaskShown : PoseSel.empty() ? nullptr : &PoseSel;
            const xskeleton_editor::viewport_item Item{ .m_pSkeleton = pSkeleton, .m_Bones = m_PoseBones, .m_pSelected = pSel };
            // the preview skin under the bones: its own skeleton follows ours by bone name (skin_under::PoseFrom)
            if (auto* pDesc = Desc(); pDesc && pDesc->m_PreviewSkin.m_Instance.m_Value != m_SkinRef.m_Instance.m_Value)
            {
                xresource::g_Mgr.ReleaseRef(m_SkinRef); xresource::g_Mgr.ReleaseRef(m_SkinSkelRef);
                m_SkinRef = {}; m_SkinRef.m_Instance = pDesc->m_PreviewSkin.m_Instance; m_SkinSkelRef = {};
            }
            if (m_bShowSkin && !m_SkinRef.empty() && m_PoseWorlds.size() == pSkeleton->getBones().size())
                if (auto* pSkin = xresource::g_Mgr.getResource(m_SkinRef); pSkin && m_SkinUnder.Ready(m_pDevice))
                {
                    if (m_SkinSkelRef.empty()) { m_SkinSkelRef = {}; m_SkinSkelRef.m_Instance = pSkin->getSkeletonRef().m_Instance; }
                    if (auto* pSkinSkel = xresource::g_Mgr.getResource(m_SkinSkelRef))
                    {
                        m_SkinUnder.PoseFrom(*pSkeleton, m_PoseWorlds, *pSkinSkel, m_SkinWorlds);
                        m_Viewport.m_Under = [this, pSkin, pSkinSkel](const ImVec2& Min, const ImVec2& Avail) { m_SkinUnder.Render(*pSkin, *pSkinSkel, m_SkinWorlds, m_Viewport, Min, Avail, m_bOpen); };
                    }
                }
            DrawPreviewProps();
            m_Viewport.Render(std::span<const xskeleton_editor::viewport_item>(&Item, 1));
            m_Viewport.m_Under = nullptr;
            if (!m_Recorder.Running()) PoseAfterRender(Item);     // (a video shows the clip, not the handles)

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

            // The footer line stays at the bottom; the layer timeline (the clip and its layers as rows) takes what is above it
            const float Footer = ImGui::GetTextLineHeightWithSpacing();
            (void)pName;
            if (SelectedOv()) RenderLayerTimeline(std::max(ImGui::GetContentRegionAvail().y - Footer, 60.0f));
            else if (xgpu::tools::imgui::timeline::Draw(m_Timeline, m_TimeSeconds, Length, static_cast<float>(pClip->m_FPS), {}, "playback_timeline", pName, std::max(ImGui::GetContentRegionAvail().y - Footer, 0.0f)))
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
            if (pClip) Text += std::format("does: moves {:.2f} units/s at {:.0f} deg from its facing, turns {:.1f} deg/s, ends {:.1f} deg apart{}, centroid height {:.2f}; {} events\n"
                , pClip->m_Speed, pClip->m_Direction, pClip->m_TurnRate, pClip->m_LoopError, pClip->m_LoopError < 5 ? " (loops)" : "", pClip->m_Height, pClip->m_nEvents);
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
            std::string Path; float Vx = 0, Vz = 0; bool bPath = false;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Path))
            {
                if (std::sscanf(Path.c_str(), "%f %f", &Vx, &Vz) != 2) return "KimodoGenerate: -Path wants \"vx vz\" (metres per second)";
                bPath = true;
            }
            return S.KimodoGenerate(Prompt, Seconds, iSeed, bPath ? &Vx : nullptr, Vz);
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
            if (Kind == "Folder" || Kind == "folder") return S.LayerAddContainer(Name, false);
            if (Kind == "Group" || Kind == "group") return S.LayerAddContainer(Name, true);
            if (!Kind.empty() && Kind != "Clip" && Kind != "clip") return "LayerAdd: -Kind is Clip, Pose, Folder or Group";
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
            std::string L, Bone, Only, Shared; int iL = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "LayerMask: bad arguments";
            S.m_iLayer = iL;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Shared)) return S.LayerMaskShared(iL, Shared);
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hB, Bone)) return "LayerMask: -Bone (or -Shared) is required";
            xeditor::cmd_util::GetArg(m_Parser, m_hC, Only);
            return S.LayerMaskToggle(iL, Bone, Only == "1" || Only == "true");
        }
        case kind::layer_parent:
        {
            std::string L, Pa; int iL = -1, iP = -2;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Pa)
                || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc() || std::from_chars(Pa.data(), Pa.data() + Pa.size(), iP).ec != std::errc()) return "LayerParent: bad arguments";
            return S.LayerParent(iL, iP);
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
        case kind::kimodo_layer_add:
        {
            std::string Pr, Se, St, Sd, Nm; double Secs = 2.0, Start = S.m_TimeSeconds; int Seed = 1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Pr)) return "KimodoLayerAdd: -Prompt text";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Se) && std::from_chars(Se.data(), Se.data() + Se.size(), Secs).ec != std::errc()) return "KimodoLayerAdd: -Seconds is a number";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hC, St) && std::from_chars(St.data(), St.data() + St.size(), Start).ec != std::errc()) return "KimodoLayerAdd: -Start is seconds";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Sd) && std::from_chars(Sd.data(), Sd.data() + Sd.size(), Seed).ec != std::errc()) return "KimodoLayerAdd: -Seed is a number";
            xeditor::cmd_util::GetArg(m_Parser, m_hE, Nm);
            return S.KimodoLayerAdd(Pr, Secs, Start, Seed, Nm);
        }
        case kind::kimodo_regenerate:
        {
            std::string L, Sd; int iL = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "KimodoRegenerate: -Layer n";
            xeditor::cmd_util::GetArg(m_Parser, m_hB, Sd);
            std::string Wt; xeditor::cmd_util::GetArg(m_Parser, m_hC, Wt);
            auto R = S.KimodoRegenerate(iL, Sd);
            if (Wt != "1" || S.m_KL.m_Job != "running") return R;
            // -Wait 1: the same background run, waited on here (scripts); the editor's frame loop would poll it the same way
            for (int i = 0; i < 6000 && S.m_KL.m_Job == "running"; ++i) { Sleep(20); S.KimodoLayerPoll(); }
            return S.m_KL.m_Text + (S.m_KL.m_Job == "candidate" ? "  nothing is written yet: KimodoAcceptCandidate -Candidate i (one undo step; the current take is kept on disk)" : std::format("  KimodoRegenerate: {} - {}", S.m_KL.m_Job, S.m_KL.m_LastError));
        }
        case kind::kimodo_accept_candidate:
        {
            std::string C; int i = 0;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hA, C) && std::from_chars(C.data(), C.data() + C.size(), i).ec != std::errc()) return "KimodoAcceptCandidate: -Candidate is an index";
            return S.KimodoAcceptCandidate(i);
        }
        case kind::kimodo_show:
        {
            std::string C; int i = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, C) || std::from_chars(C.data(), C.data() + C.size(), i).ec != std::errc()) return "KimodoShow: -Candidate i";
            if (i >= 0 && !S.KimodoLayerDone(i)) return "KimodoShow: no such candidate ready";
            S.m_KL.m_iShow = i; return std::format("KimodoShow: {}", i < 0 ? "none" : std::format("candidate {}", i));
        }
        case kind::bake_to_pose:
        {
            std::string L, Ev; int iL = -1, E = 1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "BakeToPose: -Layer n";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Ev) && std::from_chars(Ev.data(), Ev.data() + Ev.size(), E).ec != std::errc()) return "BakeToPose: -Every n";
            return S.BakeToPose(iL, E);
        }
        case kind::kimodo_take_check: return S.KimodoTakeCheck();
        case kind::pose_rotate:
        {
            std::string Bn, Tn, Tm; xmath::fvec3 Td(0, 0, 0); double T = S.m_TimeSeconds;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Bn) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Tn) || std::sscanf(Tn.c_str(), "%f %f %f", &Td.m_X, &Td.m_Y, &Td.m_Z) != 3) return "PoseRotate: -Bone name -Turn \"x y z\"";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hC, Tm) && (std::from_chars(Tm.data(), Tm.data() + Tm.size(), T).ec != std::errc() || T < 0)) return "PoseRotate: -Time is seconds";
            return S.PoseRotate(Bn, Td, T);
        }
        case kind::kimodo_takes_to_layers:
        {
            std::string L; int iL = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, L) || std::from_chars(L.data(), L.data() + L.size(), iL).ec != std::errc()) return "KimodoTakesToLayers: -Layer n";
            return S.KimodoTakesToLayers(iL);
        }
        case kind::pose_tool:
        {
            std::string On, Bone;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hA, On)) { S.m_PT.m_bOn = On == "1" || On == "true"; if (S.m_PT.m_bOn) S.PoseFrame(); }
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Bone)) { std::string E; if (!S.PoseRig(E)) return "PoseTool: " + E; S.m_PT.m_FKBone = S.m_PT.m_Rig.Find(Bone); if (S.m_PT.m_FKBone < 0) return std::format("PoseTool: no bone '{}'", Bone); }
            return S.PoseToolInfo();
        }
        case kind::pose_drag:
        {
            std::string Eff, Mv, Pins, Only, Time, Commit; xmath::fvec3 M(0, 0, 0);
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, Eff) || !xeditor::cmd_util::GetArg(m_Parser, m_hB, Mv) || std::sscanf(Mv.c_str(), "%f %f %f", &M.m_X, &M.m_Y, &M.m_Z) != 3) return "PoseDrag: -Effector name -Move \"dx dy dz\" (cm)";
            xeditor::cmd_util::GetArg(m_Parser, m_hC, Pins); xeditor::cmd_util::GetArg(m_Parser, m_hD, Only); xeditor::cmd_util::GetArg(m_Parser, m_hF, Commit);
            double T = S.m_TimeSeconds;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hE, Time) && (std::from_chars(Time.data(), Time.data() + Time.size(), T).ec != std::errc() || T < 0)) return "PoseDrag: -Time is seconds";
            return S.PoseDrag(Eff, M, Pins, Only == "1" || Only == "true", T, !(Commit == "0" || Commit == "false"));
        }
        case kind::new_clip:
        {
            std::string Name, Sec, Fps, From; double Seconds = 2; int FPS = 30;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, Name);
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Sec) && (std::from_chars(Sec.data(), Sec.data() + Sec.size(), Seconds).ec != std::errc() || !(Seconds > 0))) return "NewClip: -Seconds > 0";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hC, Fps) && (std::from_chars(Fps.data(), Fps.data() + Fps.size(), FPS).ec != std::errc() || FPS < 1 || FPS > 240)) return "NewClip: -FPS 1..240";
            xeditor::cmd_util::GetArg(m_Parser, m_hD, From);
            if (!From.empty() && From != "rest" && From != "playhead") return "NewClip: -FromPose rest|playhead";
            return S.NewClip(Name, Seconds, FPS, From == "playhead");
        }
        case kind::ik_key:
        {
            std::string Eff, Time, Pos, Pin, Rot, Del; double T = S.m_TimeSeconds;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, Eff);
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Time) && (std::from_chars(Time.data(), Time.data() + Time.size(), T).ec != std::errc() || T < 0)) return "IkKey: -Time is seconds";
            xmath::fvec3 P; const bool bPos = xeditor::cmd_util::GetArg(m_Parser, m_hC, Pos);
            if (bPos && std::sscanf(Pos.c_str(), "%f %f %f", &P.m_X, &P.m_Y, &P.m_Z) != 3) return "IkKey: -Pos \"x y z\"";
            int PinState = -1; if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Pin)) PinState = (Pin == "1" || Pin == "true") ? 1 : 0;
            xeditor::cmd_util::GetArg(m_Parser, m_hE, Rot); xeditor::cmd_util::GetArg(m_Parser, m_hF, Del);
            if (std::string Probe; xeditor::cmd_util::GetArg(m_Parser, m_hG, Probe) && (Probe == "1" || Probe == "true")) return S.HandleProbe(Eff, T);
            if (std::string Prop; xeditor::cmd_util::GetArg(m_Parser, m_hH, Prop))
            {
                if (bPos) return "IkKey: -Pos or -Prop, not both";
                std::string E; if (!S.PropAt(Prop, T, P, E)) return "IkKey: " + E;
                return S.IkKey(Eff, T, &P, PinState, Rot == "1" || Rot == "true", Del == "1" || Del == "true") + std::format(" (at prop '{}')", Prop);
            }
            return S.IkKey(Eff, T, bPos ? &P : nullptr, PinState, Rot == "1" || Rot == "true", Del == "1" || Del == "true");
        }
        case kind::key_selected:
        {
            std::string All, Time; double T = S.m_TimeSeconds;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, All);
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Time) && (std::from_chars(Time.data(), Time.data() + Time.size(), T).ec != std::errc() || T < 0)) return "KeySelected: -Time is seconds";
            return S.KeySelected(T, All == "1" || All == "true");
        }
        case kind::clip_event:
        {
            std::string A, I, D, K, T, E, B, N;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, A); xeditor::cmd_util::GetArg(m_Parser, m_hB, I); xeditor::cmd_util::GetArg(m_Parser, m_hC, D); xeditor::cmd_util::GetArg(m_Parser, m_hD, K);
            xeditor::cmd_util::GetArg(m_Parser, m_hE, T); xeditor::cmd_util::GetArg(m_Parser, m_hF, E); xeditor::cmd_util::GetArg(m_Parser, m_hG, B); xeditor::cmd_util::GetArg(m_Parser, m_hH, N);
            return S.ClipEvent(A, I, D, K, T, E, B, N);
        }
        case kind::detect_footsteps: return S.DetectFootsteps();
        case kind::render_video:
        {
            std::string F, Sec, Cancel; int Fps = 30; float Seconds = 0;
            xeditor::cmd_util::GetArg(m_Parser, m_hC, Cancel);
            if (Cancel == "1" || Cancel == "true") { if (!S.m_Recorder.Running()) return "RenderVideo: not recording"; S.m_Recorder.Cancel(); S.m_bRecorderWasRunning = false; return S.m_Recorder.m_Result; }
            if (xeditor::cmd_util::GetArg(m_Parser, m_hA, F) && (std::from_chars(F.data(), F.data() + F.size(), Fps).ec != std::errc() || Fps < 1 || Fps > 120)) return "RenderVideo: -Fps is 1 to 120";
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Sec) && (std::from_chars(Sec.data(), Sec.data() + Sec.size(), Seconds).ec != std::errc() || Seconds <= 0)) return "RenderVideo: -Seconds is a positive number";
            return S.StartVideo(Fps, Seconds);
        }
        case kind::ball_flight:
        {
            std::string Eff = "RightFoot", Time, Prop = "Ball", Off; double T = S.m_TimeSeconds;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, Eff); xeditor::cmd_util::GetArg(m_Parser, m_hC, Prop); xeditor::cmd_util::GetArg(m_Parser, m_hD, Off);
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, Time) && (std::from_chars(Time.data(), Time.data() + Time.size(), T).ec != std::errc() || T < 0)) return "BallFlight: -Time is seconds";
            if (Off == "1" || Off == "true") { S.m_Flight.clear(); return "BallFlight: hidden"; }
            return S.BallFlight(Eff, T, Prop);
        }        case kind::auto_key:
        {
            std::string On; xeditor::cmd_util::GetArg(m_Parser, m_hA, On);
            S.m_PT.m_bAutoKey = On == "1" || On == "true";
            if (S.m_PT.m_bAutoKey) S.m_PT.m_bPending = false;
            return std::format("AutoKey: {}", S.m_PT.m_bAutoKey ? "on (edits write keys)" : "off (an edit is a pending pose; K keys it)");
        }
        case kind::pose_pin:
        {
            std::string H, P, E;
            if (!S.PoseRig(E)) return "PosePin: " + E;
            auto& R = S.m_PT.m_Rig;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hB, P))
            {
                auto Set = [&](std::initializer_list<int> B) { S.m_PT.m_Pins.clear(); for (int b : B) if (b >= 0) S.m_PT.m_Pins.insert(b); };
                if (P == "Feet") Set({ R.m_LF, R.m_RF }); else if (P == "Hands") Set({ R.m_LH, R.m_RH }); else if (P == "Both") Set({ R.m_LF, R.m_RF, R.m_LH, R.m_RH }); else if (P == "None") Set({});
                else if (P == "Auto 1" || P == "Auto 0") S.m_PT.m_bAutoPins = P == "Auto 1";
                else return "PosePin: -Preset Feet|Hands|Both|None|\"Auto 1|0\"";
            }
            if (xeditor::cmd_util::GetArg(m_Parser, m_hA, H))
            {
                const int b = R.Find(H);
                if (b < 0) return std::format("PosePin: no handle '{}'", H);
                if (S.m_PT.m_Pins.count(b)) S.m_PT.m_Pins.erase(b); else S.m_PT.m_Pins.insert(b);
            }
            return S.PoseToolInfo();
        }
        case kind::pose_mirror: case kind::pose_copy: case kind::pose_paste:
        {
            std::string Time; double T = S.m_TimeSeconds;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hA, Time) && (std::from_chars(Time.data(), Time.data() + Time.size(), T).ec != std::errc() || T < 0)) return "Pose: -Time is seconds";
            if (m_Kind == kind::pose_mirror) return S.PoseMirror(T);
            if (m_Kind == kind::pose_paste) return S.PosePasteAt(T);
            std::string E; xanimation::pose P;
            if (!S.PoseRig(E) || !S.PoseAt(T, P, E)) return "PoseCopy: " + E;
            S.m_PT.m_Copy.resize(P.count()); for (int b = 0; b < P.count(); ++b) S.m_PT.m_Copy[b] = P.get(b);
            return std::format("PoseCopy: the pose at {:.3f} s ({} bones)", T, P.count());
        }
        case kind::pose_suggest:
        {
            std::string Seeds, Prompt, Acc, Show; int i = 0;
            if (xeditor::cmd_util::GetArg(m_Parser, m_hC, Acc)) { if (std::from_chars(Acc.data(), Acc.data() + Acc.size(), i).ec != std::errc()) return "PoseSuggest: -Accept is an index"; return S.PoseSuggestAccept(i); }
            if (xeditor::cmd_util::GetArg(m_Parser, m_hD, Show))
            {
                if (std::from_chars(Show.data(), Show.data() + Show.size(), i).ec != std::errc() || i >= int(S.m_Suggest.m_Candidates.size())) return "PoseSuggest: -Show is a candidate index or -1";
                S.m_PT.m_Cand = i; return std::format("PoseSuggest: showing {}", i);
            }
            xeditor::cmd_util::GetArg(m_Parser, m_hA, Seeds); xeditor::cmd_util::GetArg(m_Parser, m_hB, Prompt);
            return S.PoseSuggest(Seeds, Prompt);
        }
        case kind::layer_timeline_edit:
        {
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A)) return "LayerTimelineEdit: -Ops is required";
            const auto E = S.TLApply(A);
            return E.empty() ? std::format("LayerTimelineEdit: done (revision {})", S.TLRevision()) : "LayerTimelineEdit: " + E;
        }
        case kind::layer_timeline:
        {
            std::string T0, T1, T2, T3, T4;
            xeditor::cmd_util::GetArg(m_Parser, m_hA, T0); xeditor::cmd_util::GetArg(m_Parser, m_hB, T1); xeditor::cmd_util::GetArg(m_Parser, m_hC, T2);
            xeditor::cmd_util::GetArg(m_Parser, m_hD, T3); xeditor::cmd_util::GetArg(m_Parser, m_hE, T4);
            return S.TLInfo(T0, T1, T2, T3, T4);
        }
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
