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

medit.register_command("lua-status", show_status_summary)
medit.register_command("edit-lua-init", edit_lua_init)
medit.register_command("find-file", find_file)
medit.register_command("grep", grep)
medit.register_health_check("find-file", find_file_health)
medit.register_health_check("grep", grep_health)
