fc_patch(
    ID ScreenshotFix
    DEFINITION fusioncutter::patches::screenshot_fix::definition
    ARCHITECTURES X86
    ROLES CLIENT
    SOURCES
        patch.cpp
        screenshot_fix.cpp
        screenshot_writer.cpp
)
