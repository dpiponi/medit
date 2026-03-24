local function home_path(relative)
  local home = os.getenv("HOME")
  if not home or home == "" then
    return relative
  end
  return home .. "/" .. relative
end

local init_script_dir = medit.current_working_directory() .. "/config/medit"
do
  local source = debug.getinfo(1, "S").source
  if source and string.sub(source, 1, 1) == "@" then
    local script_path = string.sub(source, 2)
    local dir = string.match(script_path, "^(.*)/[^/]*$")
    if dir and dir ~= "" then
      init_script_dir = dir
    end
  end
end

local function show_status_summary()
  local status = medit.status()
  medit.set_status(
    string.format(
      "mode=%s buffer=%d window=%d buffers=%d windows=%d",
      status.mode,
      status.active_buffer_id,
      status.active_window_id,
      status.buffer_count,
      status.window_count
    )
  )
end

local function edit_lua_init()
  medit.open_file(home_path(".config/medit/init.lua"))
  medit.set_status("Opened Lua init")
end

local function project_file_list_command(root)
  local command
  if medit.executable_exists("fd") then
    command = "fd -t f -H -I -E .git"
  elseif medit.executable_exists("fdfind") then
    command = "fdfind -t f -H -I -E .git"
  else
    command = "rg --files --hidden --no-ignore -g '!.git'"
  end

  if root and root ~= "" then
    return "cd " .. medit.shell_quote(root) .. " && " .. command
  end
  return command
end

local function find_file()
  if not medit.executable_exists("fzf") then
    medit.set_status("Missing executable: fzf")
    return
  end

  local selection, err = medit.run_picker(project_file_list_command() .. " | fzf")
  if not selection or selection == "" then
    medit.set_status(err or "picker canceled")
    return
  end

  medit.open_file(selection)
  medit.set_status("Opened " .. selection)
end

local function handle_startup(event)
  if not event.directory or #event.args ~= 1 then
    return false
  end
  if not medit.executable_exists("fzf") then
    medit.set_status("Missing executable: fzf")
    return true
  end

  local selection, err = medit.run_picker(project_file_list_command(event.directory) .. " | fzf")
  if not selection or selection == "" then
    if err and err ~= "" then
      medit.set_status(err)
    end
    return true
  end

  medit.open_location(event.directory .. "/" .. selection, { row = 0, column = 0 })
  medit.set_status("Opened " .. event.directory .. "/" .. selection)
  return true
end

local function find_file_health()
  local finder = "missing"
  if medit.executable_exists("fd") then
    finder = "fd"
  elseif medit.executable_exists("fdfind") then
    finder = "fdfind"
  elseif medit.executable_exists("rg") then
    finder = "rg"
  end

  local fzf = medit.executable_exists("fzf") and "yes" or "no"
  return string.format("finder=%s fzf=%s", finder, fzf)
end

local function parse_grep_selection(selection)
  local path, row, column = string.match(selection, "^(.-):(%d+):(%d+):")
  if not path then
    return nil
  end

  return {
    path = path,
    row = tonumber(row) - 1,
    column = tonumber(column) - 1
  }
end

local function resolve_path(path)
  if string.sub(path, 1, 1) == "/" then
    return path
  end
  return medit.current_working_directory() .. "/" .. path
end

local function split_lines(text)
  local lines = {}
  text = text or ""
  if text == "" then
    return lines
  end
  if string.sub(text, -1) ~= "\n" then
    text = text .. "\n"
  end
  for line in string.gmatch(text, "(.-)\n") do
    table.insert(lines, line)
  end
  return lines
end

local annotation_sources = {}

local function current_buffer_id()
  return medit.status().active_buffer_id
end

local function merged_annotations_for_buffer(buffer_id)
  local sources = annotation_sources[buffer_id]
  if not sources then
    return {}
  end

  local merged = {}
  local source_names = {}
  for source_name, _ in pairs(sources) do
    table.insert(source_names, source_name)
  end
  table.sort(source_names)

  for _, source_name in ipairs(source_names) do
    for _, annotation in ipairs(sources[source_name]) do
      table.insert(merged, annotation)
    end
  end

  table.sort(merged, function(left, right)
    if left.line ~= right.line then
      return left.line < right.line
    end
    return (left.source or "") < (right.source or "")
  end)
  return merged
end

local function apply_annotation_sources(buffer_id)
  local merged = merged_annotations_for_buffer(buffer_id)
  if #merged == 0 then
    medit.clear_line_annotations(buffer_id)
    return
  end
  medit.set_line_annotations(merged, buffer_id)
end

local function set_annotation_source(buffer_id, source_name, annotations)
  local sources = annotation_sources[buffer_id]
  local had_source = sources ~= nil and sources[source_name] ~= nil

  if (not annotations or #annotations == 0) and not had_source then
    return
  end

  if not sources then
    sources = {}
    annotation_sources[buffer_id] = sources
  end
  if annotations and #annotations > 0 then
    sources[source_name] = annotations
  else
    sources[source_name] = nil
  end
  if next(sources) == nil then
    annotation_sources[buffer_id] = nil
  end

  local ok, err = pcall(apply_annotation_sources, buffer_id)
  if not ok then
    medit.clear_line_annotations(buffer_id)
    medit.set_status(err)
  end
end

local function is_theme_json_path(path)
  if not path or path == "" then
    return false
  end
  local normalized = string.gsub(path, "\\", "/")
  return string.match(normalized, "/themes/") ~= nil and string.match(normalized, "%.json$") ~= nil
end

local function theme_annotation_for_line(row, line)
  local key, value = string.match(line, '"(foreground)"%s*:%s*"([^"]+)"')
  if not key then
    key, value = string.match(line, '"(background)"%s*:%s*"([^"]+)"')
  end
  if not key or not value then
    return nil
  end

  local annotation = {
    line = row,
    text = string.format(" %s %s ", key == "foreground" and "fg" or "bg", value),
    severity = "info",
    source = "theme-preview"
  }
  if not medit.theme_color_supported(value) then
    annotation.severity = "warning"
    annotation.text = string.format(" %s invalid %s ", key, value)
    return annotation
  end
  if key == "foreground" then
    annotation.style = {
      foreground = value,
      bold = true
    }
  else
    annotation.style = {
      foreground = "black",
      background = value,
      bold = true
    }
  end
  return annotation
end

local function refresh_theme_preview()
  local buffer_id = current_buffer_id()
  local path = medit.current_file_path()
  if not is_theme_json_path(path) then
    set_annotation_source(buffer_id, "theme-preview", nil)
    return
  end

  local annotations = {}
  local lines = split_lines(medit.get_buffer_text())
  for row, line in ipairs(lines) do
    local ok, annotation = pcall(theme_annotation_for_line, row - 1, line)
    if ok and annotation then
      table.insert(annotations, annotation)
    elseif not ok then
      table.insert(annotations, {
        line = row - 1,
        text = " theme preview parse failed ",
        severity = "warning",
        source = "theme-preview"
      })
    end
  end

  set_annotation_source(buffer_id, "theme-preview", annotations)
end

local calc_modes = {}

local function calc_state_for_buffer(buffer_id)
  local state = calc_modes[buffer_id]
  if not state then
    state = {
      line_cache = {}
    }
    calc_modes[buffer_id] = state
  end
  return state
end

local function trim(text)
  return string.match(text or "", "^%s*(.-)%s*$")
end

local function first_line(text)
  if not text or text == "" then
    return ""
  end
  local line = string.match(text, "([^\r\n]+)")
  return line or ""
end

local function calc_expression_for_line(line)
  local expr = string.match(line, "^%s*calc:%s*(.-)%s*$")
  if expr and expr ~= "" then
    return expr
  end
  return nil
end

local function calc_annotation_for_line(row, line)
  local expr = calc_expression_for_line(line)
  if not expr or expr == "" then
    return false
  end
  if string.sub(expr, -1) == "\\" then
    return false
  end

  local output, err = medit.run_filter("bc -l", expr .. "\n")
  if not output then
    return {
      line = row,
      text = " calc error ",
      severity = "warning",
      source = "calc"
    }
  end

  local result = trim(first_line(output))
  if result == "" then
    local summary = trim(first_line(err))
    if summary == "" then
      summary = "no result"
    end
    return {
      line = row,
      text = " calc " .. summary .. " ",
      severity = "warning",
      source = "calc"
    }
  end

  return {
    line = row,
    text = " = " .. result .. " ",
    severity = "info",
    source = "calc"
  }
end

local function calc_annotations_from_cache(line_cache)
  local annotations = {}
  local rows = {}
  for row, _ in pairs(line_cache) do
    table.insert(rows, row)
  end
  table.sort(rows)

  for _, row in ipairs(rows) do
    local entry = line_cache[row]
    if entry and entry.annotation then
      table.insert(annotations, entry.annotation)
    end
  end
  return annotations
end

local function refresh_calc_line(buffer_id, row)
  local state = calc_modes[buffer_id]
  if not state or not state.enabled then
    return
  end

  local ok, line = pcall(medit.get_line_text, row)
  if not ok then
    state.line_cache[row] = nil
  else
    local annotation = calc_annotation_for_line(row, line)
    state.line_cache[row] = {
      line = line,
      annotation = annotation
    }
  end

  set_annotation_source(buffer_id, "calc", calc_annotations_from_cache(state.line_cache))
end

local function refresh_calc_buffer(buffer_id)
  local state = calc_modes[buffer_id]
  if not state or not state.enabled then
    set_annotation_source(buffer_id, "calc", nil)
    return
  end

  local lines = split_lines(medit.get_buffer_text())
  local line_cache = {}
  for row, line in ipairs(lines) do
    line_cache[row - 1] = {
      line = line,
      annotation = calc_annotation_for_line(row - 1, line)
    }
  end
  state.line_cache = line_cache
  set_annotation_source(buffer_id, "calc", calc_annotations_from_cache(line_cache))
end

local function calc_mode_enabled(buffer_id)
  local state = calc_modes[buffer_id]
  return state ~= nil and state.enabled == true
end

local function enable_calc_mode()
  if not medit.executable_exists("bc") then
    medit.set_status("Missing executable: bc")
    return
  end

  local buffer_id = current_buffer_id()
  local state = calc_state_for_buffer(buffer_id)
  state.enabled = true
  refresh_calc_buffer(buffer_id)
  medit.set_status("Calc mode on")
end

local function disable_calc_mode()
  local buffer_id = current_buffer_id()
  calc_modes[buffer_id] = nil
  set_annotation_source(buffer_id, "calc", nil)
  medit.set_status("Calc mode off")
end

local function toggle_calc_mode()
  local buffer_id = current_buffer_id()
  if calc_mode_enabled(buffer_id) then
    disable_calc_mode()
  else
    enable_calc_mode()
  end
end

local function refresh_calc_mode(event)
  local buffer_id = current_buffer_id()
  if not calc_mode_enabled(buffer_id) then
    return
  end

  if event and event.range and event.range.start and event["text"] ~= nil then
    local start_row = event.range.start.row
    local end_row = event.range["end"] and event.range["end"].row or start_row
    local text = event.text or ""
    if start_row == end_row and not string.find(text, "\n", 1, true) then
      refresh_calc_line(buffer_id, start_row)
      return
    end
  end

  refresh_calc_buffer(buffer_id)
end

local function calc_mode_health()
  local bc = medit.executable_exists("bc") and "yes" or "no"
  return string.format("bc=%s", bc)
end

local notebook_modes = {}

local function notebook_python_executable()
  if medit.executable_exists("python3") then
    return "python3"
  end
  if medit.executable_exists("python") then
    return "python"
  end
  return nil
end

local function notebook_helper_path()
  return init_script_dir .. "/notebook_python.py"
end

local function notebook_state_for_buffer(buffer_id)
  local state = notebook_modes[buffer_id]
  if not state then
    state = {
      enabled = false,
      cells = {},
      cell_states = {},
      queued_cells = {},
      last_protocol_activity_at = 0,
      protocol_buffer = "",
      kernel_process_id = nil,
      output_buffer_id = nil
    }
    notebook_modes[buffer_id] = state
  end
  return state
end

local function notebook_trim(text)
  return string.match(text or "", "^%s*(.-)%s*$")
end

local function notebook_compact_summary(text)
  local summary = notebook_trim(first_line(text))
  if summary == "" then
    return nil
  end
  if #summary > 60 then
    summary = string.sub(summary, 1, 57) .. "..."
  end
  return summary
end

local function notebook_output_buffer_name(buffer_id)
  return string.format("notebook-python-%d", buffer_id)
end

local function notebook_output_buffer_id(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  state.output_buffer_id = medit.create_buffer(notebook_output_buffer_name(buffer_id), "output")
  return state.output_buffer_id
end

local function notebook_append_output(buffer_id, text)
  if not text or text == "" then
    return
  end
  medit.append_buffer(notebook_output_buffer_id(buffer_id), text)
end

local function notebook_clear_output_buffer(buffer_id)
  local buffer_id_value = notebook_output_buffer_id(buffer_id)
  medit.clear_buffer(buffer_id_value)
end

local function notebook_show_output(buffer_id)
  medit.show_buffer_in_panel(notebook_output_buffer_id(buffer_id), false)
end

local function notebook_cell_header(line)
  return string.match(line or "", "^%s*# %%%%")
end

local function notebook_non_whitespace_line(line)
  return notebook_trim(line or "") ~= ""
end

local function notebook_parse_cells(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  local cells = {}
  local lines = split_lines(medit.get_buffer_text())
  local current = nil

  for row, line in ipairs(lines) do
    local zero_row = row - 1
    if notebook_cell_header(line) then
      if current then
        current.end_row = zero_row - 1
        current.code = table.concat(current.code_lines, "\n")
        table.insert(cells, current)
      end
      current = {
        index = #cells + 1,
        header_row = zero_row,
        annotation_row = zero_row,
        start_row = zero_row,
        end_row = zero_row,
        code_lines = {}
      }
    elseif current then
      table.insert(current.code_lines, line)
      current.end_row = zero_row
      if notebook_non_whitespace_line(line) then
        current.annotation_row = zero_row
      end
      current.code = table.concat(current.code_lines, "\n")
    end
  end

  if current then
    current.code = table.concat(current.code_lines, "\n")
    table.insert(cells, current)
  end

  state.cells = cells
  local new_states = {}
  for _, cell in ipairs(cells) do
    new_states[cell.index] = state.cell_states[cell.index] or {}
  end
  state.cell_states = new_states
end

local function notebook_cell_for_cursor(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  local cursor = medit.get_cursor()
  for _, cell in ipairs(state.cells) do
    if cursor.row >= cell.start_row and cursor.row <= cell.end_row then
      return cell
    end
  end
  return nil
end

local function notebook_refresh_annotations(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  local annotations = {}
  for _, cell in ipairs(state.cells) do
    local cell_state = state.cell_states[cell.index] or {}
    local text = nil
    local severity = "info"
    if cell_state.running then
      text = " running "
    elseif cell_state.error_summary then
      text = " error: " .. cell_state.error_summary .. " "
      severity = "error"
    elseif cell_state.result_summary then
      text = " = " .. cell_state.result_summary .. " "
    elseif cell_state.output_summary then
      text = " out: " .. cell_state.output_summary .. " "
    end

    if text then
      table.insert(annotations, {
        line = cell.annotation_row or cell.header_row,
        text = text,
        severity = severity,
        source = "notebook-python"
      })
    end
  end
  set_annotation_source(buffer_id, "notebook-python", annotations)
end

local function notebook_reset_cell_state(cell_state)
  cell_state.running = false
  cell_state.result_summary = nil
  cell_state.output_summary = nil
  cell_state.error_summary = nil
end

local function notebook_kernel_running(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  if not state.kernel_process_id then
    return false
  end
  local status = medit.process_status(state.kernel_process_id)
  return status ~= nil and status.running == true
end

local function notebook_running_cell_index(buffer_id)
  local state = notebook_modes[buffer_id]
  if not state then
    return nil
  end
  for cell_index, cell_state in pairs(state.cell_states) do
    if cell_state.running then
      return cell_index
    end
  end
  return nil
end

local function notebook_run_is_stale(buffer_id, stale_seconds)
  local state = notebook_modes[buffer_id]
  if not state then
    return false
  end
  local running_index = notebook_running_cell_index(buffer_id)
  if not running_index then
    return false
  end
  local last_activity = state.last_protocol_activity_at or 0
  return os.time() - last_activity >= stale_seconds
end

local function notebook_handle_protocol_event(buffer_id, event_name, cell_index, payload)
  local state = notebook_modes[buffer_id]
  if not state then
    return
  end
  state.last_protocol_activity_at = os.time()
  local cell_state = state.cell_states[cell_index]

  if event_name == "ready" then
    return
  end
  if not cell_state then
    return
  end

  if event_name == "started" then
    notebook_reset_cell_state(cell_state)
    cell_state.running = true
    notebook_refresh_annotations(buffer_id)
    return
  end

  if event_name == "stdout" then
    notebook_append_output(buffer_id, payload)
    cell_state.output_summary = notebook_compact_summary(payload) or cell_state.output_summary
    notebook_refresh_annotations(buffer_id)
    return
  end

  if event_name == "stderr" then
    notebook_append_output(buffer_id, payload)
    cell_state.error_summary = notebook_compact_summary(payload) or "stderr"
    notebook_refresh_annotations(buffer_id)
    return
  end

  if event_name == "result" then
    notebook_append_output(buffer_id, "=> " .. payload .. "\n")
    cell_state.result_summary = notebook_compact_summary(payload)
    notebook_refresh_annotations(buffer_id)
    return
  end

  if event_name == "error" then
    notebook_append_output(buffer_id, payload)
    cell_state.error_summary = notebook_compact_summary(payload) or "error"
    notebook_refresh_annotations(buffer_id)
    return
  end

  if event_name == "done" then
    cell_state.running = false
    notebook_refresh_annotations(buffer_id)

    local next_index = nil
    if payload == "ok" and #state.queued_cells > 0 then
      next_index = table.remove(state.queued_cells, 1)
    else
      state.queued_cells = {}
    end
    if next_index then
      local next_cell = state.cells[next_index]
      if next_cell then
        notebook_append_output(buffer_id, string.format("\n# Cell %d\n", next_cell.index))
        notebook_reset_cell_state(state.cell_states[next_cell.index])
        medit.process_send(
          state.kernel_process_id,
          string.format("MEDIT exec %d %d\n%s", next_cell.index, #next_cell.code, next_cell.code)
        )
      end
    end
  end
end

local function notebook_consume_protocol(buffer_id, chunk)
  local state = notebook_modes[buffer_id]
  if not state then
    return
  end
  state.protocol_buffer = (state.protocol_buffer or "") .. (chunk or "")

  while true do
    local header_end = string.find(state.protocol_buffer, "\n", 1, true)
    if not header_end then
      return
    end

    local header = string.sub(state.protocol_buffer, 1, header_end - 1)
    local event_name, cell_text, length_text = string.match(header, "^MEDIT ([a-z_]+) ([^ ]+) (%d+)$")
    if not event_name then
      state.protocol_buffer = ""
      return
    end

    local payload_length = tonumber(length_text)
    local total_length = header_end + payload_length
    if #state.protocol_buffer < total_length then
      return
    end

    local payload = string.sub(state.protocol_buffer, header_end + 1, header_end + payload_length)
    state.protocol_buffer = string.sub(state.protocol_buffer, total_length + 1)
    notebook_handle_protocol_event(buffer_id, event_name, tonumber(cell_text) or 0, payload)
  end
end

local function notebook_kernel_command(buffer_id)
  local helper_path = notebook_helper_path()
  if not medit.file_exists(helper_path) then
    return nil, "Missing notebook helper: " .. helper_path
  end

  local python = notebook_python_executable()
  if not python then
    return nil, "Missing executable: python3/python"
  end

  return python .. " -u " .. medit.shell_quote(helper_path)
end

local function notebook_start_kernel(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  if notebook_kernel_running(buffer_id) then
    return true
  end

  local command, err = notebook_kernel_command(buffer_id)
  if not command then
    medit.set_status(err)
    return false
  end

  state.protocol_buffer = ""
  state.kernel_process_id = medit.process_start({
    command = command,
    on_stdout = function(_, text)
      notebook_consume_protocol(buffer_id, text)
    end,
    on_stderr = function(_, text)
      notebook_append_output(buffer_id, text)
      medit.set_status("Notebook Python kernel stderr")
    end,
    on_exit = function(_, exit_code)
      local current = notebook_modes[buffer_id]
      if not current then
        return
      end
      current.kernel_process_id = nil
      current.queued_cells = {}
      for _, cell_state in pairs(current.cell_states) do
        cell_state.running = false
      end
      notebook_refresh_annotations(buffer_id)
      medit.set_status(string.format("Notebook Python kernel exited (%d)", exit_code))
    end
  })
  return true
end

local function notebook_stop_kernel(buffer_id)
  local state = notebook_state_for_buffer(buffer_id)
  if state.kernel_process_id then
    medit.process_stop(state.kernel_process_id)
    state.kernel_process_id = nil
  end
  state.queued_cells = {}
  state.protocol_buffer = ""
  state.last_protocol_activity_at = 0
end

local function notebook_submit_cell(buffer_id, cell)
  local state = notebook_state_for_buffer(buffer_id)
  if not cell then
    medit.set_status("No notebook cell under cursor")
    return false
  end
  if notebook_trim(cell.code or "") == "" then
    medit.set_status("Notebook cell is empty")
    return false
  end
  if not notebook_start_kernel(buffer_id) then
    return false
  end

  notebook_append_output(buffer_id, string.format("\n# Cell %d\n", cell.index))
  notebook_reset_cell_state(state.cell_states[cell.index])
  state.last_protocol_activity_at = os.time()
  local ok = medit.process_send(
    state.kernel_process_id,
    string.format("MEDIT exec %d %d\n%s", cell.index, #cell.code, cell.code)
  )
  if not ok then
    medit.set_status("Notebook kernel is not accepting input")
    return false
  end
  return true
end

local function notebook_refresh_active_buffer(event)
  local buffer_id = event and event.buffer_id or current_buffer_id()
  if buffer_id ~= current_buffer_id() then
    return
  end
  local state = notebook_modes[buffer_id]
  if not state or not state.enabled then
    return
  end
  notebook_parse_cells(buffer_id)
  notebook_refresh_annotations(buffer_id)
end

local function notebook_python_on()
  local buffer_id = current_buffer_id()
  local state = notebook_state_for_buffer(buffer_id)
  state.enabled = true
  notebook_parse_cells(buffer_id)
  notebook_refresh_annotations(buffer_id)
  if not notebook_start_kernel(buffer_id) then
    return
  end
  medit.set_status("Notebook Python on")
end

local function notebook_python_off()
  local buffer_id = current_buffer_id()
  local state = notebook_modes[buffer_id]
  if not state then
    return
  end
  notebook_stop_kernel(buffer_id)
  notebook_modes[buffer_id] = nil
  set_annotation_source(buffer_id, "notebook-python", nil)
  medit.set_status("Notebook off")
end

local function notebook_run_cell()
  local buffer_id = current_buffer_id()
  local state = notebook_state_for_buffer(buffer_id)
  if not state.enabled then
    medit.set_status("Notebook is off")
    return
  end
  local running_index = notebook_running_cell_index(buffer_id)
  if running_index then
    if notebook_run_is_stale(buffer_id, 5) then
      notebook_stop_kernel(buffer_id)
      for _, cell_state in pairs(state.cell_states) do
        cell_state.running = false
      end
      notebook_refresh_annotations(buffer_id)
      medit.set_status("Notebook kernel restarted after stalled run")
    else
      medit.set_status("Notebook run already in progress")
      return
    end
  end
  if #state.queued_cells > 0 then
    medit.set_status("Notebook run already in progress")
    return
  end
  notebook_parse_cells(buffer_id)
  local cell = notebook_cell_for_cursor(buffer_id)
  if notebook_submit_cell(buffer_id, cell) then
    medit.set_status(string.format("Notebook cell %d running", cell.index))
  end
end

local function notebook_run_all()
  local buffer_id = current_buffer_id()
  local state = notebook_state_for_buffer(buffer_id)
  if not state.enabled then
    medit.set_status("Notebook is off")
    return
  end
  local running_index = notebook_running_cell_index(buffer_id)
  if running_index then
    if notebook_run_is_stale(buffer_id, 5) then
      notebook_stop_kernel(buffer_id)
      for _, cell_state in pairs(state.cell_states) do
        cell_state.running = false
      end
      notebook_refresh_annotations(buffer_id)
      medit.set_status("Notebook kernel restarted after stalled run")
    else
      medit.set_status("Notebook run already in progress")
      return
    end
  end
  if #state.queued_cells > 0 then
    medit.set_status("Notebook run already in progress")
    return
  end
  notebook_parse_cells(buffer_id)
  if #state.cells == 0 then
    medit.set_status("No notebook cells found")
    return
  end

  state.queued_cells = {}
  for index = 2, #state.cells do
    table.insert(state.queued_cells, index)
  end
  if notebook_submit_cell(buffer_id, state.cells[1]) then
    medit.set_status("Notebook run all started")
  else
    state.queued_cells = {}
  end
end

local function notebook_clear_output()
  local buffer_id = current_buffer_id()
  local state = notebook_modes[buffer_id]
  if not state then
    medit.set_status("Notebook is off")
    return
  end
  notebook_clear_output_buffer(buffer_id)
  for _, cell_state in pairs(state.cell_states) do
    notebook_reset_cell_state(cell_state)
  end
  notebook_refresh_annotations(buffer_id)
  medit.set_status("Notebook output cleared")
end

local function notebook_restart_kernel()
  local buffer_id = current_buffer_id()
  local state = notebook_modes[buffer_id]
  if not state or not state.enabled then
    medit.set_status("Notebook is off")
    return
  end
  notebook_stop_kernel(buffer_id)
  if notebook_start_kernel(buffer_id) then
    medit.set_status("Notebook Python kernel restarted")
  end
end

local function notebook_show_output_command()
  local buffer_id = current_buffer_id()
  local state = notebook_modes[buffer_id]
  if not state or not state.enabled then
    medit.set_status("Notebook is off")
    return
  end
  notebook_show_output(buffer_id)
  medit.set_status("Notebook output")
end

local function notebook_cleanup(event)
  local buffer_id = event and event.buffer_id
  if not buffer_id then
    return
  end
  local state = notebook_modes[buffer_id]
  if not state then
    return
  end
  notebook_stop_kernel(buffer_id)
  notebook_modes[buffer_id] = nil
end

local function notebook_python_health()
  local python = notebook_python_executable() and "yes" or "no"
  local helper = medit.file_exists(notebook_helper_path()) and "yes" or "no"
  return string.format("python=%s helper=%s", python, helper)
end

local function dirname(path)
  if not path or path == "" then
    return nil
  end
  local normalized = string.gsub(path, "\\", "/")
  local dir = string.match(normalized, "^(.*)/[^/]*$")
  return dir
end

local function resolve_direct_file_reference(token)
  if not token or token == "" then
    return nil
  end
  if string.sub(token, 1, 1) == "/" then
    return medit.file_exists(token) and token or nil
  end

  local roots = {}
  local current_file = medit.current_file_path()
  if current_file then
    local current_dir = dirname(current_file)
    if current_dir then
      table.insert(roots, current_dir)
    end
  end
  local workspace_root = medit.workspace_root()
  if workspace_root and workspace_root ~= "" then
    table.insert(roots, workspace_root)
  end
  table.insert(roots, medit.current_working_directory())

  for _, root in ipairs(roots) do
    local candidate = root .. "/" .. token
    if medit.file_exists(candidate) then
      return candidate
    end
  end
  return nil
end

local function workspace_file_matches(token)
  local workspace_root = medit.workspace_root()
  if not workspace_root or workspace_root == "" then
    return {}
  end
  local finder = nil
  if medit.executable_exists("fdfind") then
    finder = "fdfind"
  elseif medit.executable_exists("fd") then
    finder = "fd"
  else
    return {}
  end

  local pattern = token
  if not string.match(token, "/") then
    local filename = string.match(token, "([^/\\]+)$")
    pattern = filename or token
  else
    pattern = "*" .. token
  end

  local command = finder ..
    " -a -t f -g " .. medit.shell_quote(pattern) ..
    " " .. medit.shell_quote(workspace_root) ..
    " 2>/dev/null"
  local output, err = medit.run_filter(command, "")
  if not output then
    return nil, err
  end

  local matches = {}
  local seen = {}
  for _, line in ipairs(split_lines(output)) do
    if medit.file_exists(line) and not seen[line] then
      seen[line] = true
      table.insert(matches, line)
    end
  end
  table.sort(matches)
  return matches
end

local function open_file_under_cursor()
  local token = medit.token_under_cursor()
  if not token then
    medit.set_status("No file reference under cursor")
    return
  end

  local direct = resolve_direct_file_reference(token)
  if direct then
    medit.open_file(direct)
    medit.set_status("Opened " .. direct)
    return
  end

  local matches, err = workspace_file_matches(token)
  if matches == nil then
    medit.set_status(err or "file lookup failed")
    return
  end
  if #matches == 0 then
    if not medit.executable_exists("fdfind") and not medit.executable_exists("fd") then
      medit.set_status("Missing executable: fdfind/fd")
    else
      medit.set_status("File not found: " .. token)
    end
    return
  end
  if #matches > 1 then
    medit.set_status("Ambiguous file reference: " .. token)
    return
  end

  medit.open_file(matches[1])
  medit.set_status("Opened " .. matches[1])
end

local function grep(argument)
  if argument == nil or argument == "" then
    medit.set_status("No grep pattern")
    return
  end
  if not medit.executable_exists("rg") then
    medit.set_status("Missing executable: rg")
    return
  end
  if not medit.executable_exists("fzf") then
    medit.set_status("Missing executable: fzf")
    return
  end

  local command = "rg --column --line-number --no-heading --color=never --smart-case " ..
    medit.shell_quote(argument) .. " | fzf"
  local selection, err = medit.run_picker(command)
  if not selection or selection == "" then
    medit.set_status(err or "picker canceled")
    return
  end

  local match = parse_grep_selection(selection)
  if not match then
    medit.show_popup("grep", selection)
    medit.set_status("Could not parse grep result")
    return
  end

  medit.open_location(
    resolve_path(match.path),
    { row = match.row, column = match.column })
  medit.set_status("Opened " .. resolve_path(match.path))
end

local function grep_health()
  local rg = medit.executable_exists("rg") and "yes" or "no"
  local fzf = medit.executable_exists("fzf") and "yes" or "no"
  return string.format("rg=%s fzf=%s", rg, fzf)
end

local function pick_theme()
  local themes = medit.list_themes()
  if #themes == 0 then
    medit.set_status("No themes found")
    return
  end
  if not medit.executable_exists("fzf") then
    medit.set_status("Missing executable: fzf")
    return
  end

  local command = "printf '%s\\n'"
  for _, theme in ipairs(themes) do
    command = command .. " " .. medit.shell_quote(theme)
  end
  command = command .. " | fzf"

  local selection, err = medit.run_picker(command)
  if not selection or selection == "" then
    medit.set_status(err or "picker canceled")
    return
  end

  medit.set_config_value("colors", selection)
  medit.reload_config()
  medit.set_status("Theme: " .. selection)
end

local function pick_theme_health()
  local themes = medit.list_themes()
  local fzf = medit.executable_exists("fzf") and "yes" or "no"
  return string.format("themes=%d fzf=%s", #themes, fzf)
end

local function ai_command(argument, popup_only)
  if argument == nil or argument == "" then
    medit.set_status(popup_only and "No AI prompt" or "No AI instruction")
    return
  end

  local command_prefix = medit.resolve_ai_command()
  if not command_prefix then
    medit.set_status("No AI helper found; install medit-ai or set ai_command")
    return
  end

  local selection_text = medit.get_selection_text()
  local input_text = selection_text or medit.get_buffer_text()
  if not popup_only and input_text == "" then
    medit.set_status("No selection or buffer text")
    return
  end

  local provider = medit.resolve_ai_provider()
  local model = medit.resolve_ai_model(provider)
  local command = command_prefix ..
    " --mode " .. medit.shell_quote(popup_only and "ask" or "edit") ..
    " --provider " .. medit.shell_quote(provider) ..
    " --model " .. medit.shell_quote(model) ..
    " --prompt " .. medit.shell_quote(argument)

  local output, err = medit.run_filter(command, input_text)
  if not output then
    medit.set_status(err or "AI command failed")
    return
  end

  if popup_only then
    medit.show_popup("AI", output)
    medit.set_status("AI response ready")
    return
  end

  local changed
  if selection_text ~= nil then
    changed = medit.replace_selection(output)
  else
    changed = medit.replace_buffer(output)
  end
  medit.set_status(changed and "AI edit applied" or "AI produced no changes")
end

local function ai_edit(argument)
  ai_command(argument, false)
end

local function ai_popup(argument)
  ai_command(argument, true)
end

local function ai_health()
  local command = medit.resolve_ai_command()
  local provider = medit.resolve_ai_provider()
  local model = medit.resolve_ai_model(provider)
  local auth = "no"
  if provider == "openai" then
    auth = (os.getenv("OPENAI_API_KEY") and os.getenv("OPENAI_API_KEY") ~= "") and "yes" or "no"
  elseif provider == "mistral" then
    auth = (os.getenv("MISTRAL_API_KEY") and os.getenv("MISTRAL_API_KEY") ~= "") and "yes" or "no"
  end
  return string.format(
    "command=%s provider=%s model=%s auth=%s",
    command or "missing",
    provider,
    model,
    auth)
end

local function format_adjusted_number(original, value)
  local digits = string.match(original, "^%-?(%d+)$")
  if not digits then
    return tostring(value)
  end

  if string.sub(digits, 1, 1) == "0" and #digits > 1 then
    local sign = value < 0 and "-" or ""
    return sign .. string.format("%0" .. tostring(#digits) .. "d", math.abs(value))
  end

  return tostring(value)
end

local function char_at(row, column)
  return medit.get_text({
    start = { row = row, column = column },
    ["end"] = { row = row, column = column + 1 }
  })
end

local function is_digit_char(ch)
  return ch ~= nil and string.match(ch, "^%d$") ~= nil
end

local function adjust_number_under_cursor(delta)
  local cursor = medit.get_cursor()
  local row = cursor.row
  local column = cursor.column
  local current = char_at(row, column)
  local start_column = nil
  local end_column = nil

  if is_digit_char(current) then
    start_column = column
    end_column = column + 1
  elseif current == "-" and is_digit_char(char_at(row, column + 1)) then
    start_column = column
    end_column = column + 2
  elseif column > 0 and is_digit_char(char_at(row, column - 1)) then
    start_column = column - 1
    end_column = column
  else
    medit.set_status("No number under cursor")
    return
  end

  while start_column > 0 and is_digit_char(char_at(row, start_column - 1)) do
    start_column = start_column - 1
  end
  if start_column > 0 and char_at(row, start_column - 1) == "-" then
    start_column = start_column - 1
  end
  while is_digit_char(char_at(row, end_column)) do
    end_column = end_column + 1
  end

  local matched = medit.get_text({
    start = { row = row, column = start_column },
    ["end"] = { row = row, column = end_column }
  })
  local value = tonumber(matched)
  if value == nil then
    medit.set_status("Could not parse number")
    return
  end

  local updated = format_adjusted_number(matched, value + delta)
  local changed = medit.replace_range(
    {
      start = { row = row, column = start_column },
      ["end"] = { row = row, column = end_column }
    },
    updated)

  local relative_column = column - start_column
  local new_column = start_column + math.min(relative_column, #updated > 0 and #updated - 1 or 0)
  medit.set_cursor({ row = row, column = new_column })
  medit.set_status(changed and ("Number: " .. updated) or "Number unchanged")
end

local function increment_number()
  adjust_number_under_cursor(1)
end

local function decrement_number()
  adjust_number_under_cursor(-1)
end

local function make_health()
  local make = medit.executable_exists("make") and "yes" or "no"
  return string.format("make=%s", make)
end

local function make_command(argument)
  if not medit.executable_exists("make") then
    medit.set_status("Missing executable: make")
    return
  end

  local command = "make"
  if argument ~= nil and argument ~= "" then
    command = command .. " " .. argument
  end

  local buffer_id = medit.create_buffer("make", "output")
  medit.clear_buffer(buffer_id)
  medit.append_buffer(buffer_id, "$ " .. command .. "\n\n")
  medit.show_buffer_in_panel(buffer_id, false)

  local job_id = medit.job_start({
    command = command,
    buffer_id = buffer_id,
    on_exit = function(_, exit_code)
      medit.append_buffer(buffer_id, string.format("\n[make exited %d]\n", exit_code))
      if exit_code == 0 then
        medit.set_status("make finished")
      else
        medit.set_status(string.format("make failed (%d)", exit_code))
      end
    end
  })

  medit.set_status(string.format("Started make job %d", job_id))
end

local function calc_refresh()
  refresh_calc_mode()
  medit.set_status("Calc refreshed")
end

medit.register_command("lua-status", show_status_summary, { detail = "show Lua status summary" })
medit.register_command("edit-lua-init", edit_lua_init, { detail = "open init.lua for editing" })
medit.register_command("find-file", find_file, { detail = "find file with project picker" })
medit.register_command("open-file-under-cursor", open_file_under_cursor, {
  detail = "open file path under cursor",
  aliases = { "gf" }
})
medit.register_command("grep", grep, { detail = "search project with ripgrep" })
medit.register_command("pick-theme", pick_theme, { detail = "pick theme with preview" })
medit.register_command("ai", ai_edit, { detail = "rewrite selection or buffer via AI" })
medit.register_command("ai-popup", ai_popup, { detail = "show AI output in popup" })
medit.register_command("increment-number", increment_number, { detail = "increment number under cursor" })
medit.register_command("decrement-number", decrement_number, { detail = "decrement number under cursor" })
medit.register_command("make", make_command, { detail = "run make asynchronously" })
medit.register_command("theme-preview-refresh", refresh_theme_preview, { detail = "refresh theme preview annotations" })
medit.register_command("calc-mode-on", enable_calc_mode, { detail = "enable calc annotations" })
medit.register_command("calc-mode-off", disable_calc_mode, { detail = "disable calc annotations" })
medit.register_command("calc-mode-toggle", toggle_calc_mode, { detail = "toggle calc annotations" })
medit.register_command("calc-refresh", calc_refresh, { detail = "refresh calc annotations" })
medit.register_command("notebook-python-on", notebook_python_on, { detail = "enable Python notebook mode" })
medit.register_command("notebook-python-off", notebook_python_off, { detail = "disable Python notebook mode" })
medit.register_command("notebook-run-cell", notebook_run_cell, { detail = "run current notebook cell" })
medit.register_command("notebook-run-all", notebook_run_all, { detail = "run all notebook cells" })
medit.register_command("notebook-clear-output", notebook_clear_output, { detail = "clear notebook output annotations" })
medit.register_command("notebook-restart-kernel", notebook_restart_kernel, { detail = "restart notebook kernel" })
medit.register_command("notebook-show-output", notebook_show_output_command, { detail = "show notebook output popup" })
medit.register_health_check("find-file", find_file_health)
medit.register_health_check("grep", grep_health)
medit.register_health_check("pick-theme", pick_theme_health)
medit.register_health_check("ai", ai_health)
medit.register_health_check("make", make_health)
medit.register_health_check("calc-mode", calc_mode_health)
medit.register_health_check("notebook-python", notebook_python_health)
medit.on_startup(handle_startup)
medit.on("document_opened", function(event)
  refresh_theme_preview()
  refresh_calc_mode(event)
  notebook_refresh_active_buffer(event)
end)
medit.on("document_changed", function(event)
  refresh_theme_preview()
  refresh_calc_mode(event)
  notebook_refresh_active_buffer(event)
end)
medit.on("document_saved", function(event)
  refresh_theme_preview()
  refresh_calc_mode(event)
  notebook_refresh_active_buffer(event)
end)
medit.on("document_closed", function(event)
  notebook_cleanup(event)
end)
