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
        enum class kind { select_clip, play, pause, seek, set_speed, info, list_clips, kimodo_generate, kimodo_accept, kimodo_discard, kimodo_info };

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
            default: break;
            }
        }
        std::string Query() noexcept override;

        xcmdline::parser::handle m_hA, m_hB, m_hC;
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
        bool KimodoRetargetPreview(std::string& Error) noexcept
        {
            auto* pDesc = Desc();
            const std::uint64_t Key = pDesc->m_Kimodo.m_Retarget.m_Instance.m_Value;
            if (!m_K.m_pRetarget || m_K.m_pRetarget->m_Key != Key)
            {
                auto Ctx = std::make_unique<kimodo_state::retarget_ctx>();
                const auto Path = ProjectPath() / std::format(L"Descriptors/Retarget/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", Key & 0xff, (Key & 0xff00) >> 8, Key);
                xproperty::settings::context Context{};
                if (auto Err = Ctx->m_Descriptor.Serialize(true, Path.wstring(), Context); Err) { Error = "the Retarget could not be read"; return false; }
                if (auto Err = xretarget_desc::MakeMapping(Ctx->m_Descriptor, Ctx->m_Mapping); Err) { Error = "the Retarget's mapping could not be made"; return false; }
                Ctx->m_Retargeter.Init(Ctx->m_Mapping.sourceCalibrated(), Ctx->m_Mapping.destinationCalibrated(), Ctx->m_Mapping.m_Auto.m_Map);
                Ctx->m_Key = Key;
                m_K.m_pRetarget = std::move(Ctx);
            }
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
            if (auto* pClip = SelectedClip())
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
            const xskeleton_editor::viewport_item Item{ .m_pSkeleton = pSkeleton, .m_Bones = m_PoseBones };
            m_Viewport.Render(std::span<const xskeleton_editor::viewport_item>(&Item, 1));
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
