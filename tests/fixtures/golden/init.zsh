command pixy palette set
zmodload zsh/datetime
autoload -Uz add-zsh-hook
typeset -gF __pixy_started_at=0
__pixy_preexec() {
  __pixy_started_at=$EPOCHREALTIME
}
__pixy_precmd() {
  local pixy_status=$?
  local pixy_duration=0
  if (( __pixy_started_at > 0 )); then
    pixy_duration=$(( (EPOCHREALTIME - __pixy_started_at) * 1000 ))
  fi
  PROMPT="$(command pixy render prompt.left --target zsh --palette --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="${#jobstates}" --set language="${PIXY_LANGUAGE:-}" --set vimode="${KEYMAP:-}")"
  RPROMPT="$(command pixy render prompt.right --target zsh --palette --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="${#jobstates}" --set language="${PIXY_LANGUAGE:-}" --set vimode="${KEYMAP:-}")"
}
add-zsh-hook preexec __pixy_preexec
add-zsh-hook precmd __pixy_precmd
