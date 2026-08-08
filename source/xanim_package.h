#ifndef XANIM_PACKAGE_RUNTIME_H
#define XANIM_PACKAGE_RUNTIME_H
#pragma once

#include "dependencies/xmath/source/xmath_fvector.h"
#include "dependencies/xmath/source/xmath_ftransform.h"
#include "dependencies/xserializer/source/xserializer.h"
#include <span>

namespace xanim_package
{
    enum class root_motion_mode : std::uint8_t { NONE, XZ_ONLY, XYZ };

    // Plain per-clip header - deliberately owns NO pointers of its own (xserializer has no precedent
    // anywhere in this codebase for an array whose own elements each own further nested arrays; every
    // existing resource, including xskeleton, sticks to flat, single-level parallel arrays). All of a
    // clip's actual curve data lives in anim_package's shared, concatenated arrays below instead -
    // this header just says where its own slice starts.
    //
    // Every clip in a package is bound to the SAME skeleton, so bone count/order is a PACKAGE-level
    // invariant (anim_package::m_nBones, same order as the bound xskeleton::skeleton::getBones()) -
    // not a per-clip one. The compiler fills every bone slot for every clip (falling back to the
    // skeleton's own rest pose for bones a given clip doesn't animate), so playback indexes bone i's
    // curve directly with the skeleton's own bone index - no per-frame name-hash lookup.
    struct clip
    {
        std::uint32_t       m_NameHash          {};    // CRC32 of the compiled (possibly renamed) clip name
        std::int32_t        m_FPS               {0};
        std::int32_t        m_nFrames           {0};
        bool                m_bLoop             {false};
        root_motion_mode    m_RootMotionMode    {root_motion_mode::NONE};
        xmath::fvec3        m_LoopDisplacement  {};     // = last frame's root-motion delta (zero if NONE)

        std::uint32_t       m_iFirstKeyFrame    {0};    // offset into anim_package::m_pKeyFrame; this clip's block is m_nFrames*anim_package::m_nBones entries, frame-major: [iFrame*m_nBones+iBone]
        std::uint32_t       m_iFirstRootMotion  {0};    // offset into anim_package::m_pRootMotion; only meaningful if m_RootMotionMode != NONE
    };

    struct anim_package
    {
        inline static constexpr auto xserializer_version_v = 1;

                                        anim_package    (void)                          noexcept = default;
        inline                         anim_package    (xserializer::stream& Stream)    noexcept;
        inline void                    Kill            (void)                          noexcept;
        inline void                    Initialize      (void)                          noexcept;
        inline int                     findClipIndex   (std::uint32_t NameHash) const   noexcept;

        inline std::span<clip>                  getClips           (void)          const noexcept { return { m_pClips, m_nClips }; }
        inline std::span<std::uint32_t>         getBoneNameHashes  (void)          const noexcept { return { m_pBoneNameHashes, m_nBones }; }
        inline std::span<xmath::transform3>     getClipFrame       (int iClip, int iFrame) const noexcept
        {
            auto& C = m_pClips[iClip];
            return { m_pKeyFrame + C.m_iFirstKeyFrame + static_cast<std::size_t>(iFrame) * m_nBones, m_nBones };
        }
        inline std::span<xmath::fvec3>          getClipRootMotion  (int iClip)     const noexcept
        {
            auto& C = m_pClips[iClip];
            if (C.m_RootMotionMode == root_motion_mode::NONE) return {};
            return { m_pRootMotion + C.m_iFirstRootMotion, static_cast<std::size_t>(C.m_nFrames) };
        }

        clip*               m_pClips            {nullptr};   // m_nClips entries
        std::uint32_t*      m_pBoneNameHashes   {nullptr};   // m_nBones entries, package-wide, SAME order as
                                                               // the bound xskeleton::skeleton - kept only as
                                                               // an optional bind-time integrity check, never
                                                               // read per-frame
        xmath::transform3*  m_pKeyFrame         {nullptr};   // m_nTotalKeyFrames entries, concatenated per-clip
        xmath::fvec3*       m_pRootMotion       {nullptr};   // m_nTotalRootMotionFrames entries, concatenated across clips with root motion
        std::uint16_t       m_nClips                  {0};
        std::uint32_t       m_nBones                  {0};   // shared bone width/order for every clip
        std::uint32_t       m_nTotalKeyFrames         {0};
        std::uint32_t       m_nTotalRootMotionFrames  {0};
    };

    //-------------------------------------------------------------------------

    anim_package::anim_package(xserializer::stream& Stream) noexcept
    {
        //xassert( Stream.getResourceVersion() == anim_package::xserializer_version_v );
    }

    //-------------------------------------------------------------------------

    void anim_package::Initialize(void) noexcept
    {
        std::memset(this, 0, sizeof(*this));
    }

    //-------------------------------------------------------------------------
    // Every array here is flat and independently owned (same shape as xskeleton::skeleton's own
    // arrays) - a plain delete[] per array is correct and sufficient, no per-element walk needed.
    void anim_package::Kill(void) noexcept
    {
        if (m_pClips)          delete[] m_pClips;
        if (m_pBoneNameHashes) delete[] m_pBoneNameHashes;
        if (m_pKeyFrame)       delete[] m_pKeyFrame;
        if (m_pRootMotion)     delete[] m_pRootMotion;

        Initialize();
    }

    //-------------------------------------------------------------------------

    int anim_package::findClipIndex(std::uint32_t NameHash) const noexcept
    {
        for (auto i = 0u; i < m_nClips; ++i)
            if (m_pClips[i].m_NameHash == NameHash) return static_cast<int>(i);
        return -1;
    }
}

//-------------------------------------------------------------------------
// serializer
//-------------------------------------------------------------------------
namespace xserializer::io_functions
{
    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xmath::fvec3>(xserializer::stream& Stream, const xmath::fvec3& V) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(V.m_X))
            || (Err = Stream.Serialize(V.m_Y))
            || (Err = Stream.Serialize(V.m_Z))
            ;
        return Err;
    }

    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xmath::transform3>(xserializer::stream& Stream, const xmath::transform3& T) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(T.m_Scale.m_X))
            || (Err = Stream.Serialize(T.m_Scale.m_Y))
            || (Err = Stream.Serialize(T.m_Scale.m_Z))

            || (Err = Stream.Serialize(T.m_Rotation.m_X))
            || (Err = Stream.Serialize(T.m_Rotation.m_Y))
            || (Err = Stream.Serialize(T.m_Rotation.m_Z))
            || (Err = Stream.Serialize(T.m_Rotation.m_W))

            || (Err = Stream.Serialize(T.m_Position.m_X))
            || (Err = Stream.Serialize(T.m_Position.m_Y))
            || (Err = Stream.Serialize(T.m_Position.m_Z))
            ;
        return Err;
    }

    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xanim_package::clip>(xserializer::stream& Stream, const xanim_package::clip& Clip) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(Clip.m_NameHash))
            || (Err = Stream.Serialize(Clip.m_FPS))
            || (Err = Stream.Serialize(Clip.m_nFrames))
            || (Err = Stream.Serialize(Clip.m_bLoop))
            || (Err = Stream.Serialize(Clip.m_RootMotionMode))
            || (Err = Stream.Serialize(Clip.m_LoopDisplacement.m_X))
            || (Err = Stream.Serialize(Clip.m_LoopDisplacement.m_Y))
            || (Err = Stream.Serialize(Clip.m_LoopDisplacement.m_Z))
            || (Err = Stream.Serialize(Clip.m_iFirstKeyFrame))
            || (Err = Stream.Serialize(Clip.m_iFirstRootMotion))
            ;
        return Err;
    }

    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xanim_package::anim_package>(xserializer::stream& Stream, const xanim_package::anim_package& Package) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(Package.m_nClips))
            || (Err = Stream.Serialize(Package.m_pClips,           Package.m_nClips))
            || (Err = Stream.Serialize(Package.m_nBones))
            || (Err = Stream.Serialize(Package.m_pBoneNameHashes,  Package.m_nBones))
            || (Err = Stream.Serialize(Package.m_nTotalKeyFrames))
            || (Err = Stream.Serialize(Package.m_pKeyFrame,        Package.m_nTotalKeyFrames))
            || (Err = Stream.Serialize(Package.m_nTotalRootMotionFrames))
            || (Err = Stream.Serialize(Package.m_pRootMotion,      Package.m_nTotalRootMotionFrames))
            ;
        return Err;
    }
}

#endif
