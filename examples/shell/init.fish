command pixy palette set
set -g __pixy_last_command ''
function __pixy_preexec --on-event fish_preexec
  set -g __pixy_last_command $argv
end
function fish_prompt
  set -g __pixy_status $status
  set -l pixy_language ''
  set -q PIXY_LANGUAGE; and set pixy_language "$PIXY_LANGUAGE"
  command pixy render prompt.left --target ansi --palette --set status=$__pixy_status --set duration_ms="$CMD_DURATION" --set jobs=(count (jobs -p)) --set language="$pixy_language" --set vimode="$fish_bind_mode" --set "last_command=$__pixy_last_command"
end
function fish_right_prompt
  set -l pixy_language ''
  set -q PIXY_LANGUAGE; and set pixy_language "$PIXY_LANGUAGE"
  command pixy render prompt.right --target ansi --palette --set status="$__pixy_status" --set duration_ms="$CMD_DURATION" --set jobs=(count (jobs -p)) --set language="$pixy_language" --set vimode="$fish_bind_mode"
end
