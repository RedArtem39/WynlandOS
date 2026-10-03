# WynlandOS defaults for fish (shipped by tools/stage_base.sh)
set -gx PATH /usr/local/bin /usr/bin /usr/sbin /sbin
set -q TERM; or set -gx TERM xterm-256color
set -q LANG; or set -gx LANG C.UTF-8
set -q PAGER; or set -gx PAGER less
set -q EDITOR; or set -gx EDITOR nano
set -g fish_greeting "WynlandOS -- fish "(string split -f1 ' ' $version)
