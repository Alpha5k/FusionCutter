fc_patch(
    ID WeaponSwapReplayFix
    DEFINITION fusioncutter::patches::weapon_swap_replay::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        input.cpp
        layout.cpp
        local_ledger.cpp
        packed_ledger.cpp
        patch.cpp
        presentation.cpp
        reconciliation.cpp
        weapon_swap.cpp
)
