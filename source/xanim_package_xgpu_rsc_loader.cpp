#include "xanim_package.h"
#include "xanim_package_xgpu_rsc_loader.h"

#include "dependencies/xresource_guid/source/bridges/xresource_xproperty_bridge.h"

//
// Register the loader and the properties
//
inline static auto s_AnimPackageRegistrations = xresource::common_registrations<xrsc::anim_package_type_guid_v>{};

//------------------------------------------------------------------
// An anim package is pure CPU reference data (per-bone keyframe curves) - like xskeleton, there's no
// device to create buffers on, so Load() is just a deserialize.

xresource::loader< xrsc::anim_package_type_guid_v >::data_type* xresource::loader< xrsc::anim_package_type_guid_v >::Load(xresource::mgr& Mgr, const full_guid& GUID)
{
    std::wstring              Path      = Mgr.getResourcePath(GUID, type_name_v);
    xanim_package::anim_package* pPackage = nullptr;

    // A missing/not-yet-compiled resource is an expected, recoverable case (same reasoning as
    // xtexture_xgpu_rsc_loader.cpp's identical fix) - every caller already handles getResource()
    // returning null. Nothing here dereferences pPackage before returning it, so this was never
    // actually unsafe in Release - just an unconditional Debug abort for an ordinary condition.
    xserializer::stream Stream;
    if (auto Err = Stream.Load(Path, pPackage); Err)
    {
        return nullptr;
    }

    return pPackage;
}

//------------------------------------------------------------------
// xserializer::stream::Load deserializes the whole object graph (top-level struct + every top-level
// Serialize(pView,Count) array - clips, bone hashes, keyframes, root motion) into ONE single
// contiguous allocation with pointer fixup, exactly like xskeleton's own loader. Freeing it is the
// same single call regardless of how many flat arrays the type has.

void xresource::loader< xrsc::anim_package_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID)
{
    xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, &Data);
}
