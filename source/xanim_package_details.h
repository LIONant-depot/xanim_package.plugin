#ifndef XANIM_PACKAGE_DETAILS_H
#define XANIM_PACKAGE_DETAILS_H
#pragma once

namespace xanim_package_desc
{
    // Read-only, editor-facing view of every clip actually found across every source file listed in
    // the descriptor's m_ImportSources - before any override (ignore/rename/trim/FPS/root-motion) is
    // applied. Flat, not a tree - clips have no parent/child relationship, unlike xskeleton's bones.
    struct details
    {
        struct clip
        {
            std::string  m_Name                 = {};    // raw imported name
            std::wstring m_SourceFile            = {};    // which import_source entry this came from
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
            , obj_member<"SourceFile",           &clip::m_SourceFile >
            , obj_member<"OriginalFPS",          &clip::m_OriginalFPS >
            , obj_member<"OriginalFrameCount",   &clip::m_OriginalFrameCount >
            , obj_member<"DurationSeconds",      &clip::m_DurationSeconds >
            , obj_member<"NumBonesAnimated",     &clip::m_NumBonesAnimated >
            , obj_member<"RootMotionCandidate",  &clip::m_bRootMotionCandidate >
            )
        };

        int findClip(std::string_view Name) const noexcept
        {
            for (auto& E : m_ClipList)
                if (E.m_Name == Name) return static_cast<int>(&E - m_ClipList.data());
            return -1;
        }

        std::vector<clip>  m_ClipList       = {};
        int                m_NumClips       = 0;
        int                m_NumSourceFiles = 0;

        XPROPERTY_DEF
        ( "details", details
        , obj_member<"ClipList",        &details::m_ClipList,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"NumClips",        &details::m_NumClips,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"NumSourceFiles",  &details::m_NumSourceFiles, member_flags<flags::SHOW_READONLY> >
        )
    };
    XPROPERTY_REG(details)
    XPROPERTY_REG2(anim_clip_info_, details::clip)
}

#endif
