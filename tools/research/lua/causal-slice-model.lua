-- Call-path, return, and access matching for the SH-4 causal-slice watcher.
--
-- `model.new(context)` returns the per-event handlers. The model owns the
-- bookkeeping over `context.state` (call stack, pending call, open producer
-- frames) and delegates every record emission to the entry script through
-- `context.anomaly(kind, event, detail)` and
-- `context.emit_slice(frame, outcome, terminal_event)`.

local model = {}

function model.new(context)
  local state = context.state
  local max_call_depth = context.max_call_depth
  local anomaly = context.anomaly
  local emit_slice = context.emit_slice
  local handlers = {}

  local function reset_context(origin)
    state.call_stack = {}
    state.pending_call = nil
    state.context_epoch = state.context_epoch + 1
    state.context_origin = origin
    state.context_resets = state.context_resets + 1
  end
  handlers.reset_context = reset_context

  local function same_owner(event, pending)
    return event.backend == pending.backend
      and event.pc == pending.pc
      and event.opcode == pending.opcode
      and event.delay_slot_depth == pending.delay_slot_depth
      and event.tick_decimal == pending.tick_decimal
  end

  local function commit_pending(event, reason)
    local pending = state.pending_call
    if not pending then return end
    if #state.call_stack >= max_call_depth then
      anomaly("call-depth-limit", event, "observed call depth exceeds configured maximum")
      reset_context("after-call-depth-limit")
      return
    end
    state.calls = state.calls + 1
    pending.invocation_id = state.calls
    pending.completion_reason = reason
    pending.completion_ordinal = event and event.ordinal_decimal or nil
    table.insert(state.call_stack, pending)
    state.pending_call = nil
  end

  local function advance_pending(event)
    local pending = state.pending_call
    if not pending then return end
    if same_owner(event, pending) then
      if event.event == "instruction" then
        commit_pending(event, "owner-instruction-completed")
      end
      return
    end
    if event.delay_slot_depth > pending.delay_slot_depth then return end
    commit_pending(event, "later-instruction-boundary-observed")
  end

  local function copy_call_path()
    local copy = {}
    for index, frame in ipairs(state.call_stack) do copy[index] = frame end
    return copy
  end

  local function frame_for(event)
    local frame = state.active_frames[event.delay_slot_depth]
    if frame and frame.pc == event.pc and frame.opcode == event.opcode
        and frame.backend == event.backend then
      return frame
    end
    return nil
  end

  local function close_frame(event, outcome)
    local frame = frame_for(event)
    if not frame then
      anomaly("unmatched-instruction-" .. outcome, event,
        "producer instruction terminal event has no matching begin")
      return
    end
    state.active_frames[event.delay_slot_depth] = nil
    if #frame.accesses == 0 then return end
    state.slices = state.slices + 1
    if outcome == "completed" then state.completed = state.completed + 1
    else state.aborted = state.aborted + 1 end
    emit_slice(frame, outcome, event)
  end

  function handlers.begin(event)
    advance_pending(event)
    if state.active_frames[event.delay_slot_depth] then
      anomaly("duplicate-instruction-begin", event,
        "producer depth already owns an open instruction")
    end
    state.active_frames[event.delay_slot_depth] = {
      backend = event.backend,
      pc = event.pc,
      opcode = event.opcode,
      delay_slot_depth = event.delay_slot_depth,
      begin_ordinal = event.ordinal_decimal,
      begin_tick = event.tick_decimal,
      registers_before = event.registers,
      accesses = {},
      exception = nil,
      context_epoch = state.context_epoch,
      context_origin = state.context_origin,
      call_path = copy_call_path(),
    }
  end

  function handlers.finish(event)
    advance_pending(event)
    close_frame(event, "completed")
  end

  function handlers.abort(event)
    if state.pending_call and same_owner(event, state.pending_call) then
      state.pending_call = nil
    else
      advance_pending(event)
    end
    local frame = frame_for(event)
    if frame then close_frame(event, "aborted") end
  end

  function handlers.access(event)
    advance_pending(event)
    local frame = frame_for(event)
    if not frame then
      anomaly("access-without-producer-begin", event,
        "matching native access was delivered without its producer begin")
      return
    end
    if #frame.accesses >= 64 then
      anomaly("instruction-access-limit", event,
        "one instruction exceeded 64 matching accesses")
      return
    end
    table.insert(frame.accesses, event)
    state.accesses = state.accesses + 1
  end

  function handlers.call(event)
    advance_pending(event)
    if state.pending_call then
      anomaly("nested-pending-call", event,
        "a second call appeared before the first call resolved")
      reset_context("after-nested-pending-call")
    end
    state.pending_call = {
      backend = event.backend,
      pc = event.pc,
      opcode = event.opcode,
      delay_slot_depth = event.delay_slot_depth,
      ordinal_decimal = event.ordinal_decimal,
      tick_decimal = event.tick_decimal,
      call_kind = event.call_kind,
      target_pc = event.target_pc,
      return_pc = event.return_pc,
      delay_slot_pc = event.delay_slot_pc,
      registers = event.registers,
    }
  end

  function handlers.ret(event)
    advance_pending(event)
    local top = state.call_stack[#state.call_stack]
    if not top then
      state.unmatched_returns = state.unmatched_returns + 1
      state.context_origin = "after-unobserved-root-return"
      return
    end
    if top.return_pc ~= event.target_pc then
      anomaly("return-mismatch", event,
        "RTS target does not close the latest observed call")
      reset_context("after-return-mismatch")
      return
    end
    table.remove(state.call_stack)
    state.returns = state.returns + 1
  end

  function handlers.exception(event)
    advance_pending(event)
    local frame = frame_for(event)
    if frame then frame.exception = event end
    reset_context("after-exception")
  end

  return handlers
end

return model
