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
  for line in string.gmatch(text, "([^\n]+)") do
    table.insert(lines, line)
  end
  return lines
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
medit.register_health_check("find-file", find_file_health)
medit.register_health_check("grep", grep_health)
medit.register_health_check("pick-theme", pick_theme_health)
medit.register_health_check("ai", ai_health)
