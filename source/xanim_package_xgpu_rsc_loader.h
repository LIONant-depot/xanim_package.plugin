#ifndef XANIM_PACKAGE_XGPU_RSC_LOADER_H
#define XANIM_PACKAGE_XGPU_RSC_LOADER_H
#pragma once

#include "dependencies/xresource_mgr/source/xresource_mgr.h"
#include "xanim_package.h"

// All the information about the resource
namespace xrsc
{
    inline static constexpr auto    anim_package_type_guid_v    = xresource::type_guid(xresource::guid_generator::Instance64FromString("xAnimPackage"));
    using                           anim_package                = xresource::def_guid<anim_package_type_guid_v>;
}

// Now we specify the loader and we must fill in all the information.
// Like xskeleton, an anim package owns no GPU buffers - it's reference data (per-bone keyframe
// curves, stored as flat concatenated arrays with per-clip offsets - see xanim_package.h) consumed
// by the CPU-side animation system. So there's no xgpu-wrapper subclass here and no device buffer
// creation in Load().
template<>
struct xresource::loader< xrsc::anim_package_type_guid_v >
{
    //--- Expected static parameters ---
    constexpr static inline auto            type_name_v         = L"AnimPackage";    // This name is used to construct the path to the resource (if not provided)
    constexpr static inline auto            use_death_march_v   = false;             // xGPU already has a death march implemented inside itself...
    using                                   data_type           = xanim_package::anim_package;

    static data_type*                       Load        (xresource::mgr& Mgr, const full_guid& GUID);
    static void                             Destroy     (xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID);
};

#endif
