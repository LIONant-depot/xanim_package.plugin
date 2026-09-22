#ifndef XANIM_PACKAGE_EDITOR_H
#define XANIM_PACKAGE_EDITOR_H
#pragma once

// The Anim Package editor: opens an animation package from the asset browser in its own window. The clips the compiler found in the import
// files are listed with their options (name, delete, loop, down sample, trim, root motion), edited in the table or through commands, all
// undoable. A clip plays on the skeleton it is bound to, in a 3D view, with a transport bar and a timeline. Hosts include this header and open
// editors through xeditor::open_resource_editors.
#include "source/Tools/Editor/xeditor_descriptor_editor.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_InspectorPickers.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_Resources.h"
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_editor_scene.h"
#include "plugins/xanim_package.plugin/source/xanim_package.h"
#include "plugins/xanim_package.plugin/source/xanim_package_descriptor.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit
#include "source/tools/xgpu_imgui_timeline.h"

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
        enum class kind { select_clip, play, pause, seek, set_speed, info, list_clips };

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
            default: break;
            }
        }
        std::string Query() noexcept override;

        xcmdline::parser::handle m_hA, m_hB;
    };

    //--------------------------------------------------------------------------------------------
    // The editor
    //--------------------------------------------------------------------------------------------
    struct session : xeditor::descriptor_editor
    {
        using desc = xanim_package_desc::descriptor;

        playback_cmd                                    m_SelectClip, m_Play, m_Pause, m_Seek, m_SetSpeed, m_Info, m_ListClips;

        xskeleton_editor::scene                         m_Scene;
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
        int                                             m_iSpeedIndex = xgpu::tools::editors::g_DefaultSpeedIndex;
        xgpu::tools::imgui::timeline::state             m_Timeline;

        session(xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : descriptor_editor("AnimPackage", Guid, LibraryGuid, pDevice)
            , m_SelectClip(m_Undo, *this, playback_cmd::kind::select_clip, "SelectClip", "Selects a clip to preview. Usage: SelectClip -Source index -Clip index (see ListClips)")
            , m_Play      (m_Undo, *this, playback_cmd::kind::play,        "Play",       "Plays the selected clip. Usage: Play")
            , m_Pause     (m_Undo, *this, playback_cmd::kind::pause,       "Pause",      "Pauses the playback. Usage: Pause")
            , m_Seek      (m_Undo, *this, playback_cmd::kind::seek,        "Seek",       "Moves the playback to a time in seconds. Usage: Seek -Time seconds")
            , m_SetSpeed  (m_Undo, *this, playback_cmd::kind::set_speed,   "SetSpeed",   "Sets the playback speed step (0 = 0.25x, 3 = 1x, 7 = 3x). Usage: SetSpeed -Index step")
            , m_Info      (m_Undo, *this, playback_cmd::kind::info,        "PlaybackInfo","The selected clip, the time and whether it plays. Usage: PlaybackInfo")
            , m_ListClips (m_Undo, *this, playback_cmd::kind::list_clips,  "ListClips",  "Every clip with its options and whether it is compiled. Usage: ListClips")
            , m_CameraCmds(m_Undo, m_Scene.Camera())
        {
            // Reading the descriptor also cross-checks the skeleton's compiled bone list: not having it yet does not make the clips unusable
            m_Document.m_TolerateReadError = [](const xresource_pipeline::descriptor::base& D) { return !static_cast<const desc&>(D).m_SkeletonRef.empty(); };
            m_Document.Load();
            BindDescriptorInspector();
            e10::WireResourcePickerCallbacks(m_DescriptorInspector.m_Inspector);
            MergeDetails();

            AddPanel("Clips",                dock::right,  [this] { RenderClips(); });
            AddPanel("AnimPackage Viewport", dock::center, [this] { RenderViewport(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            AddPanel("Playback",             dock::bottom, [this] { RenderPlayback(); });
            AddPanel("Description",          dock::left,   [this] { m_DescriptorInspector.Show(); });

            m_bReady = pDevice && m_Scene.Init(*pDevice);
            Reload();
        }

        ~session() noexcept override
        {
            xresource::g_Mgr.ReleaseRef(m_Ref);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            m_Scene.Release();
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
            m_Scene.m_Center  = Center;
            m_Scene.m_Radius  = Radius;
            m_Scene.m_bReframe = true;

            m_bShowable = true;
            if (m_iSelectedSource >= 0) SelectDescriptorClip(m_iSelectedSource, m_iSelectedDescriptorClip);
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

        // Once a frame, whichever tab is showing: the time advances while playing
        void Render() noexcept override
        {
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
            xeditor::Run(m_Undo, std::format("SetProperty -Path {} -Value {} -Before {}", xeditor::Base64Encode(ClipPath(iSource, iClip, pField)), xeditor::Base64Encode(Value), xeditor::Base64Encode(Before)));
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
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pTip);
                };
                Header(0, "Name",       "Compiled clip name - hover a row's name for its original import stats and source file");
                Header(1, g_DeleteIcon, "Delete Clip - excluded from the compiled output entirely (still listed here, so it can be re-enabled)");
                Header(2, "Loop",       "Loop - play this clip as a seamless loop; the compiler blends out any start/end pose mismatch automatically");
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
                                    ImGui::SetTooltip("Imported as \"%s\"\nSource: %s\n%d fps, %d frames, %.2fs\n(double-click to rename)", Clip.m_OriginalName.c_str(), FileName.empty() ? "(no path set)" : FileName.c_str(), D.m_OriginalFPS, D.m_OriginalFrameCount, D.m_DurationSeconds);
                                }
                                else
                                    ImGui::SetTooltip("Imported as \"%s\"\nSource: %s (not in the last import)\n(double-click to rename)", Clip.m_OriginalName.c_str(), FileName.empty() ? "(no path set)" : FileName.c_str());
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
            auto* pHost   = xeditor::host::current();
            auto* pWindow = pHost ? pHost->find<xgpu::window>() : nullptr;
            if (!m_bReady || !pWindow) { ImGui::TextDisabled("The 3D view needs a GPU device (open from E29)."); return; }
            if (!m_bShowable) { ImGui::TextWrapped("%s", m_ErrorMessage.empty() ? "Nothing to show." : m_ErrorMessage.c_str()); return; }
            auto* pSkeleton = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkeleton) return;

            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const ImVec2 Min   = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(Min, ImVec2(Min.x + Avail.x, Min.y + Avail.y), IM_COL32(115, 115, 115, 255));        // the depth tint fades toward this
            ImGui::InvisibleButton("##AnimViewport", Avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            m_Scene.HandleInput();
            if (ImGui::IsItemHovered() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false) && SelectedClip()) m_bPlaying = !m_bPlaying;

            m_Scene.UpdateView(Avail.x, Avail.y);
            EvaluatePose(*pSkeleton);

            const std::vector<bool> NoTwist;
            const std::set<int>     NoSelection;
            xskeleton_editor::BuildWedgeGeometry(*pSkeleton, m_PoseBones, NoTwist, m_Scene.Style(), NoSelection, m_Scene.m_Radius
                , xskeleton_editor::render_color_mode::NORMAL_RENDER, -1, -1, m_Lines, true);
            m_Scene.SetLines(m_Lines);

            xgpu::tools::imgui::AddCustomRenderCallback([this](xgpu::cmd_buffer& CmdBuffer, const ImVec2&, const ImVec2&)
            {
                if (m_bOpen) m_Scene.Draw(CmdBuffer);
            });
        }

        void RenderPlayback() noexcept
        {
            auto* pClip = SelectedClip();
            if (!pClip) { ImGui::TextDisabled("Select a clip to preview."); return; }
            namespace ed = xgpu::tools::editors;
            const float Length = ClipLength(*pClip);

            if (ImGui::Button(m_bPlaying ? ed::g_PauseIcon : ed::g_PlayIcon)) m_bPlaying = !m_bPlaying;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m_bPlaying ? "Pause" : "Play");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToStartIcon)) { m_TimeSeconds = 0.0f; m_LoopsElapsed = 0; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to start");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToEndIcon)) m_TimeSeconds = Length;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to end");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderInt("##speed", &m_iSpeedIndex, 0, ed::g_NumPlaybackSpeeds - 1, ed::g_PlaybackSpeedLabels[m_iSpeedIndex]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");

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
        case kind::play:  if (!S.SelectedClip()) return "Play: no clip is selected"; S.m_bPlaying = true;  return "Play: playing";
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
        }
        return {};
    }

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::anim_package_type_guid_v
    , [](xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XANIM_PACKAGE_EDITOR_H
