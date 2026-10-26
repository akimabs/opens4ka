on run argv
    if (count of argv) is not 1 then error "expected the mounted volume path"

    set mountPath to item 1 of argv
    set volumeAlias to POSIX file mountPath as alias
    set backgroundAlias to POSIX file (mountPath & "/.background/dmg-background.png") as alias

    tell application "Finder"
        open volumeAlias
        delay 1

        set current view of front window to icon view
        set toolbar visible of front window to false
        set statusbar visible of front window to false
        set bounds of front window to {100, 100, 860, 580}

        set arrangement of icon view options of front window to not arranged
        set icon size of icon view options of front window to 108
        set text size of icon view options of front window to 13
        set background picture of icon view options of front window to backgroundAlias

        set position of item "OpenS4ka.app" of front window to {190, 255}
        set position of item "Applications" of front window to {570, 255}

        delay 2
        close front window
    end tell
end run
