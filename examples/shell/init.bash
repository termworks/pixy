__pixy_prompt_command() {
  local pixy_status=${__pixy_last_status:-0}
  local pixy_duration=0
  local pixy_jobs=$(( $(jobs -p | wc -l) ))
  __pixy_prompt_active=1
  if [[ -n ${__pixy_started_seconds:-} ]]; then
    pixy_duration=$(( (SECONDS - __pixy_started_seconds) * 1000 ))
  fi
  PS1="$(command pixy render prompt.left --target bash --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="$pixy_jobs" --set language="${PIXY_LANGUAGE:-}" --set vimode="${PIXY_VIMODE:-}")"
}
__pixy_prompt_finish() {
  __pixy_prompt_active=0
  __pixy_command_running=0
}
__pixy_preexec() {
  local pixy_status=$?
  if [[ $BASH_COMMAND == __pixy_prompt_command* ]]; then
    __pixy_last_status=$pixy_status
    __pixy_prompt_active=1
    return
  fi
  if [[ ${__pixy_prompt_active:-0} -eq 0 && ${__pixy_command_running:-0} -eq 0 ]]; then
    __pixy_started_seconds=$SECONDS
    __pixy_command_running=1
  fi
}
trap '__pixy_preexec' DEBUG
PROMPT_COMMAND="__pixy_prompt_command${PROMPT_COMMAND:+;$PROMPT_COMMAND};__pixy_prompt_finish"
