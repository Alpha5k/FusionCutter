#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define FC_EXTERN_C extern "C"
#define FC_NOEXCEPT noexcept
#else
#define FC_EXTERN_C
#define FC_NOEXCEPT
#endif

#if defined(_MSC_VER)
#define FC_CALL __cdecl
#else
#define FC_CALL
#endif

// Fixed-width scalar aliases keep the shared C ABI independent of compiler enum representation.
typedef uint32_t FC_Bool;
#define FC_FALSE UINT32_C(0)
#define FC_TRUE UINT32_C(1)

// Host roles are bit flags because one support declaration may cover both loader roles.
typedef uint32_t FC_HostRole;
#define FC_HOST_ROLE_CLIENT UINT32_C(1)
#define FC_HOST_ROLE_SERVER UINT32_C(2)
#define FC_HOST_ROLE_ALL (FC_HOST_ROLE_CLIENT | FC_HOST_ROLE_SERVER)

// Architecture identifies the native address width and calling rules used by one framework or plugin build.
typedef uint32_t FC_Architecture;
#define FC_ARCH_X86 UINT32_C(1)
#define FC_ARCH_X64 UINT32_C(2)

// Target layouts are bit flags; exact image identity is reported separately through FC_TargetImage.
typedef uint32_t FC_TargetLayout;
#define FC_LAYOUT_GAMESPY_RETAIL UINT32_C(1)
#define FC_LAYOUT_STEAM_RETAIL UINT32_C(2)
#define FC_LAYOUT_GOG_RETAIL UINT32_C(4)
#define FC_LAYOUT_MOD_TOOLS UINT32_C(8)
#define FC_LAYOUT_CLASSIC_COLLECTION UINT32_C(16)
#define FC_LAYOUT_ALL                                                                                                  \
    (FC_LAYOUT_GAMESPY_RETAIL | FC_LAYOUT_STEAM_RETAIL | FC_LAYOUT_GOG_RETAIL | FC_LAYOUT_MOD_TOOLS |                  \
     FC_LAYOUT_CLASSIC_COLLECTION)

// Physical images distinguish the executable or DLL whose RVAs a support declaration addresses.
typedef uint32_t FC_TargetImage;
#define FC_IMAGE_GAME UINT32_C(1)
#define FC_IMAGE_BOOTSTRAP UINT32_C(2)
#define FC_IMAGE_GALAXY_PEER UINT32_C(3)

// Failure policy decides whether one applicable patch failure remains local or stops startup globally.
typedef uint32_t FC_FailurePolicy;
#define FC_FAILURE_INHERIT UINT32_C(0)
#define FC_FAILURE_CONTINUE UINT32_C(1)
#define FC_FAILURE_FATAL UINT32_C(2)

#pragma pack(push, 8)

// Borrowed byte-counted views. Receivers retain neither pointer unless their call contract says it is copied.
typedef struct FC_StringView {
    const char* data;
    uint32_t size;
} FC_StringView;

typedef struct FC_ByteView {
    const uint8_t* data;
    uint32_t size;
} FC_ByteView;

#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(FC_Bool) == 4);
static_assert(sizeof(FC_HostRole) == 4);
static_assert(sizeof(FC_Architecture) == 4);
static_assert(sizeof(FC_TargetLayout) == 4);
static_assert(sizeof(FC_TargetImage) == 4);
static_assert(sizeof(FC_FailurePolicy) == 4);
static_assert(sizeof(FC_StringView) == sizeof(void*) * 2);
static_assert(sizeof(FC_ByteView) == sizeof(void*) * 2);
static_assert(alignof(FC_StringView) == alignof(void*));
static_assert(alignof(FC_ByteView) == alignof(void*));
static_assert(offsetof(FC_StringView, size) == sizeof(void*));
static_assert(offsetof(FC_ByteView, size) == sizeof(void*));
#else
_Static_assert(sizeof(FC_Bool) == 4, "FC_Bool must remain 32-bit");
_Static_assert(sizeof(FC_HostRole) == 4, "FC_HostRole must remain 32-bit");
_Static_assert(sizeof(FC_Architecture) == 4, "FC_Architecture must remain 32-bit");
_Static_assert(sizeof(FC_TargetLayout) == 4, "FC_TargetLayout must remain 32-bit");
_Static_assert(sizeof(FC_TargetImage) == 4, "FC_TargetImage must remain 32-bit");
_Static_assert(sizeof(FC_FailurePolicy) == 4, "FC_FailurePolicy must remain 32-bit");
_Static_assert(sizeof(FC_StringView) == sizeof(void*) * 2, "FC_StringView layout changed");
_Static_assert(sizeof(FC_ByteView) == sizeof(void*) * 2, "FC_ByteView layout changed");
_Static_assert(_Alignof(FC_StringView) == _Alignof(void*), "FC_StringView alignment changed");
_Static_assert(_Alignof(FC_ByteView) == _Alignof(void*), "FC_ByteView alignment changed");
_Static_assert(offsetof(FC_StringView, size) == sizeof(void*), "FC_StringView field offset changed");
_Static_assert(offsetof(FC_ByteView, size) == sizeof(void*), "FC_ByteView field offset changed");
#endif
