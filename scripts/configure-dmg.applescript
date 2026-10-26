on run argv
    if (count of argv) is not 1 then error "expected the mounted volume path"

    set mountPath to item 1 of argv
    set volumeAlias to POSIX file mountPath as alias
    set backgroundAlias to POSIX file (mountPath & "/.background/dmg-background.png") as alias

    tell application "Finder"
        open volumeAlias
        delay 1

        tell front window
            set current view to icon view
            set toolbar visible to false
            set statusbar visible to false
            set bounds to {100, 100, 860, 580}

            set viewOptions to icon view options
            set arrangement of viewOptions to not arranged
            set icon size of viewOptions to 108
            set text size of viewOptions to 13
            set background picture of viewOptions to backgroundAlias

            set position of item "OpenS4ka.app" to {190, 255}
            set position of item "Applications" to {570, 255}

            update without registering applications
            delay 2
            close
        end tell
    end tell
end run
