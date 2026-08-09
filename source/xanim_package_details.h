#ifndef XANIM_PACKAGE_DETAILS_H
#define XANIM_PACKAGE_DETAILS_H
#pragma once

namespace xanim_package_desc
{
    // Read-only, editor-facing view of every clip actually found across every source file listed in
    // the descriptor's m_ImportSources - before any override (ignore/rename/trim/FPS/root-motion) is
    // applied. Nested by source file (not flat): a raw clip's name is only guaranteed unique WITHIN
    // the file it came from - assimp/FBX exporters frequently hand back a generic, file-scoped name
    // (e.g. Blender's "Armature|Armature|...|Scene" pattern) that two entirely different source files
    // can both produce, so grouping by source is what makes name-matching actually safe.
    struct details
    {
        struct clip
        {
            std::string  m_Name                 = {};    // raw imported name - unique within this source file
            int          m_OriginalFPS           = 0;     // FPS actually sampled at import
            int          m_OriginalFrameCount    = 0;
            float        m_DurationSeconds       = 0.f;   // convenience = frames / FPS
            int          m_NumBonesAnimated      = 0;     // distinct bones this clip carries curves for, pre-override
            bool         m_bRootMotionCandidate  = false; // informational hint: does the raw root
                                                           // bone's translation actually change from
                                                           // frame 0 to the last frame beyond an epsilon

            XPROPERTY_DEF
            ( "clip", clip
            , obj_member<"Name",                &clip::m_Name >
            , obj_member<"OriginalFPS",          &clip::m_OriginalFPS >
            , obj_member<"OriginalFrameCount",   &clip::m_OriginalFrameCount >
            , obj_member<"DurationSeconds",      &clip::m_DurationSeconds >
            , obj_member<"NumBonesAnimated",     &clip::m_NumBonesAnimated >
            , obj_member<"RootMotionCandidate",  &clip::m_bRootMotionCandidate >
            )
        };

        struct source
        {
            std::wstring       m_Path     = {};   // matches the owning descriptor::import_source::m_Path
            std::vector<clip>  m_ClipList = {};   // every clip this specific file contains

            int findClip(std::string_view Name) const noexcept
            {
                for (auto& E : m_ClipList)
                    if (E.m_Name == Name) return static_cast<int>(&E - m_ClipList.data());
                return -1;
            }

            XPROPERTY_DEF
            ( "source", source
            , obj_member<"Path",     &source::m_Path >
            , obj_member<"ClipList", &source::m_ClipList >
            )
        };

        int findSource(std::wstring_view Path) const noexcept
        {
            for (auto& E : m_Sources)
                if (E.m_Path == Path) return static_cast<int>(&E - m_Sources.data());
            return -1;
        }

        std::vector<source>  m_Sources        = {};
        int                  m_NumClips       = 0;   // total across every source, for a quick UI count
        int                  m_NumSourceFiles = 0;   // = m_Sources.size()

        XPROPERTY_DEF
        ( "details", details
        , obj_member<"Sources",         &details::m_Sources,        member_flags<flags::SHOW_READONLY> >
        , obj_member<"NumClips",        &details::m_NumClips,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"NumSourceFiles",  &details::m_NumSourceFiles, member_flags<flags::SHOW_READONLY> >
        )
    };
    XPROPERTY_REG(details)
    XPROPERTY_REG2(anim_clip_info_, details::clip)
    XPROPERTY_REG2(anim_source_info_, details::source)
}

#endif
