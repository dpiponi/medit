local function home_path(relative)
  local home = os.getenv("HOME")
  if not home or home == "" then
    return relative
  end
  return home .. "/" .. relative
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

local function project_file_list_command()
  if medit.executable_exists("fd") then
    return "fd -t f -H -I -E .git"
  end
  if medit.executable_exists("fdfind") then
    return "fdfind -t f -H -I -E .git"
  end
  return "rg --files --hidden --no-ignore -g '!.git'"
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
  local path = medit.current_file_path()
  if not is_theme_json_path(path) then
    medit.clear_line_annotations()
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

  local ok, err = pcall(medit.set_line_annotations, annotations)
  if not ok then
    medit.clear_line_annotations()
    medit.set_status(err)
  end
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

medit.register_command("lua-status", show_status_summary)
medit.register_command("edit-lua-init", edit_lua_init)
medit.register_command("find-file", find_file)
medit.register_command("open-file-under-cursor", open_file_under_cursor)
medit.register_command("grep", grep)
medit.register_command("pick-theme", pick_theme)
medit.register_command("ai", ai_edit)
medit.register_command("ai-popup", ai_popup)
medit.register_command("increment-number", increment_number)
medit.register_command("decrement-number", decrement_number)
medit.register_command("make", make_command)
medit.register_command("theme-preview-refresh", refresh_theme_preview)
medit.register_health_check("find-file", find_file_health)
medit.register_health_check("grep", grep_health)
medit.register_health_check("pick-theme", pick_theme_health)
medit.register_health_check("ai", ai_health)
medit.register_health_check("make", make_health)
medit.on("document_opened", function()
  refresh_theme_preview()
end)
medit.on("document_changed", function()
  refresh_theme_preview()
end)
medit.on("document_saved", function()
  refresh_theme_preview()
end)
