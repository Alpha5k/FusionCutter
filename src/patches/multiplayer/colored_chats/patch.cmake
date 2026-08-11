fc_patch(
    ID ColoredChats
    DEFINITION fusioncutter::patches::colored_chats::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        colored_chats.cpp
)
