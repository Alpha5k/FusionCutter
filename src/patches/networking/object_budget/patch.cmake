fc_patch(
    ID ObjectBudget
    DEFINITION fusioncutter::patches::object_budget::definition
    ARCHITECTURES X86
    ROLES SERVER
    SOURCES
        object_budget.cpp
        patch.cpp
)
