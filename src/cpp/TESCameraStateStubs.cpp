// TESCameraStateStubs.cpp
//
// CommonLibSSE declares RE::TESCameraState's virtual methods in the header
// but does not provide compiled definitions in CommonLibSSE.lib.  When
// ERCameraState (CameraStateManager.cpp) subclasses TESCameraState the
// linker requires bodies for:
//   - the virtual destructor (called from ERCameraState's destructor chain)
//   - every other virtual method (vtable construction by MSVC)
//
// These stubs match the no-op / trivial semantics documented in the header
// comments.  They satisfy the linker without changing observable behaviour.
//
#include "PCH.h"

namespace RE
{
    TESCameraState::~TESCameraState() {}

    void TESCameraState::Begin() {}
    void TESCameraState::End() {}
    void TESCameraState::Update(BSTSmartPointer<TESCameraState>&) {}
    void TESCameraState::GetRotation(NiQuaternion&) {}
    void TESCameraState::GetTranslation(NiPoint3&) {}
    void TESCameraState::SaveGame(BGSSaveFormBuffer*) {}
    void TESCameraState::LoadGame(BGSLoadFormBuffer*) {}
    void TESCameraState::Revert(BGSLoadFormBuffer*) {}
}
