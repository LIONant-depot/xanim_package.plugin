#ifndef XANIM_PACKAGE_RUNTIME_H
#define XANIM_PACKAGE_RUNTIME_H
#pragma once

#include "dependencies/xmath/source/xmath_fvector.h"
#include "dependencies/xmath/source/xmath_ftransform.h"
#include "dependencies/xserializer/source/xserializer.h"
#include "dependencies/xAnimation/source/clip/xanimation_compress.h"     // K5: the compressed clips' track layout (header only)
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
        xmath::fvec3        m_LoopDisplacement  {};     // a loop: the root's travel over one pass (where the next pass starts, in this pass's start frame); else the last frame's root motion (zero if NONE)
        float               m_LoopTurn          {0};    // a loop: the root's turn over one pass (radians about +Y; the next pass starts turned by it); else 0 (C127)

        std::uint32_t       m_iFirstKeyFrame    {0};    // offset into anim_package::m_pKeyFrame; this clip's block is m_nFrames*anim_package::m_nBones entries, frame-major: [iFrame*m_nBones+iBone]
        std::uint32_t       m_iFirstRootMotion  {0};    // offset into anim_package::m_pRootMotion; only meaningful if m_RootMotionMode != NONE
        std::uint32_t       m_iFirstEvent       {0};    // offset into anim_package::m_pEvents (sorted by time)
        std::uint32_t       m_nEvents           {0};
        // what the clip does (xanimation::MeasureClip, soccer answer 4): selection and motion matching read these, never the keys
        float               m_Speed             {0};    // horizontal travel / duration (units per second)
        float               m_Direction         {0};    // degrees from the facing at the start to the travel (0 forward, 90 left, 180 back)
        float               m_TurnRate          {0};    // degrees per second the facing turns (positive: to its left)
        float               m_LoopError         {0};    // degrees between the first and last frames' local rotations (small: it loops)
        float               m_Height            {0};    // the bones' centroid's mean height
        // a MOTION SPACE (R165 phase 2): this loop blended with loop m_SpaceWith at m_SpaceSamples weights, footstep-aligned, as more matching
        // entries (the game: xanimation::BlendLoops); -1: none
        std::int32_t        m_SpaceWith         {-1};
        std::int32_t        m_SpaceSamples      {0};
    };

    // What a marker on a clip's timeline is (T5): the runtime and the tools read the kind; the name is free text (hashed)
    enum class event_kind : std::uint8_t { MARKER, FOOTSTEP, CONTACT, SYNC };

    // A clip's event, compiled: a point (Duration 0) or an interval (a contact bar). Fires once when the playing time crosses Time
    // (xanimation::Advance: the crossing rules). Bone: the skeleton bone it is about (a foot, a hand), -1 none; a mirrored clip reports the
    // mirror bone.
    struct event
    {
        std::uint32_t       m_NameHash  {};     // CRC32 of the event's name
        event_kind          m_Kind      {};
        std::int16_t        m_iBone     {-1};
        float               m_Time      {};     // seconds into the compiled clip
        float               m_Duration  {};     // 0: a point
    };

    // Smart Primitive v2 step 5: an accepted entry of an interaction, compiled (one record per entry). The start placement in the pivot's
    // frame (metres, degrees about up; heading 0 = +Z); Take: the accepted approach clip that walks from there into Clip's first frame
    struct interaction_entry
    {
        std::uint32_t       m_NameHash  {};     // CRC32 of the interaction's name (Sit)
        std::int16_t        m_iClip     {-1};   // the interaction's clip
        std::int16_t        m_iTake     {-1};   // the entry's approach clip
        float               m_X {}, m_Z {}, m_Yaw {};
        std::uint8_t        m_bRevalidate {};   // an anchor moved after it was accepted (still playable)
    };

    struct anim_package
    {
        inline static constexpr auto xserializer_version_v = 9;     // 9: interaction entries (Smart Primitive step 5); 8: motion spaces (a clip's SpaceWith/SpaceSamples); 2: events; 3: clip traits; 4: root turn; 5: compressed clips; 6: the raw keys in a sidecar; 7: seamless loops (a loop's frames exclude the next pass's first; m_LoopTurn)

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
        // The clip's root turn per frame (radians about +Y from its first frame, unwrapped), parallel to getClipRootMotion; empty: no turn
        inline std::span<float>                 getClipRootTurn    (int iClip)     const noexcept
        {
            auto& C = m_pClips[iClip];
            if (C.m_RootMotionMode == root_motion_mode::NONE || m_nRootTurnFrames != m_nTotalRootMotionFrames || m_pRootTurn == nullptr) return {};
            return { m_pRootTurn + C.m_iFirstRootMotion, static_cast<std::size_t>(C.m_nFrames) };
        }
        // The clip compressed (K5): its tracks (m_nBones from iClip * m_nBones) over the package's compressed arrays; empty: none stored
        inline xanimation::compressed_view      getClipCompressed  (int iClip)     const noexcept
        {
            if (m_pCTracks == nullptr || m_nCTracks < std::uint32_t(iClip + 1) * m_nBones) return {};
            const auto& C = m_pClips[iClip];
            return { std::span<const xanimation::compressed_clip::bone>(m_pCTracks + std::size_t(iClip) * m_nBones, m_nBones), m_pRot64, m_pQ16, m_pF32, int(m_nBones), C.m_nFrames, C.m_FPS, C.m_bLoop };
        }
        inline std::span<event>                 getClipEvents      (int iClip)     const noexcept { auto& C = m_pClips[iClip]; return { m_pEvents + C.m_iFirstEvent, C.m_nEvents }; }
        inline std::span<interaction_entry>     getInteractions    (void)          const noexcept { return { m_pInteractions, m_nInteractions }; }

        clip*               m_pClips            {nullptr};   // m_nClips entries
        std::uint32_t*      m_pBoneNameHashes   {nullptr};   // m_nBones entries, package-wide, SAME order as
                                                               // the bound xskeleton::skeleton - kept only as
                                                               // an optional bind-time integrity check, never
                                                               // read per-frame
        xmath::transform3*  m_pKeyFrame         {nullptr};   // m_nTotalKeyFrames entries, concatenated per-clip
        xmath::fvec3*       m_pRootMotion       {nullptr};   // m_nTotalRootMotionFrames entries, concatenated across clips with root motion
        event*              m_pEvents           {nullptr};   // m_nTotalEvents entries, concatenated per clip
        xanimation::compressed_clip::bone* m_pCTracks {nullptr};   // K5: m_nCTracks = 0 or m_nClips * m_nBones: each clip's compressed tracks (indices into the three arrays below)
        std::uint64_t*      m_pRot64            {nullptr};   // m_nRot64: animated rotations, 64 bits a frame
        std::uint16_t*      m_pQ16              {nullptr};   // m_nQ16: animated positions/scales, 3 x 16 bits a frame
        float*              m_pF32              {nullptr};   // m_nF32: long-range positions/scales, 3 floats a frame
        float*              m_pRootTurn         {nullptr};
        interaction_entry*  m_pInteractions     {nullptr};   // m_nInteractions entries (Smart Primitive step 5)   // m_nRootTurnFrames entries: 0 or m_nTotalRootMotionFrames, parallel to m_pRootMotion (the root's turn about +Y)
        std::uint16_t       m_nClips                  {0};
        std::uint32_t       m_nBones                  {0};   // shared bone width/order for every clip
        std::uint32_t       m_nTotalKeyFrames         {0};
        std::uint32_t       m_nTotalRootMotionFrames  {0};
        std::uint32_t       m_nTotalEvents            {0};
        std::uint32_t       m_nRootTurnFrames         {0};
        std::uint32_t       m_nCTracks = 0, m_nRot64 = 0, m_nQ16 = 0, m_nF32 = 0;
        std::uint32_t       m_nInteractions = 0;
        std::uint32_t       m_nSidecarKeys = 0;     // K5 step 2: the raw keys live in <package file>.keys (anim_keys), m_nSidecarKeys of them; m_pKeyFrame is
                                                    // null until LoadKeys attaches them (the editors, package-to-package imports); the runtime samples the compressed tracks
        void*               m_pKeysBlock   {nullptr};   // (the attached sidecar's block, freed with the package; never saved: always null in the file)
    };

    // The sidecar: the raw keys of a package, exactly as compiled from the source assets
    struct anim_keys
    {
        inline static constexpr auto xserializer_version_v = 1;
        anim_keys(void) noexcept = default;
        anim_keys(xserializer::stream&) noexcept {}
        xmath::transform3*  m_pKeyFrame = nullptr;
        std::uint32_t       m_nKeys     = 0;
    };

    // Attaches the sidecar's raw keys to a loaded package (Path: the package's file). False: none needed (no sidecar keys) or it could not be read
    inline bool LoadKeys(anim_package& Pkg, std::wstring_view PackagePath) noexcept
    {
        if (Pkg.m_nSidecarKeys == 0 || Pkg.m_pKeyFrame) return false;
        anim_keys* pKeys = nullptr; xserializer::stream Stream;
        if (Stream.Load(std::wstring(PackagePath) + L".keys", pKeys) || !pKeys) return false;
        if (pKeys->m_nKeys != Pkg.m_nSidecarKeys) { xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, pKeys); return false; }
        Pkg.m_pKeyFrame = pKeys->m_pKeyFrame; Pkg.m_nTotalKeyFrames = pKeys->m_nKeys; Pkg.m_pKeysBlock = pKeys;
        return true;
    }
    // Frees a package loaded in place, its sidecar too
    inline void FreeLoaded(anim_package* pPkg) noexcept
    {
        if (pPkg == nullptr) return;
        if (pPkg->m_pKeysBlock) xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, pPkg->m_pKeysBlock);
        xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, pPkg);
    }

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
        if (m_pEvents)         delete[] m_pEvents;
        if (m_pRootTurn)       delete[] m_pRootTurn;
        if (m_pInteractions)   delete[] m_pInteractions;
        if (m_pCTracks)        delete[] m_pCTracks;
        if (m_pRot64)          delete[] m_pRot64;
        if (m_pQ16)            delete[] m_pQ16;
        if (m_pF32)            delete[] m_pF32;

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
    xerr SerializeIO<xanim_package::interaction_entry>(xserializer::stream& Stream, const xanim_package::interaction_entry& E) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(E.m_NameHash)) || (Err = Stream.Serialize(E.m_iClip)) || (Err = Stream.Serialize(E.m_iTake))
            || (Err = Stream.Serialize(E.m_X)) || (Err = Stream.Serialize(E.m_Z)) || (Err = Stream.Serialize(E.m_Yaw)) || (Err = Stream.Serialize(E.m_bRevalidate))
            ;
        return Err;
    }

    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xanim_package::event>(xserializer::stream& Stream, const xanim_package::event& E) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(E.m_NameHash))
            || (Err = Stream.Serialize(E.m_Kind))
            || (Err = Stream.Serialize(E.m_iBone))
            || (Err = Stream.Serialize(E.m_Time))
            || (Err = Stream.Serialize(E.m_Duration))
            ;
        return Err;
    }
    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xanim_package::anim_keys>(xserializer::stream& Stream, const xanim_package::anim_keys& K) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(K.m_nKeys))
            || (Err = Stream.Serialize(K.m_pKeyFrame, K.m_nKeys))
            ;
        return Err;
    }
    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xanimation::compressed_clip::bone>(xserializer::stream& Stream, const xanimation::compressed_clip::bone& B) noexcept
    {
        xerr Err;
        false
            || (Err = Stream.Serialize(B.m_Rot)) || (Err = Stream.Serialize(B.m_Pos)) || (Err = Stream.Serialize(B.m_Scale))
            || (Err = Stream.Serialize(B.m_RotConst.m_X)) || (Err = Stream.Serialize(B.m_RotConst.m_Y)) || (Err = Stream.Serialize(B.m_RotConst.m_Z)) || (Err = Stream.Serialize(B.m_RotConst.m_W))
            || (Err = Stream.Serialize(B.m_PosConst.m_X)) || (Err = Stream.Serialize(B.m_PosConst.m_Y)) || (Err = Stream.Serialize(B.m_PosConst.m_Z))
            || (Err = Stream.Serialize(B.m_ScaleConst.m_X)) || (Err = Stream.Serialize(B.m_ScaleConst.m_Y)) || (Err = Stream.Serialize(B.m_ScaleConst.m_Z))
            || (Err = Stream.Serialize(B.m_PosMin.m_X)) || (Err = Stream.Serialize(B.m_PosMin.m_Y)) || (Err = Stream.Serialize(B.m_PosMin.m_Z))
            || (Err = Stream.Serialize(B.m_PosExt.m_X)) || (Err = Stream.Serialize(B.m_PosExt.m_Y)) || (Err = Stream.Serialize(B.m_PosExt.m_Z))
            || (Err = Stream.Serialize(B.m_ScaleMin.m_X)) || (Err = Stream.Serialize(B.m_ScaleMin.m_Y)) || (Err = Stream.Serialize(B.m_ScaleMin.m_Z))
            || (Err = Stream.Serialize(B.m_ScaleExt.m_X)) || (Err = Stream.Serialize(B.m_ScaleExt.m_Y)) || (Err = Stream.Serialize(B.m_ScaleExt.m_Z))
            || (Err = Stream.Serialize(B.m_iRot)) || (Err = Stream.Serialize(B.m_iPos)) || (Err = Stream.Serialize(B.m_iScale))
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
            || (Err = Stream.Serialize(Clip.m_LoopTurn))
            || (Err = Stream.Serialize(Clip.m_iFirstKeyFrame))
            || (Err = Stream.Serialize(Clip.m_iFirstRootMotion))
            || (Err = Stream.Serialize(Clip.m_iFirstEvent))
            || (Err = Stream.Serialize(Clip.m_nEvents))
            || (Err = Stream.Serialize(Clip.m_Speed)) || (Err = Stream.Serialize(Clip.m_Direction)) || (Err = Stream.Serialize(Clip.m_TurnRate))
            || (Err = Stream.Serialize(Clip.m_LoopError)) || (Err = Stream.Serialize(Clip.m_Height))
            || (Err = Stream.Serialize(Clip.m_SpaceWith)) || (Err = Stream.Serialize(Clip.m_SpaceSamples))
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
            || (Err = Stream.Serialize(Package.m_nTotalEvents))
            || (Err = Stream.Serialize(Package.m_pEvents,          Package.m_nTotalEvents))
            || (Err = Stream.Serialize(Package.m_nRootTurnFrames))
            || (Err = Stream.Serialize(Package.m_pRootTurn,        Package.m_nRootTurnFrames))
            || (Err = Stream.Serialize(Package.m_nCTracks))
            || (Err = Stream.Serialize(Package.m_pCTracks,         Package.m_nCTracks))
            || (Err = Stream.Serialize(Package.m_nRot64))
            || (Err = Stream.Serialize(Package.m_pRot64,           Package.m_nRot64))
            || (Err = Stream.Serialize(Package.m_nQ16))
            || (Err = Stream.Serialize(Package.m_pQ16,             Package.m_nQ16))
            || (Err = Stream.Serialize(Package.m_nF32))
            || (Err = Stream.Serialize(Package.m_pF32,             Package.m_nF32))
            || (Err = Stream.Serialize(Package.m_nInteractions))
            || (Err = Stream.Serialize(Package.m_pInteractions,    Package.m_nInteractions))
            || (Err = Stream.Serialize(Package.m_nSidecarKeys))
            ;
        return Err;
    }
}

#endif
