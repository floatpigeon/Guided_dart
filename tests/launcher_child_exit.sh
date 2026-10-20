#!/bin/sh
if [ "$#" -eq 2 ]; then
    for descriptor in /proc/$$/fd/* /proc/$PPID/fd/*; do
        if [ "$(readlink "$descriptor")" = "$1" ]; then
            printf '%s\n' "$descriptor" > "$2"
        fi
    done
fi
printf '%s\n' "$$" > "$DART_EXIT_MARKER"
